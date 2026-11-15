// admin 后台 API 验收（2026-10-11 用户令·波次 1）：真 admin 进程（Drogon
// setup+run 进程内起服）× 真库逐腿打穿——登录门（同源 PBKDF2/错口令/
// CSRF 头）、角色门（org-admin 管理面；基础 admin≠org-admin；member 越权
// 403 留痕）、账号与组织（建号/部门/归属/资料/角色授权吊销/汇报环）、
// 策略下发、留存策略（双人审批红线：审批人≠执行人）、归档检索与导出
// （auditor 证，org-admin 不自动可读消息＝平台-6 红线）、设备与登录台账、
// 健康面。判权拒绝与审计被拒都落库/落日志，腿内回读对账。

#include "admin_api/common.hpp"

int main() {
  // —— 造库：org-admin（base admin + 追加 org-admin）、auditor、member ——
  if (seed_db() != 0) return 2;   // 造库失败原样退出码

  PHASE("seed-done");
  // —— 起 admin（进程内另线程跑主循环）——
  const std::string port = "18089";
  std::vector<std::string> args = {"test_admin_api", "--port", port, "--db", db,
                                   "--www", MEMEX_ADMIN_WWW_DIR, "--session-ttl", "60"};
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  PHASE("setup-call");
  CHECK(memex::admin::setup(static_cast<int>(argv.size()) - 1, argv.data()) == 0);
  std::thread loop([] { drogon::app().run(); });

  PHASE("setup-done");
  // 等监听就绪（探 whoami 通即可，401/200 都算活着）
  bool up = false;
  for (int i = 0; i < 100 && !up; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    up = http("GET", "/api/whoami").code != 0;
  }
  CHECK(up);


  leg_auth();
  leg_policy();
  leg_archive();
  leg_surface();
  leg_notify();
  leg_files();
  leg_groups();
  leg_bizflow();
  leg_brand();
  leg_tasks_pull();

  // ㊷ 登出后会话失效
  {
    CHECK(http("POST", "/api/logout", "{}", root_cookie).code == 200);
    CHECK(http("GET", "/api/whoami", {}, root_cookie).code == 401);
  }

  drogon::app().quit();
  loop.join();
  std::filesystem::remove_all(tmp);

  if (g_failures == 0) {
    PHASE("done");
    std::cout << "admin api tests: all passed\n";
    return 0;
  }
  PHASE("done-fail");
    std::cerr << "admin api tests: " << g_failures << " failure(s)\n";
  return 1;
}
