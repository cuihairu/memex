// 个人任务面（LAY-003 拆分）：R27-1 清单/完成/状态/类型/提醒与
// R27-5 任务源拉取（主密钥派生、凭据密封落库）。

#include "files_server/conn.hpp"

namespace memex::server {

// —— R27-1 个人任务清单 ——
void FileConn::route_task_create(const std::string& body) {
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
      (j.contains("start_ms") && !j["start_ms"].is_number_integer()) ||
      (j.contains("type") && !j["type"].is_string()) ||
      (j.contains("assignee") && !j["assignee"].is_string()) ||
      (j.contains("provider") && !j["provider"].is_string()) ||
      (j.contains("ext_key") && !j["ext_key"].is_string())) {
    respond_json(
        400,
        {{"ok", false},
         {"error",
          "缺少字段：title（非空）；note/due_ms/start_ms/type/assignee/"
          "provider/ext_key 可选"}});
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
  // R27-6：类型须在服务端清单内（缺省=默认类型「任务」）；开始时间
  // 0=未设
  std::string type = impl_.store.task_type_default();
  if (j.contains("type")) {
    type = j["type"].get<std::string>();
    bool known = false;
    for (const auto& t : impl_.store.task_types()) {
      if (t.name == type) known = true;
    }
    if (!known) {
      respond_json(400, {{"ok", false},
                         {"error", "未知任务类型（清单在 /files/task-types）"}});
      return;
    }
  }
  std::int64_t start_ms = 0;
  if (j.contains("start_ms")) {
    start_ms = j["start_ms"].get<std::int64_t>();
    if (start_ms < 0) {
      respond_json(400, {{"ok", false}, {"error", "start_ms 须 ≥0"}});
      return;
    }
  }
  const std::int64_t id = impl_.store.task_create(
      assignee, account, j["title"].get<std::string>(), note, due_ms,
      now_ms(), provider, ext_key, type, start_ms);
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
// 任务行统一序列化（列表 mine/assigned 与领导视角泳道同形；assigned
// 行 owner 为动作主体，creator 同带上不碍事）
json FileConn::task_row_json(const ServerStore::TaskRow& t) {
  return {{"id", t.id},
          {"owner", t.owner},
          {"creator", t.creator},
          {"title", t.title},
          {"note", t.note},
          {"due_ms", t.due_ms},
          {"reminded_ms", t.reminded_ms},
          {"done", t.done},
          {"done_ms", t.done_ms},
          {"created_ms", t.created_ms},
          {"provider", t.provider},
          {"ext_key", t.ext_key},
          {"start_ms", t.start_ms},
          {"start_reminded_ms", t.start_reminded_ms},
          {"type", t.type},
          {"status", t.status}};
}
void FileConn::route_tasks_list() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json mine = json::array();
  for (const auto& t : impl_.store.tasks_of(account)) {
    mine.push_back(task_row_json(t));
  }
  json assigned = json::array();
  for (const auto& t : impl_.store.tasks_assigned_by(account)) {
    assigned.push_back(task_row_json(t));
  }
  respond_json(200, {{"ok", true}, {"tasks", mine},
                     {"assigned_by_me", assigned}});
}
// 领导视角（R27-6 设计 §6.4）：直属下属逐人聚合其任务清单＋负荷统计。
// 下属集合=org_reporting_lines 反查（只一级，不递归跨级关闭）；逐人
// 再过 az 与单读同源（task-team-read：只有直属上级可读，平级/其他部
// 门领导/org-admin 一律不可见，转岗即时生效）。负荷统计口径写死：
// 未完成数（status≠完成）、已逾期数（due_ms 已过且未完成）、7 天内
// 到期数（0<due_ms−now≤7 天且未完成）。
void FileConn::route_tasks_team() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  const std::int64_t now = now_ms();
  const std::int64_t week = 7LL * 86400000;
  json team = json::array();
  for (const auto& sub : impl_.store.direct_reports(account)) {
    const Decision d = impl_.az.authorize(
        {account, "task:team-read", "user:" + sub, "owner=" + sub});
    if (!d.allowed) continue;
    int open = 0;
    int overdue = 0;
    int due_soon = 0;
    json rows = json::array();
    for (const auto& t : impl_.store.tasks_of(sub)) {
      const bool done = t.done || t.status == "完成";
      if (!done) {
        ++open;
        if (t.due_ms > 0 && t.due_ms <= now) ++overdue;
        if (t.due_ms > now && t.due_ms - now <= week) ++due_soon;
      }
      rows.push_back(task_row_json(t));
    }
    team.push_back({{"account", sub},
                    {"loads", {{"open", open},
                               {"overdue", overdue},
                               {"due_soon", due_soon}}},
                    {"tasks", std::move(rows)}});
  }
  respond_json(200, {{"ok", true}, {"team", std::move(team)}});
}
void FileConn::route_task_done(const std::string& body) {
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
// R27-6 状态切换（待办/进行中/完成——看板列语义；完成↔done 双写，
// 读取以 status 为准）；判权同勾完成：清单主人
void FileConn::route_task_status(const std::string& body) {
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
      !j.contains("status") || !j["status"].is_string()) {
    respond_json(400, {{"ok", false}, {"error", "缺少字段：id/status"}});
    return;
  }
  const std::string status = j["status"].get<std::string>();
  if (status != "待办" && status != "进行中" && status != "完成") {
    respond_json(400,
                 {{"ok", false},
                  {"error", "status 须为 待办/进行中/完成"}});
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
                       {"error", "只有清单主人能改任务状态"}});
    return;
  }
  if (!impl_.store.task_set_status(id, status, now_ms())) {
    respond_json(500, {{"ok", false}, {"error", "任务状态更新失败"}});
    return;
  }
  respond_json(200, {{"ok", true}});
}
// R27-6 任务类型清单（服务端配置，admin 增删改——客户端拉取展示；
// 缺省类型单列，建任务缺省用它）
void FileConn::route_task_types() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  json arr = json::array();
  for (const auto& t : impl_.store.task_types()) {
    arr.push_back({{"name", t.name}, {"is_default", t.is_default}});
  }
  respond_json(200, {{"ok", true},
                     {"types", arr},
                     {"default", impl_.store.task_type_default()}});
}
void FileConn::route_task_reminded(const std::string& body) {
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
  // R27-6：kind=start＝开始到点提醒回执（独立位，不与 deadline 互抢）；
  // 缺省=deadline 到期提醒（兼容现口径）
  const bool start_kind = j.contains("kind") && j["kind"].is_string() &&
                          j["kind"].get<std::string>() == "start";
  if (start_kind) {
    impl_.store.task_mark_start_reminded(id, now_ms());
  } else {
    impl_.store.task_mark_reminded(id, now_ms());
  }
  // 邮箱·通知入箱（mailbox.md §I：任务到期归通知类——提醒回执与服务
  // 端提醒时刻同源（客户端 30s 轮询判到期后回执一次），dedup 按
  // 任务+提醒位幂等，重复回执不重复入箱；不走新推送链）
  (void)impl_.store.mailbox_add(
      "notice", "任务「" + row->title + "」" +
                    (start_kind ? "开始时间已到" : "已到截止时间"),
      "任务到期 · 通知中心三级分层 IMPORTANT 强提醒", "任务到期", "tasks",
      (start_kind ? "task-start:" : "task-remind:") + std::to_string(id),
      {row->owner}, now_ms());
  respond_json(200, {{"ok", true}});
}
void FileConn::route_task_delete(const std::string& body) {
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
// —— R27-5 任务源拉取面（主密钥未配 503；owner=会话本人＝自建语义；
// 源凭据 AES-256-GCM 密封落库、列表只见掩码位，明文与密文都不回客户端
// 不进日志；admin 全局开关与主密钥在拉取器侧兜底——本面只管配置）——
void FileConn::route_task_source_set(const std::string& body) {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  if (impl_.task_pull_key.empty()) {
    respond_json(503, {{"ok", false},
                       {"error", "任务源拉取面未启用（服务端未配主密钥）"}});
    return;
  }
  json j;
  try {
    j = json::parse(body);
  } catch (const std::exception&) {
    respond_json(400, {{"ok", false}, {"error", "请求体不是合法 JSON"}});
    return;
  }
  if (!j.is_object() || !j.contains("provider") ||
      !j["provider"].is_string() ||
      j["provider"].get<std::string>().empty() ||
      !j.contains("repo") || !j["repo"].is_string()) {
    respond_json(400,
                 {{"ok", false},
                  {"error", "缺少字段：provider（非空）/repo"}});
    return;
  }
  const std::string provider = j["provider"].get<std::string>();
  const std::string repo = j["repo"].get<std::string>();
  // provider 白名单＝honest 拒绝（gitlab/jira/linear 随后按需接入）
  if (provider != "github") {
    respond_json(400, {{"ok", false},
                       {"error", "provider 未接入（现只接 github）"}});
    return;
  }
  if (repo.empty() || repo.find('/') == std::string::npos ||
      repo.front() == '/' || repo.back() == '/') {
    respond_json(400,
                 {{"ok", false}, {"error", "repo 须为 owner/repo 形态"}});
    return;
  }
  // 部分语义：body 未携带的字段保留原行现值（clear_secret 单独扳闸不回吹
  // 其它字段）；新行缺省 enabled=true / interval_s=300
  bool enabled = true;
  int interval_s = 300;
  for (const auto& old : impl_.store.task_pull_configs()) {
    if (old.owner == account && old.provider == provider) {
      enabled = old.enabled;
      interval_s = old.interval_s;
      break;
    }
  }
  if (j.contains("enabled")) {
    if (!j["enabled"].is_boolean()) {
      respond_json(400, {{"ok", false}, {"error", "enabled 须为布尔"}});
      return;
    }
    enabled = j["enabled"].get<bool>();
  }
  if (j.contains("interval_s")) {
    if (!j["interval_s"].is_number_integer()) {
      respond_json(400,
                   {{"ok", false}, {"error", "interval_s 须为整数秒"}});
      return;
    }
    interval_s = j["interval_s"].get<int>();
    if (interval_s < 0) {
      respond_json(400, {{"ok", false},
                         {"error", "interval_s 须 ≥0（0=每拍都拉，测试口径）"}});
      return;
    }
  }
  std::string secret;
  if (j.contains("secret")) {
    if (!j["secret"].is_string()) {
      respond_json(400, {{"ok", false}, {"error", "secret 须为字符串"}});
      return;
    }
    secret = j["secret"].get<std::string>();
  }
  const bool clear_secret =
      j.contains("clear_secret") && j["clear_secret"].is_boolean() &&
      j["clear_secret"].get<bool>();
  ServerStore::TaskPullConfig c;
  c.owner = account;
  c.provider = provider;
  c.repo = repo;
  c.enabled = enabled;
  c.interval_s = interval_s;
  c.updated_ms = now_ms();
  if (!impl_.store.task_pull_config_set(c)) {
    respond_json(500, {{"ok", false}, {"error", "任务源配置落库失败"}});
    return;
  }
  const char* cred_op = "kept";
  if (!secret.empty()) {
    // 密封后落库（GCM；密封失败＝不给落明文）
    const std::string sealed = gcm_seal(impl_.task_pull_key, secret);
    if (sealed.empty()) {
      respond_json(500, {{"ok", false}, {"error", "凭据密封失败"}});
      return;
    }
    impl_.store.task_source_cred_set(account, provider, sealed, now_ms());
    cred_op = "set";
  } else if (clear_secret) {
    impl_.store.task_source_cred_delete(account, provider);
    cred_op = "cleared";
  }
  // 审计行：凭据明文与密文都不出现（只记操作位）
  std::cout << "[MEMEX] files task-source set account=" << account
            << " provider=" << provider << " repo=" << repo
            << " enabled=" << (c.enabled ? 1 : 0)
            << " interval_s=" << c.interval_s << " cred=" << cred_op
            << std::endl;
  respond_json(200, {{"ok", true}});
}
void FileConn::route_task_sources_list() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  if (impl_.task_pull_key.empty()) {
    respond_json(503, {{"ok", false},
                       {"error", "任务源拉取面未启用（服务端未配主密钥）"}});
    return;
  }
  json arr = json::array();
  for (const auto& c : impl_.store.task_pull_configs()) {
    if (c.owner != account) continue; // 自建语义：只回本人行
    const auto sealed =
        impl_.store.task_source_cred_sealed(c.owner, c.provider);
    json row = {{"provider", c.provider},
                {"repo", c.repo},
                {"enabled", c.enabled},
                {"interval_s", c.interval_s},
                {"last_pull_ms", c.last_pull_ms},
                {"updated_ms", c.updated_ms},
                {"cred_configured", sealed.has_value() && !sealed->empty()}};
    arr.push_back(std::move(row));
  }
  respond_json(200, {{"ok", true}, {"sources", arr}});
}
void FileConn::route_task_source_delete() {
  const std::string account = account_or_respond();
  if (account.empty()) return;
  if (impl_.task_pull_key.empty()) {
    respond_json(503, {{"ok", false},
                       {"error", "任务源拉取面未启用（服务端未配主密钥）"}});
    return;
  }
  const std::string provider = query_param(query_, "provider");
  if (provider.empty()) {
    respond_json(400, {{"ok", false}, {"error", "缺少参数：provider"}});
    return;
  }
  // 撤源＝配置＋凭据同撤（拉取器下一拍即不再触达）
  const bool gone = impl_.store.task_pull_config_delete(account, provider);
  impl_.store.task_source_cred_delete(account, provider);
  if (!gone) {
    respond_json(404, {{"ok", false}, {"error", "任务源不存在"}});
    return;
  }
  std::cout << "[MEMEX] files task-source delete account=" << account
            << " provider=" << provider << std::endl;
  respond_json(200, {{"ok", true}});
}

} // namespace memex::server
