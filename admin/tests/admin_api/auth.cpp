// LAY-009 拆分模块（登录门/CSRF/角色门/账号与组织面）——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

void leg_auth() {
  PHASE("leg-1");
  // ① 未登录：whoami 401；管理面同样 401
  CHECK(http("GET", "/api/whoami").code == 401);
  CHECK(http("GET", "/api/admin/accounts").code == 401);

  PHASE("leg-2");
  // ② CSRF 轻闸：缺 X-Memex-Admin 头的登录 403（测试客户端恒带头，
  //    这里临时绕开＝直接打原始请求）
  {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setPath("/api/login");
    req->setContentTypeString("application/json");
    req->setBody(R"({"account":"root","password":"root-pw"})");
    static trantor::EventLoopThread loop2;
    static auto c2 = [&] {
      loop2.run();
      return drogon::HttpClient::newHttpClient(kBase, loop2.getLoop());
    }();
    const auto [result, resp] = c2->sendRequest(req, 5.0);
    CHECK(result == drogon::ReqResult::Ok && resp &&
          static_cast<int>(resp->getStatusCode()) == 403);
  }

  PHASE("leg-3");
  // ③ 错口令 401
  {
    const auto r = http("POST", "/api/login",
                        R"({"account":"root","password":"wrong"})");
    CHECK(r.code == 401);
  }

  PHASE("leg-4");
  // ④ root 登录：org-admin 在生效角色（base admin＋追加授权并集）
  {
    const auto r = http("POST", "/api/login",
                        R"({"account":"root","password":"root-pw"})");
    CHECK(r.code == 200);
    CHECK(has_role(jbody(r.text), "org-admin"));
    root_cookie = r.set_cookie;
    CHECK(!root_cookie.empty());
  }

  PHASE("leg-5");
  // ⑤ whoami 带会话
  {
    const auto r = http("GET", "/api/whoami", {}, root_cookie);
    CHECK(r.code == 200);
    CHECK(jbody(r.text)["account"].asString() == "root");
  }

  PHASE("leg-6");
  // ⑥ member 越权：登录通过但管理面 403（default-deny 口径）
  {
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(lr.code == 200);
    const auto r = http("GET", "/api/admin/accounts", {}, lr.set_cookie);
    CHECK(r.code == 403);
    // 平台-6 红线：member（无 auditor）查归档 403 且留痕
    const auto ar = http("POST", "/api/admin/messages/search",
                         R"({"account":"m1"})", lr.set_cookie);
    CHECK(ar.code == 403);
  }

  PHASE("leg-7");
  // ⑦ 基础 admin≠org-admin：base1 无追加授权，管理面 403
  {
    const auto lr = http("POST", "/api/login",
                         R"({"account":"base1","password":"b1-pw"})");
    CHECK(lr.code == 200);
    CHECK(!has_role(jbody(lr.text), "org-admin"));
    const auto r = http("GET", "/api/admin/policy", {}, lr.set_cookie);
    CHECK(r.code == 403);
  }

  PHASE("leg-8");
  // ⑧ 建号：成功/重名 409/坏角色 400
  {
    const auto r = http("POST", "/api/admin/accounts",
                        R"({"account":"m2","password":"m2-pw","name":"M2","role":"member"})",
                        root_cookie);
    CHECK(r.code == 200);
    CHECK(http("POST", "/api/admin/accounts",
               R"({"account":"m2","password":"x","role":"member"})",
               root_cookie).code == 409);
    CHECK(http("POST", "/api/admin/accounts",
               R"({"account":"m3","password":"x","role":"superuser"})",
               root_cookie).code == 400);
  }

  PHASE("leg-9");
  // ⑨ 部门：建树＋列表回读（JSON 解析比对——jsoncpp 应答非 ASCII 转义，
  //    原文 find 不可靠）
  {
    const auto r = http("POST", "/api/admin/departments",
                        R"({"path":"公司/研发部"})", root_cookie);
    CHECK(r.code == 200);
    CHECK(jbody(r.text)["id"].asInt64() > 0);
    const auto l = http("GET", "/api/admin/departments", {}, root_cookie);
    CHECK(l.code == 200);
    // 先物化再遍历（对临时量链做 range-for＝悬垂引用）
    const Json::Value deps = jbody(l.text)["departments"];
    bool dept_seen = false;
    for (const auto& d : deps) {
      if (d["path"].asString() == "公司/研发部") dept_seen = true;
    }
    CHECK(dept_seen);
  }

  PHASE("leg-10");
  // ⑩ 归属与资料：加部门 200、重复 409、汇报环 409、自为上级 409
  {
    const auto mems = http("GET", "/api/admin/departments", {}, root_cookie);
    const Json::Value dep = jbody(mems.text)["departments"][0];
    Json::Value body;
    body["account"] = "m2";
    body["department_id"] = dep["id"];
    body["op"] = "add";
    Json::StreamWriterBuilder wb;
    CHECK(http("POST", "/api/admin/membership",
               Json::writeString(wb, body), root_cookie).code == 200);
    body["op"] = "add";
    CHECK(http("POST", "/api/admin/membership",
               Json::writeString(wb, body), root_cookie).code == 409);
    // 建档：部门＋职务＋直属上级 root
    Json::Value prof;
    prof["account"] = "m2";
    prof["department_id"] = dep["id"];
    prof["title"] = "工程师";
    prof["manager"] = "root";
    CHECK(http("POST", "/api/admin/profile",
               Json::writeString(wb, prof), root_cookie).code == 200);
    // 自为上级（manager==account）409
    prof["manager"] = "m2";
    CHECK(http("POST", "/api/admin/profile",
               Json::writeString(wb, prof), root_cookie).code == 409);
  }

  PHASE("leg-11");
  // ⑪ 角色授权：给 m1 再授 auditor→whoami 可见；吊销后消失
  {
    CHECK(http("POST", "/api/admin/roles",
               R"({"account":"m1","role":"auditor"})", root_cookie).code == 200);
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(has_role(jbody(lr.text), "auditor"));
    // m1 现在能查归档（auditor 有效）
    CHECK(http("POST", "/api/admin/messages/search",
               R"({"account":"m1"})", lr.set_cookie).code == 200);
    // 吊销（台账首行）后立即失效（角色现查不缓存）
    const auto lst = http("GET", "/api/admin/roles?account=m1", {}, root_cookie);
    const Json::Value first = jbody(lst.text)["assignments"][0];
    Json::Value rev;
    rev["id"] = first["id"];
    Json::StreamWriterBuilder wb;
    CHECK(http("POST", "/api/admin/roles/revoke",
               Json::writeString(wb, rev), root_cookie).code == 200);
    CHECK(has_role(jbody(http("GET", "/api/whoami", {}, lr.set_cookie).text),
                   "auditor") == false);
  }

}
