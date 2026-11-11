// FileServer::Impl 实体（LAY-003 拆分）：判权策略、会话令牌、上传/删除
// 受理（原 files_server.cpp 私有类，内部头供各路由模块共享）。
#pragma once

#include "files_server.hpp"


#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"

#include "files_server/common.hpp"

namespace memex::server {

struct FileServer::Impl {
  ServerStore& store;
  std::shared_ptr<S3Storage> storage;
  // 544 LDAP 企业身份：可空＝未配目录（判登全本地）。
  LdapDirectory* ldap{nullptr};
  AuthorizationService az;
  // R23-4：true=外网单向面实例（只挂 /uplink/* 写入端点）
  bool uplink_mode{false};
  // R23-5 外网面防护参数（只在 uplink_mode=true 实例消费）
  UplinkPolicy uplink_policy;
  // 会话令牌库：共享句柄（R23-4 双实例一面一份没意义，见 FileSessions
  // 注）。单 io_context 线程驱动（与消息面/webhook 同一线程模型），不加锁。
  std::shared_ptr<FileSessions> sessions;
  // R25-2 工具结果卡片回群回调（缺省未设＝只落账不回群）
  GroupNoticeFn notice;
  // R26 群工具结果卡片回群回调（TEXT 群消息；缺省未设＝不回群）
  GroupTextFn group_text;
  // R25-4 凭据面 GCM 密钥（主密钥 SHA-256 派生 32B raw；空＝主密钥未
  // 配置＝凭据面未启用，路由 503）
  std::string tool_cred_key;
  // R27-5 任务源拉取面 GCM 密钥（derive_task_pull_key 派生 32B raw；空＝
  // 主密钥未配置＝拉取面未启用，/files/task-sources 一律 503）
  std::string task_pull_key;
  // 远程协助媒体槽（二期）：会话 id → 最新帧＋待取输入事件。跨连接共享
  // 故挂 Impl（推/拉分属两条连接）；单 io_context 线程不加锁。屏幕帧
  // 只存最新一笔且不落库（内容敏感），会话终态即擦槽。
  struct AssistMedia {
    std::int64_t frame_seq{0};
    std::string frame_b64;
    std::vector<std::string> inputs; // FIFO，受控方取走即清
  };
  std::unordered_map<std::string, AssistMedia> assist_media;

  explicit Impl(ServerStore& s, std::shared_ptr<S3Storage> st,
                bool uplink = false,
                std::shared_ptr<FileSessions> shared_sessions = {},
                UplinkPolicy policy = {}, LdapDirectory* l = nullptr)
      : store(s), storage(std::move(st)), ldap(l), uplink_mode(uplink),
        uplink_policy(std::move(policy)),
        sessions(shared_sessions ? std::move(shared_sessions)
                                 : std::make_shared<FileSessions>()) {
    register_policies();
  }

  // —— 判权策略（R23-2 文件面；冲突规则见 authz.hpp 顶注）——
  void register_policies() {
    // 显式拒绝：非 normal 状态文件不给读字节（隔离=杀毒待审、过期）
    az.add_rule(RuleEffect::ExplicitDeny, "file-not-normal",
                [this](const AuthzQuery& q) {
                  if (q.action != "file:read") return false;
                  const auto fid = resource_file_id(q.resource);
                  if (!fid) return false;
                  const auto m = store.file_by_id(*fid);
                  return m.has_value() &&
                         m->status != ServerStore::FileStatus::Normal;
                });
    // 显式允许：个人文件仅本人（读写管全归 owner）
    az.add_rule(RuleEffect::ExplicitAllow, "personal-owner",
                [this](const AuthzQuery& q) {
                  const std::string uid = resource_owner_prefix(q.resource);
                  return !uid.empty() && uid == q.subject;
                });
    // 显式允许：群主管群文件（删/置顶/配额/读/列；传=成员继承面）
    az.add_rule(RuleEffect::ExplicitAllow, "group-owner",
                [this](const AuthzQuery& q) {
                  const auto gid = resource_group_id(q.resource);
                  if (!gid) return false;
                  if (q.action == "file:upload") return false;
                  const auto info = store.group_info(*gid);
                  return info.has_value() && info->owner == q.subject;
                });
    // 显式允许：群管理员管群文件（权限模型：管理员=工具配置/成员管理/
    // 维护；同群主的管理动作，唯不可改 owner 身份——那不归文件面；
    // vault:config=密码箱授权名单收窄权，设计仅群主）
    az.add_rule(RuleEffect::ExplicitAllow, "group-admin",
                [this](const AuthzQuery& q) {
                  const auto gid = resource_group_id(q.resource);
                  if (!gid) return false;
                  if (q.action == "file:upload") return false;
                  if (q.action == "vault:config") return false;
                  return store.group_role(*gid, q.subject) == "admin";
                });
    // 继承允许：群成员读/列/传（入群即继承、退群即失——group_role 现查）
    az.add_rule(RuleEffect::Inherited, "group-member",
                [this](const AuthzQuery& q) {
                  const auto gid = resource_group_id(q.resource);
                  if (!gid) return false;
                  if (q.action != "file:read" && q.action != "file:list" &&
                      q.action != "file:upload") {
                    return false;
                  }
                  return !store.group_role(*gid, q.subject).empty();
                });
    // 显式允许：任务分配（R27-1 权限模型「谁能分配」）——同任一群的
    // 共同成员，或同部门（department_path 相同且非空）；无关系默认拒
    az.add_rule(RuleEffect::ExplicitAllow, "task-assign",
                [this](const AuthzQuery& q) {
                  if (q.action != "task:assign") return false;
                  const std::string target =
                      resource_owner_prefix(q.resource);
                  if (target.empty() || target == q.subject) return false;
                  if (store.co_members(q.subject, target)) return true;
                  const auto pa = store.member_profile(q.subject);
                  const auto pb = store.member_profile(target);
                  return pa.has_value() && pb.has_value() &&
                         !pa->department_path.empty() &&
                         pa->department_path == pb->department_path;
                });
    // 显式拒绝：自建自审不成立（申请人本人对己申请无决定权）。deny 层
    // 先于一切 allow——放 deny 是钉死不变量：后续任何 allow 规则（含
    // personal-owner 这类不滤 action 的属主规则）都越不过自审红线
    az.add_rule(RuleEffect::ExplicitDeny, "approval-self-decide",
                [](const AuthzQuery& q) {
                  if (q.action != "approval:decide") return false;
                  const std::string applicant =
                      resource_owner_prefix(q.resource);
                  return !applicant.empty() && applicant == q.subject;
                });
    // 显式允许：审批决定（二期·审批）——申请人的直属上级（平台-3
    // 汇报线现查现裁），或 org-admin 基础角色兜底且申请人无直属上级；
    // 未命中 default-deny 不自造
    az.add_rule(RuleEffect::ExplicitAllow, "approval-decide",
                [this](const AuthzQuery& q) {
                  if (q.action != "approval:decide") return false;
                  const std::string applicant =
                      resource_owner_prefix(q.resource);
                  if (applicant.empty() || applicant == q.subject) {
                    return false; // 自审不成立（自建申请须他人决）
                  }
                  const auto chain = store.manager_chain(applicant);
                  if (!chain.empty()) return chain.front() == q.subject;
                  // 无直属上级：org-admin 兜底（基础或追加角色皆算）
                  const auto roles = store.effective_roles(q.subject, now_ms());
                  for (const auto& r : roles) {
                    if (r == "org-admin") return true;
                  }
                  return false;
                });
    // 显式允许：日报周报查看（二期）——自己看自己，或作者的直属上级
    //（平台-3 权威表现查 front() 命中；org-admin 不兜底=设计口径日报
    // 只对直属上级开放）；未命中 default-deny（幽灵账号同 403 不泄露）
    az.add_rule(RuleEffect::ExplicitAllow, "report-read",
                [this](const AuthzQuery& q) {
                  if (q.action != "report:read") return false;
                  const std::string author =
                      resource_owner_prefix(q.resource);
                  if (author.empty()) return false;
                  if (author == q.subject) return true; // 自己看自己
                  const auto chain = store.manager_chain(author);
                  return !chain.empty() && chain.front() == q.subject;
                });
    // 显式允许：下属任务与看板（R27-6 领导视角，设计 §6.4）——只有下属
    // 的直属上级可读（manager_chain front() 现查命中；不递归=只一级，
    // 跨级关闭）；平级与其他部门领导一律不可见，本规则**无 org-admin
    // 兜底**（按回执原文写死，与 report-read 的直属上级分支同口径）；
    // 未命中 default-deny（幽灵账号同 403 不泄露存在性）
    az.add_rule(RuleEffect::ExplicitAllow, "task-team-read",
                [this](const AuthzQuery& q) {
                  if (q.action != "task:team-read") return false;
                  const std::string owner =
                      resource_owner_prefix(q.resource);
                  if (owner.empty()) return false;
                  const auto chain = store.manager_chain(owner);
                  return !chain.empty() && chain.front() == q.subject;
                });
    // 显式允许：会话审计查阅（二期）——持 auditor 有效角色者（平台-6
    // 口径：SecurityAuditor≠SystemAdmin，admin 有效角色不自动可读消息）；
    // 被拒尝试在路由层落 audit.denied（与 CLI T3.2 同款，审计自身被拒
    // 可对账）
    az.add_rule(RuleEffect::ExplicitAllow, "audit-read",
                [this](const AuthzQuery& q) {
                  if (q.action != "audit:read") return false;
                  const auto roles = store.effective_roles(q.subject, now_ms());
                  for (const auto& r : roles) {
                    if (r == "auditor") return true;
                  }
                  return false;
                });
    // 显式允许：办公室位置图编辑（二期）——org-admin 基础角色（组织级
    // 面归组织管理员；群面才是 group-admin，层级不混）；未命中 default-deny
    az.add_rule(RuleEffect::ExplicitAllow, "office-manage",
                [this](const AuthzQuery& q) {
                  if (q.action != "office:manage") return false;
                  const auto roles = store.effective_roles(q.subject, now_ms());
                  for (const auto& r : roles) {
                    if (r == "org-admin") return true;
                  }
                  return false;
                });
    // 显式允许：品牌物料管理（设计稿 docs/design/品牌物料.md）——同
    // office-manage 口径：组织级面归 org-admin；未命中 default-deny
    az.add_rule(RuleEffect::ExplicitAllow, "branding-manage",
                [this](const AuthzQuery& q) {
                  if (q.action != "branding:manage") return false;
                  const auto roles = store.effective_roles(q.subject, now_ms());
                  for (const auto& r : roles) {
                    if (r == "org-admin") return true;
                  }
                  return false;
                });
  }

  // 资源串约定："group:{gid}[/file:{id}]"、"user:{uid}[/file:{id}]"
  static std::optional<std::uint64_t> resource_group_id(
      const std::string& r) {
    if (r.rfind("group:", 0) != 0) return std::nullopt;
    const std::size_t slash = r.find('/');
    const std::string gid =
        r.substr(6, slash == std::string::npos ? std::string::npos
                                               : slash - 6);
    if (gid.empty() || gid.find_first_not_of("0123456789") != std::string::npos) {
      return std::nullopt;
    }
    return std::strtoull(gid.c_str(), nullptr, 10);
  }
  static std::string resource_owner_prefix(const std::string& r) {
    if (r.rfind("user:", 0) != 0) return "";
    const std::size_t slash = r.find('/');
    return r.substr(5, slash == std::string::npos ? std::string::npos
                                                  : slash - 5);
  }
  static std::optional<std::int64_t> resource_file_id(const std::string& r) {
    const std::size_t slash = r.find("/file:");
    if (slash == std::string::npos) return std::nullopt;
    const std::string id = r.substr(slash + 6);
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
      return std::nullopt;
    }
    return std::strtoll(id.c_str(), nullptr, 10);
  }

  // —— 会话令牌 ——
  std::string mint_session(const std::string& account, bool uplink = false) {
    prune_sessions();
    const std::string token = random_salt_hex(); // 16B 熵 → 32 hex
    if (token.empty()) return "";
    const std::int64_t now = now_ms();
    const std::string hash = sha256_hex(token);
    sessions->map[hash] = FileSession{account, now + kSessionTtlMs, uplink};
    // 平台-2 会话台账：签发落行（device_id 预留空；尽力落、裁决仍在内存）
    store.session_insert(ServerStore::SessionRecord{hash, account,
                                                    uplink ? "uplink"
                                                           : "internal",
                                                    "", now,
                                                    now + kSessionTtlMs, 0,
                                                    ""});
    return token;
  }
  // 有效回会话；无效/过期回空。scope 判定走它（R23-4 两道闸之一）。
  std::optional<FileSession> auth_session_full(
      const std::string& header_value) {
    const std::string prefix = "Bearer ";
    if (header_value.rfind(prefix, 0) != 0) return std::nullopt;
    const std::string token = header_value.substr(prefix.size());
    if (token.empty()) return std::nullopt;
    prune_sessions();
    const auto it = sessions->map.find(sha256_hex(token));
    if (it == sessions->map.end()) return std::nullopt;
    if (it->second.expires_ms < now_ms()) {
      sessions->map.erase(it);
      return std::nullopt;
    }
    return it->second;
  }
  // 有效返回账号；无效/过期返回空串
  std::string auth_session(const std::string& header_value) {
    const auto s = auth_session_full(header_value);
    return s.has_value() ? s->account : "";
  }
  // 请求头里是否 uplink 令牌（内网面读端点前的单向闸，解析失败=否）
  bool is_uplink_token(const std::string& header_value) {
    const auto s = auth_session_full(header_value);
    return s.has_value() && s->uplink;
  }
  void prune_sessions() {
    const std::int64_t now = now_ms();
    for (auto it = sessions->map.begin(); it != sessions->map.end();) {
      if (it->second.expires_ms < now) {
        it = sessions->map.erase(it);
      } else {
        ++it;
      }
    }
  }

  // —— 上传（受理序：判权 → 秒传 → 扣费 → 落字节 → 落元数据；任一步
  //     失败已扣的费/已落的字节全额回滚，不留半截状态）——
  struct UploadOutcome {
    int http_status{200};
    std::string error;
    std::int64_t file_id{0};
    bool second_transfer{false};
    std::string file_hash;  // 带回给审计流水（R23-4 uplink）
    std::string object_key; // 同上（落点留痕：谁/何时/什么/落到哪个键）
  };
  UploadOutcome handle_upload(
      const std::string& account, bool is_group, std::uint64_t gid,
      const std::string& uid, const std::string& file_name,
      const std::string& body, bool is_inbox,
      ServerStore::FileSource source = ServerStore::FileSource::Internal,
      const UploadScanner* scanner = nullptr) {
    UploadOutcome out;
    if (!storage) {
      out.http_status = 503;
      out.error = "存储后端未配置";
      return out;
    }
    if (file_name.empty() || file_name.size() > 255 ||
        file_name.find('\r') != std::string::npos ||
        file_name.find('\n') != std::string::npos) {
      out.http_status = 400;
      out.error = "文件名非法（1..255 字节，不得含换行）";
      return out;
    }
    const std::string hash = sha256_hex(body);
    const std::string gid_s = is_group ? std::to_string(gid) : "";
    // 类目入秒传键：收件箱与个人空间是两个空间，互不秒传串用
    const ServerStore::FileKind kind = is_inbox
                                           ? ServerStore::FileKind::Inbox
                                           : ServerStore::FileKind::Personal;
    // 秒传：同属主+同哈希+同归属+同类目 → 复用，不扣费不落字节
    if (const auto hit =
            store.check_second_transfer(account, hash, gid_s, uid, kind)) {
      out.file_id = hit->id;
      out.second_transfer = true;
      out.file_hash = hit->file_hash;
      out.object_key = hit->object_key;
      return out;
    }
    // R23-5 扫描钩子（受理序：判权→秒传→扫描→扣费→落字节→落元数据）。
    // 秒传命中已跳过——同哈希同归属内容不变，判定幂等；命中拒收 422
    //（此步在扣费/落字节之前，无回滚面；审计流水由调用面照记）
    if (scanner && scanner->scan(file_name, body) ==
                       UploadScanner::Verdict::Infected) {
      out.http_status = 422;
      out.error = "文件未通过安全扫描（已拒收）";
      return out;
    }
    // 对象键按内容寻址于目标前缀内（设计铁律 3：groups/{gid}/、users/{uid}/）
    const std::string object_key =
        is_group ? "groups/" + gid_s + "/" + hash : "users/" + uid + "/" + hash;
    // 两级配额受检扣费（群文件扣群+传者个人；个人文件只扣个人）
    const bool charge_g = !is_group || store.charge_group_quota(gid_s, static_cast<std::int64_t>(body.size()));
    const bool charge_u = store.charge_user_quota(account, static_cast<std::int64_t>(body.size()));
    if (!charge_g || !charge_u) {
      if (charge_g && is_group) store.add_group_quota_used(gid_s, -static_cast<std::int64_t>(body.size()));
      if (charge_u) store.add_user_quota_used(account, -static_cast<std::int64_t>(body.size()));
      out.http_status = 413;
      out.error = "配额不足";
      return out;
    }
    std::string etag;
    if (!storage->put_object(object_key, body, &etag)) {
      if (is_group) store.add_group_quota_used(gid_s, -static_cast<std::int64_t>(body.size()));
      store.add_user_quota_used(account, -static_cast<std::int64_t>(body.size()));
      out.http_status = 502;
      out.error = "对象存储写入失败";
      return out;
    }
    ServerStore::FileMeta meta;
    meta.owner = account;
    meta.belong_gid = gid_s;
    meta.belong_uid = uid;
    meta.file_name = file_name;
    meta.file_size = static_cast<std::int64_t>(body.size());
    meta.file_hash = hash;
    meta.object_key = object_key;
    meta.source = source;
    meta.upload_ts = now_ms();
    meta.kind = kind;
    out.file_hash = hash;
    out.object_key = object_key;
    out.file_id = store.create_file_meta(meta);
    if (out.file_id <= 0) {
      if (is_group) store.add_group_quota_used(gid_s, -static_cast<std::int64_t>(body.size()));
      store.add_user_quota_used(account, -static_cast<std::int64_t>(body.size()));
      storage->delete_object(object_key);
      out.http_status = 500;
      out.error = "元数据落库失败";
      return out;
    }
    // 秒传判未中但建行命中既有行＝同键行存在但非 normal（如隔离）：
    // 回滚扣费，明示冲突——不给隔离文件变相放行，也不二次扣费。
    // 字节不删：键内容寻址且被既有行引用（同哈希同归属必同键），
    // 删了会让隔离行数据悬空（恢复后无字节可读）——引用计数归零再删。
    const auto after = store.file_by_id(out.file_id);
    if (!after.has_value() ||
        after->status != ServerStore::FileStatus::Normal) {
      if (is_group) store.add_group_quota_used(gid_s, -static_cast<std::int64_t>(body.size()));
      store.add_user_quota_used(account, -static_cast<std::int64_t>(body.size()));
      out.http_status = 409;
      out.error = "同名同哈希文件已存在且当前不可用（隔离/过期）";
      return out;
    }
    return out;
  }

  // —— 删除（判权 → 删元数据 → 引用计数归零才删字节 → 退费（逻辑账））——
  struct ManageOutcome {
    int http_status{200};
    std::string error;
    bool deleted{false};
    std::string reason; // 判权理由（审计回显）
  };
  ManageOutcome handle_delete(const std::string& account,
                              std::int64_t file_id) {
    ManageOutcome out;
    const auto meta = store.file_by_id(file_id);
    if (!meta.has_value()) {
      out.http_status = 404;
      out.error = "文件不存在";
      return out;
    }
    const std::string resource =
        meta->belong_gid.empty()
            ? "user:" + meta->belong_uid + "/file:" + std::to_string(file_id)
            : "group:" + meta->belong_gid + "/file:" + std::to_string(file_id);
    const Decision d = az.authorize(
        {account, "file:delete", resource, "owner=" + account});
    out.reason = d.reason;
    if (!d.allowed) {
      out.http_status = 403;
      out.error = "无权删除（" + d.reason + "）";
      return out;
    }
    // 平台-12 群文件能力门：停用即拒（群文件面整面熄灭含删除）
    if (!meta->belong_gid.empty() &&
        !store.group_capability_enabled(
            static_cast<std::uint64_t>(std::stoull(meta->belong_gid)),
            "files")) {
      out.http_status = 403;
      out.error = "群文件能力已被管理员停用（deny:capability-files）";
      return out;
    }
    if (!store.delete_file_meta(file_id)) {
      out.http_status = 404;
      out.error = "文件不存在";
      return out;
    }
    // 字节清理：内容寻址下同对象可能仍被其他文件行引用，归零才删。
    // 存储未配置（storage 空）只跳过字节面，元数据照删。
    // 存储层删除失败不回滚元数据（过期清理面会对账，v1 注记）。
    if (storage && store.count_file_refs(meta->object_key) == 0) {
      storage->delete_object(meta->object_key);
    }
    // 退费＝逻辑账（配额口径按文件行计，见 R23-2 设计注记）
    if (!meta->belong_gid.empty()) {
      store.add_group_quota_used(meta->belong_gid, -meta->file_size);
    }
    store.add_user_quota_used(meta->owner, -meta->file_size);
    out.deleted = true;
    return out;
  }
};

} // namespace memex::server
