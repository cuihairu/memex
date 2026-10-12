// R27-5 任务源拉取验收：三道闸逐腿（开关关/主密钥缺/单源关＝零外呼）、
// 拉取链（落库 provider/ext_key 口径、新落与变化 notice、无变化不推、
// done 不被拉取回写、水印落账）、凭据密封面（GCM 往返/错钥拒/掩码回包
// 无明文无密文）、路由判权（401/400/503/本人行隔离/撤源）、真传输腿
//（drogon HttpClient × 本地 asio http 桩——不打外网；deadline 到点收口）。
#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cred.hpp"
#include "files_server.hpp"
#include "store.hpp"
#include "task_pull.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ' ' << #cond  \
                << '\n';                                                   \
      ++g_failures;                                                        \
    }                                                                      \
  } while (false)

using memex::server::ServerStore;

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// —— 假传输：记账不打外网（链路腿；真传输另有本地桩腿） ——
class FakeTaskHttp final : public memex::server::TaskHttp {
 public:
  struct Call {
    std::string url;
    memex::server::TaskHttp::Headers headers;
  };
  std::vector<Call> calls;
  std::string body = "[]";
  int status = 200;
  bool ok = true;

  void get(const std::string& url, const Headers& h, int,
           ResponseFn done) override {
    calls.push_back({url, h});
    done(ok, status, body);
  }
};

std::string header_of(const memex::server::TaskHttp::Headers& hs,
                      const char* key) {
  for (const auto& [k, v] : hs) {
    if (k == key) return v;
  }
  return "";
}

// canned GitHub issues JSON：2 条 issue＋1 条 PR（滤除）＋1 条空标题（滤除）
const char* kIssues =
    "[{\"number\":1,\"title\":\"Fix login\",\"body\":\"step one\"},"
    " {\"number\":2,\"title\":\"Add dark mode\",\"body\":null},"
    " {\"number\":3,\"title\":\"PR row\",\"body\":\"x\","
    "\"pull_request\":{\"url\":\"u\"}},"
    " {\"number\":4,\"title\":\"\",\"body\":\"y\"}]";

// —— 阻塞 HTTP 客户端（真 TCP；服务端 Connection: close） ——
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
  rep.status = std::atoi(raw.substr(9, 3).c_str());
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

bool jbool(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto pos = j.find(k);
  return pos != std::string::npos && j.compare(pos + k.size(), 4, "true") == 0;
}

} // namespace

int main() {
  const std::int64_t ts = now_ms();
  // —— 凭据密钥与 GCM 面（密码箱同水位派生）——
  CHECK(memex::server::derive_task_pull_key("").empty()); // 未配=未启用
  const std::string key = memex::server::derive_task_pull_key("deploy-secret");
  CHECK(key.size() == 32);
  CHECK(key == memex::server::derive_task_pull_key("deploy-secret")); // 确定
  {
    const std::string sealed = memex::server::gcm_seal(key, "gh-token-123");
    CHECK(!sealed.empty());
    CHECK(sealed.find("gh-token-123") == std::string::npos); // 无明文
    CHECK(memex::server::gcm_open(key, sealed) == "gh-token-123");
    CHECK(memex::server::gcm_open(
              memex::server::derive_task_pull_key("other"), sealed)
              .empty()); // 错钥拒
  }

  ServerStore store;
  CHECK(store.open(":memory:"));
  for (const char* a : {"alice", "bob"}) {
    CHECK(store.create_account(a, "pw-" + std::string(a), a));
  }

  // —— store 面：任务源配置/凭据/开关/外部任务 upsert ——
  ServerStore::TaskPullConfig c;
  c.owner = "alice";
  c.provider = "github";
  c.repo = "acme/api";
  c.enabled = true;
  c.interval_s = 0; // 0=每拍都拉（测试口径）
  c.updated_ms = ts;
  CHECK(store.task_pull_config_set(c));
  {
    auto all = store.task_pull_configs();
    CHECK(all.size() == 1);
    CHECK(all[0].owner == "alice" && all[0].repo == "acme/api");
    // 覆盖语义：repo/enabled/interval 整行换，last_pull 不动
    c.repo = "acme/other";
    c.enabled = false;
    CHECK(store.task_pull_config_set(c));
    all = store.task_pull_configs();
    CHECK(all.size() == 1 && !all[0].enabled && all[0].repo == "acme/other");
    c.repo = "acme/api";
    c.enabled = true;
    CHECK(store.task_pull_config_set(c));
  }
  CHECK(store.task_pull_config_mark_pulled("alice", "github", ts + 5));
  CHECK(store.task_pull_configs()[0].last_pull_ms == ts + 5);
  CHECK(!store.task_pull_config_delete("bob", "github")); // 幽灵拒
  CHECK(store.task_pull_config_delete("alice", "github"));
  CHECK(store.task_pull_configs().empty());
  CHECK(store.task_pull_config_set(c)); // 后续腿用

  CHECK(!store.server_flag_get("task_pull_enabled").has_value()); // 缺行=关
  CHECK(store.server_flag_set("task_pull_enabled", "1", ts));
  CHECK(store.server_flag_get("task_pull_enabled").has_value());
  CHECK(*store.server_flag_get("task_pull_enabled") == "1");
  CHECK(store.server_flag_set("task_pull_enabled", "0", ts));
  CHECK(*store.server_flag_get("task_pull_enabled") == "0"); // 覆盖
  store.server_flag_set("task_pull_enabled", "1", ts);

  const std::string sealed = memex::server::gcm_seal(key, "gh-token-123");
  CHECK(store.task_source_cred_set("alice", "github", sealed, ts));
  CHECK(store.task_source_cred_sealed("alice", "github").has_value());
  CHECK(*store.task_source_cred_sealed("alice", "github") == sealed);
  CHECK(memex::server::gcm_open(
            key, *store.task_source_cred_sealed("alice", "github")) ==
        "gh-token-123"); // 库里密文可开封＝落库口径对

  // 外部任务 upsert：建→同内容不动→变化更新→done 不被回写
  {
    const auto r1 = store.task_upsert_external("alice", "Fix login",
                                               "step one", "github", "1", ts);
    CHECK(r1.created && r1.changed && r1.id > 0);
    const auto r2 = store.task_upsert_external("alice", "Fix login",
                                               "step one", "github", "1", ts);
    CHECK(!r2.created && !r2.changed && r2.id == r1.id);
    const auto r3 = store.task_upsert_external("alice", "Fix login v2", "x",
                                               "github", "1", ts);
    CHECK(!r3.created && r3.changed && r3.id == r1.id);
    CHECK(store.task_set_done(r1.id, true, ts));
    const auto r4 = store.task_upsert_external("alice", "Fix login v2", "x",
                                               "github", "1", ts);
    CHECK(!r4.created && !r4.changed); // 无变化不动（更不碰 done）
    bool done_kept = false;
    for (const auto& t : store.tasks_of("alice")) {
      if (t.id == r1.id) done_kept = t.done;
    }
    CHECK(done_kept);
    store.task_delete(r1.id);
  }

  // —— GitHub 解析：滤 PR/空标题、编号即键、body 可空 ——
  {
    const auto tasks = memex::server::parse_github_issues(kIssues);
    CHECK(tasks.size() == 2);
    CHECK(tasks[0].ext_key == "1" && tasks[0].title == "Fix login" &&
          tasks[0].note == "step one");
    CHECK(tasks[1].ext_key == "2" && tasks[1].note.empty());
    CHECK(memex::server::parse_github_issues("not json").empty());
    CHECK(memex::server::parse_github_issues("{\"a\":1}").empty());
  }

  // —— 拉取器：三道闸与拉取链（假传输记账） ——
  asio::io_context io;
  auto fake = std::make_unique<FakeTaskHttp>();
  FakeTaskHttp* wire = fake.get();
  memex::server::TaskPuller puller(io, store);
  puller.set_key(key);
  puller.set_http(std::move(fake));
  std::vector<std::string> notices; // "target|title|content|urgency"
  puller.set_notice([&notices](const std::string& target,
                               const std::string& title,
                               const std::string& content, int urgency) {
    notices.push_back(target + "|" + title + "|" + content + "|" +
                      std::to_string(urgency));
  });

  // 闸②：开关关＝零外呼（配置在场也不拉）
  store.server_flag_set("task_pull_enabled", "0", ts);
  puller.tick();
  CHECK(wire->calls.empty());
  // 闸①：主密钥缺＝零外呼
  store.server_flag_set("task_pull_enabled", "1", ts);
  {
    memex::server::TaskPuller keyless(io, store);
    auto f2 = std::make_unique<FakeTaskHttp>();
    FakeTaskHttp* w2 = f2.get();
    keyless.set_http(std::move(f2));
    keyless.tick();
    CHECK(w2->calls.empty());
  }
  // 闸③：单源关＝跳过
  c.enabled = false;
  store.task_pull_config_set(c);
  puller.tick();
  CHECK(wire->calls.empty());
  c.enabled = true;
  store.task_pull_config_set(c);

  // 正链路：拉取→落库（provider/ext_key）→新落 notice→水印
  wire->body = kIssues; // 假传输上料（默认 "[]"＝空页）
  puller.tick();
  CHECK(wire->calls.size() == 1);
  CHECK(wire->calls[0].url == "https://api.github.com/repos/acme/api/"
                              "issues?state=open&per_page=50");
  CHECK(header_of(wire->calls[0].headers, "Authorization") ==
        "Bearer gh-token-123"); // 凭据内存内解密进头
  CHECK(header_of(wire->calls[0].headers, "Accept") ==
        "application/vnd.github+json");
  auto mine = store.tasks_of("alice");
  CHECK(mine.size() == 2);
  {
    bool has1 = false, has2 = false;
    for (const auto& t : mine) {
      CHECK(t.provider == "github");
      if (t.ext_key == "1") has1 = true;
      if (t.ext_key == "2") has2 = true;
    }
    CHECK(has1 && has2);
  }
  CHECK(notices.size() == 2); // 两条新落各一条
  CHECK(notices[0].find("alice|") == 0);
  CHECK(notices[0].find("GitHub") != std::string::npos);
  CHECK(store.task_pull_configs()[0].last_pull_ms > 0); // 水印落账

  // 复拍：同内容＝无变化不推（去重）
  puller.tick();
  CHECK(wire->calls.size() == 2);
  CHECK(notices.size() == 2);

  // 变化腿：#1 标题变了→只推它一笔；先前已勾完成不被拉取回写
  {
    std::int64_t done_id = 0;
    for (const auto& t : mine) {
      if (t.ext_key == "1") done_id = t.id;
    }
    CHECK(store.task_set_done(done_id, true, ts));
  }
  wire->body =
      "[{\"number\":1,\"title\":\"Fix login v2\",\"body\":\"step one\"},"
      " {\"number\":2,\"title\":\"Add dark mode\",\"body\":null}]";
  puller.tick();
  CHECK(wire->calls.size() == 3);
  CHECK(notices.size() == 3); // #2 无变化不推；#1 变化推一笔
  mine = store.tasks_of("alice");
  for (const auto& t : mine) {
    if (t.ext_key == "1") {
      CHECK(t.title == "Fix login v2");
      CHECK(t.done); // 已勾完成不被拉取回写
    }
    if (t.ext_key == "2") CHECK(t.title == "Add dark mode");
  }

  // 凭据开封失败＝无凭据裸拉（Authorization 头不出现——不泄漏残缺密文）
  {
    ServerStore::TaskPullConfig cb;
    cb.owner = "bob";
    cb.provider = "github";
    cb.repo = "acme/web";
    cb.enabled = true;
    cb.interval_s = 0;
    cb.updated_ms = ts;
    CHECK(store.task_pull_config_set(cb));
    CHECK(store.task_source_cred_set("bob", "github", "deadbeef", ts));
    puller.tick();
    // alice 每拍都拉（interval_s=0）＋bob 一笔＝5
    CHECK(wire->calls.size() == 5);
    // 按源对号（不依赖列表序）：开封失败＝无 Authorization 头（不泄漏残缺
    // 密文）；好凭据照常进头
    std::size_t bob_idx = 0, alice_idx = 0;
    for (std::size_t i = 0; i < wire->calls.size(); ++i) {
      if (wire->calls[i].url.find("acme/web") != std::string::npos)
        bob_idx = i;
      if (wire->calls[i].url.find("acme/api") != std::string::npos)
        alice_idx = i;
    }
    CHECK(header_of(wire->calls[bob_idx].headers, "Authorization").empty());
    CHECK(header_of(wire->calls[alice_idx].headers, "Authorization") ==
          "Bearer gh-token-123");
    store.task_pull_config_delete("bob", "github");
    store.task_source_cred_delete("bob", "github");
  }

  // 未接 provider＝拉取器侧跳过（配置面已 400 拦，此为第二道）
  {
    ServerStore::TaskPullConfig cg;
    cg.owner = "alice";
    cg.provider = "gitlab";
    cg.repo = "acme/x";
    cg.enabled = true;
    cg.interval_s = 0;
    cg.updated_ms = ts;
    CHECK(store.task_pull_config_set(cg));
    puller.tick();
    CHECK(wire->calls.size() == 6); // gitlab 跳过，github 照拉
    store.task_pull_config_delete("alice", "gitlab");
  }

  // interval 水位：interval_s>0 且未到期＝跳过
  {
    ServerStore::TaskPullConfig ci = c;
    ci.interval_s = 3600;
    CHECK(store.task_pull_config_set(ci));
    const std::size_t before = wire->calls.size();
    puller.tick();
    CHECK(wire->calls.size() == before); // 刚拉过＝未到期
    store.task_pull_config_delete("alice", "github");
  }

  // —— 路由腿：真 TCP × FileServer（有/无主密钥两实例共享会话库） ——
  const auto s3_null = std::shared_ptr<memex::server::S3Storage>{};
  const auto shared_sessions = memex::server::make_file_sessions();
  memex::server::FileServer files(io, store, s3_null, 0,
                                  /*uplink_mode=*/false, shared_sessions);
  files.set_task_pull_secret("deploy-secret");
  const std::uint16_t fport = files.port();
  files.start_accept();
  memex::server::FileServer nokey(io, store, s3_null, 0,
                                  /*uplink_mode=*/false, shared_sessions);
  const std::uint16_t nkport = nokey.port();
  nokey.start_accept();
  std::thread th([&io] { io.run(); });

  std::map<std::string, std::string> tok;
  for (const char* a : {"alice", "bob"}) {
    const auto r =
        http(fport, "POST", "/files/session", {},
             "{\"account\":\"" + std::string(a) + "\",\"password\":\"pw-" +
                 std::string(a) + "\"}");
    CHECK(r.status == 200);
    tok[a] = jstr(r.body, "token");
    CHECK(!tok[a].empty());
  }
  const auto H = [&tok](const char* a) {
    return std::map<std::string, std::string>{
        {"Authorization", "Bearer " + tok.at(a)}};
  };

  // 无主密钥实例：整面 503（配置/凭据面一并拒绝——不静默存明文）
  CHECK(http(nkport, "POST", "/files/task-sources", H("alice"),
             "{\"provider\":\"github\",\"repo\":\"a/b\"}").status == 503);
  CHECK(http(nkport, "GET", "/files/task-sources", H("alice"), "").status ==
        503);

  // 401 无会话；400 家族
  CHECK(http(fport, "POST", "/files/task-sources", {},
             "{\"provider\":\"github\",\"repo\":\"a/b\"}").status == 401);
  CHECK(http(fport, "POST", "/files/task-sources", H("alice"), "{}").status ==
        400); // 缺 provider/repo
  CHECK(http(fport, "POST", "/files/task-sources", H("alice"),
             "{\"provider\":\"gitlab\",\"repo\":\"a/b\"}").status == 400);
  CHECK(http(fport, "POST", "/files/task-sources", H("alice"),
             "{\"provider\":\"github\",\"repo\":\"justname\"}").status == 400);
  CHECK(http(fport, "POST", "/files/task-sources", H("alice"),
             "{\"provider\":\"github\",\"repo\":\"a/b\",\"interval_s\":-1}")
            .status == 400);
  CHECK(http(fport, "GET", "/files/task-sources", {}, "").status == 401);

  // 正链路：设源带凭据→列表掩码（只见 cred_configured 布尔）→清凭据→撤源
  {
    const auto r = http(fport, "POST", "/files/task-sources", H("bob"),
                        "{\"provider\":\"github\",\"repo\":\"acme/web\","
                        "\"secret\":\"bob-token-1\",\"interval_s\":60}");
    CHECK(r.status == 200);
    const auto lst = http(fport, "GET", "/files/task-sources", H("bob"), "");
    CHECK(lst.status == 200);
    CHECK(jstr(lst.body, "repo") == "acme/web");
    CHECK(jbool(lst.body, "cred_configured"));
    CHECK(lst.body.find("bob-token-1") == std::string::npos); // 明文不出
    CHECK(lst.body.find("sealed_hex") == std::string::npos);  // 密文不出
    // 本人行隔离：alice 看不到 bob 的源
    const auto la = http(fport, "GET", "/files/task-sources", H("alice"), "");
    CHECK(la.status == 200);
    CHECK(la.body.find("acme/web") == std::string::npos);
    // 清凭据（掩码翻未配置）
    const auto cl =
        http(fport, "POST", "/files/task-sources", H("bob"),
             "{\"provider\":\"github\",\"repo\":\"acme/web\","
             "\"clear_secret\":true}");
    CHECK(cl.status == 200);
    const auto lst2 = http(fport, "GET", "/files/task-sources", H("bob"), "");
    CHECK(!jbool(lst2.body, "cred_configured"));
    CHECK(lst2.body.find("\"interval_s\":60") != std::string::npos);
    // 撤源；再撤 404
    CHECK(http(fport, "DELETE", "/files/task-sources?provider=github", H("bob"),
               "").status == 200);
    CHECK(store.task_source_cred_sealed("bob", "github").has_value() == false);
    CHECK(http(fport, "DELETE", "/files/task-sources?provider=github", H("bob"),
               "").status == 404);
  }

  // —— 真传输腿：drogon HttpClient × 本地 asio http 桩（不打外网） ——
  {
    asio::io_context tio;
    asio::ip::tcp::acceptor acc(
        tio, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t tport = acc.local_endpoint().port();
    const std::string canned = kIssues;
    std::thread srv([&acc, &canned] {
      asio::ip::tcp::socket s(acc.get_executor());
      asio::error_code ec;
      if (acc.accept(s, ec)) return;
      char buf[4096];
      (void)s.read_some(asio::buffer(buf), ec); // GET 请求头（一包内）
      const std::string resp =
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
          "Content-Length: " +
          std::to_string(canned.size()) + "\r\nConnection: close\r\n\r\n" +
          canned;
      asio::write(s, asio::buffer(resp), ec);
      s.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    });
    auto real_http = memex::server::make_task_http(tio);
    // 桩 acceptor 用阻塞 accept＝无在途 async_accept＝run 会空转即退：
    // work guard 兜住循环，等 drogon 回包 post 进来（收尾 stop 释放）
    const auto guard = asio::make_work_guard(tio);
    std::thread tth([&tio] { tio.run(); });
    struct R {
      bool ok{false};
      int status{0};
      std::string body;
    };
    R got;
    std::atomic<bool> done{false};
    real_http->get("http://127.0.0.1:" + std::to_string(tport) +
                       "/repos/acme/api/issues?state=open&per_page=50",
                   {{"Accept", "application/vnd.github+json"}}, 10000,
                   [&](bool ok, int status, std::string body) {
                     got = {ok, status, std::move(body)};
                     done = true;
                   });
    while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(got.ok);
    CHECK(got.status == 200);
    CHECK(got.body == canned);
    CHECK(memex::server::parse_github_issues(got.body).size() == 2);
    // deadline 到点收口：桩收下请求不回包——1.2s 后 ok=false（不拖链）
    std::atomic<bool> done2{false};
    std::atomic<bool> hung{true};
    std::thread srv2([&acc, &hung] {
      asio::ip::tcp::socket s(acc.get_executor());
      asio::error_code ec;
      if (acc.accept(s, ec)) return;
      char buf[4096];
      (void)s.read_some(asio::buffer(buf), ec);
      while (hung) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    const auto t0 = std::chrono::steady_clock::now();
    real_http->get("http://127.0.0.1:" + std::to_string(tport) + "/slow", {},
                   1200,
                   [&](bool ok, int, std::string) {
                     const auto ms = std::chrono::duration_cast<
                         std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - t0);
                     CHECK(!ok);
                     CHECK(ms.count() < 5000); // 到点收口而非干等
                     done2 = true;
                   });
    while (!done2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    hung = false;
    srv2.join();
    tio.stop();
    tth.join();
    srv.join();
  }

  io.stop(); // acceptor 常挂＝io.run 不自行返回（同 files_api/uplink 收尾）
  th.join();
  if (g_failures == 0) {
    std::cout << "test_task_pull: all green\n";
    return 0;
  }
  std::cerr << "test_task_pull: " << g_failures << " failures\n";
  return 1;
}
