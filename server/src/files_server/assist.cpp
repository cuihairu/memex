// 远程协助面（LAY-003 拆分）：协议请求/应答/生命周期、媒体帧
// 推拉与输入事件中继（AssistMedia 槽在 Impl）。

#include "files_server/conn.hpp"

namespace memex::server {

// —— 远程协助（二期）：协议面＋媒体中继。模型层（平台-11）在 store：
// 五态生命周期/consent 红线（批拒撤只属受控方）/audit 红线（迁移自动
// 留痕）/部门放行开关默认禁。路由只做当事方判权与状态门——
void FileConn::route_assist_request(const std::string& body) {
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
void FileConn::route_assist_respond(const std::string& body) {
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
void FileConn::route_assist_lifecycle(const std::string& body, bool start) {
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
void FileConn::route_assist_sessions() {
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
void FileConn::route_assist_audit(const std::string& body) {
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
std::string FileConn::assist_media_gate(const std::string& id,
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
void FileConn::route_assist_frame(const std::string& body, bool push) {
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
void FileConn::route_assist_input(const std::string& body, bool send) {
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

} // namespace memex::server
