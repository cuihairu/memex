// T4.6 通讯录可见性验收（A20）：隐藏部门／成员（白名单例外）、部门限看
// 本部门、敏感人员字段隐藏。三层——库级（visible_members/visible_departments
// 直测：过滤、脱敏、上级引用抹除、本人与管理员豁免、落盘重开保留）、
// CLI 进程级（org hide/fields/allow/visibility list 真跑＋坏参拒绝）、
// 协议级（ORG_QUERY→ORG_DATA 按查询者过滤——被隐藏者不可见不可搜的
// 真实通道：数据不出服务端，客户端建群／拉人数据源同受约束）。
#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"
#include "server.hpp"
#include "store.hpp"

#ifndef MEMEX_SERVER_BIN
#error "MEMEX_SERVER_BIN 未定义（应传入 $<TARGET_FILE:memex_server>）"
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

std::string run_cli(const std::string& args, int* exit_code = nullptr) {
  const std::string out_path = "/tmp/memex-visibility-test-out.txt";
  const std::string cmd = std::string("\"" MEMEX_SERVER_BIN "\" ") + args +
                          " > " + out_path + " 2>&1";
  const int rc = std::system(cmd.c_str());
  if (exit_code) *exit_code = rc;
  std::ifstream f(out_path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 阻塞式协议客户端（与 test_org 同款；在线/常用联系人推送与本题无关自动跳过）
class TestClient {
public:
  TestClient(asio::io_context& io, std::uint16_t port) {
    socket_ = std::make_unique<asio::ip::tcp::socket>(io);
    socket_->connect(asio::ip::tcp::endpoint(
        asio::ip::make_address("127.0.0.1"), port));
  }
  ~TestClient() {
    if (socket_) {
      std::error_code ignore;
      socket_->close(ignore);
    }
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
          msg.type() != memex::protocol::v1::FAV_DATA) {
        return msg;
      }
    }
  }
  void login(const std::string& account, const std::string& fp_seed) {
    memex::protocol::Message m;
    m.set_type(memex::protocol::v1::LOGIN);
    m.set_from(fp_seed);
    m.set_to("server");
    m.set_ts_ms(now_ms());
    auto* in = m.mutable_login();
    in->set_account(account);
    in->set_password("pw");
    in->set_device_fingerprint(memex::server::sha256_hex(fp_seed));
    in->set_device_kind("desktop");
    in->set_device_name(fp_seed);
    in->set_client_version("0.1.0-test");
    send(m);
    CHECK(read().login_result().ok());
  }

private:
  std::unique_ptr<asio::ip::tcp::socket> socket_;
};

// 账号 → 条目（成员视图）；缺项即不在通讯录
std::map<std::string, memex::protocol::v1::OrgMember> member_map(
    const memex::protocol::v1::OrgData& od) {
  std::map<std::string, memex::protocol::v1::OrgMember> out;
  for (const auto& m : od.members()) out[m.account()] = m;
  return out;
}

bool dept_listed(const memex::protocol::v1::OrgData& od,
                 const std::string& path) {
  for (const auto& d : od.departments()) {
    if (d.path() == path) return true;
  }
  return false;
}

// 库级部门树下发表是否含该路径（viewer 视角）
bool store_dept_listed(memex::server::ServerStore& store,
                       const std::string& viewer, const std::string& path) {
  for (const auto& [id, p] : store.visible_departments(viewer)) {
    (void)id;
    if (p == path) return true;
  }
  return false;
}

} // namespace

int main() {
  // —— 库级：建组织（六人三部门，root 为管理员）——
  const std::string db = "/tmp/memex-visibility-test.db";
  std::remove(db.c_str());
  memex::server::ServerStore store;
  CHECK(store.open(db));
  for (const auto& [acct, role] :
       std::vector<std::pair<std::string, std::string>>{
           {"alice", "member"}, {"bob", "member"}, {"carol", "member"},
           {"dave", "member"}, {"eve", "member"}, {"root", "admin"}}) {
    CHECK(store.create_account(acct, "pw", acct, role));
  }
  CHECK(store.set_member_profile("alice", store.ensure_department_path("公司"),
                                 "总经理", ""));
  CHECK(store.set_member_profile(
      "bob", store.ensure_department_path("公司/研发部"), "研发主管", "alice"));
  CHECK(store.set_member_profile(
      "carol", store.ensure_department_path("公司/研发部/客户端组"), "组长",
      "bob"));
  CHECK(store.set_member_profile(
      "dave", store.ensure_department_path("公司/研发部/客户端组"), "工程师",
      "carol"));
  CHECK(store.set_member_profile(
      "eve", store.ensure_department_path("公司/研发部/测试组"), "测试工程师",
      "bob"));
  CHECK(store.set_member_profile("root",
                                 store.ensure_department_path("公司"), "运维",
                                 ""));

  // 无配置＝全员互见（基线）
  CHECK(store.visible_members("alice").size() == 6);

  // ① 隐藏成员：普通查看者不可见；本人自见；管理员全量
  CHECK(store.set_visibility("member", "bob", true, false, ""));
  auto va = store.visible_members("alice");
  CHECK(va.size() == 5);
  bool saw_bob = false;
  for (const auto& m : va) saw_bob = saw_bob || m.account == "bob";
  CHECK(!saw_bob);
  CHECK(store.visible_members("bob").size() == 6); // 本人豁免
  CHECK(store.visible_members("root").size() == 6); // 管理员豁免
  // 上级引用抹除：dave.manager=carol、carol.manager=bob——bob 不可见则
  // carol 的 manager 为空（不留不可见者线索）；dave 的 manager=carol 仍可
  for (const auto& m : va) {
    if (m.account == "carol") CHECK(m.manager.empty());
    if (m.account == "dave") CHECK(m.manager == "carol");
  }

  // ② 白名单例外：alice 可见 bob（字段同享）
  CHECK(store.add_visibility_allow("alice", "bob"));
  va = store.visible_members("alice");
  saw_bob = false;
  for (const auto& m : va) saw_bob = saw_bob || m.account == "bob";
  CHECK(saw_bob);
  CHECK(store.visible_members("dave").size() == 5); // 他人不受白名单影响
  CHECK(store.remove_visibility_allow("alice", "bob"));
  CHECK(store.visible_members("alice").size() == 5);
  // 坏参拒绝：账号／部门不存在、scope 非法
  CHECK(!store.set_visibility("member", "ghost", true, false, ""));
  CHECK(!store.set_visibility("dept", "不存在/路径", true, false, ""));
  CHECK(!store.set_visibility("team", "bob", true, false, ""));

  // ③ 隐藏部门（整树）：部门外不可见、部门内自己人互见
  CHECK(store.set_visibility("member", "bob", false, false, "")); // 先解除成员隐藏
  CHECK(store.set_visibility("dept", "公司/研发部/客户端组", true, false, ""));
  auto ve = store.visible_members("eve"); // 测试组（部门外）
  CHECK(ve.size() == 4);                  // alice/bob/eve/root
  for (const auto& m : ve) {
    CHECK(m.account != "carol" && m.account != "dave");
  }
  auto vc = store.visible_members("carol"); // 客户端组内
  CHECK(vc.size() == 6);                    // 自己人互见＋其余照常
  // 部门树同步：部门外视图缺该部门行，部门内视图保留
  CHECK(!store_dept_listed(store, "eve", "公司/研发部/客户端组"));
  CHECK(store_dept_listed(store, "carol", "公司/研发部/客户端组"));

  // ④ 部门限看本部门：受限部门成员只见本部门子树
  CHECK(store.set_visibility("dept", "公司/研发部/客户端组", false, false, ""));
  CHECK(store.set_visibility("dept", "公司/研发部", false, true, ""));
  auto vd = store.visible_members("dave"); // 客户端组 ⊂ 研发部
  CHECK(vd.size() == 4); // bob/carol/dave/eve（研发部子树）——alice/root 不见
  for (const auto& m : vd) {
    CHECK(m.account != "alice" && m.account != "root");
  }
  auto ve2 = store.visible_members("eve"); // 测试组 ⊂ 研发部
  CHECK(ve2.size() == 4);
  for (const auto& m : ve2) CHECK(m.account != "alice");
  CHECK(store.visible_members("alice").size() == 6); // 部门外查看者不受限
  CHECK(store.visible_members("root").size() == 6);  // 管理员不受限
  // 白名单穿透限看：dave 豁免可见 alice
  CHECK(store.add_visibility_allow("dave", "alice"));
  vd = store.visible_members("dave");
  CHECK(vd.size() == 5);
  CHECK(store.remove_visibility_allow("dave", "alice"));
  CHECK(store.set_visibility("dept", "公司/研发部", false, false, "")); // 解除

  // ⑤ 敏感字段：非管理员／非白名单脱敏；管理员与本人不脱敏
  CHECK(store.set_visibility("member", "bob", false, false, "title,manager"));
  va = store.visible_members("alice");
  saw_bob = false;
  for (const auto& m : va) {
    if (m.account == "bob") {
      saw_bob = true;
      CHECK(m.title.empty());
      CHECK(m.manager.empty());
      CHECK(m.display_name == "bob"); // 非敏感项不动
    }
  }
  CHECK(saw_bob);
  auto vb = store.visible_members("bob"); // 本人不脱敏
  for (const auto& m : vb) {
    if (m.account == "bob") {
      CHECK(m.title == "研发主管" && m.manager == "alice");
    }
  }
  auto vr = store.visible_members("root"); // 管理员不脱敏
  for (const auto& m : vr) {
    if (m.account == "bob") {
      CHECK(m.title == "研发主管" && m.manager == "alice");
    }
  }
  CHECK(store.add_visibility_allow("alice", "bob")); // 白名单同享字段
  va = store.visible_members("alice");
  for (const auto& m : va) {
    if (m.account == "bob") CHECK(m.title == "研发主管");
  }
  CHECK(store.remove_visibility_allow("alice", "bob"));

  // ⑥ 配置落盘重开保留（管理面持久）
  store.close();
  {
    memex::server::ServerStore reopen;
    CHECK(reopen.open(db));
    const auto row = reopen.visibility_row("member", "bob");
    CHECK(row.has_value());
    CHECK(!row->hidden && row->hide_fields == "title,manager");
    // bob ＋两个被解除过的部门行（解除＝标志复位，行保留）
    CHECK(reopen.visibility_list().size() == 3);
    reopen.close();
  }
  CHECK(store.open(db)); // 后续协议级沿用同一库

  // —— CLI 进程级：配置面真跑＋坏参拒绝 ——
  int rc = 0;
  CHECK(run_cli("org hide carol --db " + db, &rc).find("已隐藏成员") !=
        std::string::npos);
  CHECK(rc == 0);
  CHECK(run_cli("org allow alice --see carol --db " + db)
            .find("已加白名单") != std::string::npos);
  CHECK(run_cli("org fields carol --hide title,manager --db " + db)
            .find("已配置敏感字段") != std::string::npos);
  const std::string vlist =
      run_cli("org visibility list --db " + db);
  CHECK(vlist.find("成员 carol") != std::string::npos);
  CHECK(vlist.find("title,manager") != std::string::npos);
  CHECK(vlist.find("alice → carol") != std::string::npos);
  CHECK(run_cli("org dept restrict 公司/研发部 --db " + db)
            .find("限看本部门") != std::string::npos);
  CHECK(run_cli("org disallow alice --see carol --db " + db)
            .find("已移除白名单") != std::string::npos);
  CHECK(run_cli("org unhide carol --db " + db)
            .find("已取消隐藏成员") != std::string::npos);
  // 坏参拒绝
  CHECK(run_cli("org hide ghost --db " + db, &rc).find("不存在") !=
        std::string::npos);
  CHECK(rc != 0);
  CHECK(run_cli("org fields carol --hide phone --db " + db, &rc)
            .find("只支持") != std::string::npos);
  CHECK(rc != 0);
  CHECK(run_cli("org allow ghost --see carol --db " + db, &rc)
            .find("不存在") != std::string::npos);
  CHECK(rc != 0);
  CHECK(run_cli("org allow alice --see 不存在/路径 --db " + db, &rc)
            .find("既非账号也非部门") != std::string::npos);
  CHECK(rc != 0);
  // 收尾：清 CLI 段配置，协议级重设
  CHECK(run_cli("org dept unrestrict 公司/研发部 --db " + db, &rc)
            .find("不限看") != std::string::npos);
  CHECK(rc == 0);

  // —— 协议级：ORG_QUERY→ORG_DATA 按查询者过滤（被隐藏者不可见不可搜）——
  // 场景重设：隐藏 bob；白名单 carol→bob；隐藏部门 客户端组
  CHECK(run_cli("org hide bob --db " + db).find("已隐藏成员") !=
        std::string::npos);
  CHECK(run_cli("org allow carol --see bob --db " + db)
            .find("已加白名单") != std::string::npos);
  CHECK(run_cli("org dept hide 公司/研发部/客户端组 --db " + db)
            .find("已配置部门可见性") != std::string::npos);
  memex::server::ServerStore srv_store;
  CHECK(srv_store.open(db));
  {
    asio::io_context io;
    memex::server::CollabServer server(io, srv_store, 0);
    server.start_accept();
    std::thread io_thread([&] { io.run(); });

    auto org_of = [&](TestClient& c) {
      memex::protocol::Message q;
      q.set_type(memex::protocol::v1::ORG_QUERY);
      q.set_to("server");
      q.set_ts_ms(now_ms());
      c.send(q);
      const auto data = c.read();
      CHECK(data.type() == memex::protocol::v1::ORG_DATA);
      return member_map(data.org_data());
    };

    { // 普通成员 alice：bob 不可见（不可见即不可搜——列表里根本没有）
      TestClient c(io, server.port());
      c.login("alice", "pc-vis-a");
      const auto od = org_of(c);
      CHECK(od.count("bob") == 0);
      CHECK(od.count("alice") == 1); // 本人自见
    }
    { // 白名单 carol：bob 可见且字段完整
      TestClient c(io, server.port());
      c.login("carol", "pc-vis-c");
      const auto od = org_of(c);
      CHECK(od.count("bob") == 1);
      CHECK(od.at("bob").manager() == "alice");
    }
    { // 部门外 eve：隐藏部门 客户端组 的成员不可见、部门行不下发
      TestClient c(io, server.port());
      c.login("eve", "pc-vis-e");
      const auto od = org_of(c);
      CHECK(od.count("carol") == 0);
      CHECK(od.count("dave") == 0);
      CHECK(od.count("bob") == 0); // bob 仍被隐藏
      memex::protocol::Message q;
      q.set_type(memex::protocol::v1::ORG_QUERY);
      q.set_to("server");
      q.set_ts_ms(now_ms());
      c.send(q);
      const auto raw = c.read();
      CHECK(!dept_listed(raw.org_data(), "公司/研发部/客户端组"));
      CHECK(dept_listed(raw.org_data(), "公司/研发部"));
    }
    { // 管理员 root：全量（隐藏成员／隐藏部门均在列）
      TestClient c(io, server.port());
      c.login("root", "pc-vis-r");
      const auto od = org_of(c);
      CHECK(od.size() == 6);
      CHECK(od.count("bob") == 1 && od.count("dave") == 1);
    }
    { // 被隐藏者 bob 本人：自见（登录与自查询不受影响）
      TestClient c(io, server.port());
      c.login("bob", "pc-vis-b");
      const auto od = org_of(c);
      CHECK(od.count("bob") == 1);
    }

    io.stop();
    io_thread.join();
  }
  srv_store.close();

  if (g_failures == 0) {
    std::cout << "visibility tests: all passed\n";
    return 0;
  }
  std::cerr << "visibility tests: " << g_failures << " failure(s)\n";
  return 1;
}
