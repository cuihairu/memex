// LAY-009 拆分模块（任务源拉取与任务类型面）——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

void leg_tasks_pull() {
  // ㊹ 任务源拉取：盘点回读（缺行＝默认关）、源行掩码（只见
  //    cred_configured 位，无明文无密文）、开关写回读、缺字段 400、
  //    member 越权 403
  {
    const auto g = http("GET", "/api/admin/task-pull", {}, root_cookie);
    CHECK(g.code == 200);
    const Json::Value st = jbody(g.text);
    CHECK(st["enabled"].asBool() == false); // 缺行＝默认关（数据外发默认关）
    CHECK(st["sources"].size() == 1);
    CHECK(st["sources"][0]["owner"].asString() == "m1");
    CHECK(st["sources"][0]["provider"].asString() == "github");
    CHECK(st["sources"][0]["repo"].asString() == "acme/api");
    CHECK(st["sources"][0]["enabled"].asBool() == true);
    CHECK(st["sources"][0]["interval_s"].asInt64() == 60);
    CHECK(st["sources"][0]["cred_configured"].asBool() == true);
    CHECK(g.text.find("deadbeef-sealed") == std::string::npos); // 密文不出
    // 开关写：开→回读真值→关（测试库不留外发面开启态）
    CHECK(http("POST", "/api/admin/task-pull", R"({"enabled":true})",
               root_cookie).code == 200);
    CHECK(jbody(http("GET", "/api/admin/task-pull", {}, root_cookie)
                    .text)["enabled"].asBool() == true);
    CHECK(http("POST", "/api/admin/task-pull", R"({"enabled":false})",
               root_cookie).code == 200);
    CHECK(jbody(http("GET", "/api/admin/task-pull", {}, root_cookie)
                    .text)["enabled"].asBool() == false);
    // 缺字段 400；member 读写都 403
    CHECK(http("POST", "/api/admin/task-pull", "{}", root_cookie).code == 400);
    const auto lm1 =
        http("POST", "/api/login", R"({"account":"m1","password":"m1-pw"})");
    CHECK(http("GET", "/api/admin/task-pull", {}, lm1.set_cookie).code == 403);
    CHECK(http("POST", "/api/admin/task-pull", R"({"enabled":true})",
               lm1.set_cookie).code == 403);
  }

  // ㊺ 任务类型清单（服务端配置）：初始四类与默认回读、加类型、重名拒、
  //    缺参 400、删默认/删有引用/删不存在拒、删空类型放行、member 403
  {
    const auto g = http("GET", "/api/admin/task-types", {}, root_cookie);
    CHECK(g.code == 200);
    const Json::Value st = jbody(g.text);
    CHECK(st["types"].size() == 4);
    CHECK(st["default"].asString() == "任务");
    CHECK(http("POST", "/api/admin/task-types", R"({"name":"运维"})",
               root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/task-types", R"({"name":"运维"})",
               root_cookie).code == 400); // 重名
    CHECK(http("POST", "/api/admin/task-types", "{}", root_cookie).code ==
          400); // 缺参
    CHECK(http("POST", "/api/admin/task-types/delete", R"({"name":"任务"})",
               root_cookie).code == 400); // 默认类型拒删
    CHECK(http("POST", "/api/admin/task-types/delete", R"({"name":"运维"})",
               root_cookie).code == 400); // 有任务引用拒删（存量不悬空）
    CHECK(http("POST", "/api/admin/task-types", R"({"name":"临时"})",
               root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/task-types/delete", R"({"name":"临时"})",
               root_cookie).code == 200); // 空类型放行
    CHECK(http("POST", "/api/admin/task-types/delete", R"({"name":"临时"})",
               root_cookie).code == 400); // 二次删＝不存在
    const auto lm2 =
        http("POST", "/api/login", R"({"account":"m1","password":"m1-pw"})");
    CHECK(http("GET", "/api/admin/task-types", {}, lm2.set_cookie).code ==
          403);
    CHECK(http("POST", "/api/admin/task-types", R"({"name":"x"})",
               lm2.set_cookie).code == 403);
  }
}
