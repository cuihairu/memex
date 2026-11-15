// LAY-009 拆分模块（文件与配额面（台账/删除/两级配额/存储状态））——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

void leg_files() {
  PHASE("leg-24");
  // ㉔ 文件与配额·台账与置顶：全量台账回读（群/个人两行）、pin 回读、
  //    未知 id 404、member 读写 403
  {
    const auto l = http("GET", "/api/admin/files", {}, root_cookie);
    CHECK(l.code == 200);
    CHECK(l.text.find("group-a.bin") != std::string::npos);
    CHECK(l.text.find("personal-b.txt") != std::string::npos);
    Json::StreamWriterBuilder wb;
    Json::Value pinb;
    pinb["id"] = file_a;
    pinb["pin"] = true;
    CHECK(http("POST", "/api/admin/files/pin", Json::writeString(wb, pinb),
               root_cookie).code == 200);
    const Json::Value f1 =
        jbody(http("GET", "/api/admin/files?limit=50", {}, root_cookie)
                  .text)["files"];
    bool pinned = false;
    for (const auto& x : f1) {
      if (x["id"].asInt64() == file_a) pinned = x["pin"].asBool();
    }
    CHECK(pinned); // pin 回读
    pinb["pin"] = false;
    CHECK(http("POST", "/api/admin/files/pin", Json::writeString(wb, pinb),
               root_cookie).code == 200);
    Json::Value nof;
    nof["id"] = 999999;
    nof["pin"] = true;
    CHECK(http("POST", "/api/admin/files/pin", Json::writeString(wb, nof),
               root_cookie).code == 404);
    // member 越权：读台账与写置顶都 403
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(http("GET", "/api/admin/files", {}, lr.set_cookie).code == 403);
    CHECK(http("POST", "/api/admin/files/pin",
               Json::writeString(wb, pinb), lr.set_cookie).code == 403);
  }

  PHASE("leg-25");
  // ㉕ 文件与配额·删除：台账行消失＋个人配额退费（回读对账）、二次删除 404
  {
    Json::StreamWriterBuilder wb;
    Json::Value d;
    d["id"] = file_b;
    CHECK(http("POST", "/api/admin/files/delete", Json::writeString(wb, d),
               root_cookie).code == 200);
    const auto l = http("GET", "/api/admin/files?limit=50", {}, root_cookie);
    CHECK(l.code == 200);
    CHECK(l.text.find("personal-b.txt") == std::string::npos); // 行已删
    CHECK(l.text.find("group-a.bin") != std::string::npos);    // A 不受影响
    CHECK(http("POST", "/api/admin/files/delete", Json::writeString(wb, d),
               root_cookie).code == 404);
  }

  PHASE("leg-26");
  // ㉖ 文件与配额·两级配额：群/个人写入回读一致（含删除退费对账：
  //    m1 used=1500-500=1000）、校验面（坏 scope/负限/缺 target 400、
  //    未知群/未知号 404）、member 403
  {
    Json::StreamWriterBuilder wb;
    Json::Value sg;
    sg["scope"] = "group";
    sg["target"] = std::to_string(gid);
    sg["limit_bytes"] = 10000;
    CHECK(http("POST", "/api/admin/quotas", Json::writeString(wb, sg),
               root_cookie).code == 200);
    Json::Value su;
    su["scope"] = "user";
    su["target"] = "m1";
    su["limit_bytes"] = 5000;
    CHECK(http("POST", "/api/admin/quotas", Json::writeString(wb, su),
               root_cookie).code == 200);
    const Json::Value q =
        jbody(http("GET", "/api/admin/quotas", {}, root_cookie).text);
    bool gok = false, uok = false;
    for (const auto& g : q["groups"]) {
      if (g["target"].asString() == std::to_string(gid)) {
        gok = g["limit_bytes"].asInt64() == 10000 &&
              g["used_bytes"].asInt64() == 1000;
      }
    }
    for (const auto& u : q["users"]) {
      if (u["target"].asString() == "m1") {
        uok = u["limit_bytes"].asInt64() == 5000 &&
              u["used_bytes"].asInt64() == 1000; // B 删除退费 500 后
      }
    }
    CHECK(gok);
    CHECK(uok);
    // 校验面
    Json::Value bad;
    bad["scope"] = "org"; // 非 group|user
    bad["target"] = "1";
    bad["limit_bytes"] = 1;
    CHECK(http("POST", "/api/admin/quotas", Json::writeString(wb, bad),
               root_cookie).code == 400);
    bad["scope"] = "group";
    bad["limit_bytes"] = -1; // 负限
    CHECK(http("POST", "/api/admin/quotas", Json::writeString(wb, bad),
               root_cookie).code == 400);
    bad["target"] = "";
    bad["limit_bytes"] = 1; // 缺 target
    CHECK(http("POST", "/api/admin/quotas", Json::writeString(wb, bad),
               root_cookie).code == 400);
    bad["target"] = "999999"; // 未知群
    CHECK(http("POST", "/api/admin/quotas", Json::writeString(wb, bad),
               root_cookie).code == 404);
    Json::Value gu;
    gu["scope"] = "user";
    gu["target"] = "ghost"; // 未知账号
    gu["limit_bytes"] = 1;
    CHECK(http("POST", "/api/admin/quotas", Json::writeString(wb, gu),
               root_cookie).code == 404);
    // member 越权写 403
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(http("POST", "/api/admin/quotas", Json::writeString(wb, su),
               lr.set_cookie).code == 403);
  }

  PHASE("leg-27");
  // ㉗ 存储状态只读断言：GET 回注记（只读不代启停）与未配置口径、写入口
  //    不存在（POST 同路径 404）、member 403
  {
    const auto r = http("GET", "/api/admin/storage/status", {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value st = jbody(r.text);
    CHECK(std::string(st["note"].asString()).find("只读") != std::string::npos);
    CHECK(std::string(st["files"].asString()).find("未配置") !=
          std::string::npos); // 测试进程未配 MEMEX_FILES_URL
    CHECK(st["storage"].isString());
    // 无写入口：路由只注册 GET，POST 同路径必须拒（404/405，框架口径）
    const int post_code =
        http("POST", "/api/admin/storage/status", "{}", root_cookie).code;
    CHECK(post_code == 404 || post_code == 405);
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(http("GET", "/api/admin/storage/status", {}, lr.set_cookie).code ==
          403);
  }

  // —— 群组管理面（㉙~㉟）：member 会话登一次跨腿复用 ——
  {
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(lr.code == 200);
    m1ck = lr.set_cookie;
  }

}
