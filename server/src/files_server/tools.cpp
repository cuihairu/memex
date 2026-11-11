// 群工具框架（LAY-003 拆分）：R25-1 框架与能力闸、CI/CD、打包
// 导出、凭据面（GCM 密封）。

#include "files_server/conn.hpp"

namespace memex::server {

// —— R25-1 群工具框架 ——
// call 判定（入群即授权/退群即失，route 叠加不自造规则）：群成员
// （file:read 群继承）即备格；白名单另查（工具只暴露声明的动作）。
bool FileConn::tool_call_allowed(const std::string& account, std::uint64_t gid) {
  return impl_.az
      .authorize({account, "file:read", group_resource(gid),
                  "owner=" + account})
      .allowed;
}
// —— 平台-12 权限模型接线：群能力面统一门 ——
// 未配置=现行允许、配置禁即拒（首段步进口径）。放在既有角色判权之后、
// 动作之前：非成员先吃 403 角色拒，不向未授权者泄露能力配置状态。
bool FileConn::capability_gate(std::uint64_t gid, const char* cap,
                     const char* label) {
  if (impl_.store.group_capability_enabled(gid, cap)) return true;
  respond_json(403,
               {{"ok", false},
                {"error", std::string(label) +
                     "能力已被管理员停用（deny:capability-" + cap + "）"}});
  return false;
}
// 工具白名单配置（仅群主/管理员＝memo:config；actions 须为字符串数组）
void FileConn::route_group_tools_config(const std::string& body) {
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
void FileConn::route_group_tools_list() {
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
void FileConn::route_group_tools_call(const std::string& body) {
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
void FileConn::route_group_tools_audit() {
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
void FileConn::route_group_ci_pipeline(const std::string& body) {
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
void FileConn::route_group_ci_list() {
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
void FileConn::route_group_ci_trigger(const std::string& body) {
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
void FileConn::route_group_ci_runs() {
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
void FileConn::route_group_pack_build(const std::string& body) {
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
void FileConn::route_group_pack_list() {
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
void FileConn::route_group_pack_delete(const std::string& body) {
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
void FileConn::route_group_export() {
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
// 群组-2 群记录导出（定案④ Markdown .md；定案② 全员＝本群成员；
// 定案③ 一次导出落 audit_reads 恰一行）：
//   GET /files/group-record-export?gid&from&to&with_ai&page_size&cap&fetched&after_id
// 首页（after_id=0）组装三段头＋聊天记录行；续页仅聊天记录行（客户端拼接）。
// 权限定案②：导出者须为本群成员（group_members 现查），非群成员 403——
// 路由层判权（与 search_messages 的成员资格子查询同源，退群天然失权）。
// 归档只读：本路由对 messages 只 SELECT，不写不动（R3 副本口径）。
void FileConn::route_group_record_export() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  std::uint64_t gid = 0;
  if (!parse_gid_param(query_param(query_, "gid"), &gid)) {
    respond_json(400, {{"ok", false}, {"error", "gid 须为正整数群号"}});
    return;
  }
  const auto info = impl_.store.group_info(gid);
  if (!info.has_value()) {
    respond_json(404, {{"ok", false}, {"error", "群不存在"}});
    return;
  }
  // 定案②判权：全员＝本群成员（路由层 az；非群成员一律不可导）
  if (!impl_.store.is_group_member(gid, account)) {
    respond_json(
        403, {{"ok", false}, {"error", "无权导出（非本群成员）"}});
    return;
  }
  auto ms_param = [](const std::string& s, std::int64_t* out) {
    if (s.empty()) return false;
    if (s.find_first_not_of("0123456789") != std::string::npos) return false;
    *out = std::strtoll(s.c_str(), nullptr, 10);
    return true;
  };
  std::int64_t from_ms = 0, to_ms = 0;
  ms_param(query_param(query_, "from"), &from_ms);
  ms_param(query_param(query_, "to"), &to_ms);
  const bool with_ai = query_param(query_, "with_ai") == "1";
  int page_size = 1000;
  std::int64_t ps = 0;
  if (ms_param(query_param(query_, "page_size"), &ps) && ps > 0) {
    page_size = ps > 1000 ? 1000 : static_cast<int>(ps);
  }
  std::int64_t cap = 10000;
  std::int64_t cp = 0;
  if (ms_param(query_param(query_, "cap"), &cp) && cp > 0) {
    cap = cp > 10000 ? 10000 : cp;
  }
  std::int64_t fetched = 0;
  ms_param(query_param(query_, "fetched"), &fetched);
  std::int64_t after_id = 0;
  ms_param(query_param(query_, "after_id"), &after_id);
  const bool first_page = after_id == 0;

  // 单次上限截断：剩余额度即本页上限（不静默截尾——尾部 truncated 标注）
  std::int64_t remaining = cap - fetched;
  if (remaining <= 0) {
    respond_json(200, {{"ok", true},
                       {"gid", gid},
                       {"md", ""},
                       {"count", 0},
                       {"has_more", false},
                       {"next_after_id", 0},
                       {"truncated", true},
                       {"ai_enabled", false}});
    return;
  }
  const int limit_used =
      static_cast<int>(remaining < page_size ? remaining : page_size);

  MessageSearch q;
  q.account = account;
  q.peer = "group:" + std::to_string(gid);
  q.since_ms = from_ms;
  q.until_ms = to_ms;
  q.limit = limit_used;
  q.asc = true;           // 升序＝对话体自然序
  q.after_id = after_id;  // keyset 游标续传
  const auto rows = impl_.store.search_messages(q);

  // 首页三段头（续页仅聊天记录行，客户端逐页拼接）
  std::string md;
  if (first_page) {
    const auto fmt_ms = [](std::int64_t ts) -> std::string {
      if (ts <= 0) return "不限";
      std::time_t t = static_cast<std::time_t>(ts / 1000);
      std::tm tmv{};
      localtime_r(&t, &tmv);
      char buf[32];
      std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
      return buf;
    };
    md += "# 群记录：" + info->name + "（群号 " + std::to_string(gid) +
           "）\n\n";
    md += "> 导出者：" + account + "　导出时间：" + fmt_ms(now_ms()) + "\n\n";
    md += "## 参与人\n";
    for (const auto& m : impl_.store.group_members_detail(gid)) {
      // 群主身份在 groups.owner（不在 group_members.role 列）——
      // 明细行的 role 列默认为 member，此处按群主现查映射。
      const std::string role =
          (info->owner == m.account) ? "owner" : m.role;
      md += "- " + m.account + "（" + role + "）加入于 " +
             fmt_ms(m.joined_ms) + "\n";
    }
    md += "> 离开时间不含：退群即摘行，现库不留痕。\n\n";
    md += "## 时间范围\n";
    md += "- 建群时间：" + fmt_ms(info->created_ms) + "\n";
    md += "- 导出区间：" + fmt_ms(from_ms) + " ~ " + fmt_ms(to_ms) + "\n\n";
    if (with_ai) {
      // 定案③降级口径：模型未接线（R11 三期）→ 普通文档＋「AI 未启用」
      // 注明；不许假装有总结、不许空总结章节占位。
      md += "> AI 未启用：本次导出为普通文档（模型未接线/网关不可达自动降"
            "级）。\n\n";
    }
    md += "## 聊天记录\n";
  }
  for (const auto& m : rows) {
    std::time_t t = static_cast<std::time_t>(m.ts_ms / 1000);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    md += "[";
    md += buf;
    md += "] ";
    md += m.from_account;
    if (m.recalled) md += "（撤回）: ";
    else md += ": ";
    md += m.text;
    md += "\n";
  }

  const bool reached_cap = fetched + static_cast<std::int64_t>(rows.size()) >= cap;
  const bool has_more = !rows.empty() && rows.size() == static_cast<std::size_t>(limit_used) && !reached_cap;
  // 定案③：一次导出恰一行审计——导出收尾页（无更多或触顶）落
  // audit_reads（谁/何时/哪个群/区间/条数/是否含 AI 总结；filters 不含正文）。
  // first_page 兜底：单页装完（无更多）时首页即收尾，只此一页落行——
  // 客户端在 has_more=false 时不再发请求，杜绝同一导出重复落行。
  if (!has_more || reached_cap) {
    const std::int64_t total = fetched + static_cast<std::int64_t>(rows.size());
    json filters = {{"gid", gid},
                    {"from_ms", from_ms},
                    {"to_ms", to_ms},
                    {"with_ai", with_ai}};
    impl_.store.add_audit_read({0, account, "group_record_export",
                                filters.dump(), static_cast<int>(total),
                                now_ms()});
    std::cout << "[MEMEX] files group-record-export account=" << account
              << " gid=" << gid << " rows=" << total
              << (with_ai ? " ai_requested=1" : "") << std::endl;
  }
  respond_json(200, {{"ok", true},
                     {"gid", gid},
                     {"md", md},
                     {"count", static_cast<int>(rows.size())},
                     {"has_more", has_more},
                     {"next_after_id",
                      has_more && !rows.empty()
                          ? static_cast<std::int64_t>(rows.back().row_id)
                          : 0},
                     {"truncated", reached_cap},
                     {"ai_enabled", false}});
}
// —— R25-4 凭据面：工具外部凭据只存服务端密文（gcm_seal hex 落库，
// 行内永不见明文）。管理面（memo:config＝owner/admin）；主密钥未配＝
// 503 明示「未启用」而非静默存明文；回包只带掩码元数据。
void FileConn::route_group_tools_credential(const std::string& body) {
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
void FileConn::route_group_tools_credentials() {
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

} // namespace memex::server
