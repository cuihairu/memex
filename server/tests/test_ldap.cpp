// 544 LDAP/AD 企业身份后端验收：最小客户端（BER bind/search）＋判登路由
// fail-closed＋组→角色显式映射＋目录禁用（新登录拒＋存量会话踢下线）＋
// 首登自动绑定/影子账号（默认全关）＋本地账号常备。
// 假目录＝测试进程内 POSIX 监听（按连接重置的脚本化应答）——真 AD 另批。
// 三类必测用例（LDAP 不可用／组未映射／目录禁用）在本文件腿 6/13/5+16。
#include <sys/socket.h>
#include <unistd.h>

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"
#include "ldap.hpp"
#include "server.hpp"
#include "store.hpp"

using asio::ip::tcp;
using memex::server::AuthRoute;
using memex::server::CollabServer;
using memex::server::LdapConfig;
using memex::server::LdapDirectory;
using memex::server::LdapVerdict;
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

// —— 测试侧 BER 小工具（与 ldap.cpp 内部实现独立重复，互不依赖）——
std::string tlv(unsigned char tag, const std::string& content) {
  std::string len;
  if (content.size() < 0x80) {
    len.assign(1, static_cast<char>(content.size()));
  } else {
    const auto n = static_cast<unsigned char>(content.size());
    len.assign(1, static_cast<char>(0x81));
    len.push_back(static_cast<char>(n));
  }
  return std::string(1, static_cast<char>(tag)) + len + content;
}

struct Ber {
  unsigned char tag{0};
  std::string content;
};

bool ber_node(const std::string& buf, std::size_t& pos, Ber& out) {
  if (pos + 2 > buf.size()) return false;
  out.tag = static_cast<unsigned char>(buf[pos++]);
  const auto first = static_cast<unsigned char>(buf[pos++]);
  std::size_t len = 0;
  if (first < 0x80) {
    len = first;
  } else {
    const int n = first & 0x7F;
    if (n == 0 || n > 4 || pos + static_cast<std::size_t>(n) > buf.size())
      return false;
    for (int i = 0; i < n; ++i)
      len = (len << 8) | static_cast<unsigned char>(buf[pos++]);
  }
  if (pos + len > buf.size()) return false;
  out.content = buf.substr(pos, len);
  pos += len;
  return true;
}

std::vector<Ber> ber_kids(const std::string& content) {
  std::vector<Ber> out;
  std::size_t pos = 0;
  Ber n;
  while (pos < content.size() && ber_node(content, pos, n))
    out.push_back(n);
  return out;
}

// —— 假目录：脚本按连接重置；记录 bind（DN/口令）与 search（base）——
struct FakeDir {
  struct Entry {
    std::string dn;
    std::vector<std::pair<std::string, std::vector<std::string>>> attrs;
  };
  struct BindRec {
    std::string dn;
    std::string password;
  };
  struct SearchRec {
    std::string base;
    std::size_t entry_count{0};
  };

  // 脚本（每次 accept 新连接从头走）
  std::vector<int> bind_codes{0}; // 依次 bind 应答码
  std::vector<std::vector<Entry>> search_replies; // 依次 search 应答条目
  int search_code{0};

  // 记录
  std::mutex mu;
  std::vector<BindRec> binds;
  std::vector<SearchRec> searches;

  int listen_fd{-1};
  std::uint16_t port{0};
  std::thread th;
  std::atomic<bool> stop{false};

  bool start() {
    listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
            0 ||
        ::listen(listen_fd, 4) != 0)
      return false;
    socklen_t slen = sizeof(addr);
    ::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &slen);
    port = ntohs(addr.sin_port);
    th = std::thread([this] { serve_loop(); });
    return true;
  }

  void stop_server() {
    stop = true;
    if (listen_fd >= 0) {
      ::shutdown(listen_fd, SHUT_RDWR);
      ::close(listen_fd);
      listen_fd = -1;
    }
    if (th.joinable()) th.join();
  }

  ~FakeDir() { stop_server(); }

  bool read_msg(int fd, std::string& out) const {
    char head[2] = {0, 0};
    std::size_t got = 0;
    while (got < 2) {
      const ssize_t n = ::recv(fd, head + got, 2 - got, 0);
      if (n <= 0) return false;
      got += static_cast<std::size_t>(n);
    }
    const auto first = static_cast<unsigned char>(head[1]);
    std::size_t len = 0;
    if (first < 0x80) {
      len = first;
    } else {
      const int nb = first & 0x7F;
      if (nb == 0 || nb > 4) return false;
      char lb[4] = {0, 0, 0, 0};
      std::size_t g2 = 0;
      while (g2 < static_cast<std::size_t>(nb)) {
        const ssize_t n = ::recv(fd, lb + g2,
                                 static_cast<std::size_t>(nb) - g2, 0);
        if (n <= 0) return false;
        g2 += static_cast<std::size_t>(n);
      }
      for (int i = 0; i < nb; ++i)
        len = (len << 8) | static_cast<unsigned char>(lb[i]);
    }
    out.assign(head, head + 2);
    std::string body(len, '\0');
    std::size_t g3 = 0;
    while (g3 < len) {
      const ssize_t n = ::recv(fd, body.data() + g3, len - g3, 0);
      if (n <= 0) return false;
      g3 += static_cast<std::size_t>(n);
    }
    out += body;
    return true;
  }

  static bool send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
      if (n <= 0) return false;
      off += static_cast<std::size_t>(n);
    }
    return true;
  }

  static std::string bind_response(std::int64_t id, int code,
                                   const std::string& diag) {
    // SEQUENCE { id, [APPLICATION 1] { ENUM code, DN "", diag } }
    return tlv(0x30, tlv(0x02, std::string(1, static_cast<char>(id))) +
                         tlv(0x61, tlv(0x0A, std::string(
                                                 1, static_cast<char>(code))) +
                                       tlv(0x04, "") + tlv(0x04, diag)));
  }

  static std::string search_entry(std::int64_t id, const Entry& e) {
    std::string attrs;
    for (const auto& [type, vals] : e.attrs) {
      std::string set;
      for (const auto& v : vals) set += tlv(0x04, v);
      attrs += tlv(0x30, tlv(0x04, type) + tlv(0x31, set));
    }
    // SEQUENCE { id, [APPLICATION 4] { DN, attributes } }
    return tlv(0x30, tlv(0x02, std::string(1, static_cast<char>(id))) +
                         tlv(0x64, tlv(0x04, e.dn) + tlv(0x30, attrs)));
  }

  static std::string search_done(std::int64_t id, int code) {
    return tlv(0x30, tlv(0x02, std::string(1, static_cast<char>(id))) +
                         tlv(0x65, tlv(0x0A, std::string(
                                                1, static_cast<char>(code))) +
                                       tlv(0x04, "") + tlv(0x04, "")));
  }

  void serve_conn(int fd) {
    std::size_t bind_idx = 0, search_idx = 0;
    std::string buf;
    for (;;) {
      if (!read_msg(fd, buf)) {
        ::close(fd);
        return;
      }
      std::size_t pos = 0;
      Ber msg;
      if (!ber_node(buf, pos, msg) || msg.tag != 0x30) continue;
      const auto kids = ber_kids(msg.content);
      if (kids.size() < 2) continue;
      const int id = static_cast<int>(
          static_cast<unsigned char>(kids[0].content.empty()
                                         ? '\0'
                                         : kids[0].content[0]));
      const unsigned char op = kids[1].tag;
      if (op == 0x60) { // bindRequest：记录 DN/口令并按脚本应答
        const auto parts = ber_kids(kids[1].content);
        BindRec rec;
        if (parts.size() >= 2) rec.dn = parts[1].content;
        if (parts.size() >= 3) rec.password = parts[2].content;
        int code = 0;
        {
          std::lock_guard<std::mutex> lk(mu);
          code = bind_idx < bind_codes.size() ? bind_codes[bind_idx] : 0;
          ++bind_idx;
          binds.push_back(rec);
        }
        send_all(fd, bind_response(id, code, code == 0 ? "" : "no"));
      } else if (op == 0x63) { // searchRequest：按脚本回条目＋done
        const auto parts = ber_kids(kids[1].content);
        std::vector<Entry> entries;
        int code = search_code;
        {
          std::lock_guard<std::mutex> lk(mu);
          SearchRec rec;
          if (!parts.empty()) rec.base = parts[0].content;
          if (search_idx < search_replies.size()) {
            entries = search_replies[search_idx];
            rec.entry_count = entries.size();
          }
          ++search_idx;
          searches.push_back(rec);
        }
        for (const auto& e : entries) send_all(fd, search_entry(id, e));
        send_all(fd, search_done(id, code));
      } else if (op == 0x42) { // unbind：断开
        ::close(fd);
        return;
      }
      // 0x77 StartTLS：假目录不支持（测试用 allow_plain），不回＝客户端
      // 超时——本文件没有 starttls 线
    }
  }

  void serve_loop() {
    while (!stop) {
      const int fd = ::accept(listen_fd, nullptr, nullptr);
      if (fd < 0) {
        if (stop) return;
        continue;
      }
      serve_conn(fd); // 串行处理（客户端同步一次一连接）
    }
  }

  std::vector<BindRec> snapshot_binds() {
    std::lock_guard<std::mutex> lk(mu);
    return binds;
  }
  void clear_records() {
    std::lock_guard<std::mutex> lk(mu);
    binds.clear();
    searches.clear();
  }
  std::vector<SearchRec> snapshot_searches() {
    std::lock_guard<std::mutex> lk(mu);
    return searches;
  }
};

// 服务模式基线配置（allow_plain＝假目录无 TLS）
LdapConfig base_cfg(std::uint16_t port) {
  LdapConfig c;
  c.host = "127.0.0.1";
  c.port = port;
  c.allow_plain = true;
  c.user_base = "ou=people,dc=corp";
  c.user_attr = "uid";
  c.bind_dn = "cn=srv,ou=svc,dc=corp";
  c.bind_password = "svc-pass";
  return c;
}

// —— 会话级测试客户端（test_accounts 同款帧收发）——
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

  memex::protocol::Message read() {
    for (;;) {
      std::array<char, 4> head{};
      asio::read(*socket_, asio::buffer(head));
      const std::uint32_t len = (std::uint8_t(head[0]) << 24) |
                                (std::uint8_t(head[1]) << 16) |
                                (std::uint8_t(head[2]) << 8) |
                                std::uint8_t(head[3]);
      std::string payload(len, '\0');
      asio::read(*socket_, asio::buffer(payload));
      auto msg = memex::protocol::decode_payload(payload);
      if (msg.type() != memex::protocol::v1::PRESENCE_DATA &&
          msg.type() != memex::protocol::v1::FAV_DATA)
        return msg;
    }
  }

  memex::protocol::Message read_login_result() {
    for (int i = 0; i < 8; ++i) {
      auto r = read();
      if (r.type() == memex::protocol::v1::LOGIN_RESULT) return r;
    }
    CHECK(false);
    return read();
  }

private:
  std::unique_ptr<tcp::socket> socket_;
};

memex::protocol::Message make_login(const std::string& account,
                                    const std::string& password) {
  memex::protocol::Message m;
  m.set_type(memex::protocol::v1::LOGIN);
  m.set_from(account + "-dev");
  m.set_to("server");
  m.set_ts_ms(now_ms());
  auto* in = m.mutable_login();
  in->set_account(account);
  in->set_password(password);
  in->set_device_fingerprint(memex::server::sha256_hex(account + "-dev"));
  in->set_device_kind("desktop");
  in->set_device_name(account + "-dev");
  in->set_client_version("0.1.0-test");
  return m;
}

// —— 腿 1：TLS 姿态校验（明文默认拒）——
void test_tls_posture() {
  LdapConfig c;
  c.host = "h";
  c.user_base = "b";
  c.bind_dn = "dn";
  c.bind_password = "p";
  CHECK(!c.tls_posture_error().empty()); // 三态全空＝拒（密码不明文红线）
  c.starttls = true;
  CHECK(c.tls_posture_error().empty());
  c.use_ssl = true;
  CHECK(!c.tls_posture_error().empty()); // ssl 与 starttls 互斥
  c.starttls = false;
  CHECK(c.tls_posture_error().empty());
  c.use_ssl = false;
  c.allow_plain = true;
  CHECK(c.tls_posture_error().empty());
}

// —— 腿 2：服务模式认证成功＋线上记录核对 ——
void test_service_mode_ok(FakeDir& dir) {
  dir.clear_records(); // 各腿独立清记录（binds/searches 跨腿累积）
  LdapConfig cfg = base_cfg(dir.port);
  cfg.group_roles = {{"cn=g,ou=groups,dc=corp", "viewer"}};
  dir.bind_codes = {0, 0};
  dir.search_replies = {
      {{"uid=jdoe,ou=people,dc=corp", {{"memberOf", {"CN=G,OU=Groups,DC=corp"}}}}}};
  LdapDirectory d(cfg);
  const auto v = d.verify_user("jdoe", "user-pass");
  CHECK(v.outcome == LdapVerdict::Outcome::Ok);
  CHECK(v.groups.size() == 1 && v.groups[0] == "CN=G,OU=Groups,DC=corp");
  CHECK(v.user_dn == "uid=jdoe,ou=people,dc=corp");
  const auto binds = dir.snapshot_binds();
  CHECK(binds.size() == 2);
  CHECK(binds[0].dn == "cn=srv,ou=svc,dc=corp" &&
        binds[0].password == "svc-pass"); // 服务账号先绑
  CHECK(binds[1].dn == "uid=jdoe,ou=people,dc=corp" &&
        binds[1].password == "user-pass"); // 用户后绑
  // 组→角色显式映射（大小写不敏感；映射外组忽略）
  const auto roles = d.map_roles(
      {"CN=G,OU=Groups,DC=corp", "cn=other,ou=groups", "cn=g,ou=groups,dc=corp"});
  CHECK(roles.size() == 1 && roles[0] == "viewer"); // other 未映射，G 去重
}

// —— 腿 3：口令错 → BadCredentials ——
void test_bad_password(FakeDir& dir) {
  dir.clear_records();
  LdapConfig cfg = base_cfg(dir.port);
  dir.bind_codes = {0, 49};
  dir.search_replies = {
      {{"uid=jdoe,ou=people,dc=corp", {}}}};
  LdapDirectory d(cfg);
  const auto v = d.verify_user("jdoe", "wrong");
  CHECK(v.outcome == LdapVerdict::Outcome::BadCredentials);
}

// —— 腿 4：目录无此用户 → BadCredentials（≠Unavailable）——
void test_user_not_found(FakeDir& dir) {
  dir.clear_records();
  LdapConfig cfg = base_cfg(dir.port);
  dir.bind_codes = {0};
  dir.search_replies = {{}};
  LdapDirectory d(cfg);
  const auto v = d.verify_user("ghost", "x");
  CHECK(v.outcome == LdapVerdict::Outcome::BadCredentials);
}

// —— 腿 5：目录禁用（禁用检测命中 → Disabled，用户绑定不发生）——
void test_disabled(FakeDir& dir) {
  dir.clear_records();
  LdapConfig cfg = base_cfg(dir.port);
  cfg.disabled_attr = "userAccountControl";
  cfg.disabled_rule = "1.2.840.113556.1.4.803";
  cfg.disabled_value = "2";
  dir.bind_codes = {0};
  dir.search_replies = {
      {{"uid=jdoe,ou=people,dc=corp", {}}},
      {{"uid=jdoe,ou=people,dc=corp", {{"userAccountControl", {"514"}}}}}};
  LdapDirectory d(cfg);
  const auto v = d.verify_user("jdoe", "user-pass");
  CHECK(v.outcome == LdapVerdict::Outcome::Disabled);
  CHECK(dir.snapshot_binds().size() == 1); // 只绑服务账号，用户绑定不发生
}

// —— 腿 6：LDAP 不可用 → Unavailable＋熔断（fail-closed 材料）——
void test_unavailable_and_breaker() {
  LdapConfig cfg = base_cfg(1); // 端口 1＝必拒
  LdapDirectory d(cfg);
  const auto v = d.verify_user("jdoe", "p");
  CHECK(v.outcome == LdapVerdict::Outcome::Unavailable);
  const auto t0 = std::chrono::steady_clock::now();
  const auto v2 = d.verify_user("jdoe", "p");
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  CHECK(v2.outcome == LdapVerdict::Outcome::Unavailable);
  CHECK(v2.detail.find("熔断") != std::string::npos);
  CHECK(ms < 100); // fail-fast（未触网）
}

// —— 腿 7：模板模式（DN 拼装＋转义；组自读）——
void test_template_mode(FakeDir& dir) {
  dir.clear_records();
  LdapConfig cfg = base_cfg(dir.port);
  cfg.bind_dn_template = "uid={u},ou=people,dc=corp";
  cfg.bind_dn.clear();
  cfg.bind_password.clear();
  cfg.memberof_attr = "memberOf";
  dir.bind_codes = {0}; // 模板模式只有用户直绑（无服务账号绑定）
  dir.search_replies = {
      {{"uid=j;doe,ou=people,dc=corp", {{"memberOf", {"CN=G,OU=Groups"}}}}}};
  LdapDirectory d(cfg);
  const auto v = d.verify_user("j;doe", "user-pass");
  CHECK(v.outcome == LdapVerdict::Outcome::Ok);
  // 模板模式 subject＝绑定用 DN（值已按 RFC 4514 转义——首登自动绑定
  // 落库的就是这个形态）
  CHECK(v.user_dn == "uid=j\\;doe,ou=people,dc=corp");
  const auto binds = dir.snapshot_binds();
  CHECK(binds.size() == 1); // 无服务绑定，用户直绑一次
  CHECK(binds[0].dn == "uid=j\\;doe,ou=people,dc=corp"); // DN 值转义
  CHECK(binds[0].password == "user-pass");
  const auto searches = dir.snapshot_searches();
  CHECK(searches.size() == 1); // 组自读一次
  CHECK(searches[0].base == "uid=j\\;doe,ou=people,dc=corp"); // 自读 base=DN
}

// —— 腿 8/9：路由（显式绑定＋fail-closed＋默认不触目录）——
void test_route(ServerStore& store, FakeDir& dir) {
  CHECK(store.create_account("jdoe", "local-pass", "JD"));
  CHECK(store.create_account("localguy", "local-pass", "LG"));

  // 未绑定＋默认配置 → Local（目录零触达——不做任何探测）。
  // group_roles 须在 LdapDirectory 构造前落 cfg（构造时拷贝定案）。
  LdapConfig cfg = base_cfg(dir.port);
  cfg.group_roles = {{"cn=viewers,ou=groups", "viewer"}};
  LdapDirectory d(cfg);
  const auto untouched = dir.snapshot_binds().size();
  auto r = memex::server::route_authentication(store, &d, "jdoe", "whatever");
  CHECK(r.path == AuthRoute::Path::Local);
  CHECK(dir.snapshot_binds().size() == untouched);

  // 显式绑定 → 目录判登
  CHECK(store.identity_bind("jdoe", "ldap://127.0.0.1",
                            "uid=jdoe,ou=people,dc=corp", now_ms()) > 0);
  dir.bind_codes = {0, 0};
  dir.search_replies = {
      {{"uid=jdoe,ou=people,dc=corp",
        {{"memberOf", {"CN=Viewers,OU=Groups"}}}}}};
  d.reset_breaker();
  r = memex::server::route_authentication(store, &d, "jdoe", "dir-pass");
  CHECK(r.path == AuthRoute::Path::LdapOk);
  CHECK(r.roles.size() == 1 && r.roles[0] == "viewer");

  // 绑定在、口令错 → LdapBad
  dir.bind_codes = {0, 49};
  dir.search_replies = {{{"uid=jdoe,ou=people,dc=corp", {}}}};
  r = memex::server::route_authentication(store, &d, "jdoe", "wrong");
  CHECK(r.path == AuthRoute::Path::LdapBad);

  // 绑定在、目录未配（nullptr）→ LdapUnavailable（绝不回落本地）
  r = memex::server::route_authentication(store, nullptr, "jdoe", "x");
  CHECK(r.path == AuthRoute::Path::LdapUnavailable);

  // 本地账号常备：未绑定账号 Local 路径照旧（调用方本地判登）
  r = memex::server::route_authentication(store, &d, "localguy", "x");
  CHECK(r.path == AuthRoute::Path::Local);
}

// —— 腿 10：auto_bind（已有账号无绑定，目录认证成功即自动绑定）——
void test_auto_bind(ServerStore& store, FakeDir& dir) {
  CHECK(store.create_account("newhire", "tmp-pass", "NH"));
  LdapConfig cfg = base_cfg(dir.port);
  cfg.auto_bind = true;
  cfg.group_roles = {{"cn=viewers,ou=groups", "viewer"}};
  dir.bind_codes = {0, 0};
  dir.search_replies = {
      {{"uid=newhire,ou=people,dc=corp",
        {{"memberOf", {"CN=Viewers,OU=Groups"}}}}}};
  LdapDirectory d(cfg);
  auto r = memex::server::route_authentication(store, &d, "newhire", "dir-pw");
  CHECK(r.path == AuthRoute::Path::LdapOk && r.roles.size() == 1);
  const auto b = memex::server::find_ldap_binding(store, "newhire");
  CHECK(b.has_value());
  CHECK(b->issuer == "ldap://127.0.0.1");
  CHECK(b->subject == "uid=newhire,ou=people,dc=corp");
}

// —— 腿 11：影子账号默认关（账号不存在 → Local，目录零触达）——
void test_provision_default_off(ServerStore& store, FakeDir& dir) {
  LdapConfig cfg = base_cfg(dir.port);
  LdapDirectory d(cfg);
  const auto untouched = dir.snapshot_binds().size();
  auto r = memex::server::route_authentication(store, &d, "stranger", "p");
  CHECK(r.path == AuthRoute::Path::Local);
  CHECK(!store.find_account("stranger").has_value());
  CHECK(dir.snapshot_binds().size() == untouched);
}

// —— 腿 12：auto_provision 开（影子账号＝本地判登封死）——
void test_provision_on(ServerStore& store, FakeDir& dir) {
  LdapConfig cfg = base_cfg(dir.port);
  cfg.auto_provision = true;
  dir.bind_codes = {0, 0};
  dir.search_replies = {
      {{"uid=temp,ou=people,dc=corp", {}}}};
  LdapDirectory d(cfg);
  auto r = memex::server::route_authentication(store, &d, "temp", "dir-pw");
  CHECK(r.path == AuthRoute::Path::LdapOk);
  CHECK(store.find_account("temp").has_value()); // 影子已建
  const auto cred = store.find_credential("temp", "password");
  CHECK(cred.has_value());
  // 本地口令随机不可知：用目录口令判本地路径必败（LDAP-only）
  CHECK(memex::server::pbkdf2_sha256_hex("dir-pw", cred->salt_hex, 60000) !=
        cred->digest_hex);
}

// —— 腿 13：组未映射 → 不授任何角色（幂等去重）——
void test_role_sync(ServerStore& store) {
  LdapConfig cfg;
  cfg.group_roles = {{"cn=viewers", "viewer"}};
  LdapDirectory d(cfg);
  const auto roles = d.map_roles({"CN=Viewers", "CN=Unmapped"});
  CHECK(roles.size() == 1 && roles[0] == "viewer");
  CHECK(memex::server::sync_ldap_roles(store, "localguy", roles, now_ms()) ==
        1);
  // 幂等：再同步不再加行
  CHECK(memex::server::sync_ldap_roles(store, "localguy", roles, now_ms()) ==
        0);
  const auto eff = store.effective_roles("localguy", now_ms());
  CHECK(std::find(eff.begin(), eff.end(), "viewer") != eff.end());
  CHECK(std::find(eff.begin(), eff.end(), "admin") == eff.end()); // 永不隐式提权
  // 映射表外组＝map_roles 空集＝零授权（sync 只认映射产物，不认原始组）
  CHECK(memex::server::sync_ldap_roles(store, "localguy",
                                       d.map_roles({"CN=Other"}),
                                       now_ms()) == 0);
}

// —— 腿 14/15：会话级（真登录链）：目录判登成功＋fail-closed ＋本地常备 ——
void test_session_wire(ServerStore& store, FakeDir& dir) {
  LdapConfig cfg = base_cfg(dir.port);
  cfg.group_roles = {{"cn=viewers,ou=groups", "viewer"}};
  cfg.disabled_attr = "userAccountControl"; // 14c 新登录禁用拒需检测在位
  cfg.disabled_rule = "1.2.840.113556.1.4.803";
  cfg.disabled_value = "2";
  LdapDirectory d(cfg);

  asio::io_context io;
  CollabServer server(io, store, 0, &d);
  server.start_accept();
  std::thread io_thread([&] { io.run(); });
  const std::uint16_t port = server.port();

  // 14a 绑定账号走目录判登（目录口令，本地口令根本不参与）。
  // 脚本：svc 绑 0＋用户绑 0；定位条目（带 memberOf）＋禁用检测空集（未禁）
  {
    dir.bind_codes = {0, 0};
    dir.search_replies = {
        {{"uid=jdoe,ou=people,dc=corp",
          {{"memberOf", {"CN=Viewers,OU=Groups"}}}}},
        {}};
    TestClient c(io, port);
    c.send(make_login("jdoe", "dir-pass"));
    const auto r = c.read_login_result();
    CHECK(r.login_result().ok());
    const auto eff = store.effective_roles("jdoe", now_ms());
    CHECK(std::find(eff.begin(), eff.end(), "viewer") != eff.end());
  }
  // 14b 目录不可达 → fail-closed 拒登（reason 明示，绝不静默回落本地）
  {
    LdapConfig dead = base_cfg(1);
    LdapDirectory dd(dead);
    CollabServer dead_server(io, store, 0, &dd);
    dead_server.start_accept();
    TestClient c(io, dead_server.port());
    c.send(make_login("jdoe", "anything"));
    const auto r = c.read_login_result();
    CHECK(!r.login_result().ok());
    CHECK(r.login_result().reason().find("fail-closed") != std::string::npos);
  }
  // 14c 目录禁用 → 新登录拒
  {
    dir.bind_codes = {0};
    dir.search_replies = {
        {{"uid=jdoe,ou=people,dc=corp", {}}},
        {{"uid=jdoe,ou=people,dc=corp", {{"userAccountControl", {"514"}}}}}};
    TestClient c(io, port);
    c.send(make_login("jdoe", "dir-pass"));
    const auto r = c.read_login_result();
    CHECK(!r.login_result().ok());
    CHECK(r.login_result().reason().find("禁用") != std::string::npos);
  }
  // 14d 本地账号常备：未绑定账号本地口令照常（应急恢复入口）
  {
    TestClient c(io, port);
    c.send(make_login("localguy", "local-pass"));
    const auto r = c.read_login_result();
    CHECK(r.login_result().ok());
  }

  io.stop();
  io_thread.join();
}

// —— 腿 16：存量会话踢下线（复检命中 → KICK；默认策略=踢）——
void test_sweep_kick(ServerStore& store, FakeDir& dir) {
  LdapConfig cfg = base_cfg(dir.port);
  cfg.recheck_seconds = 1;
  cfg.disabled_attr = "userAccountControl"; // 复检查禁用三元组（未配＝不复检）
  cfg.disabled_rule = "1.2.840.113556.1.4.803";
  cfg.disabled_value = "2";
  LdapDirectory d(cfg);

  asio::io_context io;
  CollabServer server(io, store, 0, &d);
  server.start_accept();
  server.start_ldap_sweep();
  std::thread io_thread([&] { io.run(); });

  TestClient c(io, server.port());
  dir.bind_codes = {0, 0};
  dir.search_replies = {
      {{"uid=jdoe,ou=people,dc=corp",
        {{"memberOf", {"CN=Viewers,OU=Groups"}}}}}};
  c.send(make_login("jdoe", "dir-pass"));
  const auto r = c.read_login_result();
  CHECK(r.login_result().ok());

  // 目录侧标禁用：下一次复检 check_disabled（独立连接：服务绑定＋禁用
  // base 检索命中）
  {
    std::lock_guard<std::mutex> lk(dir.mu);
    dir.bind_codes = {0};
    dir.search_replies = {
        {{"uid=jdoe,ou=people,dc=corp", {{"userAccountControl", {"514"}}}}}};
  }
  // 最多等 5s 收 KICK（复检 1s 间隔；PRESENCE_DATA 一并被 read 跳过）
  bool kicked = false;
  for (int i = 0; i < 10 && !kicked; ++i) {
    auto m = c.read();
    if (m.type() == memex::protocol::v1::KICK &&
        m.kick().reason().find("禁用") != std::string::npos)
      kicked = true;
  }
  CHECK(kicked);

  io.stop();
  io_thread.join();
}

} // namespace

int main() {
  std::cerr << "[leg] test_tls_posture\n";
  test_tls_posture();

  FakeDir dir;
  CHECK(dir.start());

  ServerStore store;
  const std::string db = "/tmp/memex-test-ldap-" +
                         std::to_string(::getpid()) + ".db";
  std::remove(db.c_str());
  CHECK(store.open(db));

  test_service_mode_ok(dir);  // 腿 2
  test_bad_password(dir);     // 腿 3
  test_user_not_found(dir);   // 腿 4
  test_disabled(dir);         // 腿 5
  test_unavailable_and_breaker(); // 腿 6（不触假目录）
  test_template_mode(dir);    // 腿 7
  test_route(store, dir);     // 腿 8/9
  test_auto_bind(store, dir); // 腿 10
  test_provision_default_off(store, dir); // 腿 11
  test_provision_on(store, dir);          // 腿 12
  test_role_sync(store);      // 腿 13
  test_session_wire(store, dir); // 腿 14/15
  test_sweep_kick(store, dir);   // 腿 16

  dir.stop_server();
  store.close();
  std::remove(db.c_str());

  if (g_failures == 0) {
    std::cout << "test_ldap: all legs passed\n";
    return 0;
  }
  std::cerr << "test_ldap: " << g_failures << " failure(s)\n";
  return 1;
}
