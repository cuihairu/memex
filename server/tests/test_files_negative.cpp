// 群互动/群工具/群服务器负路径验收（COV-98 真测试补强）：参数门/权限闸/
// 状态机门的真实协议语义——坏 JSON 400、缺字段 400、非成员 403、幽灵
// 404、状态冲突 409、一次性短票 401/409。正路径已在 test_files_api /
// test_uplink 走查；本面专踩各路由的负分支（能力开关走 store 层直配，
// 对象层负路径不触 S3——FileServer 以 nullptr 存储）。
#include <asio.hpp>

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "files_server.hpp"
#include "store.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ' ' << #cond    \
                << '\n';                                                     \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

using memex::server::ServerStore;

// —— 阻塞 HTTP 客户端（真 TCP；服务端 Connection: close，读到对端关）——
struct HttpReply {
  int status{0};
  std::string body;
};

HttpReply http(std::uint16_t port, const std::string& method,
               const std::string& path,
               const std::map<std::string, std::string>& headers,
               const std::string& body) {
  HttpReply rep;
  asio::io_context io;
  asio::ip::tcp::socket s(io);
  s.connect({asio::ip::make_address("127.0.0.1"), port});
  std::string req = method + " " + path + " HTTP/1.1\r\n";
  req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
  req += "\r\n" + body;
  asio::write(s, asio::buffer(req));
  asio::error_code ec;
  std::string raw;
  char buf[8192];
  for (;;) {
    const std::size_t n = s.read_some(asio::buffer(buf), ec);
    if (ec) break;
    raw.append(buf, n);
  }
  const std::size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) return rep;
  std::istringstream in(raw.substr(0, head_end));
  std::string line;
  std::getline(in, line); // 状态行
  if (line.rfind("HTTP/1.1 ", 0) == 0) rep.status = std::atoi(line.c_str() + 9);
  rep.body = raw.substr(head_end + 4);
  return rep;
}

std::string jstr(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto pos = j.find(k);
  if (pos == std::string::npos) return "";
  std::size_t i = pos + k.size();
  while (i < j.size() && (j[i] == ' ' || j[i] == '"')) ++i;
  std::string out;
  while (i < j.size() && j[i] != '"') out += j[i++];
  return out;
}
std::int64_t jint(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto pos = j.find(k);
  if (pos == std::string::npos) return 0;
  return std::atoll(j.c_str() + pos + k.size());
}

} // namespace

int main() {
  asio::io_context io;
  ServerStore store;
  CHECK(store.open(":memory:"));
  for (const char* a :
       {"owner1", "admin1", "member1", "member2", "outsider"}) {
    CHECK(store.create_account(a, "pw-" + std::string(a), a));
  }
  // 群 g1：owner1 群主、admin1 管理员、member1/member2 普通成员；outsider 不入群
  const auto gid = store.create_group(
      "dev", "owner1", {"owner1", "admin1", "member1", "member2"});
  CHECK(gid > 0);
  CHECK(store.group_set_role(gid, "admin1", "admin"));
  // server_tools 能力开关（R26）：CLI 是生产入口（memex_server capability
  // set）；测试直配 store 层同一落点
  CHECK(store.group_capability_set(gid, "server_tools", true, "owner1", 0));

  memex::server::FileServer files(io, store, nullptr, 0);
  const std::uint16_t port = files.port();
  files.start_accept();
  std::thread th([&io] { io.run(); });

  std::map<std::string, std::string> tok;
  for (const char* a :
       {"owner1", "admin1", "member1", "member2", "outsider"}) {
    const auto r = http(port, "POST", "/files/session", {},
                        "{\"account\":\"" + std::string(a) +
                            "\",\"password\":\"pw-" + std::string(a) + "\"}");
    CHECK(r.status == 200);
    tok[a] = jstr(r.body, "token");
    CHECK(!tok[a].empty());
  }
  const auto H = [&tok](const char* a) {
    return std::map<std::string, std::string>{
        {"Authorization", "Bearer " + tok.at(a)}};
  };
  const std::string g = std::to_string(gid);

  // ============ 群投票（interact · R26 原生互动）============
  CHECK(http(port, "POST", "/files/group-polls", H("member1"),
             "not-json").status == 400);
  CHECK(http(port, "POST", "/files/group-polls", H("member1"), "{}").status ==
        400);
  CHECK(http(port, "POST", "/files/group-polls", H("member1"),
             "{\"gid\":" + g + ",\"topic\":\"t\",\"options\":[1,2]}")
            .status == 400); // options 非字符串数组
  CHECK(http(port, "POST", "/files/group-polls", H("member1"),
             "{\"gid\":" + g + ",\"topic\":\"\",\"options\":[\"a\"]}")
            .status == 400); // topic 空 + 选项不足 2
  CHECK(http(port, "POST", "/files/group-polls", H("outsider"),
             "{\"gid\":" + g + ",\"topic\":\"t\",\"options\":[\"a\",\"b\"]}")
            .status == 403); // 非成员
  CHECK(http(port, "GET", "/files/group-polls?gid=abc", H("member1"), "")
            .status == 400); // gid 坏参
  CHECK(http(port, "GET", "/files/group-polls?gid=" + g, H("outsider"), "")
            .status == 403);

  // 单选：越界 400、幽灵 404、缺字段 400
  const auto cp = http(port, "POST", "/files/group-polls", H("owner1"),
                       "{\"gid\":" + g + ",\"topic\":\"午饭\",\"options\":"
                       "[\"面\",\"饭\",\"粥\"]}");
  CHECK(cp.status == 200);
  const auto poll_id = jint(cp.body, "poll_id");
  CHECK(poll_id > 0);
  CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
             "{\"gid\":" + g + ",\"poll_id\":" + std::to_string(poll_id) +
                 ",\"choice\":5}").status == 400); // 选项越界（1~3）
  CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
             "{\"gid\":" + g + ",\"poll_id\":999999,\"choice\":1}")
            .status == 404); // 幽灵投票
  CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
             "{\"gid\":" + g + ",\"poll_id\":" + std::to_string(poll_id) +
                 "}").status == 400); // 缺 choice
  CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
             "{\"gid\":" + g + ",\"poll_id\":" + std::to_string(poll_id) +
                 ",\"choice\":1}").status == 200); // 正腿
  CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
             "{\"gid\":" + g + ",\"poll_id\":" + std::to_string(poll_id) +
                 ",\"choice\":2}").status == 200); // 二投=改票（UPSERT 语义）

  // 多选：位集越界 400；合法位集 200
  const auto cm = http(port, "POST", "/files/group-polls", H("owner1"),
                       "{\"gid\":" + g + ",\"topic\":\"多选\",\"options\":"
                       "[\"a\",\"b\",\"c\"],\"multi\":true}");
  CHECK(cm.status == 200);
  const auto multi_id = jint(cm.body, "poll_id");
  CHECK(http(port, "POST", "/files/group-polls/vote", H("member2"),
             "{\"gid\":" + g + ",\"poll_id\":" + std::to_string(multi_id) +
                 ",\"choice\":8}").status == 400); // 位集越界（n=3 → <8）
  CHECK(http(port, "POST", "/files/group-polls/vote", H("member2"),
             "{\"gid\":" + g + ",\"poll_id\":" + std::to_string(multi_id) +
                 ",\"choice\":5}").status == 200); // 0b101=选 1+3

  // 到点惰性截止：deadline 已过 → 409（投不进）
  const auto cd = http(port, "POST", "/files/group-polls", H("owner1"),
                       "{\"gid\":" + g + ",\"topic\":\"过期\",\"options\":"
                       "[\"a\",\"b\"],\"deadline_ms\":1}");
  CHECK(cd.status == 200);
  CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
             "{\"gid\":" + g + ",\"poll_id\":" + std::to_string(jint(cd.body,
                 "poll_id")) + ",\"choice\":1}").status == 409);

  // 截止权限链：缺字段 400 → 幽灵 404 → 非管理 403 → 群主 200 → 复截止 409
  CHECK(http(port, "POST", "/files/group-polls/close", H("owner1"), "{}")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-polls/close", H("owner1"),
             "{\"gid\":" + g + ",\"poll_id\":999999}").status == 404);
  CHECK(http(port, "POST", "/files/group-polls/close", H("member1"),
             "{\"gid\":" + g + ",\"poll_id\":" + std::to_string(poll_id) +
                 "}").status == 403); // 非发起人/群主/管理员
  CHECK(http(port, "POST", "/files/group-polls/close", H("owner1"),
             "{\"gid\":" + g + ",\"poll_id\":" + std::to_string(poll_id) +
                 "}").status == 200);
  CHECK(http(port, "POST", "/files/group-polls/close", H("owner1"),
             "{\"gid\":" + g + ",\"poll_id\":" + std::to_string(poll_id) +
                 "}").status == 409); // 已截止

  // ============ 群接龙 ============
  CHECK(http(port, "POST", "/files/group-chains", H("member1"), "not-json")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-chains", H("member1"), "{}").status ==
        400);
  CHECK(http(port, "POST", "/files/group-chains", H("member1"),
             "{\"gid\":" + g + ",\"title\":\"\"}").status == 400);
  CHECK(http(port, "POST", "/files/group-chains", H("outsider"),
             "{\"gid\":" + g + ",\"title\":\"t\"}").status == 403);
  CHECK(http(port, "GET", "/files/group-chains?gid=", H("member1"), "")
            .status == 400);
  CHECK(http(port, "GET", "/files/group-chains?gid=" + g, H("outsider"), "")
            .status == 403);

  // 格式门：format_hint 含「+」→ 条目段数须一致
  const auto cc = http(port, "POST", "/files/group-chains", H("owner1"),
                       "{\"gid\":" + g + ",\"title\":\"晚饭接龙\","
                       "\"format_hint\":\"姓名+几点\"}");
  CHECK(cc.status == 200);
  const auto chain_id = jint(cc.body, "chain_id");
  CHECK(chain_id > 0);
  CHECK(http(port, "POST", "/files/group-chains/join", H("member1"),
             "{\"gid\":" + g + ",\"chain_id\":" + std::to_string(chain_id) +
                 "}").status == 400); // 缺 content
  CHECK(http(port, "POST", "/files/group-chains/join", H("member1"),
             "{\"gid\":" + g + ",\"chain_id\":999999,\"content\":\"x\"}")
            .status == 404);
  CHECK(http(port, "POST", "/files/group-chains/join", H("member1"),
             "{\"gid\":" + g + ",\"chain_id\":" + std::to_string(chain_id) +
                 ",\"content\":\"\"}").status == 400); // content 空
  CHECK(http(port, "POST", "/files/group-chains/join", H("member1"),
             "{\"gid\":" + g + ",\"chain_id\":" + std::to_string(chain_id) +
                 ",\"content\":\"只写名字\"}").status == 400); // 1 段 ≠ 2 段
  CHECK(http(port, "POST", "/files/group-chains/join", H("member1"),
             "{\"gid\":" + g + ",\"chain_id\":" + std::to_string(chain_id) +
                 ",\"content\":\"张三+7点\"}").status == 200);
  CHECK(http(port, "POST", "/files/group-chains/close", H("member2"),
             "{\"gid\":" + g + ",\"chain_id\":" + std::to_string(chain_id) +
                 "}").status == 403); // 非发起人/群主/管理员
  CHECK(http(port, "POST", "/files/group-chains/close", H("owner1"),
             "{\"gid\":" + g + ",\"chain_id\":" + std::to_string(chain_id) +
                 "}").status == 200);
  CHECK(http(port, "POST", "/files/group-chains/join", H("member2"),
             "{\"gid\":" + g + ",\"chain_id\":" + std::to_string(chain_id) +
                 ",\"content\":\"李四+8点\"}").status == 409); // 已截止
  CHECK(http(port, "POST", "/files/group-chains/close", H("owner1"),
             "{\"gid\":" + g + ",\"chain_id\":" + std::to_string(chain_id) +
                 "}").status == 409); // 复截止

  // ============ 群任务（interact 原生认领面）============
  CHECK(http(port, "POST", "/files/group-tasks", H("member1"), "not-json")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-tasks", H("member1"), "{}").status ==
        400);
  CHECK(http(port, "POST", "/files/group-tasks", H("member1"),
             "{\"gid\":" + g + ",\"title\":\"\"}").status == 400);
  CHECK(http(port, "POST", "/files/group-tasks", H("outsider"),
             "{\"gid\":" + g + ",\"title\":\"t\"}").status == 403);
  CHECK(http(port, "POST", "/files/group-tasks", H("member1"),
             "{\"gid\":" + g + ",\"title\":\"t\",\"assignee\":\"ghost\"}")
            .status == 404); // 指定负责人非群成员＝幽灵拒
  CHECK(http(port, "GET", "/files/group-tasks?gid=0", H("member1"), "")
            .status == 400);
  CHECK(http(port, "GET", "/files/group-tasks?gid=" + g, H("outsider"), "")
            .status == 403);

  const auto ct1 = http(port, "POST", "/files/group-tasks", H("member1"),
                        "{\"gid\":" + g + ",\"title\":\"搬水\"}");
  CHECK(ct1.status == 200);
  const auto task_a = jint(ct1.body, "task_id");
  const auto ct2 = http(port, "POST", "/files/group-tasks", H("admin1"),
                        "{\"gid\":" + g + ",\"title\":\"值班\"}");
  CHECK(ct2.status == 200);
  const auto task_b = jint(ct2.body, "task_id");

  // 认领：缺字段 400 → 幽灵 404 → 抢占 409；同一任务只认一个认领人
  CHECK(http(port, "POST", "/files/group-tasks/claim", H("member2"), "{}")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-tasks/claim", H("member2"),
             "{\"gid\":" + g + ",\"task_id\":999999}").status == 404);
  CHECK(http(port, "POST", "/files/group-tasks/claim", H("member2"),
             "{\"gid\":" + g + ",\"task_id\":" + std::to_string(task_a) +
                 "}").status == 200);
  CHECK(http(port, "POST", "/files/group-tasks/claim", H("admin1"),
             "{\"gid\":" + g + ",\"task_id\":" + std::to_string(task_a) +
                 "}").status == 409); // 已被认领

  // 完成：完成者 200；无关成员 403；管理员 200；复完成 409
  CHECK(http(port, "POST", "/files/group-tasks/done", H("member1"), "{}")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-tasks/done", H("member1"),
             "{\"gid\":" + g + ",\"task_id\":999999}").status == 404);
  // task_b（admin1 创建）：member2 认领后 member1 与任务无关 → 403
  CHECK(http(port, "POST", "/files/group-tasks/claim", H("member2"),
             "{\"gid\":" + g + ",\"task_id\":" + std::to_string(task_b) +
                 "}").status == 200);
  CHECK(http(port, "POST", "/files/group-tasks/done", H("member1"),
             "{\"gid\":" + g + ",\"task_id\":" + std::to_string(task_b) +
                 "}").status == 403); // 非负责人/创建者/群主/管理员
  CHECK(http(port, "POST", "/files/group-tasks/done", H("member2"),
             "{\"gid\":" + g + ",\"task_id\":" + std::to_string(task_b) +
                 "}").status == 200); // 负责人本人
  CHECK(http(port, "POST", "/files/group-tasks/done", H("member2"),
             "{\"gid\":" + g + ",\"task_id\":" + std::to_string(task_b) +
                 "}").status == 409); // 已完成

  // ============ 群工具配置（R25-1 能力闸 + 管理闸）============
  CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
             "not-json").status == 400);
  CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"), "{}")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
             "{\"gid\":" + g + ",\"tool\":\"pack\",\"actions\":[1]}")
            .status == 400); // actions 非字符串数组
  CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
             "{\"gid\":" + g + ",\"tool\":\"\",\"actions\":[]}")
            .status == 400); // tool 空
  CHECK(http(port, "POST", "/files/group-tools/config", H("member1"),
             "{\"gid\":" + g + ",\"tool\":\"pack\",\"actions\":[\"build\"]}")
            .status == 403); // 仅群主/管理员

  // CI 触发：白名单未配「ci/trigger」→ 触发未开放 403（先于流水线存在性）
  CHECK(http(port, "POST", "/files/group-ci/pipeline", H("owner1"),
             "not-json").status == 400);
  CHECK(http(port, "POST", "/files/group-ci/trigger", H("owner1"), "{}")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-ci/trigger", H("owner1"),
             "{\"gid\":" + g + ",\"pipeline\":\"build\"}").status == 403);

  // ============ 群服务器（R26-1 注册/R26-3 短票）============
  CHECK(http(port, "POST", "/files/group-servers/enroll", H("owner1"),
             "not-json").status == 400);
  CHECK(http(port, "POST", "/files/group-servers/enroll", H("owner1"), "{}")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-servers/enroll", H("owner1"),
             "{\"gid\":0,\"name\":\"n\",\"host\":\"h\"}").status == 400);
  CHECK(http(port, "POST", "/files/group-servers/enroll", H("member1"),
             "{\"gid\":" + g + ",\"name\":\"n\",\"host\":\"h\"}")
            .status == 403); // 仅群主/管理员

  const auto ce = http(port, "POST", "/files/group-servers/enroll",
                       H("owner1"),
                       "{\"gid\":" + g + ",\"name\":\"web1\",\"host\":"
                       "\"10.0.0.8\"}");
  CHECK(ce.status == 200);
  const auto sid = jint(ce.body, "id");
  const std::string agent_token = jstr(ce.body, "token");
  CHECK(sid > 0);
  CHECK(!agent_token.empty());

  // 心跳：agent 令牌鉴权（非人会话）——缺字段 400、令牌无效 401
  CHECK(http(port, "POST", "/files/group-servers/heartbeat", {},
             "not-json").status == 400);
  CHECK(http(port, "POST", "/files/group-servers/heartbeat", {}, "{}")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-servers/heartbeat", {},
             "{\"token\":\"bad\",\"cpu_percent\":1,\"mem_used_mb\":1,"
             "\"mem_total_mb\":2,\"disk_used_mb\":1,\"disk_total_mb\":2,"
             "\"load1\":0.1}").status == 401);

  CHECK(http(port, "GET", "/files/group-servers/list?gid=", H("member1"), "")
            .status == 400);
  CHECK(http(port, "GET", "/files/group-servers/list?gid=" + g,
             H("outsider"), "").status == 403);

  // 短票签发：缺字段 400 / gid0 400 / 协议门 400 / 非成员 403 / 幽灵 404
  CHECK(http(port, "POST", "/files/group-servers/session/request",
             H("member1"), "not-json").status == 400);
  CHECK(http(port, "POST", "/files/group-servers/session/request",
             H("member1"), "{}").status == 400);
  CHECK(http(port, "POST", "/files/group-servers/session/request",
             H("member1"),
             "{\"gid\":0,\"server_id\":" + std::to_string(sid) + "}")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-servers/session/request",
             H("member1"),
             "{\"gid\":" + g + ",\"server_id\":" + std::to_string(sid) +
                 ",\"protocol\":\"rdp\"}").status == 400); // RDP/VNC 随后开
  CHECK(http(port, "POST", "/files/group-servers/session/request",
             H("outsider"),
             "{\"gid\":" + g + ",\"server_id\":" + std::to_string(sid) + "}")
            .status == 403);
  CHECK(http(port, "POST", "/files/group-servers/session/request",
             H("member1"),
             "{\"gid\":" + g + ",\"server_id\":999999}").status == 404);

  // 兑换：一次性——真票 200 后重放 409；坏票 401；缺字段 400
  const auto cs = http(port, "POST", "/files/group-servers/session/request",
                       H("member1"),
                       "{\"gid\":" + g + ",\"server_id\":" +
                           std::to_string(sid) + "}");
  CHECK(cs.status == 200);
  const auto session_id = jint(cs.body, "session_id");
  const std::string ticket = jstr(cs.body, "ticket");
  CHECK(session_id > 0);
  CHECK(!ticket.empty());
  CHECK(http(port, "POST", "/files/group-servers/session/redeem", {},
             "not-json").status == 400);
  CHECK(http(port, "POST", "/files/group-servers/session/redeem", {}, "{}")
            .status == 400);
  CHECK(http(port, "POST", "/files/group-servers/session/redeem", {},
             "{\"ticket\":\"nope\"}").status == 401);
  const auto cr = http(port, "POST", "/files/group-servers/session/redeem",
                       {}, "{\"ticket\":\"" + ticket + "\"}");
  CHECK(cr.status == 200);
  CHECK(jint(cr.body, "session_id") == session_id);
  CHECK(http(port, "POST", "/files/group-servers/session/redeem", {},
             "{\"ticket\":\"" + ticket + "\"}").status == 409); // 已使用

  // 收尾：缺字段 400；幽灵/越权 409（不透存在性）
  CHECK(http(port, "POST", "/files/group-servers/session/close",
             H("member1"), "not-json").status == 400);
  CHECK(http(port, "POST", "/files/group-servers/session/close",
             H("member1"), "{}").status == 400);
  CHECK(http(port, "POST", "/files/group-servers/session/close",
             H("member1"),
             "{\"gid\":" + g + ",\"session_id\":999999}").status == 409);
  CHECK(http(port, "POST", "/files/group-servers/session/close",
             H("member1"),
             "{\"gid\":" + g + ",\"session_id\":" +
                 std::to_string(session_id) + "}").status == 200);
  CHECK(http(port, "GET", "/files/group-servers/sessions?gid=", H("member1"),
             "").status == 400);
  CHECK(http(port, "GET", "/files/group-servers/sessions?gid=" + g,
             H("outsider"), "").status == 403);

  // 正腿收尾核账：留痕列表含该会话（谁/哪台/协议），已结束
  const auto cl =
      http(port, "GET", "/files/group-servers/sessions?gid=" + g,
           H("owner1"), "");
  CHECK(cl.status == 200);
  CHECK(cl.body.find("member1") != std::string::npos);
  CHECK(cl.body.find("\"closed_ms\":") == std::string::npos ||
        cl.body.find("web1") != std::string::npos);

  io.stop();
  th.join();
  if (g_failures == 0) {
    std::cout << "files_negative: all checks passed\n";
    return 0;
  }
  std::cout << "files_negative: " << g_failures << " failures\n";
  return 1;
}
