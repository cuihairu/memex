// 会话与 uplink 面（LAY-003 拆分）：/files/session 与 /uplink/*
// 外网单向端点（登录/登出/上传/清单/删除）。

#include "files_server/conn.hpp"

namespace memex::server {

// 换会话令牌：内网面 /files/session（scope=internal）与外网面
// /uplink/session（scope=uplink，只许写入）同源账号库同 PBKDF2。
void FileConn::route_session(const std::string& body, bool uplink) {
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
  // 与消息面同源判登（credentials 表，平台-2；token 只存哈希）。
  // 544：绑定 issuer=ldap 的账号走目录判登（route_authentication 同一
  // 路由——fail-closed 与消息面同口径），角色同步随登录幂等落库。
  const auto route =
      route_authentication(impl_.store, impl_.ldap, account, password);
  std::string token;
  if (route.path == AuthRoute::Path::LdapOk) {
    sync_ldap_roles(impl_.store, account, route.roles, now_ms());
    token = impl_.mint_session(account, uplink);
  } else if (route.path == AuthRoute::Path::LdapBad ||
             route.path == AuthRoute::Path::LdapDisabled ||
             route.path == AuthRoute::Path::LdapUnavailable) {
    // 口径回包（不落 500：目录不可达是环境态；密码永不进回包）
    respond_json(401,
                 {{"ok", false},
                  {"error", route.path == AuthRoute::Path::LdapBad
                                ? "账号或口令错误"
                                : route.path == AuthRoute::Path::LdapDisabled
                                      ? "目录账号已禁用"
                                      : "目录认证不可用（fail-closed）"}});
    return;
  } else {
    const auto cred = impl_.store.find_credential(account, "password");
    if (cred.has_value()) {
      // 摘要比对用常量时间思路不引入（内网面 v1 与消息面同口径）
      token = pbkdf2_sha256_hex(password, cred->salt_hex, 60000) ==
                      cred->digest_hex
                  ? impl_.mint_session(account, uplink)
                  : "";
    }
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
void FileConn::route_logout() {
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
std::string FileConn::uplink_account_or_respond() {
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
void FileConn::route_uplink_upload(const std::string& body) {
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
void FileConn::route_uplink_mine() {
  const std::string account = uplink_account_or_respond();
  if (account.empty()) return;
  int limit = 200, offset = 0;
  const std::string lim = query_param(query_, "limit");
  const std::string off = query_param(query_, "offset");
  limit = safe_int_param(lim, 200, 1000);
  offset = safe_int_param(off, 0, 999999999);
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
void FileConn::route_uplink_delete() {
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

} // namespace memex::server
