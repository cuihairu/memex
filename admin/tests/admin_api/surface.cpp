// LAY-009 拆分模块（设备台账/健康/静态资源面）——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

void leg_surface() {
  PHASE("leg-16");
  // ⑯ 设备与登录台账（空表 200；登录台账已有本次测试的 /files 面外记录＝
  //    admin 登录不走 login_records，表应为空——只验面通）
  CHECK(http("GET", "/api/admin/devices", {}, root_cookie).code == 200);
  CHECK(http("GET", "/api/admin/logins", {}, root_cookie).code == 200);

  PHASE("leg-17");
  // ⑰ 健康：ok＋db open（未配 files-url＝显式「未配置」）
  {
    const auto r = http("GET", "/api/admin/health", {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value h = jbody(r.text);
    CHECK(h["ok"].asBool() == true);
    CHECK(h["db"].asBool() == true);
    CHECK(std::string(h["files"].asString()).find("未配置") != std::string::npos);
  }

  PHASE("leg-18");
  // ⑱ 静态页：/ 回 index.html；LAY-008 拆分后 index.html 引用的页面模块
  //    须真能取到（目录内静态路由，缺一个则前端加载期直接崩）
  {
    static trantor::EventLoopThread loop3;
    static auto c3 = [&] {
      loop3.run();
      return drogon::HttpClient::newHttpClient(kBase, loop3.getLoop());
    }();
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Get);
    req->setPath("/");
    const auto [result, resp] = c3->sendRequest(req, 5.0);
    const std::string home = resp ? std::string(resp->getBody()) : "";
    CHECK(result == drogon::ReqResult::Ok && resp &&
          home.find("Memex 管理后台") != std::string::npos);
    // index.html 里每个 script src 逐一取回：200＋非空，且引用顺序即加载序
    const std::vector<std::string> scripts = {
      "/admin/helpers.js", "/admin/accounts.js", "/admin/policy.js",
      "/admin/messages.js", "/admin/notify.js", "/admin/tasks.js",
      "/admin/files.js", "/admin/groups.js", "/admin/flow.js",
      "/admin/brand.js", "/admin/admin.js"};
    for (const auto& s : scripts) {
      CHECK(home.find(s) != std::string::npos);
      const auto r = http("GET", s);
      CHECK(r.code == 200 && !r.text.empty());
    }
    // 公共面须先于入口加载（pages 注册表在 helpers，路由在 admin.js）
    CHECK(home.find("/admin/helpers.js") != std::string::npos &&
          home.find("/admin/helpers.js") < home.find("/admin/admin.js"));
  }

}
