#include "files_server.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"

namespace memex::server {

namespace {

using json = nlohmann::json;
using tcp = asio::ip::tcp;

constexpr std::size_t kMaxHead = 8 * 1024;      // 请求头上限（防呆）
constexpr std::size_t kMaxJsonBody = 64 * 1024; // session/manage JSON 上限
constexpr std::size_t kMaxUpload = 512u * 1024 * 1024; // 单文件上限 512MiB
constexpr std::int64_t kSessionTtlMs = 12 * 3600 * 1000; // 令牌 12h
// R26-1 agent 在线判定窗：默认 30s 心跳的 3 倍宽限（错过两拍仍算在线）
constexpr std::int64_t kAgentOnlineMs = 90 * 1000;
// R26-3 一次性短票有效期：签发后 60s 内须兑现（过期=401 重签）
constexpr std::int64_t kSessionTicketTtlMs = 60 * 1000;

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 日报归属日校验（YYYY-MM-DD 形态；不做历法深查——客户端日期控件给出，
// 形态门只挡明显坏值）
bool is_date_ymd(const std::string& s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i == 4 || i == 7) continue;
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

const char* reason_phrase(int status) {
  switch (status) {
  case 200: return "OK";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 413: return "Payload Too Large";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  default: return "Internal Server Error";
  }
}

struct FileSession {
  std::string account;
  std::int64_t expires_ms{0};
  // 令牌 scope（R23-4 两套路由两套 scope）：false=内网文件面全量；
  // true=外网单向 uplink（只许 /uplink/* 写入，内网面一律 403）
  bool uplink{false};
};

// "group:3" / "me" / "inbox" → is_group/gid/uid/is_inbox；非法返回 false
// inbox=文件助手收件箱：个人空间归属（uid=本人），类目由调用方落 kind
bool parse_target(const std::string& t, const std::string& me, bool& is_group,
                  std::uint64_t& gid, std::string& uid, bool& is_inbox) {
  is_inbox = false;
  if (t == "me" || t == "inbox") {
    is_group = false;
    gid = 0;
    uid = me;
    is_inbox = t == "inbox";
    return !me.empty();
  }
  if (t.rfind("group:", 0) == 0) {
    const std::string n = t.substr(6);
    if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos) {
      return false;
    }
    is_group = true;
    gid = std::strtoull(n.c_str(), nullptr, 10);
    uid.clear();
    return true;
  }
  return false;
}

// query 串取参数（不考虑 urldecode：参数值只用 [A-Za-z0-9:_]）
std::string query_param(const std::string& query, const char* key) {
  const std::string prefix = std::string(key) + "=";
  std::size_t pos = 0;
  while ((pos = query.find(prefix, pos)) != std::string::npos) {
    if (pos == 0 || query[pos - 1] == '&') {
      std::size_t end = query.find('&', pos);
      if (end == std::string::npos) end = query.size();
      // percent-decode（R24-2 搜索词等中文参数走 %XX 编码；数字/ASCII
      // 参数无 % 不受影响）。只解 %XX——"+" 转空格属 form 惯例，此处不动。
      std::string raw =
          query.substr(pos + prefix.size(), end - pos - prefix.size());
      std::string out;
      out.reserve(raw.size());
      for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size() &&
            std::isxdigit(static_cast<unsigned char>(raw[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(raw[i + 2]))) {
          const auto hex = [](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
          };
          out += static_cast<char>(
              (hex(raw[i + 1]) << 4) | hex(raw[i + 2]));
          i += 2;
        } else {
          out += raw[i];
        }
      }
      return out;
    }
    ++pos;
  }
  return "";
}

// R23-5 外网面扩展名判定：白名单模式（allowlist 非空）严格比对——后缀
// 不在名单内一律拒、无后缀也拒；黑名单模式无后缀不命中（放过常规无后缀
// 文件名）。后缀取最后一段小写化比对。
bool ext_allowed(const UplinkPolicy& p, const std::string& file_name) {
  const auto dot = file_name.rfind('.');
  const bool has_ext =
      dot != std::string::npos && dot + 1 < file_name.size() &&
      file_name.find_first_of("/\\", dot) == std::string::npos;
  std::string ext;
  if (has_ext) {
    ext = file_name.substr(dot + 1);
    for (auto& c : ext) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  if (!p.ext_allowlist.empty()) {
    return has_ext && std::find(p.ext_allowlist.begin(), p.ext_allowlist.end(),
                                ext) != p.ext_allowlist.end();
  }
  if (!has_ext) return true;
  return std::find(p.ext_denylist.begin(), p.ext_denylist.end(), ext) ==
         p.ext_denylist.end();
}

// 远程协助权限名 ↔ 权限位（与 CLI assist --perms 同名：view/keyboard/
// mouse/clipboard/file；非法名=0 由 assist_mask_valid 拒）
int assist_perm_mask(const std::string& name) {
  if (name == "view") return ServerStore::kAssistView;
  if (name == "keyboard") return ServerStore::kAssistKeyboard;
  if (name == "mouse") return ServerStore::kAssistMouse;
  if (name == "clipboard") return ServerStore::kAssistClipboard;
  if (name == "file") return ServerStore::kAssistFileTransfer;
  return 0;
}

bool assist_mask_valid(int m) {
  const int all = ServerStore::kAssistView | ServerStore::kAssistKeyboard |
                  ServerStore::kAssistMouse | ServerStore::kAssistClipboard |
                  ServerStore::kAssistFileTransfer;
  return m != 0 && (m & ~all) == 0;
}

} // namespace

// 会话库实体（hpp 前置声明的 memex::server::FileSessions）：双实例部署时
// 内外两面共享一份——scope 闸才有实体：uplink 令牌打内网面被识别后 403
// 「外网会话只许写入」（而非当作未知令牌 401）、内网令牌打 uplink 面 403
// 「须外网 uplink 会话」同理；单实例缺省自建句柄，行为不变。
struct FileSessions {
  std::unordered_map<std::string, FileSession> map;
};

std::shared_ptr<FileSessions> make_file_sessions() {
  return std::make_shared<FileSessions>();
}

struct FileServer::Impl {
  ServerStore& store;
  std::shared_ptr<S3Storage> storage;
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
  // R25-4 凭据面 GCM 密钥（主密钥 SHA-256 派生 32B raw；空＝主密钥未
  // 配置＝凭据面未启用，路由 503）
  std::string tool_cred_key;
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
                UplinkPolicy policy = {})
      : store(s), storage(std::move(st)), uplink_mode(uplink),
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

// —— 单连接 HTTP 会话（骨架同 webhook.cpp：头解析/100-continue/Content-
//     Length；差别＝路由感知的 body 上限、二进制响应、查询参数）——
namespace {

class FileConn : public std::enable_shared_from_this<FileConn> {
 public:
  FileConn(tcp::socket sock, FileServer::Impl& impl)
      : sock_(std::move(sock)), impl_(impl) {}

  void start() { do_read(); }

 private:
  void do_read() {
    auto self = shared_from_this();
    sock_.async_read_some(
        asio::buffer(chunk_),
        [self](std::error_code ec, std::size_t n) {
          if (ec) return;
          self->buf_.append(self->chunk_.data(), n);
          self->on_data();
        });
  }

  void on_data() {
    const std::size_t head_end = buf_.find("\r\n\r\n");
    if (head_end == std::string::npos) {
      if (buf_.size() > kMaxHead) {
        respond_json(400, {{"ok", false}, {"error", "请求头过长"}});
        return;
      }
      do_read();
      return;
    }
    if (!head_parsed_) {
      if (!parse_head(head_end)) return;
      head_parsed_ = true;
      if (content_length_ > body_cap_) {
        respond_json(413, {{"ok", false}, {"error", "body 超过上限"}});
        return;
      }
      if (expect_continue_) {
        write_raw(std::make_shared<const std::string>(
                      "HTTP/1.1 100 Continue\r\n\r\n"),
                  false, [this](std::error_code ec) {
                    if (!ec) do_read();
                  });
        return;
      }
    }
    const std::size_t body_start = head_end + 4;
    if (buf_.size() < body_start + content_length_) {
      do_read();
      return;
    }
    process(buf_.substr(body_start, content_length_));
  }

  bool parse_head(std::size_t head_end) {
    const std::string head = buf_.substr(0, head_end);
    std::istringstream in(head);
    std::string method, uri, version;
    if (!(in >> method >> uri >> version)) {
      respond_json(400, {{"ok", false}, {"error", "请求行无法解析"}});
      return false;
    }
    method_ = method;
    // path 与 query 拆分
    const std::size_t qpos = uri.find('?');
    path_ = qpos == std::string::npos ? uri : uri.substr(0, qpos);
    query_ = qpos == std::string::npos ? "" : uri.substr(qpos + 1);
    std::string line;
    std::getline(in, line); // 请求行余部
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string key = line.substr(0, colon);
      for (auto& c : key) c = static_cast<char>(std::tolower(c));
      std::string val = line.substr(colon + 1);
      const auto vb = val.find_first_not_of(" \t");
      val = vb == std::string::npos ? std::string{} : val.substr(vb);
      if (key == "content-length") {
        seen_content_length_ = true;
        if (val.empty() ||
            val.find_first_not_of("0123456789") != std::string::npos) {
          content_length_ = static_cast<std::size_t>(-1);
        } else {
          content_length_ = static_cast<std::size_t>(
              std::strtoull(val.c_str(), nullptr, 10));
        }
      } else if (key == "expect" &&
                 val.find("100-continue") != std::string::npos) {
        expect_continue_ = true;
      } else if (key == "authorization") {
        authorization_ = val;
      } else if (key == "x-file-name") {
        file_name_ = val;
      }
    }
    // 无请求体方法缺 Content-Length 视为 0（curl/浏览器/Qt QNAM 的
    // GET/HEAD 都不发该头，R23-3 走查实录）；带体方法仍强制。
    if (!seen_content_length_ && (method_ == "GET" || method_ == "DELETE" ||
                                  method_ == "HEAD")) {
      content_length_ = 0;
    } else if (!seen_content_length_ ||
               content_length_ == static_cast<std::size_t>(-1)) {
      respond_json(400, {{"ok", false}, {"error", "Content-Length 缺失或非法"}});
      return false;
    }
    // 路由感知上限：upload 收大字节，其余路由只收小 JSON/空 body；
    // uplink 面可配独立上限（R23-5，0=沿用全局）
    if (path_ == "/files/upload") {
      body_cap_ = kMaxUpload;
    } else if (path_ == "/uplink/upload") {
      body_cap_ = impl_.uplink_policy.max_upload_bytes > 0
                      ? static_cast<std::size_t>(
                            impl_.uplink_policy.max_upload_bytes)
                      : kMaxUpload;
    } else {
      body_cap_ = kMaxJsonBody;
    }
    return true;
  }

  std::string account_or_respond() {
    const std::string account = impl_.auth_session(authorization_);
    if (account.empty()) {
      respond_json(401, {{"ok", false},
                         {"error", "会话无效或过期（先 POST /files/session）"}});
    }
    return account;
  }

  void process(const std::string& body) {
    // —— 路由 ——
    // 健康探针（无鉴权）：容器 HEALTHCHECK／运维探活用；只回固定 ok，
    // 不泄账号/配置信息（uplink 面同款 /uplink/health，探活口径一致）
    if ((path_ == "/files/health" || path_ == "/uplink/health") &&
        method_ == "GET") {
      return respond_json(200, {{"ok", true}});
    }
    // R23-4 外网单向面：只挂写入端点，无任何下载/读取内网数据的路由
    //（404 兜底）。物理面分离＝独立监听口；scope 闸见各 route。
    if (impl_.uplink_mode) {
      if (path_ == "/uplink/session") {
        return route_session(body, /*uplink=*/true);
      }
      if (path_ == "/uplink/upload" && method_ == "POST") {
        return route_uplink_upload(body);
      }
      if (path_ == "/uplink/mine" && method_ == "GET") {
        return route_uplink_mine();
      }
      if (path_ == "/uplink/delete" && method_ == "POST") {
        return route_uplink_delete();
      }
      respond_json(404, {{"ok", false}, {"error", "路径不存在"}});
      return;
    }
    // 单向性第二道闸：uplink 令牌打到内网端口，读端点一律 403
    //（session/health 无需会话，豁免）
    if (path_.rfind("/files/", 0) == 0 && path_ != "/files/session" &&
        impl_.is_uplink_token(authorization_)) {
      respond_json(403, {{"ok", false},
                         {"error", "外网会话只许写入（单向 uplink）"}});
      return;
    }
    if (path_ == "/files/session") {
      return route_session(body);
    }
    if (path_ == "/files/logout" && method_ == "POST") {
      return route_logout();
    }
    if (path_ == "/files/upload" && method_ == "POST") {
      return route_upload(body);
    }
    if (path_ == "/files/download" && method_ == "GET") {
      return route_download();
    }
    if (path_ == "/files/list" && method_ == "GET") {
      return route_list();
    }
    if (path_ == "/files/quota" && method_ == "GET") {
      return route_quota_get();
    }
    if (path_ == "/files/manage/delete" && method_ == "POST") {
      return route_delete();
    }
    if (path_ == "/files/manage/pin" && method_ == "POST") {
      return route_pin();
    }
    if (path_ == "/files/manage/quota" && method_ == "POST") {
      return route_quota_set(body);
    }
    // R23-3 文件助手备忘录：POST 建/改、GET 单条/列表、DELETE 删
    if (path_ == "/files/memo" && method_ == "POST") {
      return route_memo_write(body);
    }
    if (path_ == "/files/memo" && method_ == "GET") {
      return route_memo();
    }
    if (path_ == "/files/memo" && method_ == "DELETE") {
      return route_memo_delete();
    }
    // R27-1 个人任务清单：建（分配=「谁能分配」判权）/我的清单＋我派出/
    // 完成（清单主人）/提醒回执（清单主人）/撤回（主人或分配人）
    if (path_ == "/files/tasks" && method_ == "POST") {
      return route_task_create(body);
    }
    if (path_ == "/files/tasks" && method_ == "GET") {
      return route_tasks_list();
    }
    if (path_ == "/files/tasks/done" && method_ == "POST") {
      return route_task_done(body);
    }
    if (path_ == "/files/tasks/reminded" && method_ == "POST") {
      return route_task_reminded(body);
    }
    if (path_ == "/files/tasks/delete" && method_ == "POST") {
      return route_task_delete(body);
    }
    // 二期·审批（请假起步）：申请/双列表/决定/撤回；判权 az
    // approval-decide（直属上级现查或无上级 org-admin 兜底）
    if (path_ == "/files/approvals" && method_ == "POST") {
      return route_approval_create(body);
    }
    if (path_ == "/files/approvals" && method_ == "GET") {
      return route_approvals_list();
    }
    if (path_ == "/files/approvals/decide" && method_ == "POST") {
      return route_approval_decide(body);
    }
    if (path_ == "/files/approvals/withdraw" && method_ == "POST") {
      return route_approval_withdraw(body);
    }
    // 日报周报（二期）：写自己的（当日 upsert）/看自己/直属上级看下属
    if (path_ == "/files/reports" && method_ == "POST") {
      return route_report_save(body);
    }
    if (path_ == "/files/reports" && method_ == "GET") {
      return route_reports_mine();
    }
    if (path_ == "/files/reports/read" && method_ == "GET") {
      return route_reports_read();
    }
    if (path_ == "/files/reports/team" && method_ == "GET") {
      return route_reports_team();
    }
    // 会话审计（二期）：在线检索＋查阅日志（持 auditor 有效角色）
    if (path_ == "/files/audit/search" && method_ == "POST") {
      return route_audit_search(body);
    }
    if (path_ == "/files/audit/reads" && method_ == "GET") {
      return route_audit_reads();
    }
    // 办公室位置图（二期）：看自楼层（org-admin 可 ?floor= 指定层）/
    // 编辑工位/绑定占用者
    if (path_ == "/files/office-map" && method_ == "GET") {
      return route_office_map();
    }
    if (path_ == "/files/office-map/seat" && method_ == "POST") {
      return route_office_seat(body);
    }
    if (path_ == "/files/office-map/bind" && method_ == "POST") {
      return route_office_bind(body);
    }
    // 远程协助（二期）：生命周期＋台账＋媒体中继（模型层平台-11 在 store）
    if (path_ == "/files/assist/request" && method_ == "POST") {
      return route_assist_request(body);
    }
    if (path_ == "/files/assist/respond" && method_ == "POST") {
      return route_assist_respond(body);
    }
    if (path_ == "/files/assist/start" && method_ == "POST") {
      return route_assist_lifecycle(body, /*start=*/true);
    }
    if (path_ == "/files/assist/end" && method_ == "POST") {
      return route_assist_lifecycle(body, /*start=*/false);
    }
    if (path_ == "/files/assist/sessions" && method_ == "GET") {
      return route_assist_sessions();
    }
    if (path_ == "/files/assist/audit" && method_ == "POST") {
      return route_assist_audit(body);
    }
    if (path_ == "/files/assist/frame" && method_ == "POST") {
      return route_assist_frame(body, /*push=*/true);
    }
    if (path_ == "/files/assist/frame" && method_ == "GET") {
      return route_assist_frame(body, /*push=*/false);
    }
    if (path_ == "/files/assist/input" && method_ == "POST") {
      return route_assist_input(body, /*send=*/true);
    }
    if (path_ == "/files/assist/input" && method_ == "GET") {
      return route_assist_input(body, /*send=*/false);
    }
    // R24-2 群备忘录：群维度共享知识（管理员维护；开放编辑后成员可写，
    // 全部编辑逐笔留痕可回滚）。判权走 AuthorizationService 群规则。
    if (path_ == "/files/group-memo/list" && method_ == "GET") {
      return route_group_memo_list();
    }
    if (path_ == "/files/group-memo/save" && method_ == "POST") {
      return route_group_memo_save(body);
    }
    if (path_ == "/files/group-memo/delete" && method_ == "POST") {
      return route_group_memo_delete(body);
    }
    if (path_ == "/files/group-memo/history" && method_ == "GET") {
      return route_group_memo_history();
    }
    if (path_ == "/files/group-memo/rollback" && method_ == "POST") {
      return route_group_memo_rollback(body);
    }
    if (path_ == "/files/group-memo/open-edit" && method_ == "POST") {
      return route_group_memo_open_edit(body);
    }
    // R24-3 群密码箱：全程密文（服务端只存 b64 密文/包裹块）；解锁面在
    // 客户端本地派生 KEK；授权名单=route 叠加判定（空=全成员，群主可收窄）。
    if (path_ == "/files/group-vault/info" && method_ == "GET") {
      return route_group_vault_info();
    }
    if (path_ == "/files/group-vault/init" && method_ == "POST") {
      return route_group_vault_init(body);
    }
    if (path_ == "/files/group-vault/rekey" && method_ == "POST") {
      return route_group_vault_rekey(body);
    }
    if (path_ == "/files/group-vault/list" && method_ == "GET") {
      return route_group_vault_list();
    }
    if (path_ == "/files/group-vault/access" && method_ == "POST") {
      return route_group_vault_access(body);
    }
    if (path_ == "/files/group-vault/save" && method_ == "POST") {
      return route_group_vault_save(body);
    }
    if (path_ == "/files/group-vault/delete" && method_ == "POST") {
      return route_group_vault_delete(body);
    }
    if (path_ == "/files/group-vault/acl" && method_ == "POST") {
      return route_group_vault_acl(body);
    }
    if (path_ == "/files/group-vault/audit" && method_ == "GET") {
      return route_group_vault_audit();
    }
    if (path_ == "/files/group-tools/config" && method_ == "POST") {
      return route_group_tools_config(body);
    }
    if (path_ == "/files/group-tools/list" && method_ == "GET") {
      return route_group_tools_list();
    }
    if (path_ == "/files/group-tools/call" && method_ == "POST") {
      return route_group_tools_call(body);
    }
    if (path_ == "/files/group-tools/audit" && method_ == "GET") {
      return route_group_tools_audit();
    }
    if (path_ == "/files/group-ci/pipeline" && method_ == "POST") {
      return route_group_ci_pipeline(body);
    }
    if (path_ == "/files/group-ci/list" && method_ == "GET") {
      return route_group_ci_list();
    }
    if (path_ == "/files/group-ci/trigger" && method_ == "POST") {
      return route_group_ci_trigger(body);
    }
    if (path_ == "/files/group-ci/runs" && method_ == "GET") {
      return route_group_ci_runs();
    }
    if (path_ == "/files/group-pack/build" && method_ == "POST") {
      return route_group_pack_build(body);
    }
    if (path_ == "/files/group-pack/list" && method_ == "GET") {
      return route_group_pack_list();
    }
    if (path_ == "/files/group-pack/delete" && method_ == "POST") {
      return route_group_pack_delete(body);
    }
    if (path_ == "/files/group-export" && method_ == "GET") {
      return route_group_export();
    }
    // R25-4 凭据面：写/删凭据（管理面 memo:config；回包绝不回显 value）
    // 与掩码元数据列表（客户端零凭据——明文只在服务端代理内存内）
    if (path_ == "/files/group-tools/credential" && method_ == "POST") {
      return route_group_tools_credential(body);
    }
    if (path_ == "/files/group-tools/credentials" && method_ == "GET") {
      return route_group_tools_credentials();
    }
    // R26-1 服务器 agent 面：登记（管理面）/心跳（agent 注册令牌，非
    // 人会话）/列表（群成员——入群即授权）
    if (path_ == "/files/group-servers/enroll" && method_ == "POST") {
      return route_group_servers_enroll(body);
    }
    if (path_ == "/files/group-servers/heartbeat" && method_ == "POST") {
      return route_group_servers_heartbeat(body);
    }
    if (path_ == "/files/group-servers/list" && method_ == "GET") {
      return route_group_servers_list();
    }
    // R26-3 远程会话（SSH 起步）：签一次性短票＋接入留痕＋短票兑现＋
    // 收尾（谁/何时/连哪台/时长全程可回溯）
    if (path_ == "/files/group-servers/session/request" &&
        method_ == "POST") {
      return route_group_servers_session_request(body);
    }
    if (path_ == "/files/group-servers/session/redeem" &&
        method_ == "POST") {
      return route_group_servers_session_redeem(body);
    }
    if (path_ == "/files/group-servers/session/close" &&
        method_ == "POST") {
      return route_group_servers_session_close(body);
    }
    if (path_ == "/files/group-servers/sessions" && method_ == "GET") {
      return route_group_servers_sessions();
    }
    // R26-4 服务器凭据：目标机凭据只存服务端（加密落库、回包只见掩码
    // 元数据——客户端零凭据，真用凭据的操作归 croupier）
    if (path_ == "/files/group-servers/credential" && method_ == "POST") {
      return route_group_servers_credential(body);
    }
    // 二期群工具三件（原生互动，不走 R25 外部工具代理）：判权全循
    // file:read 群继承（tool_call_allowed 同构），身份约束服务端逻辑判
    if (path_ == "/files/group-polls" && method_ == "POST") {
      return route_group_poll_create(body);
    }
    if (path_ == "/files/group-polls" && method_ == "GET") {
      return route_group_polls_list();
    }
    if (path_ == "/files/group-polls/vote" && method_ == "POST") {
      return route_group_poll_vote(body);
    }
    if (path_ == "/files/group-polls/close" && method_ == "POST") {
      return route_group_poll_close(body);
    }
    if (path_ == "/files/group-chains" && method_ == "POST") {
      return route_group_chain_create(body);
    }
    if (path_ == "/files/group-chains" && method_ == "GET") {
      return route_group_chains_list();
    }
    if (path_ == "/files/group-chains/join" && method_ == "POST") {
      return route_group_chain_join(body);
    }
    if (path_ == "/files/group-chains/close" && method_ == "POST") {
      return route_group_chain_close(body);
    }
    if (path_ == "/files/group-tasks" && method_ == "POST") {
      return route_group_task_create(body);
    }
    if (path_ == "/files/group-tasks" && method_ == "GET") {
      return route_group_tasks_list();
    }
    if (path_ == "/files/group-tasks/claim" && method_ == "POST") {
      return route_group_task_claim(body);
    }
    if (path_ == "/files/group-tasks/done" && method_ == "POST") {
      return route_group_task_done(body);
    }
    respond_json(404, {{"ok", false}, {"error", "路径不存在"}});
  }

  // 换会话令牌：内网面 /files/session（scope=internal）与外网面
  // /uplink/session（scope=uplink，只许写入）同源账号库同 PBKDF2。
  void route_session(const std::string& body, bool uplink = false) {
    if (method_ != "POST") {
      respond_json(405, {{"ok", false}, {"error", "方法不支持：请用 POST"}});
      return;
    }
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("account") || !j["account"].is_string() ||
        !j.contains("password") || !j["password"].is_string()) {
      respond_json(400, {{"ok", false},
                         {"error", "缺少字段：account/password（字符串）"}});
      return;
    }
    const std::string account = j["account"].get<std::string>();
    const std::string password = j["password"].get<std::string>();
    // R23-5 二次验证（仅外网面、部署显式配置）：secondary 常量时间比对
    //（双方过 sha256 再逐字节异或累计，长度信息不经比较时序外泄）
    if (uplink && !impl_.uplink_policy.login_secret.empty()) {
      const std::string provided =
          j.contains("secondary") && j["secondary"].is_string()
              ? j["secondary"].get<std::string>()
              : "";
      const std::string a = sha256_hex(impl_.uplink_policy.login_secret);
      const std::string b = sha256_hex(provided);
      unsigned char diff = 0;
      for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
      if (diff != 0) {
        respond_json(401, {{"ok", false}, {"error", "二级口令缺失或不匹配"}});
        return;
      }
    }
    // 与消息面同源口令校验（credentials 表判登，平台-2；token 只存哈希）
    const auto cred = impl_.store.find_credential(account, "password");
    std::string token;
    if (cred.has_value()) {
      // 摘要比对用常量时间思路不引入（内网面 v1 与消息面同口径）
      token = pbkdf2_sha256_hex(password, cred->salt_hex, 60000) ==
                      cred->digest_hex
                  ? impl_.mint_session(account, uplink)
                  : "";
    }
    if (token.empty()) {
      respond_json(401, {{"ok", false}, {"error", "账号或口令错误"}});
      return;
    }
    respond_json(200,
                 {{"ok", true},
                  {"token", token},
                  {"expires_in", kSessionTtlMs / 1000},
                  {"scope", uplink ? "uplink" : "internal"}});
  }

  // 登出（平台-2 Session 模型收尾）：台账落 logout_reason＋内存即刻
  // 失效——令牌登出后立即不可再用（不等 12h TTL）。
  void route_logout() {
    static constexpr const char* kPrefix = "Bearer ";
    const auto s = impl_.auth_session_full(authorization_);
    if (!s.has_value() ||
        authorization_.rfind(kPrefix, 0) != 0) {
      respond_json(401, {{"ok", false},
                         {"error", "会话无效或过期（先 POST /files/session）"}});
      return;
    }
    const std::string hash =
        sha256_hex(authorization_.substr(std::strlen(kPrefix)));
    impl_.store.session_close(hash, "user_logout", now_ms());
    impl_.sessions->map.erase(hash);
    std::cout << "[MEMEX] files logout account=" << s->account << std::endl;
    respond_json(200, {{"ok", true}});
  }

  // —— R23-4 外网单向 uplink：铁律=外网会话只有「写入」权限，永远没有
  //     「读取内网数据」的权限（唯一落点=文件助手收件箱；内网用户转发进
  //     群走内网面，人工过一道目）——

  // uplink 面鉴权：无效令牌 401；有效但非 uplink scope 403（内网令牌
  // 打到 uplink 端口同样拒）
  std::string uplink_account_or_respond() {
    const auto s = impl_.auth_session_full(authorization_);
    if (!s.has_value()) {
      respond_json(401, {{"ok", false},
                         {"error", "会话无效或过期（先 POST /uplink/session）"}});
      return "";
    }
    if (!s->uplink) {
      respond_json(403, {{"ok", false},
                         {"error", "须外网 uplink 会话（内网令牌不通用）"}});
      return "";
    }
    return s->account;
  }

  // 上传：唯一落点=文件助手收件箱（target 参数不收——外网面无选择权），
  // 全量审计（uplink_logs 流水＋控制台留痕：谁/何时/什么/落点）
  void route_uplink_upload(const std::string& body) {
    const std::string account = uplink_account_or_respond();
    if (account.empty()) return;
    // R23-5 类型白名单/黑名单（外网入口不受信，先于判权做廉价检查）
    if (!ext_allowed(impl_.uplink_policy, file_name_)) {
      respond_json(422, {{"ok", false},
                         {"error", "该文件类型在外网入口不允许上传"}});
      return;
    }
    const Decision d = impl_.az.authorize(
        {account, "file:upload", "user:" + account, "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error", "无权上传（" + d.reason + "）"}});
      return;
    }
    const auto r =
        impl_.handle_upload(account, /*is_group=*/false, /*gid=*/0, account,
                            file_name_, body, /*is_inbox=*/true,
                            ServerStore::FileSource::Uplink,
                            impl_.uplink_policy.scanner.get());
    if (r.http_status == 422) {
      // 扫描拒收也是审计事件（外网上传全留痕：谁/何时/什么/为什么拒）
      std::cout << "[MEMEX] uplink scan-reject account=" << account
                << " name=" << file_name_ << " size=" << body.size()
                << std::endl;
    }
    if (r.http_status != 200) {
      respond_json(r.http_status, {{"ok", false}, {"error", r.error}});
      return;
    }
    impl_.store.add_uplink_log(
        {0, account, file_name_, static_cast<std::int64_t>(body.size()),
         r.file_hash, r.object_key, now_ms()});
    std::cout << "[MEMEX] uplink upload account=" << account
              << " id=" << r.file_id << " size=" << body.size()
              << " key=" << r.object_key
              << (r.second_transfer ? " second-transfer" : "") << std::endl;
    respond_json(200, {{"ok", true},
                       {"id", r.file_id},
                       {"second_transfer", r.second_transfer}});
  }

  // 我的上传记录（只有记录：无任何取回内网数据的端点）
  void route_uplink_mine() {
    const std::string account = uplink_account_or_respond();
    if (account.empty()) return;
    int limit = 200, offset = 0;
    const std::string lim = query_param(query_, "limit");
    const std::string off = query_param(query_, "offset");
    if (!lim.empty() &&
        lim.find_first_not_of("0123456789") == std::string::npos) {
      limit = std::atoi(lim.c_str());
    }
    if (!off.empty() &&
        off.find_first_not_of("0123456789") == std::string::npos) {
      offset = std::atoi(off.c_str());
    }
    const auto rows = impl_.store.list_uplink_files(account, limit, offset);
    json arr = json::array();
    for (const auto& m : rows) {
      arr.push_back({{"id", m.id},
                     {"file_name", m.file_name},
                     {"file_size", m.file_size},
                     {"file_hash", m.file_hash},
                     {"upload_ts", m.upload_ts}});
    }
    respond_json(200, {{"ok", true}, {"files", arr}});
  }

  // 删自己的上传：只许 source=uplink 且 owner=自己（内网文件/他人记录
  // 一律 403，不借道 handle_delete 的判权放宽）
  void route_uplink_delete() {
    const std::string account = uplink_account_or_respond();
    if (account.empty()) return;
    const std::string id_s = query_param(query_, "id");
    if (id_s.empty() ||
        id_s.find_first_not_of("0123456789") != std::string::npos) {
      respond_json(400, {{"ok", false}, {"error", "id 须为数字"}});
      return;
    }
    const std::int64_t id = std::strtoll(id_s.c_str(), nullptr, 10);
    const auto meta = impl_.store.file_by_id(id);
    if (!meta.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "文件不存在"}});
      return;
    }
    if (meta->source != ServerStore::FileSource::Uplink ||
        meta->owner != account) {
      respond_json(
          403, {{"ok", false}, {"error", "外网会话只能删除自己的上传"}});
      return;
    }
    const auto r = impl_.handle_delete(account, id);
    if (r.http_status != 200) {
      respond_json(r.http_status, {{"ok", false}, {"error", r.error}});
      return;
    }
    std::cout << "[MEMEX] uplink delete account=" << account << " id=" << id
              << std::endl;
    respond_json(200, {{"ok", true}});
  }

  // —— R27-1 个人任务清单 ——

  void route_task_create(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("title") ||
        !j["title"].is_string() ||
        j["title"].get<std::string>().empty() ||
        (j.contains("note") && !j["note"].is_string()) ||
        (j.contains("due_ms") && !j["due_ms"].is_number_integer()) ||
        (j.contains("assignee") && !j["assignee"].is_string()) ||
        (j.contains("provider") && !j["provider"].is_string()) ||
        (j.contains("ext_key") && !j["ext_key"].is_string())) {
      respond_json(
          400,
          {{"ok", false},
           {"error",
            "缺少字段：title（非空）；note/due_ms/assignee/provider/"
            "ext_key 可选"}});
      return;
    }
    // R27-2 外部任务登记：provider/ext_key 成对，空=本地任务。外部条目
    // 是个人登记（详情 URL 由客户端 provider SPI 解析，服务端只存引用），
    // 不转派——分配语义（同群/同部门互派）只对本地任务定义。
    std::string provider, ext_key;
    if (j.contains("provider")) provider = j["provider"].get<std::string>();
    if (j.contains("ext_key")) ext_key = j["ext_key"].get<std::string>();
    if (provider.empty() != ext_key.empty()) {
      respond_json(400,
                   {{"ok", false},
                    {"error",
                     "外部任务须 provider 与 ext_key 成对提供"}});
      return;
    }
    std::string assignee = account;
    if (j.contains("assignee") && !j["assignee"].get<std::string>().empty()) {
      assignee = j["assignee"].get<std::string>();
    }
    if (!provider.empty() && assignee != account) {
      respond_json(400, {{"ok", false},
                         {"error", "外部任务不转派（个人登记）"}});
      return;
    }
    if (assignee != account) {
      // 分配=特权动作：同群/同部门才可互派（task-assign 规则现查现裁）
      const Decision d = impl_.az.authorize(
          {account, "task:assign", "user:" + assignee, "owner=" + account});
      if (!d.allowed) {
        respond_json(403,
                     {{"ok", false},
                      {"error", "无权分配给 " + assignee + "（" + d.reason +
                           "；同群或同部门才能互派任务）"}});
        return;
      }
    }
    const std::int64_t due_ms =
        j.contains("due_ms") ? j["due_ms"].get<std::int64_t>() : 0;
    const std::string note =
        j.contains("note") ? j["note"].get<std::string>() : "";
    const std::int64_t id = impl_.store.task_create(
        assignee, account, j["title"].get<std::string>(), note, due_ms,
        now_ms(), provider, ext_key);
    if (id <= 0) {
      respond_json(404, {{"ok", false}, {"error", "任务接收人不存在"}});
      return;
    }
    if (assignee != account) {
      std::cout << "[MEMEX] files task assign account=" << account
                << " to=" << assignee << " id=" << id << std::endl;
    }
    respond_json(200, {{"ok", true}, {"id", id}});
  }

  void route_tasks_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json mine = json::array();
    for (const auto& t : impl_.store.tasks_of(account)) {
      mine.push_back({{"id", t.id},
                      {"creator", t.creator},
                      {"title", t.title},
                      {"note", t.note},
                      {"due_ms", t.due_ms},
                      {"reminded_ms", t.reminded_ms},
                      {"done", t.done},
                      {"done_ms", t.done_ms},
                      {"created_ms", t.created_ms},
                      {"provider", t.provider},
                      {"ext_key", t.ext_key}});
    }
    json assigned = json::array();
    for (const auto& t : impl_.store.tasks_assigned_by(account)) {
      assigned.push_back({{"id", t.id},
                          {"owner", t.owner},
                          {"title", t.title},
                          {"note", t.note},
                          {"due_ms", t.due_ms},
                          {"done", t.done},
                          {"done_ms", t.done_ms},
                          {"created_ms", t.created_ms},
                          {"provider", t.provider},
                          {"ext_key", t.ext_key}});
    }
    respond_json(200, {{"ok", true}, {"tasks", mine},
                       {"assigned_by_me", assigned}});
  }

  void route_task_done(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer() ||
        !j.contains("done") || !j["done"].is_boolean()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：id/done"}});
      return;
    }
    const std::int64_t id = j["id"].get<std::int64_t>();
    const auto row = impl_.store.task_by_id(id);
    if (!row.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "任务不存在"}});
      return;
    }
    if (row->owner != account) {
      respond_json(403, {{"ok", false},
                         {"error", "只有清单主人能勾完成/回退"}});
      return;
    }
    if (!impl_.store.task_set_done(id, j["done"].get<bool>(), now_ms())) {
      respond_json(500, {{"ok", false}, {"error", "任务状态更新失败"}});
      return;
    }
    respond_json(200, {{"ok", true}});
  }

  void route_task_reminded(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
      return;
    }
    const std::int64_t id = j["id"].get<std::int64_t>();
    const auto row = impl_.store.task_by_id(id);
    if (!row.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "任务不存在"}});
      return;
    }
    if (row->owner != account) {
      respond_json(403, {{"ok", false},
                         {"error", "提醒回执归清单主人"}});
      return;
    }
    impl_.store.task_mark_reminded(id, now_ms());
    respond_json(200, {{"ok", true}});
  }

  void route_task_delete(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
      return;
    }
    const std::int64_t id = j["id"].get<std::int64_t>();
    const auto row = impl_.store.task_by_id(id);
    if (!row.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "任务不存在"}});
      return;
    }
    // 撤回（删）：清单主人或分配人皆可（自建=同一人自然可删）
    if (row->owner != account && row->creator != account) {
      respond_json(403, {{"ok", false},
                         {"error", "只有清单主人或分配人能撤回任务"}});
      return;
    }
    if (!impl_.store.task_delete(id)) {
      respond_json(404, {{"ok", false}, {"error", "任务不存在"}});
      return;
    }
    respond_json(200, {{"ok", true}});
  }

  // —— 二期·审批（请假起步，设计稿 docs/design/审批与日报周报.md §一）——
  // 四态 pending|approved|rejected|withdrawn；审批人判定=az 规则
  // approval-decide（直属上级现查；无上级=org-admin 兜底）；自建自审
  // 不成立；全部动作留痕不删改
  void route_approval_create(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    // 一期固定模板「请假」：类型白名单（年假/事假/病假/调休）
    static const char* kTypes[] = {"年假", "事假", "病假", "调休"};
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
      respond_json(400, {{"ok", false},
                         {"error",
                          "缺少字段：type（年假/事假/病假/调休）；"
                          "from/to/reason 可选"}});
      return;
    }
    const std::string type = j["type"].get<std::string>();
    bool type_ok = false;
    for (const char* t : kTypes) {
      if (type == t) type_ok = true;
    }
    if (!type_ok) {
      respond_json(400, {{"ok", false},
                         {"error", "type 须为 年假/事假/病假/调休"}});
      return;
    }
    const std::string from = j.contains("from") && j["from"].is_string()
                                 ? j["from"].get<std::string>()
                                 : "";
    const std::string to = j.contains("to") && j["to"].is_string()
                               ? j["to"].get<std::string>()
                               : "";
    const std::string reason = j.contains("reason") && j["reason"].is_string()
                                   ? j["reason"].get<std::string>()
                                   : "";
    const std::int64_t id = impl_.store.approval_create(
        account, type, from, to, reason, now_ms());
    if (id <= 0) {
      respond_json(500, {{"ok", false}, {"error", "申请创建失败"}});
      return;
    }
    respond_json(200, {{"ok", true}, {"id", id}});
  }

  void route_approvals_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json mine = json::array();
    for (const auto& a : impl_.store.approvals_of(account)) {
      mine.push_back({{"id", a.id},
                      {"type", a.type},
                      {"from", a.leave_from},
                      {"to", a.leave_to},
                      {"reason", a.reason},
                      {"status", a.status},
                      {"decider", a.decider},
                      {"decision_note", a.decision_note},
                      {"created_ms", a.created_ms},
                      {"decided_ms", a.decided_ms}});
    }
    // 待我决：全量 pending 逐行走 az approval:decide 现裁（不向无权者
    // 泄露他人申请的存在；判权不过的行直接不出现）
    json pending = json::array();
    for (const auto& a : impl_.store.approvals_pending()) {
      const Decision d = impl_.az.authorize(
          {account, "approval:decide", "user:" + a.applicant,
           "owner=" + a.applicant});
      if (!d.allowed) continue;
      pending.push_back({{"id", a.id},
                         {"applicant", a.applicant},
                         {"type", a.type},
                         {"from", a.leave_from},
                         {"to", a.leave_to},
                         {"reason", a.reason},
                         {"created_ms", a.created_ms}});
    }
    respond_json(200, {{"ok", true}, {"mine", mine}, {"pending", pending}});
  }

  void route_approval_decide(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer() ||
        !j.contains("approved") || !j["approved"].is_boolean() ||
        (j.contains("note") && !j["note"].is_string())) {
      respond_json(400,
                   {{"ok", false}, {"error", "缺少字段：id/approved"}});
      return;
    }
    const std::int64_t id = j["id"].get<std::int64_t>();
    const auto row = impl_.store.approval_by_id(id);
    if (!row.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "申请不存在"}});
      return;
    }
    // 判权先于状态检查（对无权者与对不存在者同口径 403，不泄露状态）
    const Decision d = impl_.az.authorize(
        {account, "approval:decide", "user:" + row->applicant,
         "owner=" + row->applicant});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error", "无权决定该申请（" + d.reason +
                                   "；直属上级或无上级时 org-admin）"}});
      return;
    }
    if (row->status != "pending") {
      respond_json(409, {{"ok", false},
                         {"error", "该申请已决（" + row->status +
                                   "），不可重复决定"}});
      return;
    }
    const std::string note =
        j.contains("note") && j["note"].is_string()
            ? j["note"].get<std::string>()
            : "";
    if (!impl_.store.approval_decide(id, account, j["approved"].get<bool>(),
                                     note, now_ms())) {
      respond_json(409, {{"ok", false}, {"error", "决定落库失败（状态已变）"}});
      return;
    }
    std::cout << "[MEMEX] approval decide account=" << account
              << " id=" << id
              << " result=" << (j["approved"].get<bool>() ? "approved"
                                                          : "rejected")
              << std::endl;
    respond_json(200, {{"ok", true}});
  }

  void route_approval_withdraw(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
      return;
    }
    const std::int64_t id = j["id"].get<std::int64_t>();
    const auto row = impl_.store.approval_by_id(id);
    if (!row.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "申请不存在"}});
      return;
    }
    if (row->applicant != account) {
      respond_json(403, {{"ok", false}, {"error", "只有申请人能撤回"}});
      return;
    }
    if (row->status != "pending") {
      respond_json(409, {{"ok", false},
                         {"error", "该申请已决（" + row->status + "）"}});
      return;
    }
    if (!impl_.store.approval_withdraw(id, account)) {
      respond_json(409, {{"ok", false}, {"error", "撤回落库失败（状态已变）"}});
      return;
    }
    respond_json(200, {{"ok", true}});
  }

  // —— 日报周报（二期，设计稿 §二）：个人日报台账，当日重复=upsert
  //    更新不留修订史；周报=按周聚合视图不单设表；直属上级可看下属
  //   （az report:read，org-admin 不兜底）——
  static json report_row_json(const ServerStore::ReportRow& r) {
    return {{"id", r.id},
            {"author", r.author},
            {"date", r.report_date},
            {"content", r.content},
            {"created_ms", r.created_ms},
            {"updated_ms", r.updated_ms}};
  }

  void route_report_save(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("content") ||
        !j["content"].is_string() || j["content"].get<std::string>().empty()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：content"}});
      return;
    }
    const std::string date = j.contains("date") && j["date"].is_string()
                                 ? j["date"].get<std::string>()
                                 : "";
    if (!is_date_ymd(date)) {
      respond_json(400, {{"ok", false},
                         {"error", "date 须为 YYYY-MM-DD（日报归属日）"}});
      return;
    }
    const Decision d = impl_.az.authorize(
        {account, "report:write", "user:" + account, "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false}, {"error", "无权写入"}});
      return;
    }
    const std::int64_t id = impl_.store.report_upsert(
        account, date, j["content"].get<std::string>(), now_ms());
    if (id <= 0) {
      respond_json(500, {{"ok", false}, {"error", "日报落库失败"}});
      return;
    }
    std::cout << "[MEMEX] report save account=" << account
              << " date=" << date << std::endl;
    respond_json(200, {{"ok", true}, {"id", id}});
  }

  void route_reports_mine() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json arr = json::array();
    for (const auto& r : impl_.store.reports_of(account)) {
      arr.push_back(report_row_json(r));
    }
    respond_json(200, {{"ok", true}, {"reports", std::move(arr)}});
  }

  void route_reports_read() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string author = query_param(query_, "author");
    if (author.empty()) {
      respond_json(400, {{"ok", false}, {"error", "缺少参数：author"}});
      return;
    }
    // 判权不过=403（幽灵账号同口径：链空且非本人——不泄露存在性）
    const Decision d = impl_.az.authorize(
        {account, "report:read", "user:" + author, "owner=" + author});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error", "无权查看该日报（直属上级可看下属）"}});
      return;
    }
    json arr = json::array();
    for (const auto& r : impl_.store.reports_of(author)) {
      arr.push_back(report_row_json(r));
    }
    respond_json(200, {{"ok", true},
                       {"author", author},
                       {"reports", std::move(arr)}});
  }

  void route_reports_team() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    // 直接下属逐人聚合（org_reporting_lines 反查）；逐行再过 az 与单读
    // 同源（平台-3 现查现裁=转岗即时生效）
    json team = json::array();
    for (const auto& sub : impl_.store.direct_reports(account)) {
      const Decision d = impl_.az.authorize(
          {account, "report:read", "user:" + sub, "owner=" + sub});
      if (!d.allowed) continue;
      json rows = json::array();
      for (const auto& r : impl_.store.reports_of(sub)) {
        rows.push_back(report_row_json(r));
      }
      team.push_back({{"author", sub}, {"reports", std::move(rows)}});
    }
    respond_json(200, {{"ok", true}, {"team", std::move(team)}});
  }

  // —— 会话审计（二期）：CLI T3.2 查阅面同口径上移服务端——持 auditor
  //    有效角色方可查（az audit:read；SecurityAuditor≠SystemAdmin）；
  //    每次检索落查阅日志（谁/何时/条件摘要/命中几条）；被拒尝试也留痕
  //   （audit.denied，审计自身被拒可对账）。查阅日志列表本身不落查阅
  //    日志（台账自阅不自指，避免递归噪音）——
  void route_audit_search(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j = json::object();
    if (!body.empty()) {
      try {
        j = json::parse(body);
      } catch (const std::exception&) {
        respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
        return;
      }
    }
    if (!j.is_object()) {
      respond_json(400, {{"ok", false}, {"error", "请求体须为对象"}});
      return;
    }
    const std::string target =
        j.contains("account") && j["account"].is_string()
            ? j["account"].get<std::string>()
            : "";
    const std::string keyword =
        j.contains("keyword") && j["keyword"].is_string()
            ? j["keyword"].get<std::string>()
            : "";
    const std::int64_t since_ms =
        j.contains("since_ms") && j["since_ms"].is_number_integer()
            ? j["since_ms"].get<std::int64_t>()
            : 0;
    const std::int64_t until_ms =
        j.contains("until_ms") && j["until_ms"].is_number_integer()
            ? j["until_ms"].get<std::int64_t>()
            : 0;
    int limit = j.contains("limit") && j["limit"].is_number_integer()
                    ? static_cast<int>(j["limit"].get<std::int64_t>())
                    : 200;
    if (limit < 1) limit = 1;
    if (limit > 1000) limit = 1000;
    // 过滤条件摘要（进查阅日志；只记条件，不记消息内容——与 CLI 同款）
    std::string filters;
    const auto append_filter = [&filters](const std::string& kv) {
      if (!filters.empty()) filters += " ";
      filters += kv;
    };
    if (!target.empty()) append_filter("账号=" + target);
    if (!keyword.empty()) append_filter("关键词=" + keyword);
    if (since_ms > 0) append_filter("起=" + std::to_string(since_ms));
    if (until_ms > 0) append_filter("止=" + std::to_string(until_ms));
    const std::int64_t now = now_ms();
    const Decision d = impl_.az.authorize(
        {account, "audit:read", "audit", "owner=" + account});
    if (!d.allowed) {
      // 被拒尝试同样留痕（与 CLI --as 拒绝路径同口径）
      AuditReadRow denied;
      denied.op_account = account;
      denied.action = "audit.denied";
      denied.filters = filters;
      denied.ts_ms = now;
      impl_.store.add_audit_read(denied);
      respond_json(403, {{"ok", false},
                         {"error",
                          "无 auditor 有效角色（SystemAdmin 不自动可读消息，"
                          "须经授权授予 auditor）；被拒尝试已留痕"}});
      return;
    }
    MessageSearch q;
    q.account = target;
    q.keyword = keyword;
    q.since_ms = since_ms;
    q.until_ms = until_ms;
    q.limit = limit;
    const auto rows = impl_.store.search_messages(q);
    AuditReadRow rec;
    rec.op_account = account;
    rec.action = "audit.message.search";
    rec.filters = filters;
    rec.result_count = static_cast<int>(rows.size());
    rec.ts_ms = now;
    impl_.store.add_audit_read(rec);
    std::cout << "[MEMEX] audit search account=" << account
              << " hits=" << rows.size() << std::endl;
    json arr = json::array();
    for (const auto& m : rows) {
      arr.push_back({{"msg_id", m.msg_id},
                     {"from", m.from_account},
                     {"to", m.to_account},
                     {"type", memex::protocol::msg_type_name(
                                  static_cast<memex::protocol::MsgType>(
                                      m.type))},
                     {"recalled", m.recalled},
                     {"ts_ms", m.ts_ms},
                     {"text", m.text}});
    }
    respond_json(200, {{"ok", true}, {"messages", std::move(arr)}});
  }

  void route_audit_reads() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const Decision d = impl_.az.authorize(
        {account, "audit:read", "audit", "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error",
                          "无 auditor 有效角色（查阅日志同样持证查阅）"}});
      return;
    }
    int limit = 100;
    const std::string lim = query_param(query_, "limit");
    if (!lim.empty()) {
      const int parsed = std::atoi(lim.c_str());
      if (parsed > 0) limit = parsed > 500 ? 500 : parsed;
    }
    json arr = json::array();
    for (const auto& r : impl_.store.audit_reads(limit)) {
      arr.push_back({{"id", r.id},
                     {"op_account", r.op_account},
                     {"action", r.action},
                     {"filters", r.filters},
                     {"result_count", r.result_count},
                     {"ts_ms", r.ts_ms}});
    }
    respond_json(200, {{"ok", true}, {"reads", std::move(arr)}});
  }

  // —— 办公室位置图（二期，设计稿 docs/design/办公室位置图.md）：
  //     看自楼层（同层互见=位置图语义本体；无工位=空图不造楼层）；
  //     编辑权 org-admin（az office:manage）；工位即楼层归属——
  static json seat_row_json(const ServerStore::SeatRow& s) {
    return {{"id", s.id},
            {"floor", s.floor},
            {"label", s.label},
            {"x", s.x},
            {"y", s.y},
            {"account", s.account}};
  }

  bool office_can_manage(const std::string& account) {
    const Decision d = impl_.az.authorize(
        {account, "office:manage", "office", "owner=" + account});
    return d.allowed;
  }

  void route_office_map() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::string floor;
    if (office_can_manage(account)) {
      // org-admin 可跨层查看（?floor= 指定；缺省=自楼层）
      floor = query_param(query_, "floor");
    }
    if (floor.empty()) {
      const auto mine = impl_.store.seat_of(account);
      floor = mine.has_value() ? mine->floor : ""; // 无工位=空图不造楼层
    }
    json arr = json::array();
    if (!floor.empty()) {
      for (const auto& s : impl_.store.seats_on_floor(floor)) {
        arr.push_back(seat_row_json(s));
      }
    }
    respond_json(200, {{"ok", true},
                       {"floor", floor},
                       {"can_manage", office_can_manage(account)},
                       {"seats", std::move(arr)}});
  }

  void route_office_seat(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    if (!office_can_manage(account)) {
      respond_json(403, {{"ok", false}, {"error", "工位编辑归 org-admin"}});
      return;
    }
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (j.is_object() && j.contains("remove") && j["remove"].is_boolean() &&
        j["remove"].get<bool>()) {
      if (!j.contains("id") || !j["id"].is_number_integer()) {
        respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
        return;
      }
      if (!impl_.store.seat_delete(j["id"].get<std::int64_t>())) {
        respond_json(404, {{"ok", false}, {"error", "工位不存在"}});
        return;
      }
      respond_json(200, {{"ok", true}});
      return;
    }
    if (!j.is_object() || !j.contains("floor") || !j["floor"].is_string() ||
        !j.contains("label") || !j["label"].is_string() ||
        !j.contains("x") || !j["x"].is_number() || !j.contains("y") ||
        !j["y"].is_number()) {
      respond_json(
          400, {{"ok", false}, {"error", "缺少字段：floor/label/x/y"}});
      return;
    }
    double x = j["x"].get<double>();
    double y = j["y"].get<double>();
    // 归一化坐标夹 0~1（越界=夹回不造假数据）
    x = std::max(0.0, std::min(1.0, x));
    y = std::max(0.0, std::min(1.0, y));
    const std::int64_t id = impl_.store.seat_upsert(
        j["floor"].get<std::string>(), j["label"].get<std::string>(), x, y,
        now_ms());
    if (id <= 0) {
      respond_json(500, {{"ok", false}, {"error", "工位落库失败"}});
      return;
    }
    respond_json(200, {{"ok", true}, {"id", id}});
  }

  void route_office_bind(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    if (!office_can_manage(account)) {
      respond_json(403, {{"ok", false}, {"error", "工位绑定归 org-admin"}});
      return;
    }
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("id") || !j["id"].is_number_integer() ||
        (j.contains("account") && !j["account"].is_string())) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
      return;
    }
    const std::string target =
        j.contains("account") ? j["account"].get<std::string>() : "";
    // 换座纪律：一人一工位（部分唯一索引守卫）——目标已占用他座=拒，
    // 先解绑再绑（不静默顶替他人座位）
    if (!impl_.store.seat_bind(j["id"].get<std::int64_t>(), target,
                               now_ms())) {
      respond_json(409, {{"ok", false},
                         {"error",
                          "绑定失败（工位不存在、账号不存在或该账号已占用"
                          "其他工位——换座须先解绑）"}});
      return;
    }
    std::cout << "[MEMEX] office seat " << (target.empty() ? "unbind"
                                                           : "bind")
              << " id=" << j["id"].get<std::int64_t>()
              << (target.empty() ? "" : " account=" + target)
              << " by=" << account << std::endl;
    respond_json(200, {{"ok", true}});
  }

  // —— 远程协助（二期）：协议面＋媒体中继。模型层（平台-11）在 store：
  // 五态生命周期/consent 红线（批拒撤只属受控方）/audit 红线（迁移自动
  // 留痕）/部门放行开关默认禁。路由只做当事方判权与状态门——
  void route_assist_request(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("target") ||
        !j["target"].is_string() || !j.contains("perms") ||
        !j["perms"].is_array()) {
      respond_json(400,
                   {{"ok", false}, {"error", "缺少字段：target/perms"}});
      return;
    }
    const std::string target = j["target"].get<std::string>();
    int mask = 0;
    for (const auto& p : j["perms"]) {
      mask |= assist_perm_mask(p.is_string() ? p.get<std::string>() : "");
    }
    if (target == account) {
      respond_json(400, {{"ok", false}, {"error", "不能向自己发起协助"}});
      return;
    }
    if (!impl_.store.find_account(target)) {
      respond_json(404, {{"ok", false}, {"error", "受控方账号不存在"}});
      return;
    }
    if (!assist_mask_valid(mask)) {
      respond_json(400,
                   {{"ok", false},
                    {"error", "perms 须为 view/keyboard/mouse/clipboard/"
                              "file 的非空子集"}});
      return;
    }
    const std::string id =
        impl_.store.assist_request(account, target, mask, now_ms());
    if (id.empty()) {
      // 部门放行开关从严双方＋默认禁（白名单口径）
      respond_json(403,
                   {{"ok", false},
                    {"error", "远程协助未对本部门放行（开关默认禁，须组织"
                              "管理员开启）"}});
      return;
    }
    respond_json(200, {{"ok", true}, {"id", id}});
  }

  void route_assist_respond(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string() ||
        !j.contains("approve") || !j["approve"].is_boolean()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：id/approve"}});
      return;
    }
    const std::string id = j["id"].get<std::string>();
    const auto s = impl_.store.assist_session(id);
    if (!s.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "会话不存在"}});
      return;
    }
    // consent 红线：批/拒只属受控方本人（store 层同样守，这里给准确错）
    if (s->target != account) {
      respond_json(403, {{"ok", false},
                         {"error", "只有受控方本人可批/拒（consent）"}});
      return;
    }
    if (!j["approve"].get<bool>()) {
      if (!impl_.store.assist_deny(id, account, now_ms())) {
        respond_json(409, {{"ok", false}, {"error", "会话不在待批态"}});
      } else {
        respond_json(200, {{"ok", true}});
      }
      return;
    }
    int granted = 0;
    if (j.contains("perms") && j["perms"].is_array()) {
      for (const auto& p : j["perms"]) {
        granted |=
            assist_perm_mask(p.is_string() ? p.get<std::string>() : "");
      }
    }
    if (!assist_mask_valid(granted)) {
      respond_json(400,
                   {{"ok", false},
                    {"error", "perms 须为非空合法子集（实批 ⊆ 申请）"}});
      return;
    }
    if (!impl_.store.assist_approve(id, account, granted, now_ms())) {
      respond_json(409,
                   {{"ok", false},
                    {"error", "批准失败（会话不在待批态，或实批超出申请"
                              "集）"}});
      return;
    }
    respond_json(200, {{"ok", true}});
  }

  void route_assist_lifecycle(const std::string& body, bool start) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
      return;
    }
    const std::string id = j["id"].get<std::string>();
    const auto s = impl_.store.assist_session(id);
    if (!s.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "会话不存在"}});
      return;
    }
    if (s->requester != account && s->target != account) {
      respond_json(403, {{"ok", false}, {"error", "非本会话当事方"}});
      return;
    }
    if (start) {
      if (!impl_.store.assist_start(id, account, now_ms())) {
        respond_json(409, {{"ok", false}, {"error", "会话不在已批态"}});
        return;
      }
      respond_json(200, {{"ok", true}});
      return;
    }
    std::string reason;
    if (j.contains("reason") && j["reason"].is_string()) {
      reason = j["reason"].get<std::string>();
    }
    if (!impl_.store.assist_end(id, account, reason, now_ms())) {
      respond_json(409, {{"ok", false}, {"error", "会话已是终态"}});
      return;
    }
    impl_.assist_media.erase(id); // 终态即擦媒体槽（屏幕内容不留存）
    respond_json(200, {{"ok", true}});
  }

  void route_assist_sessions() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json arr = json::array();
    for (const auto& s : impl_.store.assist_sessions(account)) {
      arr.push_back({{"id", s.id},
                     {"requester", s.requester},
                     {"target", s.target},
                     {"requested_mask", s.requested_mask},
                     {"granted_mask", s.granted_mask},
                     {"status", s.status},
                     {"requested_ms", s.requested_ms},
                     {"ended_ms", s.ended_ms}});
    }
    respond_json(200, {{"ok", true}, {"sessions", std::move(arr)}});
  }

  void route_assist_audit(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：id"}});
      return;
    }
    const std::string id = j["id"].get<std::string>();
    const auto s = impl_.store.assist_session(id);
    if (!s.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "会话不存在"}});
      return;
    }
    if (s->requester != account && s->target != account) {
      respond_json(403, {{"ok", false}, {"error", "非本会话当事方"}});
      return;
    }
    json arr = json::array();
    for (const auto& r : impl_.store.assist_audits(id)) {
      arr.push_back({{"actor", r.actor},
                     {"action", r.action},
                     {"detail", r.detail},
                     {"ts_ms", r.ts_ms}});
    }
    respond_json(200, {{"ok", true}, {"audits", std::move(arr)}});
  }

  // 媒体公共门：会话在、请求者是当事方、active、granted 含须位。
  // 返回非空串=错误已回。
  std::string assist_media_gate(const std::string& id,
                                const std::string& account, bool as_target,
                                int need_bit) {
    const auto s = impl_.store.assist_session(id);
    if (!s.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "会话不存在"}});
      return "not_found";
    }
    const std::string& actor_side = as_target ? s->target : s->requester;
    if (actor_side != account) {
      respond_json(403, {{"ok", false}, {"error", "非本会话该侧当事方"}});
      return "forbidden";
    }
    if (s->status != "active") {
      respond_json(409, {{"ok", false},
                         {"error", "会话不在进行中（" + s->status + "）"}});
      return "state";
    }
    if ((s->granted_mask & need_bit) == 0) {
      respond_json(403, {{"ok", false},
                         {"error", "受控方未授权该权限位"}});
      return "forbidden";
    }
    return "";
  }

  void route_assist_frame(const std::string& body, bool push) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    if (push) {
      json j;
      try {
        j = json::parse(body);
      } catch (const std::exception&) {
        respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
        return;
      }
      if (!j.is_object() || !j.contains("id") || !j["id"].is_string() ||
          !j.contains("jpeg_b64") || !j["jpeg_b64"].is_string()) {
        respond_json(400,
                     {{"ok", false}, {"error", "缺少字段：id/jpeg_b64"}});
        return;
      }
      const std::string id = j["id"].get<std::string>();
      if (!assist_media_gate(id, account, /*as_target=*/true,
                             ServerStore::kAssistView)
               .empty()) {
        return;
      }
      auto& slot = impl_.assist_media[id];
      slot.frame_b64 = j["jpeg_b64"].get<std::string>();
      if (j.contains("seq") && j["seq"].is_number_integer()) {
        slot.frame_seq = j["seq"].get<std::int64_t>();
      } else {
        ++slot.frame_seq; // 客户端不递增时服务端代递
      }
      respond_json(200, {{"ok", true}, {"seq", slot.frame_seq}});
      return;
    }
    const std::string id = query_param(query_, "id");
    if (id.empty()) {
      respond_json(400, {{"ok", false}, {"error", "缺少参数：id"}});
      return;
    }
    if (!assist_media_gate(id, account, /*as_target=*/false,
                           ServerStore::kAssistView)
             .empty()) {
      return;
    }
    const auto it = impl_.assist_media.find(id);
    if (it == impl_.assist_media.end() || it->second.frame_b64.empty()) {
      respond_json(200, {{"ok", true}, {"seq", 0}, {"jpeg_b64", ""}});
      return;
    }
    respond_json(200, {{"ok", true},
                       {"seq", it->second.frame_seq},
                       {"jpeg_b64", it->second.frame_b64}});
  }

  void route_assist_input(const std::string& body, bool send) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    if (send) {
      json j;
      try {
        j = json::parse(body);
      } catch (const std::exception&) {
        respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
        return;
      }
      if (!j.is_object() || !j.contains("id") || !j["id"].is_string() ||
          !j.contains("kind") || !j["kind"].is_string()) {
        respond_json(400, {{"ok", false}, {"error", "缺少字段：id/kind"}});
        return;
      }
      const std::string id = j["id"].get<std::string>();
      const std::string kind = j["kind"].get<std::string>();
      const int need = (kind == "key") ? ServerStore::kAssistKeyboard
                                       : ServerStore::kAssistMouse;
      if (!assist_media_gate(id, account, /*as_target=*/false, need)
               .empty()) {
        return;
      }
      auto& slot = impl_.assist_media[id];
      slot.inputs.push_back(j.dump()); // 原样转发（受控端自解字段）
      respond_json(200, {{"ok", true}});
      return;
    }
    const std::string id = query_param(query_, "id");
    if (id.empty()) {
      respond_json(400, {{"ok", false}, {"error", "缺少参数：id"}});
      return;
    }
    if (!assist_media_gate(id, account, /*as_target=*/true,
                           ServerStore::kAssistView)
             .empty()) {
      return;
    }
    json arr = json::array();
    const auto it = impl_.assist_media.find(id);
    if (it != impl_.assist_media.end()) {
      for (const auto& e : it->second.inputs) {
        arr.push_back(json::parse(e));
      }
      it->second.inputs.clear(); // 取走即清（FIFO 不重投）
    }
    respond_json(200, {{"ok", true}, {"events", std::move(arr)}});
  }

  void route_upload(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string target = query_param(query_, "target");
    bool is_group = false;
    bool is_inbox = false;
    std::uint64_t gid = 0;
    std::string uid;
    if (!parse_target(target, account, is_group, gid, uid, is_inbox)) {
      respond_json(400, {{"ok", false},
                         {"error", "target 须为 group:<数字>、me 或 inbox"}});
      return;
    }
    const std::string resource = is_group ? "group:" + std::to_string(gid)
                                          : "user:" + account;
    const Decision d = impl_.az.authorize(
        {account, "file:upload", resource, "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error", "无权上传（" + d.reason + "）"}});
      return;
    }
    if (is_group && !capability_gate(gid, "files", "群文件")) return;
    const auto r = impl_.handle_upload(account, is_group, gid, uid,
                                       file_name_, body, is_inbox);
    if (r.http_status != 200) {
      respond_json(r.http_status, {{"ok", false}, {"error", r.error}});
      return;
    }
    std::cout << "[MEMEX] files upload account=" << account << " target="
              << target << " id=" << r.file_id << " size=" << body.size()
              << (r.second_transfer ? " second-transfer" : "") << std::endl;
    respond_json(200, {{"ok", true},
                       {"id", r.file_id},
                       {"second_transfer", r.second_transfer}});
  }

  void route_download() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string id_s = query_param(query_, "id");
    if (id_s.empty() || id_s.find_first_not_of("0123456789") !=
                            std::string::npos) {
      respond_json(400, {{"ok", false}, {"error", "id 须为数字"}});
      return;
    }
    const auto meta = impl_.store.file_by_id(
        std::strtoll(id_s.c_str(), nullptr, 10));
    if (!meta.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "文件不存在"}});
      return;
    }
    const std::string resource =
        meta->belong_gid.empty()
            ? "user:" + meta->belong_uid + "/file:" + id_s
            : "group:" + meta->belong_gid + "/file:" + id_s;
    const Decision d = impl_.az.authorize(
        {account, "file:read", resource, "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权读取（" + d.reason + "）"}});
      return;
    }
    if (!meta->belong_gid.empty() &&
        !capability_gate(
            static_cast<std::uint64_t>(std::stoull(meta->belong_gid)),
            "files", "群文件")) {
      return;
    }
    if (!impl_.storage) {
      respond_json(503, {{"ok", false}, {"error", "存储后端未配置"}});
      return;
    }
    std::string bytes;
    if (!impl_.storage->get_object(meta->object_key, &bytes)) {
      respond_json(404, {{"ok", false}, {"error", "对象字节缺失"}});
      return;
    }
    // 二进制响应：文件名进头前滤控制字符（防头注入）
    std::string safe_name;
    for (const char c : meta->file_name) {
      if (static_cast<unsigned char>(c) >= 0x20 && c != '\x7f') {
        safe_name += c;
      }
    }
    std::ostringstream head;
    head << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: application/octet-stream\r\n"
         << "Content-Length: " << bytes.size() << "\r\n"
         << "X-File-Id: " << meta->id << "\r\n"
         << "X-File-Name: " << safe_name << "\r\n"
         << "Connection: close\r\n\r\n";
    write_raw(std::make_shared<const std::string>(head.str()), false,
              [this, bytes](std::error_code ec) {
                if (ec) return;
                write_raw(std::make_shared<const std::string>(std::move(bytes)),
                          true, [](std::error_code) {});
              });
  }

  void route_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string target = query_param(query_, "target");
    bool is_group = false;
    bool is_inbox = false;
    std::uint64_t gid = 0;
    std::string uid;
    if (!parse_target(target, account, is_group, gid, uid, is_inbox)) {
      respond_json(400, {{"ok", false},
                         {"error", "target 须为 group:<数字>、me 或 inbox"}});
      return;
    }
    const std::string resource = is_group ? "group:" + std::to_string(gid)
                                          : "user:" + account;
    const Decision d = impl_.az.authorize(
        {account, "file:list", resource, "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权列表（" + d.reason + "）"}});
      return;
    }
    if (is_group && !capability_gate(gid, "files", "群文件")) return;
    int limit = 200, offset = 0;
    const std::string lim = query_param(query_, "limit");
    const std::string off = query_param(query_, "offset");
    if (!lim.empty() && lim.find_first_not_of("0123456789") ==
                            std::string::npos) {
      limit = std::atoi(lim.c_str());
    }
    if (!off.empty() && off.find_first_not_of("0123456789") ==
                            std::string::npos) {
      offset = std::atoi(off.c_str());
    }
    if (is_inbox) {
      // 文件助手统一收件箱：备忘录 + 收件箱文件按时间倒序混排，
      // 分页在混排后取窗（memo 取 updated_ms，文件取 upload_ts）。
      const auto files =
          impl_.store.list_files("", account, -1, 0,
                                 static_cast<int>(ServerStore::FileKind::Inbox));
      const auto memos = impl_.store.list_memos(account, -1, 0);
      struct Item {
        std::int64_t ts{0};
        bool is_memo{false};
        ServerStore::FileMeta file;
        ServerStore::MemoRow memo;
      };
      std::vector<Item> merged;
      merged.reserve(files.size() + memos.size());
      for (const auto& f : files) merged.push_back(Item{f.upload_ts, false, f, {}});
      for (const auto& m : memos) merged.push_back(Item{m.updated_ms, true, {}, m});
      std::stable_sort(merged.begin(), merged.end(),
                       [](const Item& a, const Item& b) { return a.ts > b.ts; });
      const std::size_t begin =
          offset > 0 ? static_cast<std::size_t>(offset) : 0;
      const std::size_t end =
          std::min(merged.size(), begin + static_cast<std::size_t>(limit));
      json arr = json::array();
      for (std::size_t i = begin; i < end; ++i) {
        const Item& it = merged[i];
        if (it.is_memo) {
          arr.push_back({{"type", "memo"},
                         {"id", it.memo.id},
                         {"content", it.memo.content},
                         {"created_ms", it.memo.created_ms},
                         {"updated_ms", it.memo.updated_ms}});
        } else {
          arr.push_back({{"type", "file"},
                         {"id", it.file.id},
                         {"file_name", it.file.file_name},
                         {"file_size", it.file.file_size},
                         {"file_hash", it.file.file_hash},
                         {"pin", it.file.pin},
                         {"status", static_cast<int>(it.file.status)},
                         {"upload_ts", it.file.upload_ts}});
        }
      }
      respond_json(200, {{"ok", true}, {"items", arr}});
      return;
    }
    // 个人空间只列 kind=0：收件箱文件不混进个人文件列表（R23-3 空间隔离）
    const auto rows =
        impl_.store.list_files(is_group ? std::to_string(gid) : "",
                               is_group ? "" : account, limit, offset,
                               is_group ? -1 : 0);
    json arr = json::array();
    auto emit = [&arr](const ServerStore::FileMeta& m) {
      arr.push_back({{"id", m.id},
                     {"owner", m.owner},
                     {"file_name", m.file_name},
                     {"file_size", m.file_size},
                     {"file_hash", m.file_hash},
                     {"pin", m.pin},
                     {"status", static_cast<int>(m.status)},
                     {"upload_ts", m.upload_ts}});
    };
    if (is_group) {
      // 数据过滤（平台-1）：记录级可见性走统一裁决——非 normal 态
      // （隔离/过期）连元数据一起对全员隐没（ExplicitDeny 先于群主/
      // 管理员允许，冲突规则写死），成员经继承面见 normal 行；路由级
      // 判权已挡非成员，这里是行级第二道（蓝图§七：授权在数据访问层
      // 生效，不是只在 UI 层）。
      const auto hits = impl_.az.filter_allowed(
          rows.size(), [this, &account, &gid, &rows](std::size_t i) {
            AuthzQuery q;
            q.subject = account;
            q.action = "file:read";
            q.resource = "group:" + std::to_string(gid) + "/file:" +
                         std::to_string(rows[i].id);
            q.context = "owner=" + account;
            return q;
          });
      for (const auto& h : hits) {
        emit(rows[h.index]);
      }
      if (hits.size() != rows.size()) {
        std::cout << "[MEMEX] files list filter account=" << account
                  << " group=" << gid << " allowed=" << hits.size()
                  << " filtered=" << (rows.size() - hits.size()) << std::endl;
      }
    } else {
      // 个人/自有面：personal-owner 全允许，行级过滤恒等——不过滤
      for (const auto& m : rows) {
        emit(m);
      }
    }
    respond_json(200, {{"ok", true}, {"files", arr}});
  }

  void route_quota_get() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string target = query_param(query_, "target");
    bool is_group = false;
    bool is_inbox = false;
    std::uint64_t gid = 0;
    std::string uid;
    if (!parse_target(target, account, is_group, gid, uid, is_inbox)) {
      respond_json(400, {{"ok", false},
                         {"error", "target 须为 group:<数字>、me 或 inbox"}});
      return;
    }
    const std::string resource = is_group ? "group:" + std::to_string(gid)
                                          : "user:" + account;
    const Decision d = impl_.az.authorize(
        {account, "file:read", resource, "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权查看（" + d.reason + "）"}});
      return;
    }
    const auto q = is_group ? impl_.store.get_group_quota(std::to_string(gid))
                            : impl_.store.get_user_quota(account);
    if (!q.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "目标不存在"}});
      return;
    }
    respond_json(200, {{"ok", true},
                       {"used_bytes", q->used_bytes},
                       {"limit_bytes", q->limit_bytes}});
  }

  void route_delete() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string id_s = query_param(query_, "id");
    if (id_s.empty() || id_s.find_first_not_of("0123456789") !=
                            std::string::npos) {
      respond_json(400, {{"ok", false}, {"error", "id 须为数字"}});
      return;
    }
    const auto r = impl_.handle_delete(
        account, std::strtoll(id_s.c_str(), nullptr, 10));
    if (r.http_status != 200) {
      respond_json(r.http_status, {{"ok", false}, {"error", r.error}});
      return;
    }
    std::cout << "[MEMEX] files delete account=" << account << " id=" << id_s
              << " reason=" << r.reason << std::endl;
    respond_json(200, {{"ok", true}});
  }

  void route_pin() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string id_s = query_param(query_, "id");
    const std::string pin_s = query_param(query_, "pin");
    if (id_s.empty() || id_s.find_first_not_of("0123456789") !=
                            std::string::npos ||
        (pin_s != "0" && pin_s != "1")) {
      respond_json(400, {{"ok", false}, {"error", "id 须为数字、pin 须为 0|1"}});
      return;
    }
    const auto meta = impl_.store.file_by_id(
        std::strtoll(id_s.c_str(), nullptr, 10));
    if (!meta.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "文件不存在"}});
      return;
    }
    const std::string resource =
        meta->belong_gid.empty()
            ? "user:" + meta->belong_uid + "/file:" + id_s
            : "group:" + meta->belong_gid + "/file:" + id_s;
    const Decision d = impl_.az.authorize(
        {account, "file:pin", resource, "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权置顶（" + d.reason + "）"}});
      return;
    }
    if (!meta->belong_gid.empty() &&
        !capability_gate(
            static_cast<std::uint64_t>(std::stoull(meta->belong_gid)),
            "files", "群文件")) {
      return;
    }
    if (!impl_.store.set_file_pin(meta->id, pin_s == "1")) {
      respond_json(404, {{"ok", false}, {"error", "文件不存在"}});
      return;
    }
    respond_json(200, {{"ok", true}, {"pin", pin_s == "1"}});
  }

  void route_quota_set(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string target = query_param(query_, "target");
    // 配额管理=群动作（个人配额不开放 HTTP 面，走运维 CLI/store）
    if (target.rfind("group:", 0) != 0) {
      respond_json(400, {{"ok", false},
                         {"error", "配额管理仅支持 target=group:<数字>"}});
      return;
    }
    bool is_group = false;
    bool is_inbox = false;
    std::uint64_t gid = 0;
    std::string uid;
    if (!parse_target(target, account, is_group, gid, uid, is_inbox) ||
        !is_group) {
      respond_json(400, {{"ok", false},
                         {"error", "target 须为 group:<数字>"}});
      return;
    }
    const std::string resource = "group:" + std::to_string(gid);
    const Decision d = impl_.az.authorize(
        {account, "file:quota", resource, "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权配额（" + d.reason + "）"}});
      return;
    }
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("limit_bytes") ||
        !j["limit_bytes"].is_number_integer()) {
      respond_json(400, {{"ok", false},
                         {"error", "缺少字段：limit_bytes（整数）"}});
      return;
    }
    const std::int64_t limit = j["limit_bytes"].get<std::int64_t>();
    if (limit < 0) {
      respond_json(400, {{"ok", false}, {"error", "limit_bytes 不可为负"}});
      return;
    }
    if (!impl_.store.set_group_quota_limit(std::to_string(gid), limit)) {
      respond_json(500, {{"ok", false}, {"error", "配额设置失败"}});
      return;
    }
    respond_json(200, {{"ok", true}});
  }

  // —— R23-3 备忘录（文件助手文本面）：判权走 AuthorizationService，
  //    personal-owner 仅本人（resource "user:{owner}/memo:{id}" 前缀匹配
  //    即中、不限 action），他人一律 default-deny——不自造权限规则 ——
  static std::string memo_resource(const std::string& owner,
                                   std::int64_t id) {
    return "user:" + owner + "/memo:" + std::to_string(id);
  }

  void route_memo_write(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("content") || !j["content"].is_string() ||
        j["content"].get<std::string>().empty()) {
      respond_json(400, {{"ok", false},
                         {"error", "缺少字段：content（非空字符串）"}});
      return;
    }
    const std::string content = j["content"].get<std::string>();
    if (j.contains("id")) {
      if (!j["id"].is_number_integer()) {
        respond_json(400, {{"ok", false}, {"error", "id 须为整数"}});
        return;
      }
      const std::int64_t id = j["id"].get<std::int64_t>();
      const auto row = impl_.store.memo_by_id(id);
      if (!row.has_value()) {
        respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
        return;
      }
      const Decision d = impl_.az.authorize(
          {account, "memo:update", memo_resource(row->owner, id),
           "owner=" + account});
      if (!d.allowed) {
        respond_json(403,
                     {{"ok", false}, {"error", "无权编辑（" + d.reason + "）"}});
        return;
      }
      if (!impl_.store.update_memo(id, account, content, now_ms())) {
        respond_json(500, {{"ok", false}, {"error", "备忘录更新失败"}});
        return;
      }
      std::cout << "[MEMEX] files memo update account=" << account
                << " id=" << id << std::endl;
      respond_json(200, {{"ok", true}, {"id", id}});
      return;
    }
    const Decision d = impl_.az.authorize(
        {account, "memo:create", "user:" + account, "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权新建（" + d.reason + "）"}});
      return;
    }
    const std::int64_t id = impl_.store.create_memo(account, content, now_ms());
    if (id <= 0) {
      respond_json(500, {{"ok", false}, {"error", "备忘录落库失败"}});
      return;
    }
    std::cout << "[MEMEX] files memo create account=" << account
              << " id=" << id << std::endl;
    respond_json(200, {{"ok", true}, {"id", id}});
  }

  void route_memo() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string id_s = query_param(query_, "id");
    if (!id_s.empty()) {
      if (id_s.find_first_not_of("0123456789") != std::string::npos) {
        respond_json(400, {{"ok", false}, {"error", "id 须为数字"}});
        return;
      }
      const std::int64_t id = std::strtoll(id_s.c_str(), nullptr, 10);
      const auto row = impl_.store.memo_by_id(id);
      if (!row.has_value()) {
        respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
        return;
      }
      const Decision d = impl_.az.authorize(
          {account, "memo:read", memo_resource(row->owner, id),
           "owner=" + account});
      if (!d.allowed) {
        respond_json(403,
                     {{"ok", false}, {"error", "无权读取（" + d.reason + "）"}});
        return;
      }
      respond_json(200,
                   {{"ok", true},
                    {"memo",
                     {{"id", row->id},
                      {"owner", row->owner},
                      {"content", row->content},
                      {"created_ms", row->created_ms},
                      {"updated_ms", row->updated_ms}}}});
      return;
    }
    const Decision d = impl_.az.authorize(
        {account, "memo:read", "user:" + account, "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权列表（" + d.reason + "）"}});
      return;
    }
    int limit = 200, offset = 0;
    const std::string lim = query_param(query_, "limit");
    const std::string off = query_param(query_, "offset");
    if (!lim.empty() &&
        lim.find_first_not_of("0123456789") == std::string::npos) {
      limit = std::atoi(lim.c_str());
    }
    if (!off.empty() &&
        off.find_first_not_of("0123456789") == std::string::npos) {
      offset = std::atoi(off.c_str());
    }
    const auto rows = impl_.store.list_memos(account, limit, offset);
    json arr = json::array();
    for (const auto& m : rows) {
      arr.push_back({{"id", m.id},
                     {"owner", m.owner},
                     {"content", m.content},
                     {"created_ms", m.created_ms},
                     {"updated_ms", m.updated_ms}});
    }
    respond_json(200, {{"ok", true}, {"memos", arr}});
  }

  void route_memo_delete() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string id_s = query_param(query_, "id");
    if (id_s.empty() ||
        id_s.find_first_not_of("0123456789") != std::string::npos) {
      respond_json(400, {{"ok", false}, {"error", "id 须为数字"}});
      return;
    }
    const std::int64_t id = std::strtoll(id_s.c_str(), nullptr, 10);
    const auto row = impl_.store.memo_by_id(id);
    if (!row.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
      return;
    }
    const Decision d = impl_.az.authorize(
        {account, "memo:delete", memo_resource(row->owner, id),
         "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权删除（" + d.reason + "）"}});
      return;
    }
    if (!impl_.store.delete_memo(id, account)) {
      respond_json(500, {{"ok", false}, {"error", "备忘录删除失败"}});
      return;
    }
    std::cout << "[MEMEX] files memo delete account=" << account
              << " id=" << id << std::endl;
    respond_json(200, {{"ok", true}});
  }

  // —— R24-2 群备忘录（群维度共享知识：标题+正文；修订史逐笔留痕可回滚）——
  // 判权映射（不自造规则）：管理写 memo:write / 删 memo:delete / 开关
  // memo:config 由 group-owner/group-admin 规则命中；成员读 file:read /
  // file:list 由 group-member 继承命中。开放编辑是产品开关（设计：
  // 管理员维护或开放编辑），开着时对写/回滚叠加成员判定——route 层裁量。
  static std::string group_resource(std::uint64_t gid) {
    return "group:" + std::to_string(gid);
  }
  static std::string group_memo_resource(std::uint64_t gid, std::int64_t id) {
    return group_resource(gid) + "/memo:" + std::to_string(id);
  }
  static bool parse_gid_param(const std::string& s, std::uint64_t* out) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) {
      return false;
    }
    *out = std::strtoull(s.c_str(), nullptr, 10);
    return *out > 0;
  }
  bool group_memo_write_allowed(const std::string& account,
                                std::uint64_t gid,
                                const std::string& resource) {
    if (impl_.az
            .authorize({account, "memo:write", resource, "owner=" + account})
            .allowed) {
      return true;
    }
    return impl_.store.group_memo_open_edit(gid) &&
           impl_.az
               .authorize({account, "file:read", group_resource(gid),
                           "owner=" + account})
               .allowed;
  }

  void route_group_memo_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!capability_gate(gid, "memo", "群备忘录")) return;
    const Decision d = impl_.az.authorize(
        {account, "file:list", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权查看（" + d.reason + "）"}});
      return;
    }
    int limit = 200, offset = 0;
    const std::string lim = query_param(query_, "limit");
    const std::string off = query_param(query_, "offset");
    if (!lim.empty() &&
        lim.find_first_not_of("0123456789") == std::string::npos) {
      limit = std::atoi(lim.c_str());
    }
    if (!off.empty() &&
        off.find_first_not_of("0123456789") == std::string::npos) {
      offset = std::atoi(off.c_str());
    }
    const auto rows = impl_.store.list_group_memos(
        gid, query_param(query_, "q"), limit, offset);
    json arr = json::array();
    for (const auto& m : rows) {
      arr.push_back({{"id", m.id},
                     {"title", m.title},
                     {"content", m.content},
                     {"author", m.author},
                     {"created_ms", m.created_ms},
                     {"updated_ms", m.updated_ms}});
    }
    respond_json(200, {{"ok", true},
                       {"gid", gid},
                       {"open_edit", impl_.store.group_memo_open_edit(gid)},
                       {"memos", arr}});
  }

  void route_group_memo_save(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() ||
        !j.contains("title") || !j["title"].is_string() ||
        !j.contains("content") || !j["content"].is_string() ||
        j["title"].get<std::string>().empty() ||
        j["content"].get<std::string>().empty()) {
      respond_json(400, {{"ok", false},
                         {"error", "缺少字段：gid/title/content（非空）"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string title = j["title"].get<std::string>();
    const std::string content = j["content"].get<std::string>();
    // 新建/编辑两分支共此一门（门在分支外，防新建绕行）
    if (!capability_gate(gid, "memo", "群备忘录")) return;
    if (j.contains("id") && !j["id"].is_null()) {
      if (!j["id"].is_number_integer()) {
        respond_json(400, {{"ok", false}, {"error", "id 须为整数"}});
        return;
      }
      const std::int64_t id = j["id"].get<std::int64_t>();
      const auto row = impl_.store.group_memo_by_id(id);
      if (!row.has_value() || row->group_id != gid) {
        respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
        return;
      }
      if (!group_memo_write_allowed(account, gid,
                                    group_memo_resource(gid, id))) {
        respond_json(403, {{"ok", false},
                           {"error", "无权编辑（管理员维护中；可请管理员开放编辑）"}});
        return;
      }
      if (!impl_.store.update_group_memo(id, title, content, account,
                                         now_ms())) {
        respond_json(500, {{"ok", false}, {"error", "备忘录更新失败"}});
        return;
      }
      std::cout << "[MEMEX] files group-memo update account=" << account
                << " gid=" << gid << " id=" << id << std::endl;
      respond_json(200, {{"ok", true}, {"id", id}});
      return;
    }
    if (!group_memo_write_allowed(account, gid, group_resource(gid))) {
      respond_json(403, {{"ok", false},
                         {"error", "无权新建（管理员维护中；可请管理员开放编辑）"}});
      return;
    }
    const std::int64_t id = impl_.store.create_group_memo(
        gid, title, content, account, now_ms());
    if (id <= 0) {
      respond_json(500, {{"ok", false}, {"error", "备忘录落库失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-memo create account=" << account
              << " gid=" << gid << " id=" << id << std::endl;
    respond_json(200, {{"ok", true}, {"id", id}});
  }

  void route_group_memo_delete(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("id") ||
        !j["id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/id"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::int64_t id = j["id"].get<std::int64_t>();
    const auto row = impl_.store.group_memo_by_id(id);
    if (!row.has_value() || row->group_id != gid) {
      respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
      return;
    }
    // 删除恒归管理员（开放编辑开放的是写，不是删——防误删共享知识）
    if (!capability_gate(gid, "memo", "群备忘录")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:delete", group_memo_resource(gid, id),
         "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权删除（" + d.reason + "）"}});
      return;
    }
    if (!impl_.store.delete_group_memo(id, gid)) {
      respond_json(500, {{"ok", false}, {"error", "备忘录删除失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-memo delete account=" << account
              << " gid=" << gid << " id=" << id << std::endl;
    respond_json(200, {{"ok", true}});
  }

  void route_group_memo_history() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    const std::string id_s = query_param(query_, "id");
    if (id_s.empty() ||
        id_s.find_first_not_of("0123456789") != std::string::npos) {
      respond_json(400, {{"ok", false}, {"error", "id 须为数字"}});
      return;
    }
    const std::int64_t id = std::strtoll(id_s.c_str(), nullptr, 10);
    const auto row = impl_.store.group_memo_by_id(id);
    if (!row.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
      return;
    }
    if (!capability_gate(row->group_id, "memo", "群备忘录")) return;
    const Decision d = impl_.az.authorize(
        {account, "file:read", group_memo_resource(row->group_id, id),
         "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权查看（" + d.reason + "）"}});
      return;
    }
    json arr = json::array();
    for (const auto& r : impl_.store.group_memo_history(id)) {
      arr.push_back({{"id", r.id},
                     {"title", r.title},
                     {"content", r.content},
                     {"editor", r.editor},
                     {"ts_ms", r.ts_ms}});
    }
    respond_json(200, {{"ok", true}, {"id", id}, {"revisions", arr}});
  }

  void route_group_memo_rollback(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("id") ||
        !j["id"].is_number_integer() || !j.contains("revision_id") ||
        !j["revision_id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：id/revision_id"}});
      return;
    }
    const std::int64_t id = j["id"].get<std::int64_t>();
    const std::int64_t rev_id = j["revision_id"].get<std::int64_t>();
    const auto row = impl_.store.group_memo_by_id(id);
    if (!row.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "备忘录不存在"}});
      return;
    }
    if (!capability_gate(row->group_id, "memo", "群备忘录")) return;
    if (!group_memo_write_allowed(account, row->group_id,
                                  group_memo_resource(row->group_id, id))) {
      respond_json(403, {{"ok", false},
                         {"error", "无权回滚（管理员维护中；可请管理员开放编辑）"}});
      return;
    }
    const auto hist = impl_.store.group_memo_history(id);
    const auto it = std::find_if(
        hist.begin(), hist.end(),
        [rev_id](const ServerStore::GroupMemoRevision& r) {
          return r.id == rev_id;
        });
    if (it == hist.end()) {
      respond_json(404, {{"ok", false}, {"error", "修订笔不存在"}});
      return;
    }
    // 回滚即一次编辑：取目标笔全文写回并落新笔（editor=回滚者）——留痕链不断
    if (!impl_.store.update_group_memo(id, it->title, it->content, account,
                                       now_ms())) {
      respond_json(500, {{"ok", false}, {"error", "回滚失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-memo rollback account=" << account
              << " gid=" << row->group_id << " id=" << id
              << " to_revision=" << rev_id << std::endl;
    respond_json(200, {{"ok", true}, {"id", id}});
  }

  void route_group_memo_open_edit(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("open") ||
        !j["open"].is_boolean()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/open（布尔）"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    if (!capability_gate(gid, "memo", "群备忘录")) return;
    const Decision d = impl_.az.authorize({account, "memo:config",
                                           group_resource(gid),
                                           "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error", "无权设置（仅群主/管理员）"}});
      return;
    }
    if (!impl_.store.set_group_memo_open_edit(gid, j["open"].get<bool>())) {
      respond_json(500, {{"ok", false}, {"error", "设置失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-memo open-edit account=" << account
              << " gid=" << gid
              << " open=" << (j["open"].get<bool>() ? 1 : 0) << std::endl;
    respond_json(200, {{"ok", true},
                       {"open_edit", j["open"].get<bool>()}});
  }

  // —— R24-3 群密码箱 ——
  // 解锁/读取判定（route 叠加，不自造 az 规则）：群成员（file:read 群
  // 继承）且（owner/admin 恒可 ∨ 授权名单空=全成员 ∨ 在名单）
  bool vault_unlock_allowed(const std::string& account, std::uint64_t gid) {
    if (!impl_.az
             .authorize({account, "file:read", group_resource(gid),
                         "owner=" + account})
             .allowed) {
      return false; // 非群成员（退群即失）
    }
    if (impl_.az
            .authorize({account, "memo:write", group_resource(gid),
                        "owner=" + account})
            .allowed) {
      return true; // owner/admin 恒可（维护权即读取权）
    }
    const auto acl = impl_.store.vault_acl_list(gid);
    if (acl.empty()) return true; // 默认全成员（共享的本意）
    return std::find(acl.begin(), acl.end(), account) != acl.end();
  }

  // init/rekey 公共字段校验：gid 整数、盐/包裹块非空串、迭代数 ≥10000
  static bool vault_wrap_fields(const json& j, std::uint64_t* gid,
                                std::string* salt, int* iters,
                                std::string* wrapped) {
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("kdf_salt") ||
        !j["kdf_salt"].is_string() || !j.contains("kdf_iters") ||
        !j["kdf_iters"].is_number_integer() || !j.contains("wrapped_dek") ||
        !j["wrapped_dek"].is_string()) {
      return false;
    }
    *salt = j["kdf_salt"].get<std::string>();
    *wrapped = j["wrapped_dek"].get<std::string>();
    *iters = j["kdf_iters"].get<int>();
    if (salt->empty() || wrapped->empty() || *iters < 10000) return false;
    *gid = static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    return *gid > 0;
  }

  void route_group_vault_info() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!capability_gate(gid, "vault", "群密码箱")) return;
    if (!vault_unlock_allowed(account, gid)) {
      respond_json(403, {{"ok", false},
                         {"error", "无权查看（非群成员或不在授权名单）"}});
      return;
    }
    const auto v = impl_.store.group_vault_info(gid);
    if (!v.has_value()) {
      respond_json(200, {{"ok", true}, {"exists", false}});
      return;
    }
    json acl = json::array();
    for (const auto& a : impl_.store.vault_acl_list(gid)) acl.push_back(a);
    respond_json(200, {{"ok", true},
                       {"exists", true},
                       {"gid", gid},
                       {"kdf_salt", v->kdf_salt},
                       {"kdf_iters", v->kdf_iters},
                       {"wrapped_dek", v->wrapped_dek},
                       {"acl", acl}});
  }

  void route_group_vault_init(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    std::uint64_t gid = 0;
    std::string salt, wrapped;
    int iters = 0;
    if (!vault_wrap_fields(j, &gid, &salt, &iters, &wrapped)) {
      respond_json(400, {{"ok", false},
                         {"error",
                          "缺少字段：gid/kdf_salt/kdf_iters(≥10000)/"
                          "wrapped_dek（非空）"}});
      return;
    }
    if (!capability_gate(gid, "vault", "群密码箱")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:write", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error", "无权建箱（仅群主/管理员）"}});
      return;
    }
    if (impl_.store.group_vault_info(gid).has_value()) {
      respond_json(409, {{"ok", false}, {"error", "该群密码箱已初始化"}});
      return;
    }
    if (!impl_.store.group_vault_init(gid, salt, iters, wrapped, now_ms())) {
      respond_json(500, {{"ok", false}, {"error", "建箱失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-vault init account=" << account
              << " gid=" << gid << " iters=" << iters << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}});
  }

  void route_group_vault_rekey(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    std::uint64_t gid = 0;
    std::string salt, wrapped;
    int iters = 0;
    if (!vault_wrap_fields(j, &gid, &salt, &iters, &wrapped)) {
      respond_json(400, {{"ok", false},
                         {"error",
                          "缺少字段：gid/kdf_salt/kdf_iters(≥10000)/"
                          "wrapped_dek（非空）"}});
      return;
    }
    if (!capability_gate(gid, "vault", "群密码箱")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:write", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error", "无权重置（仅群主/管理员）"}});
      return;
    }
    if (!impl_.store.group_vault_rekey(gid, salt, iters, wrapped, now_ms())) {
      respond_json(404, {{"ok", false}, {"error", "密码箱不存在"}});
      return;
    }
    std::cout << "[MEMEX] files group-vault rekey account=" << account
              << " gid=" << gid << " iters=" << iters << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}});
  }

  void route_group_vault_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!capability_gate(gid, "vault", "群密码箱")) return;
    if (!vault_unlock_allowed(account, gid)) {
      respond_json(403, {{"ok", false},
                         {"error", "无权查看（非群成员或不在授权名单）"}});
      return;
    }
    json arr = json::array();
    for (const auto& e : impl_.store.vault_list_entries(gid)) {
      arr.push_back({{"id", e.id},
                     {"name", e.name},
                     {"account_name", e.account_name},
                     {"created_by", e.created_by},
                     {"created_ms", e.created_ms},
                     {"updated_ms", e.updated_ms}});
      // 掩码面：secret_* 不进列表（密文只经 access，每访留痕）
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"entries", arr}});
  }

  void route_group_vault_access(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("id") ||
        !j["id"].is_number_integer() || !j.contains("action") ||
        !j["action"].is_string()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/id/action"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::int64_t id = j["id"].get<std::int64_t>();
    const std::string action = j["action"].get<std::string>();
    if (action != "reveal" && action != "copy") {
      respond_json(400, {{"ok", false},
                         {"error", "action 须为 reveal（查看）或 copy（复制）"}});
      return;
    }
    if (!capability_gate(gid, "vault", "群密码箱")) return;
    if (!vault_unlock_allowed(account, gid)) {
      respond_json(403, {{"ok", false},
                         {"error", "无权查看（非群成员或不在授权名单）"}});
      return;
    }
    const auto row = impl_.store.vault_entry_by_id(id);
    if (!row.has_value() || row->group_id != gid) {
      respond_json(404, {{"ok", false}, {"error", "条目不存在"}});
      return;
    }
    // 每次访问留痕（谁/何时/哪条/何动作——设计：查看留痕、复制显式+留痕）
    impl_.store.vault_audit_add(gid, id, account, action, now_ms());
    std::cout << "[MEMEX] files group-vault " << action
              << " account=" << account << " gid=" << gid << " id=" << id
              << std::endl;
    respond_json(200,
                 {{"ok", true},
                  {"id", row->id},
                  {"name", row->name},
                  {"account_name", row->account_name},
                  {"secret_ct", row->secret_ct},
                  {"secret_nonce", row->secret_nonce},
                  {"updated_ms", row->updated_ms}});
  }

  void route_group_vault_save(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("name") ||
        !j["name"].is_string() || !j.contains("account_name") ||
        !j["account_name"].is_string() || !j.contains("secret_ct") ||
        !j["secret_ct"].is_string() || !j.contains("secret_nonce") ||
        !j["secret_nonce"].is_string() ||
        j["name"].get<std::string>().empty() ||
        j["account_name"].get<std::string>().empty() ||
        j["secret_ct"].get<std::string>().empty() ||
        j["secret_nonce"].get<std::string>().empty()) {
      respond_json(400, {{"ok", false},
                         {"error",
                          "缺少字段：gid/name/account_name/secret_ct/"
                          "secret_nonce（非空）"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string name = j["name"].get<std::string>();
    const std::string account_name = j["account_name"].get<std::string>();
    const std::string secret_ct = j["secret_ct"].get<std::string>();
    const std::string secret_nonce = j["secret_nonce"].get<std::string>();
    // 管理员维护条目（memo:write 规则命中 owner/admin；不开放成员写）
    if (!capability_gate(gid, "vault", "群密码箱")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:write", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error", "无权维护条目（仅群主/管理员）"}});
      return;
    }
    if (j.contains("id") && !j["id"].is_null()) {
      if (!j["id"].is_number_integer()) {
        respond_json(400, {{"ok", false}, {"error", "id 须为整数"}});
        return;
      }
      const std::int64_t id = j["id"].get<std::int64_t>();
      const auto row = impl_.store.vault_entry_by_id(id);
      if (!row.has_value() || row->group_id != gid) {
        respond_json(404, {{"ok", false}, {"error", "条目不存在"}});
        return;
      }
      if (!impl_.store.vault_update_entry(id, name, account_name, secret_ct,
                                          secret_nonce, account, now_ms())) {
        respond_json(500, {{"ok", false}, {"error", "条目更新失败"}});
        return;
      }
      std::cout << "[MEMEX] files group-vault update account=" << account
                << " gid=" << gid << " id=" << id << std::endl;
      respond_json(200, {{"ok", true}, {"id", id}});
      return;
    }
    const std::int64_t id = impl_.store.vault_create_entry(
        gid, name, account_name, secret_ct, secret_nonce, account, now_ms());
    if (id <= 0) {
      respond_json(500, {{"ok", false}, {"error", "条目落库失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-vault create account=" << account
              << " gid=" << gid << " id=" << id << std::endl;
    respond_json(200, {{"ok", true}, {"id", id}});
  }

  void route_group_vault_delete(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("id") ||
        !j["id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/id"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::int64_t id = j["id"].get<std::int64_t>();
    const auto row = impl_.store.vault_entry_by_id(id);
    if (!row.has_value() || row->group_id != gid) {
      respond_json(404, {{"ok", false}, {"error", "条目不存在"}});
      return;
    }
    // 删条目恒归管理员（memo:delete 规则命中 owner/admin）
    if (!capability_gate(gid, "vault", "群密码箱")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:delete", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false}, {"error", "无权删除（" + d.reason + "）"}});
      return;
    }
    if (!impl_.store.vault_delete_entry(id, gid)) {
      respond_json(500, {{"ok", false}, {"error", "条目删除失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-vault delete account=" << account
              << " gid=" << gid << " id=" << id << std::endl;
    respond_json(200, {{"ok", true}});
  }

  void route_group_vault_acl(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("accounts") ||
        !j["accounts"].is_array()) {
      respond_json(400, {{"ok", false},
                         {"error", "缺少字段：gid/accounts（数组，空=恢复全成员）"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    std::vector<std::string> accounts;
    for (const auto& a : j["accounts"]) {
      if (!a.is_string()) {
        respond_json(400, {{"ok", false}, {"error", "accounts 须为字符串数组"}});
        return;
      }
      accounts.push_back(a.get<std::string>());
    }
    // 授权名单仅群主可改（vault:config 由 group-owner 命中、group-admin
    // 显式排除——收窄群主的共享授权不归管理员）
    if (!capability_gate(gid, "vault", "群密码箱")) return;
    const Decision d = impl_.az.authorize(
        {account, "vault:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error", "无权设置授权名单（仅群主）"}});
      return;
    }
    if (!impl_.store.vault_set_acl(gid, accounts)) {
      respond_json(500, {{"ok", false}, {"error", "名单写入失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-vault acl account=" << account
              << " gid=" << gid << " size=" << accounts.size() << std::endl;
    respond_json(200, {{"ok", true},
                       {"gid", gid},
                       {"size", static_cast<int>(accounts.size())}});
  }

  void route_group_vault_audit() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    // 审计查询=管理面（memo:config 规则命中 owner/admin）
    if (!capability_gate(gid, "vault", "群密码箱")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403, {{"ok", false},
                         {"error", "无权查看审计（仅群主/管理员）"}});
      return;
    }
    json arr = json::array();
    for (const auto& a : impl_.store.vault_audit_list(gid)) {
      arr.push_back({{"id", a.id},
                     {"entry_id", a.entry_id},
                     {"actor", a.actor},
                     {"action", a.action},
                     {"ts_ms", a.ts_ms}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"rows", arr}});
  }

  // —— R25-1 群工具框架 ——
  // call 判定（入群即授权/退群即失，route 叠加不自造规则）：群成员
  // （file:read 群继承）即备格；白名单另查（工具只暴露声明的动作）。
  bool tool_call_allowed(const std::string& account, std::uint64_t gid) {
    return impl_.az
        .authorize({account, "file:read", group_resource(gid),
                    "owner=" + account})
        .allowed;
  }

  // —— 平台-12 权限模型接线：群能力面统一门 ——
  // 未配置=现行允许、配置禁即拒（首段步进口径）。放在既有角色判权之后、
  // 动作之前：非成员先吃 403 角色拒，不向未授权者泄露能力配置状态。
  bool capability_gate(std::uint64_t gid, const char* cap,
                       const char* label) {
    if (impl_.store.group_capability_enabled(gid, cap)) return true;
    respond_json(403,
                 {{"ok", false},
                  {"error", std::string(label) +
                       "能力已被管理员停用（deny:capability-" + cap + "）"}});
    return false;
  }

  // 工具白名单配置（仅群主/管理员＝memo:config；actions 须为字符串数组）
  void route_group_tools_config(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("tool") ||
        !j["tool"].is_string() || !j.contains("actions") ||
        !j["actions"].is_array()) {
      respond_json(400, {{"ok", false},
                         {"error", "缺少字段：gid/tool/actions（字符串数组）"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string tool = j["tool"].get<std::string>();
    if (gid == 0 || tool.empty()) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数、tool 非空"}});
      return;
    }
    std::vector<std::string> actions;
    for (const auto& a : j["actions"]) {
      if (!a.is_string()) {
        respond_json(400, {{"ok", false}, {"error", "actions 须为字符串数组"}});
        return;
      }
      actions.push_back(a.get<std::string>());
    }
    if (!capability_gate(gid, "tools", "群工具")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权配置工具（仅群主/管理员）"}});
      return;
    }
    json arr = json::array();
    for (const auto& a : actions) arr.push_back(a);
    if (!impl_.store.tool_set_actions(gid, tool, arr.dump(), account,
                                      now_ms())) {
      respond_json(404, {{"ok", false}, {"error", "群不存在"}});
      return;
    }
    std::cout << "[MEMEX] files group-tools config account=" << account
              << " gid=" << gid << " tool=" << tool
              << " actions=" << actions.size() << std::endl;
    respond_json(200, {{"ok", true},
                       {"gid", gid},
                       {"tool", tool},
                       {"size", static_cast<int>(actions.size())}});
  }

  void route_group_tools_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!capability_gate(gid, "tools", "群工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
      return;
    }
    json arr = json::array();
    for (const auto& c : impl_.store.tool_list(gid)) {
      json actions = json::array();
      try {
        actions = json::parse(c.actions_json);
      } catch (const std::exception&) {
        actions = json::array(); // 库内串损坏不炸路由
      }
      arr.push_back({{"tool", c.tool},
                     {"actions", actions},
                     {"updated_by", c.updated_by},
                     {"updated_ms", c.updated_ms}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"tools", arr}});
  }

  // 动作代理调用（R25-1=骨架：白名单校验＋留痕＋stub 回显；真外部系统
  // 调用随 R25-2/R25-3 落地——客户端只见按钮不见密钥）
  void route_group_tools_call(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("tool") ||
        !j["tool"].is_string() || !j.contains("action") ||
        !j["action"].is_string() || !j.contains("params") ||
        !j["params"].is_object()) {
      respond_json(
          400, {{"ok", false},
                {"error", "缺少字段：gid/tool/action/params（对象）"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string tool = j["tool"].get<std::string>();
    const std::string action = j["action"].get<std::string>();
    if (!capability_gate(gid, "tools", "群工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权调用（非群成员）"}});
      return;
    }
    const auto cfg = impl_.store.tool_config(gid, tool);
    if (!cfg.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "工具未配置"}});
      return;
    }
    // 白名单制：动作不在声明清单＝拒（无自由参数逃逸；参数校验随具体工具）
    json actions = json::array();
    try {
      actions = json::parse(cfg->actions_json);
    } catch (const std::exception&) {
      actions = json::array();
    }
    bool listed = false;
    for (const auto& a : actions) {
      if (a.is_string() && a.get<std::string>() == action) {
        listed = true;
        break;
      }
    }
    if (!listed) {
      respond_json(403, {{"ok", false},
                         {"error", "动作未开放（不在该群工具白名单）"}});
      return;
    }
    // 留痕在回包前？在回包后？——先执行后留痕：结果一并入审计行
    const json result = {{"ok", true},
                         {"stub", true},
                         {"tool", tool},
                         {"action", action},
                         {"echo", j["params"]}};
    const std::string result_str = result.dump();
    impl_.store.tool_audit_add(gid, tool, action, account, j["params"].dump(),
                               result_str, now_ms());
    std::cout << "[MEMEX] files group-tools call account=" << account
              << " gid=" << gid << " tool=" << tool
              << " action=" << action << std::endl;
    respond_json(200, {{"ok", true},
                       {"gid", gid},
                       {"tool", tool},
                       {"action", action},
                       {"result", result}});
  }

  void route_group_tools_audit() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    // 审计查询=管理面（memo:config 规则命中 owner/admin）
    if (!capability_gate(gid, "tools", "群工具")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权查看审计（仅群主/管理员）"}});
      return;
    }
    json arr = json::array();
    for (const auto& a : impl_.store.tool_audit_list(gid)) {
      json params = json::object();
      json result = json::object();
      try {
        params = json::parse(a.params_json);
      } catch (const std::exception&) {
      }
      try {
        result = json::parse(a.result_json);
      } catch (const std::exception&) {
      }
      arr.push_back({{"id", a.id},
                     {"tool", a.tool},
                     {"action", a.action},
                     {"actor", a.actor},
                     {"params", params},
                     {"result", result},
                     {"ts_ms", a.ts_ms}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"rows", arr}});
  }

  // —— R25-2 CI/CD 工具（落在 R25-1 框架上的首个具体工具）——
  // 触发判权：R25-1 框架的 call 同构（成员 file:read 群继承＋白名单
  // 「trigger」开放）；流水线定义/删除=memo:config（owner/admin）。
  // 执行器为 stub：同步完成即时出终态（真 CI 系统接入随 R25-4 凭据面）；
  // params 可带 {"fail":true} 注入失败——stub 阶段供红绿灯/卡片失败腿
  // 演练与测试，真执行器接入后由真实结果决定。
  void route_group_ci_pipeline(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("name") ||
        !j["name"].is_string()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/name"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string name = j["name"].get<std::string>();
    const std::string desc =
        j.contains("description") && j["description"].is_string()
            ? j["description"].get<std::string>()
            : std::string();
    const std::string op =
        j.contains("op") && j["op"].is_string() ? j["op"].get<std::string>()
                                                : "upsert";
    if (gid == 0 || name.empty()) {
      respond_json(400,
                   {{"ok", false}, {"error", "gid 须为正整数、name 非空"}});
      return;
    }
    if (!capability_gate(gid, "tools", "群工具")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(
          403, {{"ok", false}, {"error", "无权管理流水线（仅群主/管理员）"}});
      return;
    }
    if (op == "delete") {
      if (!impl_.store.ci_pipeline_delete(gid, name)) {
        respond_json(404, {{"ok", false}, {"error", "流水线不存在"}});
        return;
      }
      std::cout << "[MEMEX] files group-ci pipeline delete account="
                << account << " gid=" << gid << " name=" << name << std::endl;
      respond_json(200, {{"ok", true}, {"gid", gid}, {"name", name}});
      return;
    }
    if (!impl_.store.ci_pipeline_upsert(gid, name, desc, account, now_ms())) {
      respond_json(404, {{"ok", false}, {"error", "群不存在"}});
      return;
    }
    std::cout << "[MEMEX] files group-ci pipeline account=" << account
              << " gid=" << gid << " name=" << name << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"name", name}});
  }

  void route_group_ci_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!capability_gate(gid, "tools", "群工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
      return;
    }
    // 红绿灯：定义全集＋每流水线最近一笔 run 的状态（无 run=灰，未跑过）
    const auto runs = impl_.store.ci_status_list(gid);
    std::map<std::string, const ServerStore::CiRun*> last;
    for (const auto& r : runs) last[r.pipeline] = &r;
    json arr = json::array();
    for (const auto& p : impl_.store.ci_pipeline_list(gid)) {
      json item = {{"name", p.name},
                   {"description", p.description},
                   {"updated_by", p.updated_by},
                   {"updated_ms", p.updated_ms}};
      const auto it = last.find(p.name);
      if (it != last.end()) {
        item["last_status"] = it->second->status;
        item["last_actor"] = it->second->actor;
        item["last_ts_ms"] = it->second->ts_ms;
      }
      arr.push_back(item);
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"pipelines", arr}});
  }

  void route_group_ci_trigger(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("pipeline") ||
        !j["pipeline"].is_string() ||
        (j.contains("params") && !j["params"].is_object())) {
      respond_json(400,
                   {{"ok", false}, {"error", "缺少字段：gid/pipeline（params 可选须对象）"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string pipeline = j["pipeline"].get<std::string>();
    if (!capability_gate(gid, "tools", "群工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权触发（非群成员）"}});
      return;
    }
    // 白名单闸：工具「ci」须配置且动作「trigger」开放（R25-1 框架面）
    const auto cfg = impl_.store.tool_config(gid, "ci");
    bool opened = false;
    if (cfg.has_value()) {
      try {
        for (const auto& a : json::parse(cfg->actions_json)) {
          if (a.is_string() && a.get<std::string>() == "trigger") {
            opened = true;
            break;
          }
        }
      } catch (const std::exception&) {
      }
    }
    if (!opened) {
      respond_json(403, {{"ok", false},
                         {"error", "触发未开放（ci/trigger 不在工具白名单）"}});
      return;
    }
    bool exists = false;
    for (const auto& p : impl_.store.ci_pipeline_list(gid)) {
      if (p.name == pipeline) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      respond_json(404, {{"ok", false}, {"error", "流水线不存在"}});
      return;
    }
    const json params =
        j.contains("params") ? j["params"] : json::object();
    const bool fail =
        params.contains("fail") && params["fail"].is_boolean() &&
        params["fail"].get<bool>();
    const std::string status = fail ? "failed" : "success";
    const json result = {{"stub", true}, {"status", status}};
    const std::int64_t run_id = impl_.store.ci_run_add(
        gid, pipeline, account, status, params.dump(), result.dump(),
        now_ms());
    // 动作留痕进 R25-1 工具审计（与群日志同源）
    impl_.store.tool_audit_add(gid, "ci", "trigger", account, j.dump(),
                               result.dump(), now_ms());
    std::cout << "[MEMEX] files group-ci trigger account=" << account
              << " gid=" << gid << " pipeline=" << pipeline
              << " status=" << status << std::endl;
    // 结果卡片回群（回调未设＝只落账不回群；标题即红绿灯语义）
    if (impl_.notice) {
      impl_.notice("group:" + std::to_string(gid),
                   std::string(fail ? "构建失败：" : "构建成功：") + pipeline,
                   account + " 触发流水线「" + pipeline + "」，结果：" +
                       (fail ? "失败" : "成功") + "（stub 执行器）",
                   1);
    }
    respond_json(200, {{"ok", true},
                       {"gid", gid},
                       {"run_id", run_id},
                       {"pipeline", pipeline},
                       {"actor", account},
                       {"status", status}});
  }

  void route_group_ci_runs() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!capability_gate(gid, "tools", "群工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
      return;
    }
    const std::string pipeline = query_param(query_, "pipeline");
    json arr = json::array();
    for (const auto& r : impl_.store.ci_run_list(gid, pipeline)) {
      json params = json::object();
      json result = json::object();
      try {
        params = json::parse(r.params_json);
      } catch (const std::exception&) {
      }
      try {
        result = json::parse(r.result_json);
      } catch (const std::exception&) {
      }
      arr.push_back({{"id", r.id},
                     {"pipeline", r.pipeline},
                     {"actor", r.actor},
                     {"status", r.status},
                     {"params", params},
                     {"result", result},
                     {"ts_ms", r.ts_ms}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"runs", arr}});
  }

  // —— R25-3 打包＋配置导出（首批动作类示例）——
  // 打包：白名单闸动作（tool=pack 动作 build，成员可触发）；产物=台账
  // 记录（stub：真产物字节面归后续批次）；删除=memo:config。导出：管理
  // 面（memo:config——快照含成员与白名单），密文面永不进导出。
  void route_group_pack_build(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("name") ||
        !j["name"].is_string() || !j.contains("version") ||
        !j["version"].is_string() ||
        (j.contains("note") && !j["note"].is_string())) {
      respond_json(
          400, {{"ok", false}, {"error", "缺少字段：gid/name/version"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string name = j["name"].get<std::string>();
    const std::string version = j["version"].get<std::string>();
    const std::string note =
        j.contains("note") ? j["note"].get<std::string>() : std::string();
    if (!capability_gate(gid, "tools", "群工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权打包（非群成员）"}});
      return;
    }
    // 白名单闸：tool=pack 动作 build（与 CI 触发同构）
    const auto cfg = impl_.store.tool_config(gid, "pack");
    bool opened = false;
    if (cfg.has_value()) {
      try {
        for (const auto& a : json::parse(cfg->actions_json)) {
          if (a.is_string() && a.get<std::string>() == "build") {
            opened = true;
            break;
          }
        }
      } catch (const std::exception&) {
      }
    }
    if (!opened) {
      respond_json(403, {{"ok", false},
                         {"error", "打包未开放（pack/build 不在工具白名单）"}});
      return;
    }
    if (!impl_.store.pack_artifact_upsert(gid, name, version, note, account,
                                          now_ms())) {
      respond_json(404, {{"ok", false}, {"error", "群不存在"}});
      return;
    }
    const json result = {{"stub", true}, {"name", name},
                         {"version", version}};
    impl_.store.tool_audit_add(gid, "pack", "build", account, j.dump(),
                               result.dump(), now_ms());
    std::cout << "[MEMEX] files group-pack build account=" << account
              << " gid=" << gid << " name=" << name
              << " version=" << version << std::endl;
    // 打包完成卡片回群（同 CI 结果卡片面）
    if (impl_.notice) {
      impl_.notice("group:" + std::to_string(gid),
                   "打包完成：" + name + " " + version,
                   account + " 出产物「" + name + " " + version +
                       "」（stub 执行器）",
                   1);
    }
    respond_json(200, {{"ok", true},
                       {"gid", gid},
                       {"name", name},
                       {"version", version},
                       {"actor", account}});
  }

  void route_group_pack_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!capability_gate(gid, "tools", "群工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
      return;
    }
    json arr = json::array();
    for (const auto& a : impl_.store.pack_artifact_list(gid)) {
      arr.push_back({{"id", a.id},
                     {"name", a.name},
                     {"version", a.version},
                     {"note", a.note},
                     {"created_by", a.created_by},
                     {"created_ms", a.created_ms}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"artifacts", arr}});
  }

  void route_group_pack_delete(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("name") ||
        !j["name"].is_string() || !j.contains("version") ||
        !j["version"].is_string()) {
      respond_json(400,
                   {{"ok", false}, {"error", "缺少字段：gid/name/version"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    if (!capability_gate(gid, "tools", "群工具")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权删除产物（仅群主/管理员）"}});
      return;
    }
    if (!impl_.store.pack_artifact_delete(gid, j["name"].get<std::string>(),
                                          j["version"].get<std::string>())) {
      respond_json(404, {{"ok", false}, {"error", "产物不存在"}});
      return;
    }
    std::cout << "[MEMEX] files group-pack delete account=" << account
              << " gid=" << gid << " name=" << j["name"].get<std::string>()
              << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}});
  }

  void route_group_export() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    // 管理面（快照含成员与白名单；memo:config 命中 owner/admin）
    if (!capability_gate(gid, "tools", "群工具")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(
          403, {{"ok", false}, {"error", "无权导出（仅群主/管理员）"}});
      return;
    }
    const auto info = impl_.store.group_info(gid);
    if (!info.has_value()) {
      respond_json(404, {{"ok", false}, {"error", "群不存在"}});
      return;
    }
    json members = json::array();
    for (const auto& m : impl_.store.group_members(gid)) {
      members.push_back({{"account", m},
                         {"role", impl_.store.group_role(gid, m)}});
    }
    json tools = json::array();
    for (const auto& t : impl_.store.tool_list(gid)) {
      json actions = json::array();
      try {
        actions = json::parse(t.actions_json);
      } catch (const std::exception&) {
      }
      tools.push_back({{"tool", t.tool},
                       {"actions", actions},
                       {"updated_by", t.updated_by},
                       {"updated_ms", t.updated_ms}});
    }
    json pipelines = json::array();
    for (const auto& p : impl_.store.ci_pipeline_list(gid)) {
      pipelines.push_back({{"name", p.name},
                           {"description", p.description},
                           {"updated_by", p.updated_by}});
    }
    const auto vault = impl_.store.group_vault_info(gid);
    // 密文面永不进导出：密码箱只带存在性与授权名单（不带货）
    json vault_j = json::object();
    vault_j["exists"] = vault.has_value();
    if (vault.has_value()) {
      json acl = json::array();
      for (const auto& a : impl_.store.vault_acl_list(gid)) acl.push_back(a);
      vault_j["acl"] = acl;
    }
    const json snapshot = {{"group",
                            {{"gid", gid},
                             {"name", info->name},
                             {"owner", info->owner},
                             {"open_edit",
                              impl_.store.group_memo_open_edit(gid)}}},
                           {"members", members},
                           {"tools", tools},
                           {"pipelines", pipelines},
                           {"vault", vault_j}};
    // 导出=敏感动作：进工具留痕（谁/何时导出了什么）
    impl_.store.tool_audit_add(gid, "export", "export", account,
                               json::object().dump(),
                               json{{"stub", true}}.dump(), now_ms());
    std::cout << "[MEMEX] files group-export account=" << account
              << " gid=" << gid << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"snapshot", snapshot}});
  }

  // —— R25-4 凭据面：工具外部凭据只存服务端密文（gcm_seal hex 落库，
  // 行内永不见明文）。管理面（memo:config＝owner/admin）；主密钥未配＝
  // 503 明示「未启用」而非静默存明文；回包只带掩码元数据。
  void route_group_tools_credential(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("tool") ||
        !j["tool"].is_string()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/tool"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string tool = j["tool"].get<std::string>();
    if (gid == 0 || tool.empty()) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数、tool 非空"}});
      return;
    }
    const bool is_delete = j.contains("op") && j["op"].is_string() &&
                           j["op"].get<std::string>() == "delete";
    std::string value;
    if (!is_delete) {
      if (!j.contains("value") || !j["value"].is_string() ||
          j["value"].get<std::string>().empty()) {
        respond_json(
            400, {{"ok", false},
                  {"error", "缺少字段：value（非空字符串；删除传 op=\"delete\"）"}});
        return;
      }
      value = j["value"].get<std::string>();
    }
    if (!capability_gate(gid, "tools", "群工具")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权管理凭据（仅群主/管理员）"}});
      return;
    }
    if (impl_.tool_cred_key.empty()) {
      respond_json(503, {{"ok", false},
                         {"error", "凭据面未启用（服务端未配置 --tool-cred-secret）"}});
      return;
    }
    if (is_delete) {
      if (!impl_.store.tool_cred_delete(gid, tool)) {
        respond_json(404, {{"ok", false}, {"error", "凭据不存在"}});
        return;
      }
      std::cout << "[MEMEX] files group-tools credential delete account="
                << account << " gid=" << gid << " tool=" << tool << std::endl;
      respond_json(200, {{"ok", true},
                         {"gid", gid},
                         {"tool", tool},
                         {"deleted", true}});
      return;
    }
    const std::string sealed =
        gcm_seal(impl_.tool_cred_key, value);
    if (sealed.empty()) {
      respond_json(500, {{"ok", false}, {"error", "凭据加密失败"}});
      return;
    }
    if (!impl_.store.tool_cred_set(gid, tool, sealed, account, now_ms())) {
      respond_json(404, {{"ok", false}, {"error", "群不存在"}});
      return;
    }
    // 留痕不含 value（明文/密文都不进日志）
    std::cout << "[MEMEX] files group-tools credential set account=" << account
              << " gid=" << gid << " tool=" << tool
              << " sealed_bytes=" << sealed.size() / 2 << std::endl;
    respond_json(200, {{"ok", true},
                       {"gid", gid},
                       {"tool", tool},
                       {"updated", true}});
  }

  // 掩码元数据列表（工具/谁/何时更新——永不含 sealed_hex，更不含明文）
  void route_group_tools_credentials() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!capability_gate(gid, "tools", "群工具")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权查看凭据（仅群主/管理员）"}});
      return;
    }
    if (impl_.tool_cred_key.empty()) {
      respond_json(503, {{"ok", false},
                         {"error", "凭据面未启用（服务端未配置 --tool-cred-secret）"}});
      return;
    }
    json arr = json::array();
    for (const auto& m : impl_.store.tool_cred_list(gid)) {
      arr.push_back({{"tool", m.tool},
                     {"updated_by", m.updated_by},
                     {"updated_ms", m.updated_ms}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"credentials", arr}});
  }

  // —— 二期群工具三件（原生互动，不走 R25 外部工具代理）——

  // 身份约束（设计稿 §1，不新造 az 动词、服务端逻辑判）：关票/关接龙=
  // 发起人或群主/管理员（同 R24-1 公告判权口径）
  bool group_manage_allowed(std::uint64_t gid, const std::string& account,
                            const std::string& initiator) {
    if (account == initiator) return true;
    const auto info = impl_.store.group_info(gid);
    if (!info.has_value()) return false;
    if (info->owner == account) return true;
    return impl_.store.group_role(gid, account) == "admin";
  }

  void route_group_poll_create(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("topic") ||
        !j["topic"].is_string() || !j.contains("options") ||
        !j["options"].is_array() ||
        (j.contains("deadline_ms") &&
         !j["deadline_ms"].is_number_integer())) {
      respond_json(
          400, {{"ok", false},
                {"error", "缺少字段：gid/topic/options（deadline_ms 可选整数）"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string topic = j["topic"].get<std::string>();
    std::vector<std::string> options;
    for (const auto& o : j["options"]) {
      if (!o.is_string()) {
        respond_json(400, {{"ok", false}, {"error", "options 须为字符串数组"}});
        return;
      }
      options.push_back(o.get<std::string>());
    }
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
      return;
    }
    if (topic.empty() || options.size() < 2 || options.size() > 10) {
      respond_json(400,
                   {{"ok", false},
                    {"error", "topic 不可空，选项须 2~10 个"}});
      return;
    }
    const std::int64_t deadline_ms =
        j.contains("deadline_ms") ? j["deadline_ms"].get<std::int64_t>() : 0;
    const bool anonymous =
        j.contains("anonymous") && j["anonymous"].is_boolean() &&
        j["anonymous"].get<bool>();
    const bool multi =
        j.contains("multi") && j["multi"].is_boolean() &&
        j["multi"].get<bool>();
    const std::int64_t id = impl_.store.poll_create(
        gid, topic, options, deadline_ms, account, now_ms(), anonymous,
        multi);
    if (id == 0) {
      respond_json(404, {{"ok", false}, {"error", "群不存在"}});
      return;
    }
    std::cout << "[MEMEX] files group-poll create account=" << account
              << " gid=" << gid << " poll=" << id << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"poll_id", id}});
  }

  void route_group_polls_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
      return;
    }
    json arr = json::array();
    for (const auto& p : impl_.store.polls_list(gid)) {
      json counts = std::vector<int>(p.options.size(), 0);
      json votes = json::array();
      const int n = static_cast<int>(p.options.size());
      for (const auto& v : impl_.store.poll_votes(p.id)) {
        if (p.multi) {
          // 多选：choice=位集（bit i=选 i+1 号），按位展开累加
          if (v.choice >= 1 && v.choice < (1 << n)) {
            for (int i = 0; i < n; ++i) {
              if ((v.choice >> i) & 1) {
                counts[i] = counts[i].get<int>() + 1;
              }
            }
          }
        } else if (v.choice >= 1 && v.choice <= n) {
          counts[v.choice - 1] = counts[v.choice - 1].get<int>() + 1;
        }
        // 匿名票：台账不回带 voter 身份（counts 照常——库内留 account
        // 供改票 upsert 与审计，展示匿名口径见设计稿）
        if (!p.anonymous) {
          votes.push_back({{"account", v.account},
                           {"choice", v.choice},
                           {"ts_ms", v.ts_ms}});
        }
      }
      arr.push_back({{"id", p.id},
                     {"topic", p.topic},
                     {"options", p.options},
                     {"deadline_ms", p.deadline_ms},
                     {"closed", p.closed},
                     {"anonymous", p.anonymous},
                     {"multi", p.multi},
                     {"status", p.closed || (p.deadline_ms > 0 &&
                                             now_ms() >= p.deadline_ms)
                                  ? "closed"
                                  : "open"},  // 到点现算（惰性判定）
                     {"created_by", p.created_by},
                     {"created_ms", p.created_ms},
                     {"counts", counts},
                     {"votes", votes}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"polls", arr}});
  }

  void route_group_poll_vote(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("poll_id") ||
        !j["poll_id"].is_number_integer() || !j.contains("choice") ||
        !j["choice"].is_number_integer()) {
      respond_json(400,
                   {{"ok", false}, {"error", "缺少字段：gid/poll_id/choice"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::int64_t poll_id = j["poll_id"].get<std::int64_t>();
    const int choice = j["choice"].get<int>();
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
      return;
    }
    const auto p = impl_.store.poll_by_id(poll_id);
    if (!p.has_value() || p->group_id != gid) {
      respond_json(404, {{"ok", false}, {"error", "投票不存在"}});
      return;
    }
    const int n = static_cast<int>(p->options.size());
    if (p->multi) {
      // 多选：choice=位集（bit i=选 i+1 号），非零且每位都在界内
      if (choice < 1 || choice >= (1 << n)) {
        respond_json(400,
                     {{"ok", false},
                      {"error", "位集越界（每位须为 1~" +
                                    std::to_string(n) + " 号选项）"}});
        return;
      }
    } else if (choice < 1 || choice > n) {
      respond_json(400, {{"ok", false},
                         {"error", "选项越界（1~" + std::to_string(n) +
                                       "）"}});
      return;
    }
    // 截止自动关票（惰性判定，不回写 closed——closed 库值保持「手动关票」
    // 语义；到点后行为与关票完全一致，手动 close 仍可补写留痕）
    const std::int64_t now = now_ms();
    if (p->closed ||
        (p->deadline_ms > 0 && now >= p->deadline_ms)) {
      respond_json(409,
                   {{"ok", false},
                    {"error", p->closed ? "投票已截止" : "投票已到截止时间"}});
      return;
    }
    if (!impl_.store.poll_vote(poll_id, account, choice, now)) {
      respond_json(409, {{"ok", false}, {"error", "投票失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-poll vote account=" << account
              << " gid=" << gid << " poll=" << poll_id
              << " choice=" << choice << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"poll_id", poll_id}});
  }

  void route_group_poll_close(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("poll_id") ||
        !j["poll_id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/poll_id"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::int64_t poll_id = j["poll_id"].get<std::int64_t>();
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
      return;
    }
    const auto p = impl_.store.poll_by_id(poll_id);
    if (!p.has_value() || p->group_id != gid) {
      respond_json(404, {{"ok", false}, {"error", "投票不存在"}});
      return;
    }
    if (p->closed) {
      respond_json(409, {{"ok", false}, {"error", "投票已截止"}});
      return;
    }
    if (!group_manage_allowed(gid, account, p->created_by)) {
      respond_json(
          403, {{"ok", false}, {"error", "只有发起人或群主/管理员可截止"}});
      return;
    }
    impl_.store.poll_close(poll_id);
    std::cout << "[MEMEX] files group-poll close account=" << account
              << " gid=" << gid << " poll=" << poll_id << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"poll_id", poll_id}});
  }

  void route_group_chain_create(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("title") ||
        !j["title"].is_string() ||
        (j.contains("format_hint") && !j["format_hint"].is_string())) {
      respond_json(
          400, {{"ok", false},
                {"error", "缺少字段：gid/title（format_hint 可选）"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string title = j["title"].get<std::string>();
    const std::string format_hint =
        j.contains("format_hint") ? j["format_hint"].get<std::string>() : "";
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
      return;
    }
    if (title.empty()) {
      respond_json(400, {{"ok", false}, {"error", "title 不可空"}});
      return;
    }
    const std::int64_t id = impl_.store.chain_create(gid, title, format_hint,
                                                     account, now_ms());
    if (id == 0) {
      respond_json(404, {{"ok", false}, {"error", "群不存在"}});
      return;
    }
    std::cout << "[MEMEX] files group-chain create account=" << account
              << " gid=" << gid << " chain=" << id << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"chain_id", id}});
  }

  void route_group_chains_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
      return;
    }
    json arr = json::array();
    for (const auto& c : impl_.store.chains_list(gid)) {
      json entries = json::array();
      for (const auto& e : impl_.store.chain_entries(c.id)) {
        entries.push_back({{"account", e.account},
                           {"content", e.content},
                           {"ts_ms", e.ts_ms}});
      }
      arr.push_back({{"id", c.id},
                     {"title", c.title},
                     {"format_hint", c.format_hint},
                     {"closed", c.closed},
                     {"created_by", c.created_by},
                     {"created_ms", c.created_ms},
                     {"entries", entries}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"chains", arr}});
  }

  void route_group_chain_join(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("chain_id") ||
        !j["chain_id"].is_number_integer() || !j.contains("content") ||
        !j["content"].is_string()) {
      respond_json(400,
                   {{"ok", false}, {"error", "缺少字段：gid/chain_id/content"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::int64_t chain_id = j["chain_id"].get<std::int64_t>();
    const std::string content = j["content"].get<std::string>();
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
      return;
    }
    const auto c = impl_.store.chain_by_id(chain_id);
    if (!c.has_value() || c->group_id != gid) {
      respond_json(404, {{"ok", false}, {"error", "接龙不存在"}});
      return;
    }
    if (c->closed) {
      respond_json(409, {{"ok", false}, {"error", "接龙已截止"}});
      return;
    }
    if (content.empty()) {
      respond_json(400, {{"ok", false}, {"error", "content 不可空"}});
      return;
    }
    if (!impl_.store.chain_join(chain_id, account, content, now_ms())) {
      respond_json(409, {{"ok", false}, {"error", "接龙失败"}});
      return;
    }
    std::cout << "[MEMEX] files group-chain join account=" << account
              << " gid=" << gid << " chain=" << chain_id << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"chain_id", chain_id}});
  }

  void route_group_chain_close(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("chain_id") ||
        !j["chain_id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/chain_id"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::int64_t chain_id = j["chain_id"].get<std::int64_t>();
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
      return;
    }
    const auto c = impl_.store.chain_by_id(chain_id);
    if (!c.has_value() || c->group_id != gid) {
      respond_json(404, {{"ok", false}, {"error", "接龙不存在"}});
      return;
    }
    if (c->closed) {
      respond_json(409, {{"ok", false}, {"error", "接龙已截止"}});
      return;
    }
    if (!group_manage_allowed(gid, account, c->created_by)) {
      respond_json(
          403, {{"ok", false}, {"error", "只有发起人或群主/管理员可截止"}});
      return;
    }
    impl_.store.chain_close(chain_id);
    std::cout << "[MEMEX] files group-chain close account=" << account
              << " gid=" << gid << " chain=" << chain_id << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"chain_id", chain_id}});
  }

  void route_group_task_create(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("title") ||
        !j["title"].is_string() ||
        (j.contains("assignee") && !j["assignee"].is_string()) ||
        (j.contains("due_ms") && !j["due_ms"].is_number_integer())) {
      respond_json(400,
                   {{"ok", false},
                    {"error", "缺少字段：gid/title（assignee/due_ms 可选）"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string title = j["title"].get<std::string>();
    const std::string assignee =
        j.contains("assignee") ? j["assignee"].get<std::string>() : "";
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
      return;
    }
    if (title.empty()) {
      respond_json(400, {{"ok", false}, {"error", "title 不可空"}});
      return;
    }
    const std::int64_t due_ms =
        j.contains("due_ms") ? j["due_ms"].get<std::int64_t>() : 0;
    const std::int64_t id =
        impl_.store.gtask_create(gid, title, assignee, due_ms, account,
                                 now_ms());
    if (id == 0) {
      // 指定负责人而非群成员=幽灵拒（404，不透存在性细节）
      respond_json(404, {{"ok", false}, {"error", "群或负责人不存在"}});
      return;
    }
    std::cout << "[MEMEX] files group-task create account=" << account
              << " gid=" << gid << " task=" << id
              << " assignee=" << assignee << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"task_id", id}});
  }

  void route_group_tasks_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
      return;
    }
    json arr = json::array();
    for (const auto& t : impl_.store.gtasks_list(gid)) {
      arr.push_back({{"id", t.id},
                     {"title", t.title},
                     {"assignee", t.assignee},
                     {"due_ms", t.due_ms},
                     {"claimed_ms", t.claimed_ms},
                     {"status", t.status},
                     {"created_by", t.created_by},
                     {"created_ms", t.created_ms},
                     {"done_by", t.done_by},
                     {"done_ms", t.done_ms}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"tasks", arr}});
  }

  void route_group_task_claim(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("task_id") ||
        !j["task_id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/task_id"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::int64_t task_id = j["task_id"].get<std::int64_t>();
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
      return;
    }
    // 认领原子落（store 层 WHERE status='todo' AND assignee=''）；此处
    // 只区分 404 幽灵与 409 已占
    bool exists = false;
    for (const auto& t : impl_.store.gtasks_list(gid)) {
      if (t.id == task_id) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      respond_json(404, {{"ok", false}, {"error", "任务不存在"}});
      return;
    }
    if (!impl_.store.gtask_claim(task_id, account, now_ms())) {
      respond_json(409, {{"ok", false}, {"error", "任务已被认领或已完成"}});
      return;
    }
    std::cout << "[MEMEX] files group-task claim account=" << account
              << " gid=" << gid << " task=" << task_id << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"task_id", task_id}});
  }

  void route_group_task_done(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("task_id") ||
        !j["task_id"].is_number_integer()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/task_id"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::int64_t task_id = j["task_id"].get<std::int64_t>();
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
      return;
    }
    const ServerStore::GroupTask* found = nullptr;
    ServerStore::GroupTask row;
    for (const auto& t : impl_.store.gtasks_list(gid)) {
      if (t.id == task_id) {
        row = t;
        found = &row;
        break;
      }
    }
    if (!found) {
      respond_json(404, {{"ok", false}, {"error", "任务不存在"}});
      return;
    }
    if (found->status != "todo") {
      respond_json(409, {{"ok", false}, {"error", "任务已完成"}});
      return;
    }
    // 完成=负责人/创建者/群主管理员（服务端逻辑判）
    if (account != found->assignee && account != found->created_by &&
        !group_manage_allowed(gid, account, found->created_by)) {
      respond_json(403,
                   {{"ok", false},
                    {"error", "只有负责人/创建者/群主/管理员可完成"}});
      return;
    }
    if (!impl_.store.gtask_done(task_id, account, now_ms())) {
      respond_json(409, {{"ok", false}, {"error", "任务已完成"}});
      return;
    }
    std::cout << "[MEMEX] files group-task done account=" << account
              << " gid=" << gid << " task=" << task_id << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"task_id", task_id}});
  }

  // —— R26-1 服务器 agent 面 ——

  // 登记：管理面（memo:config）。生成注册令牌（明文只此一次出现在回包，
  // 库里只存 SHA-256 摘要）；同 gid+name 重登记=轮换令牌、id 稳定。
  void route_group_servers_enroll(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("name") ||
        !j["name"].is_string() || !j.contains("host") ||
        !j["host"].is_string()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：gid/name/host"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::string name = j["name"].get<std::string>();
    const std::string host = j["host"].get<std::string>();
    if (gid == 0 || name.empty() || host.empty()) {
      respond_json(400,
                   {{"ok", false},
                    {"error", "gid 须为正整数、name/host 非空"}});
      return;
    }
    if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(
          403, {{"ok", false}, {"error", "无权登记服务器（仅群主/管理员）"}});
      return;
    }
    const std::string token = random_salt_hex(); // 16B 熵 → 32 hex
    const std::uint64_t id =
        impl_.store.server_enroll(gid, name, host, sha256_hex(token), account,
                                  now_ms());
    if (id == 0) {
      respond_json(404, {{"ok", false}, {"error", "群不存在"}});
      return;
    }
    // 令牌不进日志（只进回包一次）
    std::cout << "[MEMEX] files group-servers enroll account=" << account
              << " gid=" << gid << " id=" << id << " name=" << name
              << " host=" << host << std::endl;
    respond_json(200, {{"ok", true},
                       {"gid", gid},
                       {"id", id},
                       {"name", name},
                       {"token", token}});
  }

  // 心跳：agent 令牌鉴权（非人会话——不走 Authorization 头）。指标整拍
  // 覆盖写；令牌错/轮换掉的旧令牌一律 401。
  void route_group_servers_heartbeat(const std::string& body) {
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("token") || !j["token"].is_string() ||
        j["token"].get<std::string>().empty()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：token"}});
      return;
    }
    for (const char* k :
         {"cpu_percent", "mem_used_mb", "mem_total_mb", "disk_used_mb",
          "disk_total_mb", "load1"}) {
      if (!j.contains(k) || !j[k].is_number()) {
        respond_json(400,
                     {{"ok", false},
                      {"error", std::string("缺少数值字段：") + k}});
        return;
      }
    }
    const auto srv = impl_.store.server_by_token_hash(
        sha256_hex(j["token"].get<std::string>()));
    if (!srv.has_value()) {
      respond_json(401, {{"ok", false}, {"error", "注册令牌无效"}});
      return;
    }
    if (!capability_gate(srv->group_id, "server_tools", "群服务器工具")) return;
    impl_.store.server_heartbeat(
        srv->id, j["cpu_percent"].get<double>(),
        j["mem_used_mb"].get<double>(), j["mem_total_mb"].get<double>(),
        j["disk_used_mb"].get<double>(), j["disk_total_mb"].get<double>(),
        j["load1"].get<double>(), now_ms());
    respond_json(200, {{"ok", true},
                       {"id", srv->id},
                       {"gid", srv->group_id},
                       {"interval_s", kAgentOnlineMs / 3000}});
  }

  // 列表：群成员（入群即授权）。带最近一拍指标＋在线红绿灯（last_seen
  // 新鲜度）；令牌摘要永不出现。
  void route_group_servers_list() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
      return;
    }
    const std::int64_t now = now_ms();
    json arr = json::array();
    for (const auto& r : impl_.store.server_list(gid)) {
      // cred 掩码元数据：只言「是否已配置/谁/何时」——密文与明文永不出门
      arr.push_back(
          {{"id", r.id},
           {"name", r.name},
           {"host", r.host},
           {"enrolled_by", r.enrolled_by},
           {"created_ms", r.created_ms},
           {"last_seen_ms", r.last_seen_ms},
           {"online", now - r.last_seen_ms < kAgentOnlineMs},
           {"cpu_percent", r.cpu_percent},
           {"mem_used_mb", r.mem_used_mb},
           {"mem_total_mb", r.mem_total_mb},
           {"disk_used_mb", r.disk_used_mb},
           {"disk_total_mb", r.disk_total_mb},
           {"load1", r.load1},
           {"cred", !r.cred_updated_by.empty()},
           {"cred_updated_by", r.cred_updated_by},
           {"cred_updated_ms", r.cred_updated_ms}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"servers", arr}});
  }

  // —— R26-3 远程会话（SSH 起步；memex 只做「看+连」，操作类归
  // croupier）——

  // 签发一次性短票（群成员；server_id 须属该群）。留痕即签发：行内记
  // 谁/何时/连哪台/协议；短票明文只此一次出门，库里只存摘要。
  void route_group_servers_session_request(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("server_id") ||
        !j["server_id"].is_number_integer()) {
      respond_json(400,
                   {{"ok", false}, {"error", "缺少字段：gid/server_id"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::uint64_t sid =
        static_cast<std::uint64_t>(j["server_id"].get<std::int64_t>());
    const std::string protocol =
        j.contains("protocol") && j["protocol"].is_string()
            ? j["protocol"].get<std::string>()
            : "ssh";
    if (gid == 0 || sid == 0) {
      respond_json(400,
                   {{"ok", false},
                    {"error", "gid/server_id 须为正整数"}});
      return;
    }
    if (protocol != "ssh") {
      // RDP/VNC 随后（走同一短票/留痕面，到批再开）
      respond_json(400,
                   {{"ok", false},
                    {"error", "协议暂只支持 ssh（RDP/VNC 随后）"}});
      return;
    }
    if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权发起会话（非群成员）"}});
      return;
    }
    const std::string ticket = random_salt_hex();
    const std::uint64_t id = impl_.store.server_session_open(
        gid, sid, account, protocol, sha256_hex(ticket), now_ms());
    if (id == 0) {
      respond_json(404, {{"ok", false}, {"error", "服务器不存在"}});
      return;
    }
    // 短票不进日志
    std::cout << "[MEMEX] files group-servers session request account="
              << account << " gid=" << gid << " server=" << sid
              << " session=" << id << " protocol=" << protocol << std::endl;
    respond_json(200, {{"ok", true},
                       {"gid", gid},
                       {"session_id", id},
                       {"protocol", protocol},
                       {"ticket", ticket},
                       {"expires_ms", now_ms() + kSessionTicketTtlMs}});
  }

  // 短票兑现（一次性；短票即凭据——无会话头）。客户端拉起本地 ssh 前
  // 即时兑现；过期/错票 401、重放 409。
  void route_group_servers_session_redeem(const std::string& body) {
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("ticket") || !j["ticket"].is_string() ||
        j["ticket"].get<std::string>().empty()) {
      respond_json(400, {{"ok", false}, {"error", "缺少字段：ticket"}});
      return;
    }
    const auto sess = impl_.store.server_session_by_ticket(
        sha256_hex(j["ticket"].get<std::string>()));
    if (!sess.has_value() ||
        now_ms() - sess->opened_ms > kSessionTicketTtlMs) {
      respond_json(401, {{"ok", false}, {"error", "短票无效或已过期"}});
      return;
    }
    if (!capability_gate(sess->group_id, "server_tools", "群服务器工具")) return;
    if (!impl_.store.server_session_mark_redeemed(sess->id, now_ms())) {
      respond_json(409, {{"ok", false}, {"error", "短票已使用"}});
      return;
    }
    std::cout << "[MEMEX] files group-servers session redeem session="
              << sess->id << " account=" << sess->actor << std::endl;
    respond_json(200, {{"ok", true},
                       {"session_id", sess->id},
                       {"gid", sess->group_id},
                       {"server_name", sess->server_name},
                       {"host", sess->host},
                       {"protocol", sess->protocol}});
  }

  // 收尾（本人、进行中才可）：closed_ms 落点＝时长可算（路由层算，
  // 行内存原始两拍）
  void route_group_servers_session_close(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("session_id") ||
        !j["session_id"].is_number_integer()) {
      respond_json(400,
                   {{"ok", false}, {"error", "缺少字段：gid/session_id"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::uint64_t id =
        static_cast<std::uint64_t>(j["session_id"].get<std::int64_t>());
    if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权操作（非群成员）"}});
      return;
    }
    if (!impl_.store.server_session_close(gid, id, account, now_ms())) {
      respond_json(409,
                   {{"ok", false},
                    {"error", "会话不存在/非本人/已结束"}});
      return;
    }
    std::cout << "[MEMEX] files group-servers session close session=" << id
              << " account=" << account << std::endl;
    respond_json(200, {{"ok", true}, {"gid", gid}, {"session_id", id}});
  }

  // 接入留痕列表（群成员）：谁/何时/连哪台/协议/是否兑现/时长；短票
  // 摘要永不出现
  void route_group_servers_sessions() {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    std::uint64_t gid = 0;
    if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
      respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
      return;
    }
    if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
    if (!tool_call_allowed(account, gid)) {
      respond_json(403, {{"ok", false}, {"error", "无权查看（非群成员）"}});
      return;
    }
    json arr = json::array();
    for (const auto& s : impl_.store.server_session_list(gid)) {
      arr.push_back(
          {{"id", s.id},
           {"server_id", s.server_id},
           {"server_name", s.server_name},
           {"host", s.host},
           {"actor", s.actor},
           {"protocol", s.protocol},
           {"opened_ms", s.opened_ms},
           {"redeemed", s.redeemed_ms > 0},
           {"open", s.closed_ms == 0},
           {"duration_ms",
            s.closed_ms > 0 ? s.closed_ms - s.opened_ms : 0}});
    }
    respond_json(200, {{"ok", true}, {"gid", gid}, {"sessions", arr}});
  }

  // —— R26-4 服务器凭据（管理面）：上/覆盖/删。密文落库，回包只见
  // updated 旗标——value 明文与 sealed_hex 密文都只此一次进服务端 ——
  void route_group_servers_credential(const std::string& body) {
    const std::string account = account_or_respond();
    if (account.empty()) return;
    json j;
    try {
      j = json::parse(body);
    } catch (const std::exception&) {
      respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
      return;
    }
    if (!j.is_object() || !j.contains("gid") ||
        !j["gid"].is_number_integer() || !j.contains("server_id") ||
        !j["server_id"].is_number_integer()) {
      respond_json(400,
                   {{"ok", false}, {"error", "缺少字段：gid/server_id"}});
      return;
    }
    const std::uint64_t gid =
        static_cast<std::uint64_t>(j["gid"].get<std::int64_t>());
    const std::uint64_t sid =
        static_cast<std::uint64_t>(j["server_id"].get<std::int64_t>());
    const bool is_delete = j.contains("op") && j["op"].is_string() &&
                           j["op"].get<std::string>() == "delete";
    std::string value;
    if (!is_delete) {
      if (!j.contains("value") || !j["value"].is_string() ||
          j["value"].get<std::string>().empty()) {
        respond_json(
            400, {{"ok", false},
                  {"error", "缺少字段：value（非空字符串；删除传 op=\"delete\"）"}});
        return;
      }
      value = j["value"].get<std::string>();
    }
    if (!capability_gate(gid, "server_tools", "群服务器工具")) return;
    const Decision d = impl_.az.authorize(
        {account, "memo:config", group_resource(gid), "owner=" + account});
    if (!d.allowed) {
      respond_json(403,
                   {{"ok", false}, {"error", "无权管理服务器凭据（仅群主/管理员）"}});
      return;
    }
    if (impl_.tool_cred_key.empty()) {
      respond_json(503, {{"ok", false},
                         {"error", "凭据面未启用（服务端未配置 --tool-cred-secret）"}});
      return;
    }
    if (is_delete) {
      if (!impl_.store.server_cred_delete(gid, sid)) {
        respond_json(404, {{"ok", false}, {"error", "凭据不存在或服务器不属该群"}});
        return;
      }
      std::cout << "[MEMEX] files group-servers credential delete account="
                << account << " gid=" << gid << " server=" << sid << std::endl;
      respond_json(200,
                   {{"ok", true}, {"gid", gid}, {"server_id", sid}, {"deleted", true}});
      return;
    }
    const std::string sealed = gcm_seal(impl_.tool_cred_key, value);
    if (sealed.empty()) {
      respond_json(500, {{"ok", false}, {"error", "凭据加密失败"}});
      return;
    }
    if (!impl_.store.server_cred_set(gid, sid, sealed, account, now_ms())) {
      respond_json(404, {{"ok", false}, {"error", "服务器不存在或不属该群"}});
      return;
    }
    // 留痕不含 value（明文/密文都不进日志）
    std::cout << "[MEMEX] files group-servers credential set account="
              << account << " gid=" << gid << " server=" << sid
              << " sealed_bytes=" << sealed.size() / 2 << std::endl;
    respond_json(200,
                 {{"ok", true}, {"gid", gid}, {"server_id", sid}, {"updated", true}});
  }

  void respond_json(int status, const json& body) {
    const std::string payload = body.dump();
    std::ostringstream head;
    head << "HTTP/1.1 " << status << ' ' << reason_phrase(status) << "\r\n"
         << "Content-Type: application/json; charset=utf-8\r\n"
         << "Content-Length: " << payload.size() << "\r\n"
         << "Connection: close\r\n\r\n"
         << payload;
    write_raw(std::make_shared<const std::string>(head.str()), true,
              [](std::error_code) {});
  }

  void write_raw(std::shared_ptr<const std::string> bytes,
                 bool close_when_done,
                 std::function<void(std::error_code)> done) {
    auto self = shared_from_this();
    asio::async_write(sock_, asio::buffer(*bytes),
                      [self, bytes, close_when_done,
                       done](std::error_code ec, std::size_t) {
                        if (close_when_done) {
                          std::error_code ignore;
                          self->sock_.shutdown(tcp::socket::shutdown_both,
                                               ignore);
                          self->sock_.close(ignore);
                        }
                        done(ec);
                      });
  }

  tcp::socket sock_;
  FileServer::Impl& impl_;
  std::array<char, 4096> chunk_{};
  std::string buf_;
  bool head_parsed_{false};
  bool expect_continue_{false};
  bool seen_content_length_{false};
  std::size_t body_cap_{kMaxJsonBody};
  std::string method_;
  std::string path_;
  std::string query_;
  std::string authorization_;
  std::string file_name_;
  std::size_t content_length_{0};
};

} // namespace

FileServer::FileServer(asio::io_context& io, ServerStore& store,
                       std::shared_ptr<S3Storage> storage, std::uint16_t port,
                       bool uplink_mode,
                       std::shared_ptr<FileSessions> sessions,
                       UplinkPolicy uplink_policy)
    : impl_(std::make_unique<Impl>(store, std::move(storage), uplink_mode,
                                   std::move(sessions),
                                   std::move(uplink_policy))),
      acceptor_(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {}

FileServer::~FileServer() {
  // 先落旗标再随成员析构关闭 acceptor：挂起的 accept 完成回调见到
  // false 即短路（回调持有旗标的 shared_ptr，生命周期独立于实例）
  alive_->store(false);
}

std::uint16_t FileServer::port() const {
  return acceptor_.local_endpoint().port();
}

void FileServer::set_notice(GroupNoticeFn fn) { impl_->notice = std::move(fn); }

void FileServer::set_tool_cred_secret(const std::string& secret) {
  // 派生空（secret 空）＝凭据面未启用；只存派生密钥，明文主密钥不驻留
  impl_->tool_cred_key = derive_tool_cred_key(secret);
}

void FileServer::start_accept() {
  if (impl_->uplink_mode) {
    // 安全默认（用户硬要求）：外网入口默认关闭、显式开启、开启时明示
    // 暴露范围——审计面日志固定带这行
    std::cout << "[MEMEX] uplink 监听 0.0.0.0:" << port()
              << "（外网单向：仅 /uplink/session|upload|mine|delete 与"
              << " /uplink/health；无任何下载/读取内网数据端点；上传全审计）"
              << (impl_->storage ? "" : "（存储未配置：一律 503）")
              << std::endl;
  } else {
    std::cout << "[MEMEX] Files 监听 0.0.0.0:" << port()
              << (impl_->storage ? "" : "（存储未配置：一律 503）")
              << std::endl;
  }
  do_accept();
}

void FileServer::do_accept() {
  // 旗标按值随回调持有：实例析构（旗标置 false）后，acceptor 随成员析构
  // 关闭并把挂起的 accept 以 operation_aborted 完成——回调在旗标上短路，
  // 不再触达已析构的 this；错误态一律停链（closed acceptor 续链＝空转）
  const auto alive = alive_;
  acceptor_.async_accept(
      [this, alive](std::error_code ec, asio::ip::tcp::socket socket) {
        if (ec || !alive->load()) {
          if (ec && alive->load()) {
            std::cerr << "[MEMEX] Files accept 停链：" << ec.message()
                      << std::endl;
          }
          return;
        }
        std::make_shared<FileConn>(std::move(socket), *impl_)->start();
        do_accept();
      });
}

} // namespace memex::server
