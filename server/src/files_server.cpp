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

#include "cred.hpp"

namespace memex::server {

namespace {

using json = nlohmann::json;
using tcp = asio::ip::tcp;

constexpr std::size_t kMaxHead = 8 * 1024;      // 请求头上限（防呆）
constexpr std::size_t kMaxJsonBody = 64 * 1024; // session/manage JSON 上限
constexpr std::size_t kMaxUpload = 512u * 1024 * 1024; // 单文件上限 512MiB
constexpr std::int64_t kSessionTtlMs = 12 * 3600 * 1000; // 令牌 12h

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
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
    sessions->map[sha256_hex(token)] =
        FileSession{account, now_ms() + kSessionTtlMs, uplink};
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
    // 与消息面同源口令校验（store 摘要 + PBKDF2）；token 只存哈希
    const auto row = impl_.store.find_account(account);
    std::string token;
    if (row.has_value()) {
      // 摘要比对用常量时间思路不引入（内网面 v1 与消息面同口径）
      token = pbkdf2_sha256_hex(password, row->salt_hex, 60000) ==
                      row->digest_hex
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
    for (const auto& m : rows) {
      arr.push_back({{"id", m.id},
                     {"owner", m.owner},
                     {"file_name", m.file_name},
                     {"file_size", m.file_size},
                     {"file_hash", m.file_hash},
                     {"pin", m.pin},
                     {"status", static_cast<int>(m.status)},
                     {"upload_ts", m.upload_ts}});
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

FileServer::~FileServer() = default;

std::uint16_t FileServer::port() const {
  return acceptor_.local_endpoint().port();
}

void FileServer::set_notice(GroupNoticeFn fn) { impl_->notice = std::move(fn); }

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
  acceptor_.async_accept(
      [this](std::error_code ec, asio::ip::tcp::socket socket) {
        if (!ec) {
          std::make_shared<FileConn>(std::move(socket), *impl_)->start();
        }
        do_accept();
      });
}

} // namespace memex::server
