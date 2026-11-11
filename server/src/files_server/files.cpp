// 内网文件面（LAY-003 拆分）：上传/下载/列表/配额/删除/置顶
// 路由（判权走 Impl.handle_*，本模块只做 HTTP 面）。

#include "files_server/conn.hpp"

namespace memex::server {

void FileConn::route_upload(const std::string& body) {
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
void FileConn::route_download() {
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
void FileConn::route_list() {
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
  limit = safe_int_param(lim, 200, 1000);
  offset = safe_int_param(off, 0, 999999999);
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
void FileConn::route_quota_get() {
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
void FileConn::route_delete() {
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
void FileConn::route_pin() {
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
void FileConn::route_quota_set(const std::string& body) {
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

} // namespace memex::server
