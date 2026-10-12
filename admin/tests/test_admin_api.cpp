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
          const std::string& body = {}, const std::string& cookie = {},
          const std::string& ctype = "application/json") {
  static trantor::EventLoopThread loop;
  static auto client = [&] {
    loop.run(); // getLoop() 只回指针不起线程——不起 IO 线程同步请求永挂
    return drogon::HttpClient::newHttpClient(kBase, loop.getLoop());
  }();
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(method == "GET" ? drogon::Get : drogon::Post);
  req->setPath(path);
  if (!body.empty()) {
    req->setContentTypeString(ctype); // 品牌素材上传走 image/png 原始字节体
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

// 品牌/头像测试素材：最小合法 PNG（magic 8 字节＋IHDR 尺寸位，CRC 不验）。
// w/h 按大端写进固定偏移 16/20（png_asset_check 只读这两处）。
std::string test_png(std::uint32_t w, std::uint32_t h, std::size_t pad = 0) {
  static const unsigned char kHead[24] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // PNG magic
      0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, // IHDR chunk
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // w/h 占位
  };
  std::string b(reinterpret_cast<const char*>(kHead), sizeof(kHead));
  auto put32 = [&](std::size_t off, std::uint32_t v) {
    b[off] = static_cast<char>((v >> 24) & 0xFF);
    b[off + 1] = static_cast<char>((v >> 16) & 0xFF);
    b[off + 2] = static_cast<char>((v >> 8) & 0xFF);
    b[off + 3] = static_cast<char>(v & 0xFF);
  };
  put32(16, w);
  put32(20, h);
  b.append(pad, '\0');
  return b;
}

} // namespace

int main() {
  // —— 造库：org-admin（base admin + 追加 org-admin）、auditor、member ——
  const auto tmp = std::filesystem::temp_directory_path() /
                   ("memex-admin-test-" + std::to_string(::getpid()));
  std::filesystem::create_directories(tmp);
  const std::string db = (tmp / "memex.db").string();
  std::uint64_t gid = 0; // 通知与集成腿：webhook 群级目标用
  std::uint64_t gid2 = 0; // 群组管理面腿：密码箱未建立的对照群
  std::int64_t file_a = 0, file_b = 0; // 文件与配额腿：群文件 A／个人文件 B
  std::int64_t memo_id = 0, vault_entry = 0; // 群组管理面腿：备忘录/密码箱条目
  std::int64_t poll_plain = 0, poll_anon = 0; // 明票/匿名票
  std::uint64_t srv_id = 0; // 群服务器
  std::int64_t ap1 = 0, ap2 = 0; // 业务流程面腿：待决/已决申请
  std::int64_t seat_a = 0, seat_b = 0; // 业务流程面腿：3F 工位
  std::int64_t task_a = 0; // 业务流程面腿：m1 清单任务
  std::string ra_id; // 业务流程面腿：远程协助会话
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
    // 群组管理面腿：空箱群（密码箱未建立 404 口径）＋备忘录修订史／密码
    // 箱条目与审计／明+匿名两投票／群任务／CI 流水线与 run／服务器与远程
    // 会话／工具凭据与动作审计（哈希/密文列只填合法形状占位——store 不验）
    gid2 = seed.create_group("空箱群", "root", {"m1"});
    CHECK(gid2 > 0);
    memo_id = seed.create_group_memo(gid, "盘点群公告", "v1 内容", "root",
                                     1700000300000);
    CHECK(memo_id > 0);
    CHECK(seed.update_group_memo(memo_id, "盘点群公告", "v2 内容", "m1",
                                 1700000301000));
    CHECK(seed.group_vault_init(gid, "c2FsdA==", 100000, "d3JhcA==",
                                1700000300000));
    vault_entry = seed.vault_create_entry(gid, "生产库", "prod-db", "Y3Q=",
                                          "bm9uY2U=", "root", 1700000300000);
    CHECK(vault_entry > 0);
    seed.vault_audit_add(gid, vault_entry, "m1", "reveal", 1700000302000);
    CHECK(seed.ci_pipeline_upsert(gid, "build", "main 构建", "root",
                                  1700000300000));
    CHECK(seed.ci_run_add(gid, "build", "root", "success", "{}", "{}",
                          1700000303000) > 0);
    poll_plain = seed.poll_create(gid, "周末值班", {"周六", "周日"}, 0, "root",
                                  1700000304000);
    CHECK(poll_plain > 0);
    CHECK(seed.poll_vote(poll_plain, "m1", 1, 1700000305000));
    poll_anon = seed.poll_create(gid, "匿名满意度", {"满意", "不满意"}, 0,
                                 "root", 1700000306000, true, false);
    CHECK(poll_anon > 0);
    CHECK(seed.poll_vote(poll_anon, "m1", 2, 1700000307000));
    CHECK(seed.gtask_create(gid, "群巡检", "", 0, "root", 1700000308000) > 0);
    srv_id = seed.server_enroll(gid, "srv-1", "10.0.0.2",
                                std::string(64, 'a'), "root", 1700000309000);
    CHECK(srv_id > 0);
    CHECK(seed.server_session_open(gid, srv_id, "m1", "ssh",
                                   std::string(64, 'b'), 1700000310000) > 0);
    CHECK(seed.tool_cred_set(gid, "ci", std::string(32, '0'), "root",
                             1700000311000));
    seed.tool_audit_add(gid, "ci", "call", "m1", "{}", "{}", 1700000312000);
    // 任务源拉取腿：m1 的 github 源（启用＋凭据已配——admin 面盘点与开关）
    {
      memex::server::ServerStore::TaskPullConfig tp;
      tp.owner = "m1";
      tp.provider = "github";
      tp.repo = "acme/api";
      tp.enabled = true;
      tp.interval_s = 60;
      tp.updated_ms = 1700000313000;
      CHECK(seed.task_pull_config_set(tp));
      CHECK(seed.task_source_cred_set("m1", "github", "deadbeef-sealed",
                                      1700000313001));
    }
    // 业务流程面腿：部门与汇报线（团队聚合）＋日报＋审批（待决/已决）＋
    // 办公室位置图（3F 两席）＋任务与备忘录＋远程协助会话与放行策略
    const int dept_rd = seed.ensure_department_path("研发部");
    CHECK(dept_rd > 0);
    CHECK(seed.ensure_department_path("研发部/平台组") > 0);
    // m1 的直属上级=root（团队聚合默认视角=会话账号）
    CHECK(seed.set_member_profile("m1", dept_rd, "成员", "root"));
    CHECK(seed.set_member_profile("aud1", dept_rd, "审计", "root"));
    const std::string rpt_date = "2026-10-10";
    CHECK(seed.report_upsert(
              "m1", rpt_date, "做了什么：种子数据。明日计划：测试。blockers：无。",
              1700000400000) > 0);
    // 审批：待决一笔（m1 申请）＋已决一笔（root 已批，重复决定 409 口径）
    ap1 = seed.approval_create("m1", "年假", "2026-11-02", "2026-11-03",
                               "回家", 1700000500000);
    CHECK(ap1 > 0);
    ap2 = seed.approval_create("m1", "事假", "2026-12-01", "2026-12-02",
                               "办事", 1700000510000);
    CHECK(ap2 > 0);
    CHECK(seed.approval_decide(ap2, "root", true, "已准", 1700000520000));
    // 办公室位置图：3F 两席（一空位一占用人）
    seat_a = seed.seat_upsert("3F", "A1", 0.1, 0.2, 1700000600000);
    CHECK(seat_a > 0);
    seat_b = seed.seat_upsert("3F", "B2", 0.6, 0.7, 1700000610000);
    CHECK(seat_b > 0);
    CHECK(seed.seat_bind(seat_b, "m1", 1700000620000));
    // 任务与备忘录：m1 清单一条＋派出一条（给 root）＋备忘录一条
    task_a = seed.task_create("m1", "m1", "修 CI 绿灯", "红了一周", 0,
                              1700000700000);
    CHECK(task_a > 0);
    CHECK(seed.task_create("root", "m1", "帮 m1 查库", "", 0, 1700000710000) >
          0);
    CHECK(seed.create_memo("m1", "部署检查单：1) 编 2) 测 3) 发",
                          1700000800000) > 0);
    // 远程协助：研发部放行；m1↔aud1 会话 requested 态（view|keyboard）
    CHECK(seed.assist_policy_set(true, "研发部", "root", 1700000900000));
    ra_id = seed.assist_request("m1", "aud1", 3, 1700001000000);
    CHECK(!ra_id.empty());
    CHECK(seed.assist_policy_resolve("m1"));
    // 品牌与外观腿：品牌文本三件＋logo/splash 素材（最小合法 PNG）；m1
    // 表情包 2 张（ts 递增，清点按 ts 倒序）；m1 头像 32/64/128 三档
    // （256 档故意不配，测 404「未设置头像」）
    CHECK(seed.branding_set("Memex", "#1a73e8", "企业协作套件", 1700001100000) >
          0);
    CHECK(seed.branding_set_logo(
              std::vector<unsigned char>(test_png(1, 1).begin(),
                                         test_png(1, 1).end()),
              1700001200000) > 0);
    CHECK(seed.branding_set_splash(
              std::vector<unsigned char>(test_png(2, 2).begin(),
                                         test_png(2, 2).end()),
              1700001300000) > 0);
    CHECK(seed.emoji_add("m1", "ok",
                         std::vector<unsigned char>(test_png(64, 64).begin(),
                                                    test_png(64, 64).end()),
                         1700001400000) > 0);
    CHECK(seed.emoji_add("m1", "hi",
                         std::vector<unsigned char>(test_png(64, 64).begin(),
                                                    test_png(64, 64).end()),
                         1700001500000) > 0);
    const std::vector<unsigned char> av_bytes(
        test_png(32, 32).begin(), test_png(32, 32).end());
    CHECK(seed.avatar_put("m1", 32, "image/png", av_bytes, 1700001600000));
    CHECK(seed.avatar_put("m1", 64, "image/png", av_bytes, 1700001600000));
    CHECK(seed.avatar_put("m1", 128, "image/png", av_bytes, 1700001600000));
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

  // —— 群组管理面（㉙~㉟）：member 会话登一次跨腿复用 ——
  std::string m1ck;
  {
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(lr.code == 200);
    m1ck = lr.set_cookie;
  }

  // ㉙ 群台账与能力：台账回读（成员数/管理员/备忘录开关/七能力生效值）、
  //    能力停用→台账生效值翻转（store 落查阅台账 group.capability 与 CLI
  //    同径）、词汇表外 400、未知群 404、member 403
  {
    const auto r = http("GET", "/api/admin/groups", {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value gs = jbody(r.text)["groups"];
    bool seen = false;
    for (const auto& g : gs) {
      if (g["gid"].asInt64() != static_cast<Json::Int64>(gid)) continue;
      seen = g["owner"].asString() == "root" && g["members"].size() == 2;
      for (const auto& c : g["capabilities"]) {
        if (c["capability"].asString() == "memo") {
          CHECK(c["configured"].asBool() == false);
          CHECK(c["effective"].asBool() == true); // 未配置=现行允许
        }
      }
    }
    CHECK(seen);
    Json::StreamWriterBuilder wb;
    Json::Value v;
    v["gid"] = static_cast<Json::Int64>(gid);
    v["capability"] = "memo";
    v["enabled"] = false;
    CHECK(http("POST", "/api/admin/groups/capability", Json::writeString(wb, v),
               root_cookie).code == 200);
    bool memo_off = false;
    // 回读先落命名量再迭代（jbody 临时量上迭代＝悬垂 UB，C++17 不延寿）
    const Json::Value gs2 = jbody(
        http("GET", "/api/admin/groups", {}, root_cookie).text)["groups"];
    for (const auto& g : gs2) {
      if (g["gid"].asInt64() != static_cast<Json::Int64>(gid)) continue;
      for (const auto& c : g["capabilities"]) {
        if (c["capability"].asString() == "memo") {
          memo_off = c["configured"].asBool() &&
                     c["effective"].asBool() == false &&
                     c["configured_enabled"].asBool() == false;
        }
      }
    }
    CHECK(memo_off);
    v["capability"] = "nope"; // 词汇表外
    CHECK(http("POST", "/api/admin/groups/capability", Json::writeString(wb, v),
               root_cookie).code == 400);
    v["gid"] = 999999;
    v["capability"] = "memo"; // 未知群
    CHECK(http("POST", "/api/admin/groups/capability", Json::writeString(wb, v),
               root_cookie).code == 404);
    CHECK(http("GET", "/api/admin/groups", {}, m1ck).code == 403);
  }

  // ㉚ 群 CI：流水线与 run 台账回读、触发落账（actor=会话账号＋工具审计
  //    留痕）、params.fail 失败语义、未知流水线 404、未知群 404、member 403
  {
    const std::string qs = "?gid=" + std::to_string(gid);
    const auto r = http("GET", "/api/admin/group-ci" + qs, {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value ci = jbody(r.text);
    CHECK(ci["pipelines"].size() == 1);
    CHECK(ci["pipelines"][0]["name"].asString() == "build");
    CHECK(ci["pipelines"][0]["last_status"].asString() == "success");
    const int runs_before = ci["runs"].size();
    CHECK(runs_before >= 1);
    Json::StreamWriterBuilder wb;
    Json::Value t;
    t["gid"] = static_cast<Json::Int64>(gid);
    t["pipeline"] = "build";
    const auto tr =
        http("POST", "/api/admin/group-ci/trigger", Json::writeString(wb, t),
             root_cookie);
    CHECK(tr.code == 200);
    CHECK(jbody(tr.text)["run_id"].asInt64() > 0);
    CHECK(jbody(tr.text)["actor"].asString() == "root");
    CHECK(jbody(tr.text)["status"].asString() == "success");
    Json::Value tf = t;
    tf["params"] = Json::Value(Json::objectValue);
    tf["params"]["fail"] = true; // stub 执行器失败语义
    const auto tr2 =
        http("POST", "/api/admin/group-ci/trigger", Json::writeString(wb, tf),
             root_cookie);
    CHECK(tr2.code == 200);
    CHECK(jbody(tr2.text)["status"].asString() == "failed");
    const Json::Value ci2 =
        jbody(http("GET", "/api/admin/group-ci" + qs, {}, root_cookie).text);
    CHECK(ci2["runs"].size() == runs_before + 2);
    // 动作留痕进工具审计（谁触发可回溯）
    const Json::Value ta = jbody(
        http("GET", "/api/admin/group-tools/audit" + qs, {}, root_cookie).text);
    bool trig_seen = false;
    for (const auto& a : ta["rows"]) {
      if (a["action"].asString() == "trigger" && a["actor"].asString() == "root") {
        trig_seen = true;
      }
    }
    CHECK(trig_seen);
    t["pipeline"] = "ghost";
    CHECK(http("POST", "/api/admin/group-ci/trigger", Json::writeString(wb, t),
               root_cookie).code == 404);
    t["gid"] = 999999;
    t["pipeline"] = "build";
    CHECK(http("POST", "/api/admin/group-ci/trigger", Json::writeString(wb, t),
               root_cookie).code == 404);
    CHECK(http("GET", "/api/admin/group-ci" + qs, {}, m1ck).code == 403);
  }

  // ㉛ 群服务器与远程会话：台账回读（在线红绿灯/凭据掩码）、登记 token 仅
  //    一次（回包才有、台账无 token 字段）、同 gid+name 重登记 id 稳定、
  //    会话台账（进行中/未兑现/时长 0）、未知群 404、member 403
  {
    const std::string qs = "?gid=" + std::to_string(gid);
    const auto r = http("GET", "/api/admin/group-servers" + qs, {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value sv = jbody(r.text)["servers"];
    CHECK(sv.size() == 1);
    CHECK(sv[0]["name"].asString() == "srv-1");
    CHECK(sv[0]["online"].asBool() == false); // 种子心跳停在 2023
    CHECK(sv[0]["cred"].asBool() == false);
    const auto ss =
        http("GET", "/api/admin/group-servers/sessions" + qs, {}, root_cookie);
    CHECK(ss.code == 200);
    const Json::Value sess = jbody(ss.text)["sessions"];
    CHECK(sess.size() == 1);
    CHECK(sess[0]["actor"].asString() == "m1");
    CHECK(sess[0]["open"].asBool() == true);
    CHECK(sess[0]["redeemed"].asBool() == false);
    CHECK(sess[0]["duration_ms"].asInt64() == 0);
    Json::StreamWriterBuilder wb;
    Json::Value e;
    e["gid"] = static_cast<Json::Int64>(gid);
    e["name"] = "srv-2";
    e["host"] = "10.0.0.3";
    const auto en =
        http("POST", "/api/admin/group-servers/enroll", Json::writeString(wb, e),
             root_cookie);
    CHECK(en.code == 200);
    const std::string tok = jbody(en.text)["token"].asString();
    CHECK(tok.size() == 32); // 16B 熵 → 32 hex，仅此一次
    const Json::Value sv2 = jbody(
        http("GET", "/api/admin/group-servers" + qs, {}, root_cookie).text);
    CHECK(sv2["servers"].size() == 2);
    CHECK(Json::writeString(wb, sv2).find(tok) == std::string::npos);
    // 重登记 srv-1＝同行轮换令牌（UNIQUE 闸去重——列表行数不涨；回包 id
    // 是 store last_insert_rowid，UPSERT-UPDATE 路径在先有他行插入时会
    // 取到上一行插入号——既有 files 面同源口径，admin 不另裁，如实注记）
    e["name"] = "srv-1";
    e["host"] = "10.0.0.2";
    const auto re =
        http("POST", "/api/admin/group-servers/enroll", Json::writeString(wb, e),
             root_cookie);
    CHECK(re.code == 200);
    CHECK(jbody(re.text)["token"].asString().size() == 32);
    const Json::Value sv3 = jbody(
        http("GET", "/api/admin/group-servers" + qs, {}, root_cookie).text);
    CHECK(sv3["servers"].size() == 2); // 去重生效：无第三行
    e["gid"] = 999999;
    CHECK(http("POST", "/api/admin/group-servers/enroll", Json::writeString(wb, e),
               root_cookie).code == 404);
    CHECK(http("GET", "/api/admin/group-servers" + qs, {}, m1ck).code == 403);
  }

  // ㉜ 密码箱：信息/条目回读（密文与包裹块不回显）、审计台账、acl 写读一
  //    致（空名单=恢复全成员）、rekey 回读、未建箱 404、坏字段 400、
  //    未知群 404、member 403
  {
    const std::string qs = "?gid=" + std::to_string(gid);
    const auto r = http("GET", "/api/admin/group-vault" + qs, {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value v = jbody(r.text);
    CHECK(v["exists"].asBool() == true);
    CHECK(v["kdf_iters"].asInt64() == 100000);
    CHECK(v["entries"].size() == 1);
    CHECK(v["entries"][0]["name"].asString() == "生产库");
    CHECK(v["acl"].isArray() && v["acl"].empty());
    // kdf_salt/wrapped_dek/secret_* 密文不出门（种子占位值全文无出现）
    CHECK(r.text.find("c2FsdA==") == std::string::npos);
    CHECK(r.text.find("d3JhcA==") == std::string::npos);
    CHECK(r.text.find("Y3Q=") == std::string::npos);
    const auto aud =
        http("GET", "/api/admin/group-vault/audit" + qs, {}, root_cookie);
    CHECK(aud.code == 200);
    CHECK(jbody(aud.text)["rows"].size() == 1);
    CHECK(jbody(aud.text)["rows"][0]["action"].asString() == "reveal");
    Json::StreamWriterBuilder wb;
    Json::Value a1;
    a1["gid"] = static_cast<Json::Int64>(gid);
    a1["accounts"] = Json::Value(Json::arrayValue);
    a1["accounts"].append("m1");
    CHECK(http("POST", "/api/admin/group-vault/acl", Json::writeString(wb, a1),
               root_cookie).code == 200);
    bool acl_seen = false;
    const Json::Value va2 = jbody(
        http("GET", "/api/admin/group-vault" + qs, {}, root_cookie).text);
    for (const auto& a : va2["acl"]) {
      acl_seen = a.asString() == "m1";
    }
    CHECK(acl_seen);
    a1["accounts"] = Json::Value(Json::arrayValue); // 空=恢复全成员
    CHECK(http("POST", "/api/admin/group-vault/acl", Json::writeString(wb, a1),
               root_cookie).code == 200);
    // rekey：覆盖包裹块回读
    Json::Value rk;
    rk["gid"] = static_cast<Json::Int64>(gid);
    rk["kdf_salt"] = "bmV3c2FsdA==";
    rk["kdf_iters"] = 120000;
    rk["wrapped_dek"] = "bmV3d3JhcA==";
    CHECK(http("POST", "/api/admin/group-vault/rekey", Json::writeString(wb, rk),
               root_cookie).code == 200);
    CHECK(jbody(http("GET", "/api/admin/group-vault" + qs, {}, root_cookie)
                    .text)["kdf_iters"].asInt64() == 120000);
    rk["kdf_iters"] = 9999; // 低于下限
    CHECK(http("POST", "/api/admin/group-vault/rekey", Json::writeString(wb, rk),
               root_cookie).code == 400);
    rk["kdf_iters"] = 120000;
    rk["gid"] = static_cast<Json::Int64>(gid2); // 未建箱
    CHECK(http("POST", "/api/admin/group-vault/rekey", Json::writeString(wb, rk),
               root_cookie).code == 404);
    a1["gid"] = static_cast<Json::Int64>(gid2);
    CHECK(http("POST", "/api/admin/group-vault/acl", Json::writeString(wb, a1),
               root_cookie).code == 404);
    rk["gid"] = 999999; // 未知群
    CHECK(http("POST", "/api/admin/group-vault/rekey", Json::writeString(wb, rk),
               root_cookie).code == 404);
    CHECK(http("GET", "/api/admin/group-vault" + qs, {}, m1ck).code == 403);
  }

  // ㉝ 备忘录：列表回读、修订史（首建+编辑两笔）、回滚=一次编辑（内容回
  //    v1＋修订链 +1 不断）、未知备忘录/修订 404、member 403
  {
    const std::string qs = "?gid=" + std::to_string(gid);
    const auto r = http("GET", "/api/admin/group-memos" + qs, {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value ms = jbody(r.text)["memos"];
    CHECK(ms.size() == 1);
    CHECK(ms[0]["title"].asString() == "盘点群公告");
    CHECK(ms[0]["content"].asString() == "v2 内容");
    const auto h =
        http("GET", "/api/admin/group-memo/history?id=" +
                        std::to_string(memo_id), {}, root_cookie);
    CHECK(h.code == 200);
    const Json::Value revs = jbody(h.text)["revisions"];
    CHECK(revs.size() == 2);
    // 修订史 id 倒序：首笔=最新 v2，尾笔=首建 v1
    CHECK(revs[0]["content"].asString() == "v2 内容");
    CHECK(revs[1]["content"].asString() == "v1 内容");
    const Json::Int64 rev_v1 = revs[1]["id"].asInt64();
    Json::StreamWriterBuilder wb;
    Json::Value rb;
    rb["id"] = memo_id;
    rb["revision_id"] = rev_v1;
    CHECK(http("POST", "/api/admin/group-memo/rollback",
               Json::writeString(wb, rb), root_cookie).code == 200);
    const Json::Value ms2 = jbody(
        http("GET", "/api/admin/group-memos" + qs, {}, root_cookie).text)["memos"];
    CHECK(ms2[0]["content"].asString() == "v1 内容");
    CHECK(ms2[0]["author"].asString() == "root"); // 作者不变，回滚只动内容
    const Json::Value revs2 = jbody(
        http("GET", "/api/admin/group-memo/history?id=" +
                        std::to_string(memo_id), {}, root_cookie).text)["revisions"];
    CHECK(revs2.size() == 3); // 回滚即一次编辑：新笔入链
    CHECK(revs2[0]["editor"].asString() == "root");
    rb["revision_id"] = 999999; // 未知修订
    CHECK(http("POST", "/api/admin/group-memo/rollback",
               Json::writeString(wb, rb), root_cookie).code == 404);
    rb["id"] = 999999; // 未知备忘录
    CHECK(http("POST", "/api/admin/group-memo/rollback",
               Json::writeString(wb, rb), root_cookie).code == 404);
    CHECK(http("GET", "/api/admin/group-memos" + qs, {}, m1ck).code == 403);
  }

  // ㉞ 投票：计数恒回、明票带 voter、匿名票不回带身份（成员面承诺延续）、
  //    关票幂等、未知投票 404、member 403
  {
    const std::string qs = "?gid=" + std::to_string(gid);
    const auto r = http("GET", "/api/admin/group-polls" + qs, {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value ps = jbody(r.text)["polls"];
    CHECK(ps.size() == 2);
    bool plain_ok = false, anon_ok = false;
    for (const auto& p : ps) {
      if (p["id"].asInt64() == poll_plain) {
        plain_ok = p["status"].asString() == "open" &&
                   p["counts"][0].asInt() == 1 && p["counts"][1].asInt() == 0 &&
                   p["votes"].size() == 1 &&
                   p["votes"][0]["account"].asString() == "m1";
      }
      if (p["id"].asInt64() == poll_anon) {
        anon_ok = p["anonymous"].asBool() == true &&
                  p["counts"][1].asInt() == 1 &&
                  p["votes"].empty(); // 匿名：台账不回带 voter
      }
    }
    CHECK(plain_ok);
    CHECK(anon_ok);
    Json::StreamWriterBuilder wb;
    Json::Value c;
    c["poll_id"] = poll_plain;
    CHECK(http("POST", "/api/admin/group-polls/close", Json::writeString(wb, c),
               root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/group-polls/close", Json::writeString(wb, c),
               root_cookie).code == 200); // 幂等收口
    bool closed = false;
    const Json::Value ps2 = jbody(
        http("GET", "/api/admin/group-polls" + qs, {}, root_cookie).text);
    for (const auto& p : ps2["polls"]) {
      if (p["id"].asInt64() == poll_plain) {
        closed = p["status"].asString() == "closed";
      }
    }
    CHECK(closed);
    c["poll_id"] = 999999;
    CHECK(http("POST", "/api/admin/group-polls/close", Json::writeString(wb, c),
               root_cookie).code == 404);
    CHECK(http("GET", "/api/admin/group-polls" + qs, {}, m1ck).code == 403);
  }

  // ㉟ 群任务与工具凭证：任务台账回读、凭据掩码（密文占位值不出现）、工
  //    具审计行、未知群 404、member 403
  {
    const std::string qs = "?gid=" + std::to_string(gid);
    const auto t = http("GET", "/api/admin/group-tasks" + qs, {}, root_cookie);
    CHECK(t.code == 200);
    const Json::Value ts = jbody(t.text)["tasks"];
    CHECK(ts.size() == 1);
    CHECK(ts[0]["title"].asString() == "群巡检");
    CHECK(ts[0]["status"].asString() == "todo");
    CHECK(ts[0]["assignee"].asString().empty());
    const auto c =
        http("GET", "/api/admin/group-tools/credentials" + qs, {}, root_cookie);
    CHECK(c.code == 200);
    CHECK(jbody(c.text)["credentials"].size() == 1);
    CHECK(jbody(c.text)["credentials"][0]["tool"].asString() == "ci");
    CHECK(c.text.find("0000000000") == std::string::npos); // 密文不出门
    const auto a =
        http("GET", "/api/admin/group-tools/audit" + qs, {}, root_cookie);
    CHECK(a.code == 200);
    CHECK(jbody(a.text)["rows"].size() >= 2); // 种子 call + ㉚ trigger
    CHECK(http("GET", "/api/admin/group-tasks?gid=999999", {}, root_cookie)
              .code == 404);
    CHECK(http("GET", "/api/admin/group-tools/credentials?gid=999999", {},
               root_cookie).code == 404);
    CHECK(http("GET", "/api/admin/group-tools/audit" + qs, {}, m1ck).code ==
          403);
    CHECK(http("GET", "/api/admin/group-tasks" + qs, {}, m1ck).code == 403);
  }

  // —— 业务流程面（㊲~㊶）：member 会话复用 m1ck ——

  // ㊲ 审批：待决全量台账、逐申请人台账（含已决）、决定落账（actor=会话账
  //    号）、已决不可重复、自审不成立、坏字段 400、未知 404、member 403
  {
    const auto r = http("GET", "/api/admin/approvals", {}, root_cookie);
    CHECK(r.code == 200);
    CHECK(jbody(r.text)["scope"].asString() == "pending");
    const Json::Value aps = jbody(r.text)["approvals"];
    CHECK(aps.size() == 1);
    CHECK(aps[0]["id"].asInt64() == ap1);
    CHECK(aps[0]["applicant"].asString() == "m1");
    CHECK(aps[0]["status"].asString() == "pending");
    CHECK(aps[0]["decider"].asString().empty());
    const auto mine =
        http("GET", "/api/admin/approvals?applicant=m1", {}, root_cookie);
    CHECK(mine.code == 200);
    CHECK(jbody(mine.text)["scope"].asString() == "applicant");
    CHECK(jbody(mine.text)["approvals"].size() == 2);
    CHECK(http("GET", "/api/admin/approvals?applicant=ghost", {}, root_cookie)
              .code == 404);
    Json::StreamWriterBuilder wb;
    Json::Value v;
    v["id"] = ap1;
    v["approved"] = true;
    v["note"] = "已批";
    const auto d1 = http("POST", "/api/admin/approvals/decide",
                         Json::writeString(wb, v), root_cookie);
    CHECK(d1.code == 200);
    CHECK(jbody(d1.text)["status"].asString() == "approved");
    // 已决不可重复（决定后立即再决=409）
    CHECK(http("POST", "/api/admin/approvals/decide", Json::writeString(wb, v),
               root_cookie).code == 409);
    // 决定后：待决全量空；逐申请人回读 decider=root
    CHECK(jbody(http("GET", "/api/admin/approvals", {}, root_cookie).text)
              ["approvals"]
              .size() == 0);
    const Json::Value aps2 = jbody(
        http("GET", "/api/admin/approvals?applicant=m1", {}, root_cookie)
            .text)["approvals"];
    bool decided = false;
    for (const auto& a : aps2) {
      if (a["id"].asInt64() == ap1) {
        decided = a["status"].asString() == "approved" &&
                  a["decider"].asString() == "root" &&
                  a["decision_note"].asString() == "已批";
      }
    }
    CHECK(decided);
    // 已决不可重复（ap2 种子已决）
    v["id"] = ap2;
    CHECK(http("POST", "/api/admin/approvals/decide", Json::writeString(wb, v),
               root_cookie).code == 409);
    // 自审不成立：root 自己申请自己决（另开库柄造一笔 root 申请）
    std::int64_t ap3 = 0;
    {
      memex::server::ServerStore s2;
      if (s2.open(db)) {
        ap3 = s2.approval_create("root", "调休", "2027-01-01", "2027-01-02",
                                 "自审测试", 1700000530000);
        s2.close();
      }
    }
    CHECK(ap3 > 0);
    v["id"] = ap3;
    CHECK(http("POST", "/api/admin/approvals/decide", Json::writeString(wb, v),
               root_cookie).code == 400);
    // 坏字段与未知
    v.removeMember("approved");
    CHECK(http("POST", "/api/admin/approvals/decide", Json::writeString(wb, v),
               root_cookie).code == 400);
    v["id"] = 999999;
    v["approved"] = true;
    CHECK(http("POST", "/api/admin/approvals/decide", Json::writeString(wb, v),
               root_cookie).code == 404);
    CHECK(http("POST", "/api/admin/approvals/decide", Json::writeString(wb, v),
               m1ck).code == 403);
  }

  // ㊳ 日报团队聚合：默认视角=会话账号直属下属、指定 manager、逐作者日报、
  //    缺参 400、未知 404、member 403
  {
    const auto r = http("GET", "/api/admin/reports/team", {}, root_cookie);
    CHECK(r.code == 200);
    CHECK(jbody(r.text)["manager"].asString() == "root");
    const Json::Value team = jbody(r.text)["team"];
    // ⑩ 的 m2 建档也在 root 线下（org_reporting_lines 权威表）——逐人过滤
    CHECK(team.size() == 3); // aud1 + m1 + m2
    bool m1_seen = false, m2_seen = false;
    for (const auto& g : team) {
      if (g["author"].asString() == "m1") {
        m1_seen = true;
        CHECK(g["reports"].size() == 1);
        CHECK(g["reports"][0]["report_date"].asString() == "2026-10-10");
        CHECK(g["reports"][0]["content"].asString().find("种子数据") !=
              std::string::npos);
      }
      if (g["author"].asString() == "m2") {
        m2_seen = true;
        CHECK(g["reports"].size() == 0); // 无日报
      }
    }
    CHECK(m1_seen && m2_seen);
    const auto no_sub =
        http("GET", "/api/admin/reports/team?manager=aud1", {}, root_cookie);
    CHECK(no_sub.code == 200);
    CHECK(jbody(no_sub.text)["team"].size() == 0);
    CHECK(http("GET", "/api/admin/reports/team?manager=ghost", {}, root_cookie)
              .code == 404);
    const auto one =
        http("GET", "/api/admin/reports?author=m1", {}, root_cookie);
    CHECK(one.code == 200);
    CHECK(jbody(one.text)["reports"].size() == 1);
    CHECK(http("GET", "/api/admin/reports", {}, root_cookie).code == 400);
    CHECK(http("GET", "/api/admin/reports?author=ghost", {}, root_cookie).code ==
          404);
    CHECK(http("GET", "/api/admin/reports?author=m1", {}, m1ck).code == 403);
  }

  // ㊴ 办公室位置图：跨层台账、建/改工位、坐标夹 0~1、绑定/解绑、一人一工
  //    位（换座先解绑 409）、删除与二次 404、缺参 400、member 403
  {
    const auto r = http("GET", "/api/admin/office-map?floor=3F", {},
                        root_cookie);
    CHECK(r.code == 200);
    const Json::Value seats = jbody(r.text)["seats"];
    CHECK(seats.size() == 2);
    bool a1 = false, b2 = false;
    for (const auto& s : seats) {
      if (s["id"].asInt64() == seat_a) {
        a1 = s["label"].asString() == "A1" && s["account"].asString().empty();
      }
      if (s["id"].asInt64() == seat_b) {
        b2 = s["label"].asString() == "B2" && s["account"].asString() == "m1";
      }
    }
    CHECK(a1 && b2);
    CHECK(http("GET", "/api/admin/office-map", {}, root_cookie).code == 400);
    Json::StreamWriterBuilder wb;
    Json::Value v;
    v["floor"] = "3F";
    v["label"] = "C3";
    v["x"] = 0.3;
    v["y"] = 0.9;
    CHECK(http("POST", "/api/admin/office-map/seat", Json::writeString(wb, v),
               root_cookie).code == 200);
    // 越界坐标夹回 0~1（不造假数据）
    v["label"] = "D4";
    v["x"] = 2.0;
    v["y"] = -1.0;
    CHECK(http("POST", "/api/admin/office-map/seat", Json::writeString(wb, v),
               root_cookie).code == 200);
    const Json::Value seats2 = jbody(
        http("GET", "/api/admin/office-map?floor=3F", {}, root_cookie).text)["seats"];
    CHECK(seats2.size() == 4);
    bool clamped = false;
    for (const auto& s : seats2) {
      if (s["label"].asString() == "D4") {
        clamped = s["x"].asDouble() == 1.0 && s["y"].asDouble() == 0.0;
      }
    }
    CHECK(clamped);
    // 绑定空位给 aud1；再绑 m1（已占 B2）= 409 换座须先解绑
    v["id"] = seat_a;
    v["account"] = "aud1";
    CHECK(http("POST", "/api/admin/office-map/bind", Json::writeString(wb, v),
               root_cookie).code == 200);
    v["account"] = "m1";
    CHECK(http("POST", "/api/admin/office-map/bind", Json::writeString(wb, v),
               root_cookie).code == 409);
    // 解绑后重绑成功（先解 A1 与 B2，再把 A1 绑给 m1）
    v["account"] = "";
    CHECK(http("POST", "/api/admin/office-map/bind", Json::writeString(wb, v),
               root_cookie).code == 200);
    v["id"] = seat_b;
    CHECK(http("POST", "/api/admin/office-map/bind", Json::writeString(wb, v),
               root_cookie).code == 200);
    v["id"] = seat_a;
    v["account"] = "m1";
    CHECK(http("POST", "/api/admin/office-map/bind", Json::writeString(wb, v),
               root_cookie).code == 200);
    // 删除与二次 404
    v.removeMember("account");
    v["id"] = seat_a;
    CHECK(http("POST", "/api/admin/office-map/remove", Json::writeString(wb, v),
               root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/office-map/remove", Json::writeString(wb, v),
               root_cookie).code == 404);
    v["id"] = 0;
    CHECK(http("POST", "/api/admin/office-map/remove", Json::writeString(wb, v),
               root_cookie).code == 400);
    CHECK(http("GET", "/api/admin/office-map?floor=3F", {}, m1ck).code == 403);
  }

  // ㊵ 远程协助：策略台账与设置（部门须存在 404、缺 allow 400）、会话台账
  //    （权限位名展开）、动作审计（含策略面）、member 403
  {
    const auto r = http("GET", "/api/admin/assist/policy", {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value pol = jbody(r.text)["policy"];
    CHECK(pol.size() == 1);
    CHECK(pol[0]["scope"].asString() == "研发部");
    CHECK(pol[0]["allow"].asBool() == true);
    CHECK(pol[0]["updated_by"].asString() == "root");
    Json::StreamWriterBuilder wb;
    Json::Value v;
    v["allow"] = true;
    v["department_path"] = "研发部/平台组";
    CHECK(http("POST", "/api/admin/assist/policy", Json::writeString(wb, v),
               root_cookie).code == 200);
    CHECK(jbody(http("GET", "/api/admin/assist/policy", {}, root_cookie).text)
              ["policy"]
              .size() == 2);
    v["department_path"] = "不存在部门";
    CHECK(http("POST", "/api/admin/assist/policy", Json::writeString(wb, v),
               root_cookie).code == 404);
    v.removeMember("allow");
    CHECK(http("POST", "/api/admin/assist/policy", Json::writeString(wb, v),
               root_cookie).code == 400);
    const auto s = http("GET", "/api/admin/assist/sessions", {}, root_cookie);
    CHECK(s.code == 200);
    const Json::Value sess = jbody(s.text)["sessions"];
    CHECK(sess.size() == 1);
    CHECK(sess[0]["id"].asString() == ra_id);
    CHECK(sess[0]["requester"].asString() == "m1");
    CHECK(sess[0]["target"].asString() == "aud1");
    CHECK(sess[0]["status"].asString() == "requested");
    CHECK(sess[0]["requested_perms"].asString() == "view,keyboard");
    CHECK(sess[0]["granted_perms"].asString().empty());
    const auto a =
        http("GET", "/api/admin/assist/audit?id=" + ra_id, {}, root_cookie);
    CHECK(a.code == 200);
    CHECK(jbody(a.text)["audits"].size() == 1);
    CHECK(jbody(a.text)["audits"][0]["action"].asString() == "request");
    const auto a2 = http("GET", "/api/admin/assist/audit", {}, root_cookie);
    CHECK(a2.code == 200);
    CHECK(jbody(a2.text)["audits"].size() >= 2); // 策略面＋会话面
    CHECK(http("GET", "/api/admin/assist/sessions", {}, m1ck).code == 403);
  }

  // ㊶ 任务与备忘录：清单/派出/备忘录台账回读、缺参 400、未知 404、
  //    member 403
  {
    const auto r = http("GET", "/api/admin/tasks?account=m1", {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value mine = jbody(r.text)["tasks"];
    CHECK(mine.size() == 1);
    CHECK(mine[0]["id"].asInt64() == task_a);
    CHECK(mine[0]["title"].asString() == "修 CI 绿灯");
    CHECK(mine[0]["creator"].asString() == "m1");
    const Json::Value assigned = jbody(r.text)["assigned_by_me"];
    CHECK(assigned.size() == 1);
    CHECK(assigned[0]["owner"].asString() == "root");
    CHECK(assigned[0]["title"].asString() == "帮 m1 查库");
    CHECK(http("GET", "/api/admin/tasks", {}, root_cookie).code == 400);
    CHECK(http("GET", "/api/admin/tasks?account=ghost", {}, root_cookie).code ==
          404);
    const auto m = http("GET", "/api/admin/memos?account=m1", {}, root_cookie);
    CHECK(m.code == 200);
    CHECK(jbody(m.text)["memos"].size() == 1);
    CHECK(jbody(m.text)["memos"][0]["content"].asString().find("部署检查单") !=
          std::string::npos);
    CHECK(http("GET", "/api/admin/memos", {}, root_cookie).code == 400);
    CHECK(http("GET", "/api/admin/memos?account=ghost", {}, root_cookie).code ==
          404);
    CHECK(http("GET", "/api/admin/tasks?account=m1", {}, m1ck).code == 403);
    CHECK(http("GET", "/api/admin/memos?account=m1", {}, m1ck).code == 403);
  }

  // ㊸ 品牌与外观：品牌读回（文本三件＋素材有无/大小/版本）、文本保存
  //    （空串清空、accent 形态门、无可写字段 400、布尔形态门）、素材字节
  //    回读、上传（非 PNG 415／2049 宽 413／超 512KiB 413）、清除后字节
  //    面 404、表情包元数据清点（仅元数据无字节）、头像代读（三档/未配
  //    404/档位门 400/未知账号 404）；未登录 401、成员 403
  {
    const auto br = http("GET", "/api/admin/branding", {}, root_cookie);
    CHECK(br.code == 200);
    const auto jb = jbody(br.text);
    CHECK(jb["company_name"].asString() == "Memex");
    CHECK(jb["accent"].asString() == "#1a73e8");
    CHECK(jb["slogan"].asString() == "企业协作套件");
    CHECK(jb["has_logo"].asBool());
    CHECK(jb["has_splash"].asBool());
    CHECK(jb["logo_size"].asInt64() >= 24);
    CHECK(jb["version"].asInt64() > 0);
    // 素材字节回读（PNG magic 逐字对，长度与台账一致）
    const auto lg = http("GET", "/api/admin/branding/logo", {}, root_cookie);
    CHECK(lg.code == 200);
    CHECK(lg.text.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    CHECK(lg.text.size() == static_cast<std::size_t>(jb["logo_size"].asInt64()));
    const auto sp = http("GET", "/api/admin/branding/splash", {}, root_cookie);
    CHECK(sp.code == 200);
    CHECK(sp.text.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    // 文本保存：空串清标语（三件同提交，版本 bump 一次）
    const auto sv = http("POST", "/api/admin/branding",
                         R"({"company_name":"Memex","accent":"#1a73e8","slogan":""})",
                         root_cookie);
    CHECK(sv.code == 200);
    const std::int64_t v_after = jbody(sv.text)["version"].asInt64();
    CHECK(v_after > jb["version"].asInt64());
    const auto rb = http("GET", "/api/admin/branding", {}, root_cookie);
    CHECK(jbody(rb.text)["slogan"].asString().empty());
    CHECK(jbody(rb.text)["company_name"].asString() == "Memex");
    CHECK(jbody(rb.text)["version"].asInt64() == v_after);
    // 形态门与坏体
    CHECK(http("POST", "/api/admin/branding", R"({"accent":"1a73e8"})",
               root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/branding", R"({"accent":"#1a73e"})",
               root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/branding", R"({})", root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/branding", "not json", root_cookie).code ==
          400);
    CHECK(http("POST", "/api/admin/branding", R"({"clear_logo":"true"})",
               root_cookie).code == 400);
    // 素材上传（原始字节体，content-type image/png）
    CHECK(http("POST", "/api/admin/branding/logo", test_png(4, 4), root_cookie,
               "image/png").code == 200);
    CHECK(http("POST", "/api/admin/branding/logo", "not a png", root_cookie,
               "image/png").code == 415);
    CHECK(http("POST", "/api/admin/branding/logo", test_png(2049, 1), root_cookie,
               "image/png").code == 413);
    CHECK(http("POST", "/api/admin/branding/logo", test_png(1, 1, 512 * 1024),
               root_cookie, "image/png").code == 413);
    CHECK(http("POST", "/api/admin/branding/splash", test_png(4, 4), root_cookie,
               "image/png").code == 200);
    // 清除（各 bump 一次）→ 字节面回 404
    const auto cl = http("POST", "/api/admin/branding", R"({"clear_logo":true})",
                         root_cookie);
    CHECK(cl.code == 200);
    const std::int64_t v_cl = jbody(cl.text)["version"].asInt64();
    CHECK(v_cl > v_after);
    CHECK(http("POST", "/api/admin/branding", R"({"clear_splash":true})",
               root_cookie).code == 200);
    const auto rb2 = http("GET", "/api/admin/branding", {}, root_cookie);
    CHECK(!jbody(rb2.text)["has_logo"].asBool());
    CHECK(!jbody(rb2.text)["has_splash"].asBool());
    CHECK(jbody(rb2.text)["logo_size"].asInt64() == 0);
    CHECK(http("GET", "/api/admin/branding/logo", {}, root_cookie).code ==
          404);
    CHECK(http("GET", "/api/admin/branding/splash", {}, root_cookie).code ==
          404);
    // 表情包元数据清点：ts 倒序、仅元数据（无字节/无内容键）
    const auto em = http("GET", "/api/admin/emoji?account=m1", {}, root_cookie);
    CHECK(em.code == 200);
    const auto je = jbody(em.text);
    CHECK(je["account"].asString() == "m1");
    CHECK(je["emoji"].size() == 2);
    CHECK(je["emoji"][0]["name"].asString() == "hi");
    CHECK(je["emoji"][1]["name"].asString() == "ok");
    CHECK(je["emoji"][0]["size"].asInt64() > 0);
    CHECK(je["emoji"][0]["ts_ms"].asInt64() > je["emoji"][1]["ts_ms"].asInt64());
    CHECK(!je["emoji"][0].isMember("bytes"));
    CHECK(!je["emoji"][0].isMember("content"));
    // 缺账号 400、未知账号 404
    CHECK(http("GET", "/api/admin/emoji", {}, root_cookie).code == 400);
    CHECK(http("GET", "/api/admin/emoji?account=ghost", {}, root_cookie).code ==
          404);
    // 头像代读（组织内互见口径）：三档可读、256 未配 404、档位门 400
    const auto av = http("GET", "/api/admin/avatar?account=m1&size=32", {},
                         root_cookie);
    CHECK(av.code == 200);
    CHECK(av.text.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=64", {}, root_cookie).code ==
          200);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=128", {}, root_cookie)
              .code == 200);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=256", {}, root_cookie)
              .code == 404);
    CHECK(http("GET", "/api/admin/avatar?account=m1", {}, root_cookie).code ==
          400);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=99", {}, root_cookie)
              .code == 400);
    CHECK(http("GET", "/api/admin/avatar?account=ghost&size=64", {}, root_cookie)
              .code == 404);
    // 未登录 401、成员 403（后台无代取字节/代删入口——只清点与代读）
    CHECK(http("GET", "/api/admin/branding").code == 401);
    CHECK(http("GET", "/api/admin/branding/logo").code == 401);
    CHECK(http("GET", "/api/admin/emoji?account=m1").code == 401);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=64").code == 401);
    CHECK(http("GET", "/api/admin/branding", {}, m1ck).code == 403);
    CHECK(http("POST", "/api/admin/branding", R"({"slogan":"x"})", m1ck).code ==
          403);
    CHECK(http("GET", "/api/admin/emoji?account=m1", {}, m1ck).code == 403);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=64", {}, m1ck).code ==
          403);
  }

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

  // ㊷ 登出后会话失效
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
