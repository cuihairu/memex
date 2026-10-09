// T2.6 组织架构验收：部门树、成员资料（直属上级独立字段，每人至多一名）、
// 上级链路逐级上溯、批量导入 CSV（错误行校验拒绝并报告行号）。
// T3.1 管理后台骨架：角色分级（admin／member）＋组织架构经协议下发（ORG_QUERY→ORG_DATA）。
// 库级（ServerStore 直测）＋进程级（CLI org 子命令真跑）＋协议级（真实连接）三层。
#include <asio.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
  const std::string out_path = "/tmp/memex-org-test-out.txt";
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

// 阻塞式协议客户端（与 test_archive 同款）
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
      std::string payload(len, '\0');
      asio::read(*socket_, asio::buffer(payload));
      auto msg = memex::protocol::decode_payload(payload);
      if (msg.type() != memex::protocol::v1::PRESENCE_DATA && msg.type() != memex::protocol::v1::FAV_DATA) return msg;
    }
  }

private:
  std::unique_ptr<asio::ip::tcp::socket> socket_;
};

} // namespace

int main() {
  // —— 库级：部门树 ——
  memex::server::ServerStore store;
  CHECK(store.open(":memory:"));
  for (const auto& [acct, name] : std::vector<std::pair<std::string, std::string>>{
           {"alice", "Alice"}, {"bob", "Bob"}, {"carol", "Carol"},
           {"dave", "Dave"}, {"eve", "Eve"}}) {
    CHECK(store.create_account(acct, "pw-" + acct, name));
  }

  const int dept_id = store.ensure_department_path("公司/研发部/客户端组");
  CHECK(dept_id > 0);
  const auto depts = store.department_list();
  CHECK(depts.size() == 3); // 三级逐级创建
  bool has_full_path = false;
  for (const auto& [id, path] : depts) {
    if (path == "公司/研发部/客户端组" && id == dept_id) has_full_path = true;
  }
  CHECK(has_full_path);
  CHECK(store.ensure_department_path("公司/研发部/客户端组") == dept_id); // 幂等
  CHECK(store.ensure_department_path("") == -1);
  CHECK(store.ensure_department_path("公司//坏路径") == -1);

  // —— 库级：成员资料与上级链路 ——
  CHECK(store.set_member_profile("alice", store.ensure_department_path("公司"),
                                 "总经理", ""));
  CHECK(store.set_member_profile("bob", store.ensure_department_path("公司/研发部"),
                                 "研发主管", "alice"));
  CHECK(store.set_member_profile("carol",
                                 store.ensure_department_path("公司/研发部/客户端组"),
                                 "组长", "bob"));
  CHECK(store.set_member_profile("dave",
                                 store.ensure_department_path("公司/研发部/客户端组"),
                                 "工程师", "carol"));

  const auto p = store.member_profile("dave");
  CHECK(p.has_value());
  CHECK(p->manager == "carol"); // 直属上级独立字段
  CHECK(p->title == "工程师");
  CHECK(p->department_path == "公司/研发部/客户端组");
  CHECK(p->display_name == "Dave");

  const auto chain = store.manager_chain("dave");
  CHECK(chain.size() == 3); // 逐级上溯：carol → bob → alice
  CHECK(chain[0] == "carol" && chain[1] == "bob" && chain[2] == "alice");
  CHECK(store.manager_chain("alice").empty()); // 顶到头

  // 校验拒绝：自为上级／上级账号不存在／构成汇报环
  CHECK(!store.set_member_profile("eve", -1, "", "eve"));
  CHECK(!store.set_member_profile("eve", -1, "", "ghost"));
  CHECK(!store.set_member_profile("alice", -1, "总经理", "dave")); // 环
  CHECK(store.manager_chain("alice").empty()); // 被拒后链路不变

  // —— 库级：个性签名（需求批⑪）——upsert（无档案行兜底建行）、
  // 清除（空串）、坏账号拒绝；member_list 带出
  CHECK(store.set_signature("alice", "专注交付"));
  CHECK(store.member_profile("alice")->signature == "专注交付");
  CHECK(store.set_signature("eve", "测试签")); // eve 尚无档案行 → upsert 建行
  CHECK(store.member_profile("eve")->signature == "测试签");
  CHECK(store.set_signature("alice", "")); // 空串=清除
  CHECK(store.member_profile("alice")->signature.empty());
  CHECK(!store.set_signature("ghost", "x")); // 账号不存在
  bool sig_in_list = false;
  for (const auto& m : store.member_list()) {
    if (m.account == "eve") sig_in_list = m.signature == "测试签";
  }
  CHECK(sig_in_list);

  // —— 库级：在线时长事件流水（需求批⑩）——online/offline 配对扫掠求
  // 并集时长：多端并行不叠加、窗越界端点截断、悬空 online 计到窗尾、
  // trim_dangling_online 启动自愈（补 offline 截断且幂等）
  {
    memex::server::ServerStore s;
    CHECK(s.open(":memory:"));
    CHECK(s.create_account("u1", "p", "u1"));
    CHECK(s.create_account("u2", "p", "u2"));
    CHECK(s.create_account("u3", "p", "u3"));
    // 常规配对：在线段 [1000, 60000]，窗端点截断
    CHECK(s.add_presence_event("u1", "online", 1000));
    CHECK(s.add_presence_event("u1", "offline", 60000));
    CHECK(s.online_ms_between("u1", 0, 120000) == 59000);
    CHECK(s.online_ms_between("u1", 10000, 120000) == 50000); // 起点截断
    CHECK(s.online_ms_between("u1", 70000, 120000) == 0);     // 窗在段后
    CHECK(s.online_ms_between("u1", 0, 500) == 0);            // 窗在段前
    // 并发双端：两台设备并行区间取并集 [1000, 60000]，计一次不叠加
    CHECK(s.add_presence_event("u3", "online", 1000));
    CHECK(s.add_presence_event("u3", "online", 2000));
    CHECK(s.add_presence_event("u3", "offline", 30000));
    CHECK(s.add_presence_event("u3", "offline", 60000));
    CHECK(s.online_ms_between("u3", 0, 120000) == 59000);
    // 悬空 online（断电/崩溃丢闭笔）：计到窗尾；trim 补 offline 截断
    CHECK(s.add_presence_event("u2", "online", 5000));
    CHECK(s.online_ms_between("u2", 0, 65000) == 60000);
    s.trim_dangling_online(70000);
    CHECK(s.online_ms_between("u2", 0, 70000) == 65000);
    CHECK(s.online_ms_between("u2", 0, 80000) == 65000); // 闭段后不再计
    // trim 幂等：再跑一遍无新增，配对完整的账号不受影响
    s.trim_dangling_online(75000);
    CHECK(s.online_ms_between("u1", 0, 80000) == 59000);
    CHECK(s.online_ms_between("u3", 0, 80000) == 59000);
    CHECK(s.online_ms_between("u2", 0, 80000) == 65000);
    s.close();
  }

  // —— 库级：批量导入（错误行校验拒绝）——
  std::vector<memex::server::OrgImportRow> rows;
  auto make_row = [&](int line_no, const std::string& a, const std::string& dept,
                      const std::string& title, const std::string& mgr) {
    memex::server::OrgImportRow r;
    r.line_no = line_no;
    r.account = a;
    r.dept = dept;
    r.title = title;
    r.manager = mgr;
    rows.push_back(r);
  };
  make_row(2, "eve", "公司/研发部", "测试工程师", "carol"); // 合法
  make_row(3, "ghost", "公司", "", "");                    // 账号不存在
  make_row(4, "dave", "公司", "", "ghost");                // 上级不存在
  make_row(5, "carol", "公司", "", "carol");               // 自为上级
  const auto result = store.import_members(rows);
  CHECK(result.imported == 1);
  CHECK(result.errors.size() == 3);
  bool rejected_line3 = false, rejected_line4 = false, rejected_line5 = false;
  for (const auto& e : result.errors) {
    if (e.find("第 3 行") != std::string::npos) rejected_line3 = true;
    if (e.find("第 4 行") != std::string::npos) rejected_line4 = true;
    if (e.find("第 5 行") != std::string::npos) rejected_line5 = true;
  }
  CHECK(rejected_line3 && rejected_line4 && rejected_line5);
  CHECK(store.member_profile("eve").has_value()); // 合法行已入库
  CHECK(!store.member_profile("ghost").has_value());
  store.close();

  // —— 进程级：CLI org（建库→账号→导入→详情→链路→坏更新拒绝）——
  const std::string db = "/tmp/memex-org-test.db";
  std::remove(db.c_str());
  for (const char* acct : {"alice", "bob", "carol", "dave", "eve"}) {
    CHECK(run_cli(std::string("account add ") + acct + " pw --db " + db)
              .find("已建号") != std::string::npos);
  }

  const std::string csv = "/tmp/memex-org-test.csv";
  {
    std::ofstream f(csv);
    f << "账号,部门,职务,直属上级\n"
      << "alice,公司,总经理,\n"
      << "bob,公司/研发部,研发主管,alice\n"
      << "carol,公司/研发部/客户端组,组长,bob\n"
      << "dave,公司/研发部/客户端组,工程师,carol\n"
      << "ghost,公司,,\n"          // 账号不存在
      << "fred,公司,职员\n"        // 列数不足（3 列）
      << "gary,公司,职员,ghost\n"  // 上级不存在
      << "helen,公司,职员,helen\n"; // 自为上级
  }
  const std::string imp =
      run_cli("org import " + csv + " --db " + db);
  CHECK(imp.find("成功 4 条") != std::string::npos);
  CHECK(imp.find("拒绝 4 条") != std::string::npos);
  CHECK(imp.find("第 6 行") != std::string::npos);
  CHECK(imp.find("第 7 行") != std::string::npos);
  CHECK(imp.find("第 8 行") != std::string::npos);
  CHECK(imp.find("第 9 行") != std::string::npos);

  // 成员详情：直属上级＋链路逐级上溯（CLI 输出面）
  const std::string member = run_cli("org member dave --db " + db);
  CHECK(member.find("直属上级：carol") != std::string::npos);
  CHECK(member.find("部门：公司/研发部/客户端组") != std::string::npos);
  CHECK(member.find("carol → bob → alice") != std::string::npos);

  // 坏更新拒绝：把 bob 的上级改成 dave 会构成环
  int rc = 0;
  const std::string bad = run_cli("org set bob --manager dave --db " + db, &rc);
  CHECK(rc != 0);
  CHECK(bad.find("构成汇报环") != std::string::npos);
  CHECK(run_cli("org member bob --db " + db).find("直属上级：alice") !=
        std::string::npos); // 被拒后资料不变

  // 部门树列表
  const std::string dept_out = run_cli("org dept list --db " + db);
  CHECK(dept_out.find("公司") != std::string::npos);
  CHECK(dept_out.find("公司/研发部/客户端组") != std::string::npos);

  // —— T3.1 角色分级（admin）＋协议级组织架构下发（登录后可查）——
  CHECK(run_cli("account add root pw --role admin --db " + db)
            .find("admin") != std::string::npos);
  {
    memex::server::ServerStore srv_store;
    CHECK(srv_store.open(db));
    // 权限模型「群在组织架构可见」：建一群，断言 ORG_DATA 群组清单下发
    CHECK(srv_store.create_group("平台组", "alice", {"dave"}) > 0);
    asio::io_context io;
    memex::server::CollabServer server(io, srv_store, 0);
    server.start_accept();
    std::thread io_thread([&] { io.run(); });

    TestClient c(io, server.port());
    memex::protocol::Message login;
    login.set_type(memex::protocol::v1::LOGIN);
    login.set_from("pc-a");
    login.set_to("server");
    login.set_ts_ms(now_ms());
    auto* in = login.mutable_login();
    in->set_account("alice");
    in->set_password("pw");
    in->set_device_fingerprint(memex::server::sha256_hex("pc-a"));
    in->set_device_kind("desktop");
    in->set_device_name("pc-a");
    in->set_client_version("0.1.0-test");
    c.send(login);
    CHECK(c.read().login_result().ok());

    memex::protocol::Message q;
    q.set_type(memex::protocol::v1::ORG_QUERY);
    q.set_from("alice");
    q.set_to("server");
    q.set_ts_ms(now_ms());
    c.send(q);
    const auto data = c.read();
    CHECK(data.type() == memex::protocol::v1::ORG_DATA);
    CHECK(data.has_org_data());
    bool has_dept = false, has_dave = false, has_admin = false;
    const auto& od = data.org_data();
    for (const auto& d : od.departments()) {
      if (d.path() == "公司/研发部/客户端组") has_dept = true;
    }
    for (const auto& m : od.members()) {
      if (m.account() == "dave") {
        has_dave = m.manager() == "carol" &&
                   m.department_path() == "公司/研发部/客户端组";
      }
      if (m.account() == "root" && m.role() == "admin") has_admin = true;
    }
    CHECK(has_dept);   // 部门树下发
    CHECK(has_dave);   // 成员资料（直属上级）下发
    CHECK(has_admin);  // 角色分级可见
    // 群组清单（/etc/group 类比：全量群透明可查，含成员名单）
    bool has_org_group = false;
    for (const auto& g : od.groups()) {
      if (g.name() != "平台组" || g.owner() != "alice") continue;
      has_org_group = false;
      for (const auto& m : g.members()) {
        if (m == "dave") has_org_group = true;
      }
    }
    CHECK(has_org_group);

    // —— 需求批⑪：个性签名协议级——PROFILE_CMD 受理回执、超长拒绝、
    // ORG_QUERY 重查带出 ——
    const auto profile_cmd = [&](const std::string& text) {
      memex::protocol::Message m;
      m.set_type(memex::protocol::v1::PROFILE_CMD);
      m.set_from("alice");
      m.set_to("server");
      m.set_ts_ms(now_ms());
      auto* pc = m.mutable_profile_cmd();
      pc->set_op("set_signature");
      pc->set_signature(text);
      return m;
    };
    c.send(profile_cmd("今天也要专注交付"));
    const auto sr = c.read();
    CHECK(sr.type() == memex::protocol::v1::PROFILE_RESULT);
    CHECK(sr.has_profile_result());
    CHECK(sr.profile_result().ok());
    CHECK(sr.profile_result().op() == "set_signature");
    std::string over;
    for (int i = 0; i < 121; ++i) over += "签"; // 121 字（码点口径）
    c.send(profile_cmd(over));
    const auto lr = c.read();
    CHECK(lr.type() == memex::protocol::v1::PROFILE_RESULT);
    CHECK(!lr.profile_result().ok());
    CHECK(lr.profile_result().reason().find("上限") != std::string::npos);
    c.send(q); // 重查组织架构：本人签名随 ORG_DATA 带出
    const auto data2 = c.read();
    CHECK(data2.type() == memex::protocol::v1::ORG_DATA);
    bool sig_out = false;
    for (const auto& m : data2.org_data().members()) {
      if (m.account() == "alice") {
        sig_out = m.signature() == "今天也要专注交付";
      }
    }
    CHECK(sig_out);

    // —— 需求批⑩：在线时长协议级——登录即入流水（在线中末笔 online
    // 计到查询时点）；ORG_QUERY 现算滚动窗并集秒数随成员下发；从未登录
    // 的成员三窗皆零 ——
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    c.send(q);
    const auto data3 = c.read();
    CHECK(data3.type() == memex::protocol::v1::ORG_DATA);
    bool alice_online = false, dave_zero = false;
    for (const auto& m : data3.org_data().members()) {
      if (m.account() == "alice") {
        alice_online = m.online_day_s() >= 1 &&
                       m.online_week_s() >= m.online_day_s() &&
                       m.online_month_s() >= m.online_week_s();
      }
      if (m.account() == "dave") {
        dave_zero = m.online_day_s() == 0 && m.online_week_s() == 0 &&
                    m.online_month_s() == 0;
      }
    }
    CHECK(alice_online); // 登录时长≥1s 且三窗单调包含
    CHECK(dave_zero);

    io.stop();
    io_thread.join();
  }

  // —— T3.4 策略开关（库级）：全局／部门行、逐级解析、坏路径拒绝 ——
  {
    memex::server::ServerStore s;
    CHECK(s.open(":memory:"));
    CHECK(s.create_account("alice", "p", "Alice"));
    CHECK(s.create_account("bob", "p", "Bob"));
    const int dept = s.ensure_department_path("公司/研发部");
    CHECK(s.set_member_profile("bob", dept, "工程师", ""));
    auto pol = s.resolve_policy("bob"); // 未配置＝默认宽松
    CHECK(pol.allow_anonymous && pol.allow_cross_state &&
          !pol.new_device_approval);
    CHECK(s.set_policy("", false, true, true)); // 全局收紧
    pol = s.resolve_policy("alice");            // 无部门→全局
    CHECK(!pol.allow_anonymous && pol.new_device_approval);
    CHECK(s.set_policy("公司/研发部", true, false, false)); // 部门覆盖
    pol = s.resolve_policy("bob");
    CHECK(pol.allow_anonymous && !pol.allow_cross_state &&
          !pol.new_device_approval);
    const int sub = s.ensure_department_path("公司/研发部/客户端组");
    CHECK(s.set_member_profile("alice", sub, "组长", ""));
    pol = s.resolve_policy("alice"); // 子部门未配置→上级部门行
    CHECK(pol.allow_anonymous && !pol.allow_cross_state);
    CHECK(!s.set_policy("不存在/路径", true, true, true)); // 坏路径拒绝
    s.close();
  }

  // —— T3.4（CLI＋协议级）：新设备审批挡首登→审批放行；ORG_DATA 带策略 ——
  {
    const std::string pdb = "/tmp/memex-policy-test.db";
    std::remove(pdb.c_str());
    run_cli("account add alice pw --db " + pdb);
    CHECK(run_cli("policy set --new-device-approval on --db " + pdb)
              .find("新设备 需审批") != std::string::npos);
    CHECK(run_cli("policy show alice --db " + pdb).find("需审批") !=
          std::string::npos);

    memex::server::ServerStore srv;
    CHECK(srv.open(pdb));
    const auto login_alice = [&]() {
      memex::protocol::Message m;
      m.set_type(memex::protocol::v1::LOGIN);
      m.set_from("pc-new");
      m.set_to("server");
      m.set_ts_ms(now_ms());
      auto* in = m.mutable_login();
      in->set_account("alice");
      in->set_password("pw");
      in->set_device_fingerprint(memex::server::sha256_hex("pc-new"));
      in->set_device_kind("desktop");
      in->set_device_name("pc-new");
      in->set_client_version("0.1.0-test");
      return m;
    };
    asio::io_context io;
    memex::server::CollabServer server(io, srv, 0);
    server.start_accept();
    std::thread io_thread([&] { io.run(); });
    {
      TestClient c(io, server.port());
      c.send(login_alice());
      const auto r = c.read();
      CHECK(!r.login_result().ok()); // 首登被挡
      CHECK(r.login_result().reason().find("待审批") != std::string::npos);
    }
    CHECK(srv.device_list().size() == 1); // 已建档且停用（待审批）
    CHECK(!srv.device_list()[0].enabled);
    io.stop();
    io_thread.join();
    const std::string fp = srv.device_list()[0].fingerprint;
    srv.close();

    CHECK(run_cli("device enable " + fp.substr(0, 12) + " --db " + pdb)
              .find("已启用") != std::string::npos);

    memex::server::ServerStore srv2;
    CHECK(srv2.open(pdb));
    asio::io_context io2;
    memex::server::CollabServer server2(io2, srv2, 0);
    server2.start_accept();
    std::thread io_thread2([&] { io2.run(); });
    {
      TestClient c(io2, server2.port());
      c.send(login_alice());
      CHECK(c.read().login_result().ok()); // 审批后放行
      memex::protocol::Message q;
      q.set_type(memex::protocol::v1::ORG_QUERY);
      q.set_from("alice");
      q.set_to("server");
      q.set_ts_ms(now_ms());
      c.send(q);
      const auto data = c.read();
      CHECK(data.type() == memex::protocol::v1::ORG_DATA);
      bool has_global_policy = false;
      for (const auto& p : data.org_data().policies()) {
        if (p.department_path().empty() && p.new_device_approval()) {
          has_global_policy = true; // 只设了审批开关，其余两项保持基线默认
        }
      }
      CHECK(has_global_policy); // 策略随组织架构下发
    }
    io2.stop();
    io_thread2.join();
    srv2.close();
  }

  if (g_failures == 0) {
    std::cout << "org tests: all passed\n";
    return 0;
  }
  std::cerr << "org tests: " << g_failures << " failure(s)\n";
  return 1;
}
