// admin 后台 API 验收（2026-10-11 用户令·波次 1）：真 admin 进程（Drogon
// setup+run 进程内起服）× 真库逐腿打穿——登录门（同源 PBKDF2/错口令/
// CSRF 头）、角色门（org-admin 管理面；基础 admin≠org-admin；member 越权
// 403 留痕）、账号与组织（建号/部门/归属/资料/角色授权吊销/汇报环）、
// 策略下发、留存策略（双人审批红线：审批人≠执行人）、归档检索与导出
// （auditor 证，org-admin 不自动可读消息＝平台-6 红线）、设备与登录台账、
// 健康面。判权拒绝与审计被拒都落库/落日志，腿内回读对账。
#include <admin_app.hpp>

#include <drogon/drogon.h>
#include <json/json.h>
#include <trantor/net/EventLoopThread.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <cred.hpp>
#include <store.hpp>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #cond    \
                << "\n";                                                     \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

const std::string kBase = "http://127.0.0.1:18089";

// 同步 HTTP（future 口）：method/path/body/cookie → {code, json|text, set-cookie}
struct Resp {
  int code = 0;
  std::string text;
  std::string set_cookie;
};

Resp http(const std::string& method, const std::string& path,
          const std::string& body = {}, const std::string& cookie = {}) {
  static trantor::EventLoopThread loop;
  static auto client = [&] {
    loop.run(); // getLoop() 只回指针不起线程——不起 IO 线程同步请求永挂
    return drogon::HttpClient::newHttpClient(kBase, loop.getLoop());
  }();
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(method == "GET" ? drogon::Get : drogon::Post);
  req->setPath(path);
  if (!body.empty()) {
    req->setContentTypeString("application/json");
    req->setBody(body);
  }
  if (!cookie.empty()) req->addHeader("Cookie", cookie);
  req->addHeader("X-Memex-Admin", "1");
  const auto [result, resp] = client->sendRequest(req, 5.0);
  if (result != drogon::ReqResult::Ok || !resp) return {};
  Resp r;
  r.code = static_cast<int>(resp->getStatusCode());
  r.text = std::string(resp->getBody());
  // Drogon 把应答 Cookie 解析进 cookie 表而非 headers——会话键逐字取回
  for (const auto& [name, ck] : resp->cookies()) {
    r.set_cookie = name + "=" + ck.value();
    break;
  }
  return r;
}

Json::Value jbody(const std::string& s) {
  Json::Value j;
  Json::Reader reader;
  reader.parse(s, j);
  return j;
}

bool has_role(const Json::Value& j, const char* role) {
  for (const auto& r : j["roles"]) {
    if (r.asString() == role) return true;
  }
  return false;
}

} // namespace

int main() {
  // —— 造库：org-admin（base admin + 追加 org-admin）、auditor、member ——
  const auto tmp = std::filesystem::temp_directory_path() /
                   ("memex-admin-test-" + std::to_string(::getpid()));
  std::filesystem::create_directories(tmp);
  const std::string db = (tmp / "memex.db").string();
  {
    memex::server::ServerStore seed;
    if (!seed.open(db)) {
      std::cerr << "seed db open failed\n";
      return 2;
    }
    CHECK(seed.create_account("root", "root-pw", "Root", "admin"));
    CHECK(seed.role_grant("root", "org-admin", "", 0, 0, "seed", 1) > 0);
    CHECK(seed.create_account("aud1", "aud-pw", "Aud", "member"));
    CHECK(seed.role_grant("aud1", "auditor", "", 0, 0, "seed", 1) > 0);
    CHECK(seed.create_account("m1", "m1-pw", "M1", "member"));
    CHECK(seed.create_account("base1", "b1-pw", "B1", "admin")); // 基础 admin≠org-admin
    // 归档真消息（检索/导出腿）
    CHECK(seed.store_message("msg-1", "m1", "aud1", 1, "机密文本你好", 1700000000000));
    seed.close();
  }

  // —— 起 admin（进程内另线程跑主循环）——
  const std::string port = "18089";
  std::vector<std::string> args = {"test_admin_api", "--port", port, "--db", db,
                                   "--www", MEMEX_ADMIN_WWW_DIR, "--session-ttl", "60"};
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  CHECK(memex::admin::setup(static_cast<int>(argv.size()) - 1, argv.data()) == 0);
  std::thread loop([] { drogon::app().run(); });

  // 等监听就绪（探 whoami 通即可，401/200 都算活着）
  bool up = false;
  for (int i = 0; i < 100 && !up; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    up = http("GET", "/api/whoami").code != 0;
  }
  CHECK(up);

  // ① 未登录：whoami 401；管理面同样 401
  CHECK(http("GET", "/api/whoami").code == 401);
  CHECK(http("GET", "/api/admin/accounts").code == 401);

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

  // ③ 错口令 401
  {
    const auto r = http("POST", "/api/login",
                        R"({"account":"root","password":"wrong"})");
    CHECK(r.code == 401);
  }

  // ④ root 登录：org-admin 在生效角色（base admin＋追加授权并集）
  std::string root_cookie;
  {
    const auto r = http("POST", "/api/login",
                        R"({"account":"root","password":"root-pw"})");
    CHECK(r.code == 200);
    CHECK(has_role(jbody(r.text), "org-admin"));
    root_cookie = r.set_cookie;
    CHECK(!root_cookie.empty());
  }

  // ⑤ whoami 带会话
  {
    const auto r = http("GET", "/api/whoami", {}, root_cookie);
    CHECK(r.code == 200);
    CHECK(jbody(r.text)["account"].asString() == "root");
  }

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

  // ⑦ 基础 admin≠org-admin：base1 无追加授权，管理面 403
  {
    const auto lr = http("POST", "/api/login",
                         R"({"account":"base1","password":"b1-pw"})");
    CHECK(lr.code == 200);
    CHECK(!has_role(jbody(lr.text), "org-admin"));
    const auto r = http("GET", "/api/admin/policy", {}, lr.set_cookie);
    CHECK(r.code == 403);
  }

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

  // ⑫ 策略下发：全局行写入＋列表回读
  {
    CHECK(http("POST", "/api/admin/policy",
               R"({"department_path":"","allow_anonymous":true,"allow_cross_state":false,"new_device_approval":true,"allow_cross_dept_file":true,"allow_forward_file":false})",
               root_cookie).code == 200);
    const auto l = http("GET", "/api/admin/policy", {}, root_cookie);
    CHECK(l.code == 200);
    const Json::Value row = jbody(l.text)["rows"][0];
    CHECK(row["allow_cross_state"].asBool() == false);
    CHECK(row["new_device_approval"].asBool() == true);
    CHECK(row["allow_forward_file"].asBool() == false);
  }

  // ⑬ 留存：配置 30 天；purge 双人审批红线（自审 409、缺理由 400、合规 200）
  {
    CHECK(http("POST", "/api/admin/retention",
               R"({"days":30,"department_path":""})", root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/retention",
               R"({"days":45,"department_path":""})", root_cookie).code == 409);
    // 缺理由 400
    CHECK(http("POST", "/api/admin/retention/purge",
               R"({"before_ms":1800000000000,"approved_by":"aud1"})", root_cookie).code == 400);
    // 自审 409（approved_by==purged_by=root）
    CHECK(http("POST", "/api/admin/retention/purge",
               R"({"before_ms":1800000000000,"reason":"r","approved_by":"root"})",
               root_cookie).code == 409);
    // 合规：before_ms 取早于 msg-1（1700000000000）的时刻——双人审批与
    // 台账落库走通、回执 id>0，且不清掉后续检索/导出腿要用的归档行
    const auto r = http("POST", "/api/admin/retention/purge",
                        R"({"before_ms":1600000000000,"reason":"过期清理","approved_by":"aud1","policy_days":30})",
                        root_cookie);
    CHECK(r.code == 200);
    CHECK(jbody(r.text)["id"].asInt64() > 0);
  }

  // ⑭ 审计红线：root（org-admin 无 auditor）检索 403；auditor 检索命中真
  //    消息；查阅日志含 root 的 audit.denied 留痕
  {
    const auto deny = http("POST", "/api/admin/messages/search",
                           R"({"keyword":"机密"})", root_cookie);
    CHECK(deny.code == 403);
    const auto lr = http("POST", "/api/login",
                         R"({"account":"aud1","password":"aud-pw"})");
    CHECK(lr.code == 200);
    CHECK(has_role(jbody(lr.text), "auditor"));
    const auto s = http("POST", "/api/admin/messages/search",
                        R"({"keyword":"机密"})", lr.set_cookie);
    CHECK(s.code == 200);
    const Json::Value msgs = jbody(s.text)["messages"];
    CHECK(msgs.size() == 1);
    CHECK(msgs[0]["text"].asString() == "机密文本你好");
    CHECK(msgs[0]["from"].asString() == "m1");
    // 查阅日志回读：audit.denied（root 被拒）与 audit.message.search 同表
    const auto reads = http("GET", "/api/admin/audit-reads", {}, lr.set_cookie);
    CHECK(reads.code == 200);
    CHECK(reads.text.find("audit.denied") != std::string::npos);
    CHECK(reads.text.find("audit.message.search") != std::string::npos);
    // root 无 auditor：查日志本身也 403（与 CLI 同口径）
    CHECK(http("GET", "/api/admin/audit-reads", {}, root_cookie).code == 403);
  }

  // ⑮ 导出：auditor 导出命中、JSONL 体、落 audit.message.export
  {
    const auto lr = http("POST", "/api/login",
                         R"({"account":"aud1","password":"aud-pw"})");
    const auto r = http("POST", "/api/admin/messages/export",
                        R"({"keyword":"机密"})", lr.set_cookie);
    CHECK(r.code == 200);
    // JSONL 行含命中记录（msg_id 为 ASCII 锚点，免受 jsoncpp 转义口径影响）
    CHECK(r.text.find("msg-1") != std::string::npos);
    const auto reads = http("GET", "/api/admin/audit-reads", {}, lr.set_cookie);
    CHECK(reads.text.find("audit.message.export") != std::string::npos);
  }

  // ⑯ 设备与登录台账（空表 200；登录台账已有本次测试的 /files 面外记录＝
  //    admin 登录不走 login_records，表应为空——只验面通）
  CHECK(http("GET", "/api/admin/devices", {}, root_cookie).code == 200);
  CHECK(http("GET", "/api/admin/logins", {}, root_cookie).code == 200);

  // ⑰ 健康：ok＋db open（未配 files-url＝显式「未配置」）
  {
    const auto r = http("GET", "/api/admin/health", {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value h = jbody(r.text);
    CHECK(h["ok"].asBool() == true);
    CHECK(h["db"].asBool() == true);
    CHECK(std::string(h["files"].asString()).find("未配置") != std::string::npos);
  }

  // ⑱ 静态页：/ 回 index.html
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
    CHECK(result == drogon::ReqResult::Ok && resp &&
          resp->getBody().find("Memex 管理后台") != std::string::npos);
  }

  // ⑲ 登出后会话失效
  {
    CHECK(http("POST", "/api/logout", "{}", root_cookie).code == 200);
    CHECK(http("GET", "/api/whoami", {}, root_cookie).code == 401);
  }

  drogon::app().quit();
  loop.join();
  std::filesystem::remove_all(tmp);

  if (g_failures == 0) {
    std::cout << "admin api tests: all passed\n";
    return 0;
  }
  std::cerr << "admin api tests: " << g_failures << " failure(s)\n";
  return 1;
}
