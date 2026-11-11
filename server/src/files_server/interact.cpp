// 群互动三件（LAY-003 拆分）：投票、接龙、群任务（原生互动，
// 不走 R25 外部工具代理）。

#include "files_server/conn.hpp"

namespace memex::server {

// —— 二期群工具三件（原生互动，不走 R25 外部工具代理）——
// 身份约束（设计稿 §1，不新造 az 动词、服务端逻辑判）：关票/关接龙=
// 发起人或群主/管理员（同 R24-1 公告判权口径）
bool FileConn::group_manage_allowed(std::uint64_t gid, const std::string& account,
                          const std::string& initiator) {
  if (account == initiator) return true;
  const auto info = impl_.store.group_info(gid);
  if (!info.has_value()) return false;
  if (info->owner == account) return true;
  return impl_.store.group_role(gid, account) == "admin";
}
void FileConn::route_group_poll_create(const std::string& body) {
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
void FileConn::route_group_polls_list() {
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
void FileConn::route_group_poll_vote(const std::string& body) {
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
// —— 结果卡片文本（R26：显式截止后回群，TEXT 群消息；与列表页渲染
//    同口径——counts 按位集展开、匿名不回 voter 只报参与人数）——
std::string FileConn::poll_card_text(const ServerStore::GroupPoll& p) {
  const auto votes = impl_.store.poll_votes(p.id);
  const int n = static_cast<int>(p.options.size());
  std::vector<int> counts(n, 0);
  for (const auto& v : votes) {
    if (p.multi) {
      if (v.choice >= 1 && v.choice < (1 << n)) {
        for (int i = 0; i < n; ++i) {
          if ((v.choice >> i) & 1) ++counts[i];
        }
      }
    } else if (v.choice >= 1 && v.choice <= n) {
      ++counts[v.choice - 1];
    }
  }
  std::string card = "【投票结果】#" + std::to_string(p.id) + " " +
                     p.topic + "\n";
  for (int i = 0; i < n; ++i) {
    card += "· " + p.options[i] + "×" + std::to_string(counts[i]) + "\n";
  }
  if (p.anonymous) {
    card += "匿名投票，共 " + std::to_string(votes.size()) + " 人参与\n";
  } else {
    std::string ledger;
    for (const auto& v : votes) {
      std::string choice_s;
      if (p.multi) {
        for (int i = 0; i < n; ++i) {
          if ((v.choice >> i) & 1) {
            if (!choice_s.empty()) choice_s += "+";
            choice_s += std::to_string(i + 1);
          }
        }
      } else {
        choice_s = std::to_string(v.choice);
      }
      if (!ledger.empty()) ledger += ", ";
      ledger += v.account + "→" + choice_s;
    }
    if (!ledger.empty()) card += ledger + "\n";
  }
  card += "已截止";
  return card;
}
std::string FileConn::chain_card_text(const ServerStore::GroupChain& c) {
  std::string card = "【接龙结果】#" + std::to_string(c.id) + " " +
                     c.title + "\n";
  for (const auto& e : impl_.store.chain_entries(c.id)) {
    card += "· " + e.account + "：" + e.content + "\n";
  }
  card += "已截止";
  return card;
}
void FileConn::route_group_poll_close(const std::string& body) {
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
  // 结果卡片回群（R26）：显式截止动作即回（到点惰性截止无明确动作
  // 钩子不发卡、避免重复；到点后手动 close 补写留痕时同发）
  if (impl_.group_text) {
    impl_.group_text("group:" + std::to_string(gid), account,
                     poll_card_text(*p));
  }
  respond_json(200, {{"ok", true}, {"gid", gid}, {"poll_id", poll_id}});
}
void FileConn::route_group_chain_create(const std::string& body) {
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
void FileConn::route_group_chains_list() {
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
void FileConn::route_group_chain_join(const std::string& body) {
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
  // 格式校验：format_hint 含「+」时条目段数须一致（自由文本不强校验
  // 段内容语义，只校验结构段数——「姓名+几点」=2 段；空段不计数）
  const auto segments = [](const std::string& s) {
    int n = 0;
    bool in = false;
    for (char ch : s) {
      if (ch != '+' && !in) {
        ++n;
        in = true;
      } else if (ch == '+') {
        in = false;
      }
    }
    return n;
  };
  if (c->format_hint.find('+') != std::string::npos) {
    const int want = segments(c->format_hint);
    const int got = segments(content);
    if (got != want) {
      respond_json(400,
                   {{"ok", false},
                    {"error", "条目须按格式提示「" + c->format_hint +
                                  "」分 " + std::to_string(want) +
                                  " 段（用 + 分隔），现 " +
                                  std::to_string(got) + " 段"}});
      return;
    }
  }
  if (!impl_.store.chain_join(chain_id, account, content, now_ms())) {
    respond_json(409, {{"ok", false}, {"error", "接龙失败"}});
    return;
  }
  std::cout << "[MEMEX] files group-chain join account=" << account
            << " gid=" << gid << " chain=" << chain_id << std::endl;
  respond_json(200, {{"ok", true}, {"gid", gid}, {"chain_id", chain_id}});
}
void FileConn::route_group_chain_close(const std::string& body) {
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
  // 结果卡片回群（R26）：同投票——显式截止动作即回
  if (impl_.group_text) {
    impl_.group_text("group:" + std::to_string(gid), account,
                     chain_card_text(*c));
  }
  respond_json(200, {{"ok", true}, {"gid", gid}, {"chain_id", chain_id}});
}
void FileConn::route_group_task_create(const std::string& body) {
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
void FileConn::route_group_tasks_list() {
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
void FileConn::route_group_task_claim(const std::string& body) {
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
void FileConn::route_group_task_done(const std::string& body) {
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

} // namespace memex::server
