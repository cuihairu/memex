// T2.1 验收：账号表＋密码摘要登录、设备指纹留档、登录记录全量可查、
// 桌面端单点在线互踢（第二台登录后第一台收到 KICK 下线提示）。
// T3.3 设备台账：首登自动建档、责任人登记、停用拒绝登录（启停即时生效）、
// 启用恢复、解绑（清责任人并停用）、登录记录按设备过滤；CLI 进程级验证。
// 服务端核心库直链运行（io 线程驱动，不起进程）。
#include <asio.hpp>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"
#include "server.hpp"
#include "store.hpp"

using asio::ip::tcp;
using memex::server::CollabServer;
using memex::server::ServerStore;

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

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 阻塞式测试客户端：帧收发（长度前缀 + Envelope）
class TestClient {
public:
  TestClient(asio::io_context& io, std::uint16_t port) {
    socket_ = std::make_unique<tcp::socket>(io);
    socket_->connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
  }

  void send(const memex::protocol::Message& msg) {
    const std::string frame = memex::protocol::encode(msg);
    asio::write(*socket_, asio::buffer(frame));
  }

  // T4.3 在线推送（PRESENCE_DATA）与本文件验收特性无关，自动跳过——
  // 在线表语义由 test_read_presence 显式验收。
  memex::protocol::Message read() {
    for (;;) {
      std::array<char, 4> head{};
      asio::read(*socket_, asio::buffer(head));
      const std::uint32_t len = (std::uint8_t(head[0]) << 24) |
                                (std::uint8_t(head[1]) << 16) |
                                (std::uint8_t(head[2]) << 8) |
                                std::uint8_t(head[3]);
      CHECK(len > 0 && len < memex::protocol::kMaxFrameSize);
      std::string payload(len, '\0');
      asio::read(*socket_, asio::buffer(payload));
      auto msg = memex::protocol::decode_payload(payload);
      if (msg.type() != memex::protocol::v1::PRESENCE_DATA && msg.type() != memex::protocol::v1::FAV_DATA) return msg;
    }
  }

  // 读到 LOGIN_RESULT 为止（T4.3 起成功登录伴随 PRESENCE_DATA 推送，
  // 先于回执；失败登录无推送，直接回回执——两种都兼容）
  memex::protocol::Message read_login_result() {
    for (int i = 0; i < 8; ++i) {
      auto r = read();
      if (r.type() == memex::protocol::v1::LOGIN_RESULT) return r;
      CHECK(r.type() == memex::protocol::v1::PRESENCE_DATA);
    }
    CHECK(false);
    return read();
  }

  // 对端关闭后读到 EOF（asio::read 抛 system_error）
  bool closed_after_kick() {
    try {
      std::array<char, 4> head{};
      asio::read(*socket_, asio::buffer(head));
      return false; // 还有数据，不算关闭
    } catch (const std::system_error&) {
      return true;
    }
  }

private:
  std::unique_ptr<tcp::socket> socket_;
};

memex::protocol::Message make_login(const std::string& account,
                                    const std::string& password,
                                    const std::string& device_name,
                                    const std::string& kind = "desktop") {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::LOGIN);
  m.set_from(device_name);
  m.set_to("server");
  m.set_ts_ms(now_ms());
  auto* in = m.mutable_login();
  in->set_account(account);
  in->set_password(password);
  in->set_device_fingerprint(memex::server::sha256_hex(device_name));
  in->set_device_kind(kind);
  in->set_device_name(device_name);
  in->set_client_version("0.1.0-test");
  return m;
}

void test_store_basics(ServerStore& store) {
  CHECK(store.create_account("alice", "pass-123", "Alice"));
  CHECK(!store.create_account("alice", "another", "Alice2")); // 幂等拒绝
  const auto row = store.find_account("alice");
  CHECK(row.has_value());
  CHECK(row->display_name == "Alice");
  CHECK(row->salt_hex.size() == 32);           // 16 字节盐 hex
  CHECK(row->digest_hex != "pass-123");        // 不落明文
  CHECK(row->digest_hex.size() == 64);         // SHA-256 hex
  // 摘要可复算（登录校验路径的存储半边）
  CHECK(memex::server::pbkdf2_sha256_hex("pass-123", row->salt_hex, 60000) ==
        row->digest_hex);
  CHECK(!store.find_account("nobody").has_value());
}

void test_login_and_kick(ServerStore& store) {
  store.create_account("bob", "secret-9", "Bob");

  asio::io_context io;
  CollabServer server(io, store, 0); // 系统分配端口
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  // 第一台桌面登录成功（推送＋回执一起吃）
  TestClient a(io, port);
  a.send(make_login("bob", "secret-9", "workstation-A"));
  const auto r1 = a.read_login_result();
  CHECK(r1.type() == memex::protocol::v1::LOGIN_RESULT);
  CHECK(r1.has_login_result());
  CHECK(r1.login_result().ok());
  CHECK(r1.login_result().display_name() == "Bob");

  // 心跳通路仍可用（登录态下）
  memex::protocol::Message ping;
  ping.set_type(memex::protocol::v1::PING);
  ping.set_from("bob");
  ping.set_seq(7);
  a.send(ping);
  const auto pong = a.read();
  CHECK(pong.type() == memex::protocol::v1::PONG);
  CHECK(pong.seq() == 7);

  // 第二台同账号桌面登录：第一台被原子化踢出并收到 KICK 提示
  //（上线推送由 read() 自动跳过，此处直达 KICK）
  TestClient b(io, port);
  b.send(make_login("bob", "secret-9", "workstation-B"));
  const auto r2 = b.read_login_result();
  CHECK(r2.login_result().ok());
  const auto kick = a.read();
  CHECK(kick.type() == memex::protocol::v1::KICK);
  CHECK(kick.has_kick());
  CHECK(kick.kick().reason().find("单点在线") != std::string::npos);
  CHECK(kick.kick().replaced_by() == "workstation-B");
  CHECK(a.closed_after_kick());

  // 被踢后第二台仍在线（心跳可达），旧连接不再顶替
  b.send(ping);
  CHECK(b.read().type() == memex::protocol::v1::PONG);

  // 口令不符：拒绝且留痕
  TestClient c(io, port);
  c.send(make_login("bob", "wrong", "workstation-C"));
  const auto r3 = c.read_login_result();
  CHECK(!r3.login_result().ok());
  CHECK(r3.login_result().reason() == "口令不符");

  // 不存在的账号：拒绝且留痕
  TestClient d(io, port);
  d.send(make_login("ghost", "x", "workstation-D"));
  const auto r4 = d.read_login_result();
  CHECK(!r4.login_result().ok());
  CHECK(r4.login_result().reason() == "账号不存在");

  // 登录记录全量可查：bob 共 4 条（成功×2、口令不符、账号不存在属于 ghost）
  const auto rows = store.login_records("bob");
  CHECK(rows.size() == 3);
  int ok_count = 0, bad_count = 0;
  for (const auto& r : rows) {
    CHECK(r.account == "bob");
    CHECK(r.source_ip == "127.0.0.1");
    CHECK(r.version == "0.1.0-test");
    CHECK(!r.fingerprint.empty());
    if (r.result == "ok") {
      ++ok_count;
      CHECK(r.kind == "desktop");
      CHECK((r.name == "workstation-A" || r.name == "workstation-B"));
    } else {
      ++bad_count;
      CHECK(r.result == "bad_password");
    }
  }
  CHECK(ok_count == 2);
  CHECK(bad_count == 1);
  const auto ghost_rows = store.login_records("ghost");
  CHECK(ghost_rows.size() == 1);
  CHECK(ghost_rows[0].result == "no_account");

  io.stop();
  io_thread.join();
}

// —— T3.3 设备台账（库级＋协议级＋CLI 级）——
void test_devices(const std::string& db_path) {
  std::remove(db_path.c_str());
  ServerStore store;
  CHECK(store.open(db_path));
  CHECK(store.create_account("alice", "pass-123", "Alice"));

  asio::io_context io;
  CollabServer server(io, store, 0);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  const std::string fp = memex::server::sha256_hex("pc-dev1");
  // 首登：台账自动建档（指纹／类型／名称／启用态）
  {
    TestClient c(io, port);
    c.send(make_login("alice", "pass-123", "pc-dev1"));
    CHECK(c.read_login_result().login_result().ok());
  }
  auto devices = store.device_list();
  CHECK(devices.size() == 1);
  if (!devices.empty()) {
    CHECK(devices[0].fingerprint == fp);
    CHECK(devices[0].kind == "desktop");
    CHECK(devices[0].name == "pc-dev1");
    CHECK(devices[0].enabled);
    CHECK(devices[0].owner_account.empty()); // 未登记
  }

  // 责任人登记（账号不存在拒绝）
  CHECK(store.set_device_owner(fp, "alice"));
  CHECK(!store.set_device_owner(fp, "ghost"));
  CHECK(store.find_device(fp)->owner_account == "alice");

  // 停用：登录即拒、留痕 device_disabled
  CHECK(store.set_device_enabled(fp, false));
  {
    TestClient c(io, port);
    c.send(make_login("alice", "pass-123", "pc-dev1"));
    const auto r = c.read_login_result();
    CHECK(!r.login_result().ok());
    CHECK(r.login_result().reason().find("设备已停用") != std::string::npos);
  }
  // 登录记录按设备（指纹前缀）过滤：最新一条即被拒记录
  const auto dev_rows = store.login_records("", fp.substr(0, 12));
  CHECK(dev_rows.size() >= 2);
  if (!dev_rows.empty()) CHECK(dev_rows[0].result == "device_disabled");

  // 指纹前缀定位：命中／过短拒绝
  const auto [hit, ambiguous] = store.device_by_prefix(fp.substr(0, 10));
  CHECK(!ambiguous && hit == fp);
  const auto [short_hit, short_ambiguous] = store.device_by_prefix(fp.substr(0, 4));
  CHECK(short_hit.empty() && !short_ambiguous);

  // 启用：登录恢复
  CHECK(store.set_device_enabled(fp, true));
  {
    TestClient c(io, port);
    c.send(make_login("alice", "pass-123", "pc-dev1"));
    CHECK(c.read_login_result().login_result().ok());
  }

  // 解绑：清责任人并停用 → 再拒
  CHECK(store.unbind_device(fp));
  const auto after_unbind = store.find_device(fp);
  CHECK(after_unbind.has_value());
  CHECK(after_unbind->owner_account.empty());
  CHECK(!after_unbind->enabled);
  {
    TestClient c(io, port);
    c.send(make_login("alice", "pass-123", "pc-dev1"));
    CHECK(!c.read_login_result().login_result().ok());
  }

  io.stop();
  io_thread.join();
  store.close();

  // CLI 级：台账面／责任人登记／启用／登录记录按设备过滤
  const auto run_cli = [](const std::string& args) {
    const std::string out_path = "/tmp/memex-accounts-test-out.txt";
    const std::string cmd = std::string("\"" MEMEX_SERVER_BIN "\" ") + args +
                            " > " + out_path + " 2>&1";
    const int rc = std::system(cmd.c_str());
    (void)rc;
    std::ifstream f(out_path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
  };
  const std::string prefix = fp.substr(0, 12);
  const std::string listing = run_cli("device list --db " + db_path);
  CHECK(listing.find("pc-dev1") != std::string::npos);
  CHECK(listing.find("停用") != std::string::npos); // 解绑后为停用态
  CHECK(run_cli("device set " + prefix + " --owner alice --db " + db_path)
            .find("已登记责任人") != std::string::npos);
  CHECK(run_cli("device enable " + prefix + " --db " + db_path)
            .find("已启用") != std::string::npos);
  CHECK(run_cli("device list --db " + db_path).find("alice") !=
        std::string::npos);
  CHECK(run_cli("logins --device " + prefix + " --db " + db_path)
            .find("device_disabled") != std::string::npos);
  // 坏用法：过短前缀拒绝
  CHECK(run_cli("device disable ab --db " + db_path).find("过短") !=
        std::string::npos);
}

} // namespace

int main() {
  memex::server::ServerStore store;
  CHECK(store.open(":memory:"));
  test_store_basics(store);

  memex::server::ServerStore store2;
  CHECK(store2.open(":memory:"));
  test_login_and_kick(store2);

  test_devices("/tmp/memex-accounts-devices.db");

  if (g_failures == 0) {
    std::cout << "accounts tests: all passed\n";
    return 0;
  }
  std::cerr << "accounts tests: " << g_failures << " failure(s)\n";
  return 1;
}
