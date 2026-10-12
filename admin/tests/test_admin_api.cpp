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
  std::uint64_t gid = 0; // 通知与集成腿：webhook 群级目标用
  std::int64_t file_a = 0, file_b = 0; // 文件与配额腿：群文件 A／个人文件 B
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
    // 群（webhook group: 目标校验与三级盘点群名解析用）
    gid = seed.create_group("盘点群", "root", {"m1"});
    CHECK(gid > 0);
    // 文件台账（文件与配额腿）：群文件 A（1000B）＋个人文件 B（500B）；
    // 扣费基线按 files 面上传双面计费同口径（群文件群+个人都计）
    {
      memex::server::ServerStore::FileMeta fa;
      fa.owner = "m1";
      fa.belong_gid = std::to_string(gid);
      fa.file_name = "group-a.bin";
      fa.file_size = 1000;
      fa.file_hash = "hash-file-a";
      fa.object_key = "obj-file-a";
      fa.upload_ts = 1700000100000;
      file_a = seed.create_file_meta(fa);
      CHECK(file_a > 0);
      memex::server::ServerStore::FileMeta fb;
      fb.owner = "m1";
      fb.belong_uid = "m1";
      fb.file_name = "personal-b.txt";
      fb.file_size = 500;
      fb.file_hash = "hash-file-b";
      fb.object_key = "obj-file-b";
      fb.upload_ts = 1700000200000;
      file_b = seed.create_file_meta(fb);
      CHECK(file_b > 0);
      CHECK(seed.add_group_quota_used(std::to_string(gid), 1000));
      CHECK(seed.add_user_quota_used("m1", 1500));
    }
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

  // ⑳ 通知与集成·webhook：create token 仅此一次（明文不再出现在任何
  //    list 应答）、目标校验（不存在 404）、member 越权 403、吊销二次 404
  std::string whk_token;
  {
    // 目标不存在 → 404（与 CLI create 建即校验同口径）
    CHECK(http("POST", "/api/admin/webhooks",
               R"({"target":"nosuch"})", root_cookie).code == 404);
    // 缺 target → 400
    CHECK(http("POST", "/api/admin/webhooks", "{}", root_cookie).code == 400);
    const auto r1 = http("POST", "/api/admin/webhooks",
                         R"({"target":"m1","name":"个人hook"})", root_cookie);
    CHECK(r1.code == 200);
    whk_token = jbody(r1.text)["token"].asString();
    CHECK(whk_token.rfind("whk_", 0) == 0);
    CHECK(jbody(r1.text)["id"].asInt64() > 0);
    Json::Value grp;
    grp["target"] = "group:" + std::to_string(gid);
    Json::StreamWriterBuilder wbg;
    const auto r2 = http("POST", "/api/admin/webhooks",
                         Json::writeString(wbg, grp), root_cookie);
    CHECK(r2.code == 200);
    // list：摘要前缀 12 位在台账，明文 token 不再出现在任何应答
    const auto l = http("GET", "/api/admin/webhooks", {}, root_cookie);
    CHECK(l.code == 200);
    CHECK(l.text.find(whk_token) == std::string::npos);
    const Json::Value hooks = jbody(l.text)["webhooks"];
    CHECK(hooks.size() == 2);
    CHECK(hooks[0]["token_prefix"].asString().size() == 12);
    // member 越权：读写都 403（org-admin 面口径）
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(http("GET", "/api/admin/webhooks", {}, lr.set_cookie).code == 403);
    CHECK(http("POST", "/api/admin/webhooks",
               R"({"target":"m2"})", lr.set_cookie).code == 403);
    // 吊销：一次成功，二次 404（行已吊销，与 roles/revoke 同口径）
    Json::Value rev;
    rev["id"] = hooks[0]["id"];
    Json::StreamWriterBuilder wb;
    CHECK(http("POST", "/api/admin/webhooks/revoke",
               Json::writeString(wb, rev), root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/webhooks/revoke",
               Json::writeString(wb, rev), root_cookie).code == 404);
  }

  // ㉑ bot：add token 仅此一次、':' 名 400、重名 409、禁用回读、移除二次 404
  {
    CHECK(http("POST", "/api/admin/bots", "{}", root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/bots", R"({"name":"a:b"})",
               root_cookie).code == 400);
    const auto r = http("POST", "/api/admin/bots",
                        R"({"name":"testbot"})", root_cookie);
    CHECK(r.code == 200);
    const std::string bot_token = jbody(r.text)["token"].asString();
    CHECK(bot_token.rfind("bot_", 0) == 0);
    // 重名 409
    CHECK(http("POST", "/api/admin/bots", R"({"name":"testbot"})",
               root_cookie).code == 409);
    const auto l = http("GET", "/api/admin/bots", {}, root_cookie);
    CHECK(l.code == 200);
    CHECK(l.text.find(bot_token) == std::string::npos); // 明文不再出现
    const Json::Value bots = jbody(l.text)["bots"];
    CHECK(bots.size() == 1);
    CHECK(bots[0]["full_name"].asString() == "bot:testbot");
    CHECK(bots[0]["created_by"].asString() == "root"); // 登记者=会话身份留痕
    CHECK(bots[0]["token_prefix"].asString().size() == 12);
    // 禁用→回读 disabled=true→启用
    CHECK(http("POST", "/api/admin/bots/disabled",
               R"({"name":"testbot","disabled":true})", root_cookie).code == 200);
    const Json::Value after =
        jbody(http("GET", "/api/admin/bots", {}, root_cookie).text)["bots"];
    CHECK(after[0]["disabled"].asBool() == true);
    CHECK(http("POST", "/api/admin/bots/disabled",
               R"({"name":"testbot","disabled":false})", root_cookie).code == 200);
    // 移除（连带退群/白名单清理在 store 层）；再移除 404
    CHECK(http("POST", "/api/admin/bots/remove",
               R"({"name":"testbot"})", root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/bots/remove",
               R"({"name":"testbot"})", root_cookie).code == 404);
  }

  // ㉒ 模型网关端点：api_key 只写不回显（list 应答无原文）、http:// 与
  //    ':' 名校验、重名 409、停用回读、移除二次 404
  {
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"local-ep","base_url":"http://127.0.0.1:9100","api_key":"sk-secret-123","model":"qwen-local","is_local":true})",
               root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"ext-ep","base_url":"http://10.0.0.9:8000/v1"})",
               root_cookie).code == 200);
    // 重名 409；非 http:// 400；':' 名 400；缺 base_url 400
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"local-ep","base_url":"http://x:1"})",
               root_cookie).code == 409);
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"bad","base_url":"https://x:1"})",
               root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"a:b","base_url":"http://x:1"})",
               root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"nohost"})", root_cookie).code == 400);
    const auto l = http("GET", "/api/admin/model/endpoints", {}, root_cookie);
    CHECK(l.code == 200);
    CHECK(l.text.find("sk-secret-123") == std::string::npos); // 密钥不回显
    const Json::Value eps = jbody(l.text)["endpoints"];
    CHECK(eps.size() == 2);
    CHECK(eps[0]["api_key_set"].asBool() == true);  // 只出有无布尔
    CHECK(eps[1]["api_key_set"].asBool() == false); // 未配 key 端点
    CHECK(eps[0]["is_local"].asBool() == true);
    CHECK(eps[0]["created_by"].asString() == "root");
    // 停用→回读 enabled=false
    CHECK(http("POST", "/api/admin/model/endpoints/enabled",
               R"({"name":"ext-ep","enabled":false})", root_cookie).code == 200);
    const Json::Value after =
        jbody(http("GET", "/api/admin/model/endpoints", {}, root_cookie)
                  .text)["endpoints"];
    bool ext_disabled = false;
    for (const auto& e : after) {
      if (e["name"].asString() == "ext-ep")
        ext_disabled = !e["enabled"].asBool();
    }
    CHECK(ext_disabled);
    // 移除与二次移除
    CHECK(http("POST", "/api/admin/model/endpoints/remove",
               R"({"name":"ext-ep"})", root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/model/endpoints/remove",
               R"({"name":"ext-ep"})", root_cookie).code == 404);
    // 调用审计（只读）：空台账 200
    CHECK(http("GET", "/api/admin/model/calls", {}, root_cookie).code == 200);
  }

  // ㉓ 三级通知偏好只读盘点：全局计数对账＋群/个人分档（群名解析）＋
  //    member 越权 403；纯读不改（台账现值聚合，无写入口）
  {
    const auto r = http("GET", "/api/admin/notify-overview", {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value ov = jbody(r.text);
    CHECK(ov["global"]["webhooks_total"].asInt64() == 2);  // 吊销行也在台账
    CHECK(ov["global"]["webhooks_active"].asInt64() == 1); // 一行已吊销
    CHECK(ov["global"]["bots_total"].asInt64() == 0);      // ㉑ 已移除
    CHECK(ov["global"]["endpoints_total"].asInt64() == 1); // ㉒ 移除 ext-ep
    CHECK(ov["global"]["endpoints_local"].asInt64() == 1);
    CHECK(ov["group_scope"].size() == 1);
    CHECK(ov["group_scope"][0]["target"].asString() ==
          "group:" + std::to_string(gid));
    CHECK(ov["group_scope"][0]["group_name"].asString() == "盘点群");
    CHECK(ov["group_scope"][0]["revoked"].asBool() == false);
    CHECK(ov["personal_scope"].size() == 1);
    CHECK(ov["personal_scope"][0]["target"].asString() == "m1");
    CHECK(ov["personal_scope"][0]["revoked"].asBool() == true);
    // note 如实口径（客户端本机偏好不集中存储）
    CHECK(std::string(ov["note"].asString()).find("只读") != std::string::npos);
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(http("GET", "/api/admin/notify-overview", {}, lr.set_cookie).code ==
          403);
  }

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

  // ㉘ 登出后会话失效
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
