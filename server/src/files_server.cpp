#include "files_server.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
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
      return query.substr(pos + prefix.size(), end - pos - prefix.size());
    }
    ++pos;
  }
  return "";
}

} // namespace

struct FileServer::Impl {
  ServerStore& store;
  std::shared_ptr<S3Storage> storage;
  AuthorizationService az;
  // 会话令牌：sha256(token) → 会话。单 io_context 线程驱动（与消息面/
  // webhook 同一线程模型），不加锁。
  std::unordered_map<std::string, FileSession> sessions;

  explicit Impl(ServerStore& s, std::shared_ptr<S3Storage> st)
      : store(s), storage(std::move(st)) {
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
    // 维护；同群主的管理动作，唯不可改 owner 身份——那不归文件面）
    az.add_rule(RuleEffect::ExplicitAllow, "group-admin",
                [this](const AuthzQuery& q) {
                  const auto gid = resource_group_id(q.resource);
                  if (!gid) return false;
                  if (q.action == "file:upload") return false;
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
  std::string mint_session(const std::string& account) {
    prune_sessions();
    const std::string token = random_salt_hex(); // 16B 熵 → 32 hex
    if (token.empty()) return "";
    sessions[sha256_hex(token)] = FileSession{account, now_ms() + kSessionTtlMs};
    return token;
  }
  // 有效返回账号；无效/过期返回空串
  std::string auth_session(const std::string& header_value) {
    const std::string prefix = "Bearer ";
    if (header_value.rfind(prefix, 0) != 0) return "";
    const std::string token = header_value.substr(prefix.size());
    if (token.empty()) return "";
    prune_sessions();
    const auto it = sessions.find(sha256_hex(token));
    if (it == sessions.end()) return "";
    if (it->second.expires_ms < now_ms()) {
      sessions.erase(it);
      return "";
    }
    return it->second.account;
  }
  void prune_sessions() {
    const std::int64_t now = now_ms();
    for (auto it = sessions.begin(); it != sessions.end();) {
      if (it->second.expires_ms < now) {
        it = sessions.erase(it);
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
  };
  UploadOutcome handle_upload(const std::string& account, bool is_group,
                              std::uint64_t gid, const std::string& uid,
                              const std::string& file_name,
                              const std::string& body, bool is_inbox) {
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
    meta.upload_ts = now_ms();
    meta.kind = kind;
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
    // 路由感知上限：upload 收大字节，其余路由只收小 JSON/空 body
    body_cap_ = path_ == "/files/upload" ? kMaxUpload : kMaxJsonBody;
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
    respond_json(404, {{"ok", false}, {"error", "路径不存在"}});
  }

  void route_session(const std::string& body) {
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
    // 与消息面同源口令校验（store 摘要 + PBKDF2）；token 只存哈希
    const auto row = impl_.store.find_account(account);
    std::string token;
    if (row.has_value()) {
      // 摘要比对用常量时间思路不引入（内网面 v1 与消息面同口径）
      token = pbkdf2_sha256_hex(password, row->salt_hex, 60000) ==
                      row->digest_hex
                  ? impl_.mint_session(account)
                  : "";
    }
    if (token.empty()) {
      respond_json(401, {{"ok", false}, {"error", "账号或口令错误"}});
      return;
    }
    respond_json(200, {{"ok", true},
                       {"token", token},
                       {"expires_in", kSessionTtlMs / 1000}});
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
                       std::shared_ptr<S3Storage> storage, std::uint16_t port)
    : impl_(std::make_unique<Impl>(store, std::move(storage))),
      acceptor_(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) {}

FileServer::~FileServer() = default;

std::uint16_t FileServer::port() const {
  return acceptor_.local_endpoint().port();
}

void FileServer::start_accept() {
  std::cout << "[MEMEX] Files 监听 0.0.0.0:" << port()
            << (impl_->storage ? "" : "（存储未配置：一律 503）") << std::endl;
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
