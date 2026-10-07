// R26-1 memex agent 验收：真 agent 进程 × in-process FileServer（真 TCP）。
// 群管理员登记签发令牌 → agent --once 一拍心跳（exit 0）→ 列表见绿灯＋
// 指标落账＋令牌摘要不出现；错令牌/连不上一拍即败（exit 1）；缺参
// exit 2。登记判权/心跳鉴权/列表判权矩阵与轮换语义走 test_files_api
// 服务端腿，不在此重复。
#include <asio.hpp>

#include <sys/wait.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "files_server.hpp"
#include "store.hpp"

using memex::server::ServerStore;

#ifndef MEMEX_AGENT_BIN
#error "MEMEX_AGENT_BIN 未定义（应传入 $<TARGET_FILE:memex_agent>）"
#endif

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
  rep.status = std::atoi(raw.c_str() + raw.find(' ') + 1);
  rep.body = raw.substr(head_end + 4);
  return rep;
}

int run_agent(const std::string& args) {
  const std::string cmd =
      std::string("\"") + MEMEX_AGENT_BIN + "\" " + args + " > /dev/null 2>&1";
  const int rc = std::system(cmd.c_str());
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

} // namespace

int main() {
  asio::io_context io;
  ServerStore store;
  CHECK(store.open(":memory:"));
  CHECK(store.create_account("owner1", "pw-owner1", "owner1"));
  CHECK(store.create_account("member1", "pw-member1", "member1"));
  const auto gid = store.create_group("ops", "owner1", {"owner1", "member1"});
  CHECK(gid > 0);
  const std::string gids = std::to_string(gid);

  memex::server::FileServer files(io, store, nullptr, 0);
  const std::uint16_t port = files.port();
  files.start_accept();
  std::thread th([&io] { io.run(); });

  const auto H = [](const std::string& t) {
    return std::map<std::string, std::string>{
        {"Authorization", "Bearer " + t}};
  };
  const auto ot = http(port, "POST", "/files/session", {},
                       "{\"account\":\"owner1\",\"password\":\"pw-owner1\"}");
  CHECK(ot.status == 200);
  const std::string owner_tok = jstr(ot.body, "token");
  CHECK(!owner_tok.empty());

  // 登记（群主）：签发一次性注册令牌
  const auto en = http(port, "POST", "/files/group-servers/enroll",
                       H(owner_tok),
                       "{\"gid\":" + gids +
                           ",\"name\":\"web-1\",\"host\":\"10.0.0.9\"}");
  CHECK(en.status == 200);
  const std::string token = jstr(en.body, "token");
  CHECK(!token.empty());

  // agent --once 正拍：exit 0，列表见绿灯＋真实指标（内存/磁盘非占位 0）
  CHECK(run_agent("--server 127.0.0.1:" + std::to_string(port) +
                  " --token " + token + " --once") == 0);
  const auto sl = http(port, "GET",
                       "/files/group-servers/list?gid=" + gids,
                       H(owner_tok), "");
  CHECK(sl.status == 200);
  CHECK(sl.body.find("\"name\":\"web-1\"") != std::string::npos);
  CHECK(sl.body.find("\"online\":true") != std::string::npos);
  CHECK(sl.body.find("\"mem_total_mb\":0") == std::string::npos);
  CHECK(sl.body.find("\"disk_total_mb\":0") == std::string::npos);
  CHECK(sl.body.find("token") == std::string::npos);

  // 错令牌：一拍即败 exit 1（服务端 401）
  CHECK(run_agent("--server 127.0.0.1:" + std::to_string(port) +
                  " --token deadbeef --once") == 1);
  // 连不上：一拍即败 exit 1（连接拒绝）
  CHECK(run_agent("--server 127.0.0.1:1 --token " + token + " --once") == 1);
  // 缺参：exit 2（--server/--token 缺一不可；缺 --once 合法＝常驻，
  // 不在此测——会常驻）
  CHECK(run_agent("--token x") == 2);
  CHECK(run_agent("--server 127.0.0.1:" + std::to_string(port)) == 2);
  io.stop();
  th.join();

  if (g_failures == 0) {
    std::cout << "test_agent: all checks passed\n";
    return 0;
  }
  std::cout << "test_agent: " << g_failures << " check(s) FAILED\n";
  return 1;
}
