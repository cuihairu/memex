// test_files_api 拆分模块（LAY-005）：本套件场景体自原文件逐字搬移；
// 共享状态经 Fx 夹具传入（见 common.hpp）。套件按原序由 main 调用。
#include "common.hpp"

namespace memex::server::tests_api {

void files_suite(Fx& f) {
  auto H = [&f](const char* a) { return f.H(a); };
  auto& store = f.store;
  auto& fake = f.fake;
  auto& port = f.port;
  auto& tok = f.tok;
  auto& gid = f.gid;
  auto& gtarget = f.gtarget;
  auto& gid2 = f.gid2;
  auto& idp = f.idp;

  // —— 健康探针：无鉴权 GET 恒 200（容器 HEALTHCHECK 口径），不动方法/路径
  //    语义（POST /files/health 仍 404）——
  {
    const auto h = http(port, "GET", "/files/health", {}, "");
    CHECK(h.status == 200);
    CHECK(h.body.find("\"ok\":true") != std::string::npos);
    CHECK(http(port, "POST", "/files/health", {}, "").status == 404);
  }

  // —— 会话令牌：错口令/未知账号 401；四人各换 token ——
  CHECK(http(port, "POST", "/files/session", {},
             "{\"account\":\"member1\",\"password\":\"wrong\"}").status == 401);
  CHECK(http(port, "POST", "/files/session", {},
             "{\"account\":\"ghost\",\"password\":\"pw-member1\"}").status == 401);
  for (const char* a : {"owner1", "admin1", "member1", "outsider"}) {
    const auto r = http(port, "POST", "/files/session", {},
                        "{\"account\":\"" + std::string(a) +
                            "\",\"password\":\"pw-" + std::string(a) + "\"}");
    CHECK(r.status == 200);
    tok[a] = jstr(r.body, "token");
    CHECK(!tok[a].empty());
  }

  // —— 上传：群成员可传；两级扣费（群+传者个人）；对象键=groups/{gid}/{hash} ——
  const std::string blob = "hello R23-2\n"; // 12 字节
  const auto blob_n = static_cast<std::int64_t>(blob.size());
  const auto up1 = http(port, "POST", "/files/upload?target=" + gtarget,
                        {{"Authorization", "Bearer " + tok["member1"]},
                         {"X-File-Name", "a.txt"}},
                        blob);
  CHECK(up1.status == 200);
  const auto id1 = jint(up1.body, "id");
  CHECK(id1 > 0);
  CHECK(up1.body.find("\"second_transfer\":false") != std::string::npos);
  const std::string gkey =
      "groups/" + std::to_string(gid) + "/" + memex::server::sha256_hex(blob);
  CHECK(fake->objects_.count(gkey) == 1);
  CHECK(gq_used(store, std::to_string(gid)) == blob_n);
  CHECK(uq_used(store, "member1") == blob_n);

  // —— 无令牌 401；非成员默认拒（default-deny，白名单口径）——
  CHECK(http(port, "POST", "/files/upload?target=" + gtarget,
             {{"X-File-Name", "x.txt"}}, blob).status == 401);
  const auto up_out = http(port, "POST", "/files/upload?target=" + gtarget,
                           {{"Authorization", "Bearer " + tok["outsider"]},
                            {"X-File-Name", "x.txt"}},
                           blob);
  CHECK(up_out.status == 403);
  CHECK(jstr(up_out.body, "error").find("default-deny") != std::string::npos);

  // —— 秒传：同属主+同哈希+同归属复用（同 id、不扣费、不再落字节）——
  const auto up2 = http(port, "POST", "/files/upload?target=" + gtarget,
                        {{"Authorization", "Bearer " + tok["member1"]},
                         {"X-File-Name", "b.txt"}},
                        blob);
  CHECK(up2.status == 200);
  CHECK(jint(up2.body, "id") == id1);
  CHECK(up2.body.find("\"second_transfer\":true") != std::string::npos);
  CHECK(gq_used(store, std::to_string(gid)) == blob_n);
  CHECK(fake->objects_.size() == 1);

  // —— 换目标＝新文件行（新对象键、两级再扣费）——
  f.gid2 = f.store.create_group("dev2", "owner1", {"owner1", "member1"});
  CHECK(gid2 > 0);
  const auto up3 =
      http(port, "POST", "/files/upload?target=group:" + std::to_string(gid2),
           {{"Authorization", "Bearer " + tok["member1"]},
            {"X-File-Name", "a.txt"}},
           blob);
  CHECK(up3.status == 200);
  CHECK(jint(up3.body, "id") != id1);
  CHECK(gq_used(store, std::to_string(gid2)) == blob_n);
  CHECK(uq_used(store, "member1") == 2 * blob_n);

  // —— 下载：成员可读、字节一致、X-File-Name 回原名；非成员 403；未知 id 404 ——
  const auto dl1 = http(port, "GET",
                        "/files/download?id=" + std::to_string(id1),
                        H("member1"), "");
  CHECK(dl1.status == 200);
  CHECK(dl1.body == blob);
  CHECK(dl1.headers.at("x-file-name") == "a.txt");
  CHECK(http(port, "GET", "/files/download?id=" + std::to_string(id1),
             H("outsider"), "").status == 403);
  CHECK(http(port, "GET", "/files/download?id=99999", H("member1"), "")
            .status == 404);

  // —— 退群即失权（授权盘点 P0-1 补漏）：成员被移出后群文件即时 403，
  //    群成员判定每请求现查，恢复成员身份后可读 ——
  CHECK(store.group_leave(gid, "member1"));
  CHECK(http(port, "GET", "/files/download?id=" + std::to_string(id1),
             H("member1"), "").status == 403);
  CHECK(store.group_invite(gid, "member1"));
  CHECK(http(port, "GET", "/files/download?id=" + std::to_string(id1),
             H("member1"), "").status == 200);

  // —— 个人文件：仅本人可读；他人（含群主）403；只扣个人配额 ——
  const auto up_p = http(port, "POST", "/files/upload?target=me",
                         {{"Authorization", "Bearer " + tok["member1"]},
                          {"X-File-Name", "private.txt"}},
                         "my own bytes"); // 12 字节
  CHECK(up_p.status == 200);
  f.idp = jint(up_p.body, "id");
  CHECK(http(port, "GET", "/files/download?id=" + std::to_string(idp),
             H("owner1"), "").status == 403);
  CHECK(http(port, "GET", "/files/download?id=" + std::to_string(idp),
             H("member1"), "").status == 200);
  CHECK(uq_used(store, "member1") == 2 * blob_n + 12);
  // 列表隔离：本人 me 列表恰该行；他人 me 列表（=查看者本人空间）不含他人文件
  const auto ls_me =
      http(port, "GET", "/files/list?target=me", H("member1"), "");
  CHECK(ls_me.status == 200);
  CHECK(jint(ls_me.body, "id") == idp);
  const auto ls_owner_me =
      http(port, "GET", "/files/list?target=me", H("owner1"), "");
  CHECK(ls_owner_me.status == 200);
  CHECK(ls_owner_me.body.find("private.txt") == std::string::npos);

  // —— 置顶：成员 403；管理员/群主可置；列表置顶行居首、pin 回真 ——
  const std::string pin_path =
      "/files/manage/pin?id=" + std::to_string(id1) + "&pin=1";
  CHECK(http(port, "POST", pin_path, H("member1"), "").status == 403);
  CHECK(http(port, "POST", pin_path, H("admin1"), "").status == 200);
  CHECK(http(port, "POST", pin_path, H("owner1"), "").status == 200);
  const auto ls_g =
      http(port, "GET", "/files/list?target=" + gtarget, H("member1"), "");
  CHECK(ls_g.status == 200);
  CHECK(jint(ls_g.body, "id") == id1);
  CHECK(ls_g.body.find("\"pin\":true") != std::string::npos);
  CHECK(http(port, "GET", "/files/list?target=" + gtarget, H("outsider"), "")
            .status == 403); // 非成员不可列
  // 分页参数生效（两行的 gid2 取 limit=1 只出一行）
  const auto ls_g2 = http(
      port, "GET",
      "/files/list?target=group:" + std::to_string(gid2) + "&limit=1&offset=0",
      H("member1"), "");
  CHECK(ls_g2.status == 200);
  CHECK(ls_g2.body.find("\"files\":[{") != std::string::npos);
  CHECK(ls_g2.body.find("},{") == std::string::npos); // 只有一行

  // —— 删除：成员（含传者本人）403（设计：群文件管理归群主/管理员）；
  //    管理员删除成功 → 引用归零删字节、两级退费 ——
  CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(id1),
             H("member1"), "").status == 403);
  CHECK(fake->objects_.count(gkey) == 1);
  CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(id1),
             H("admin1"), "").status == 200);
  CHECK(fake->objects_.count(gkey) == 0); // 引用归零 → 字节清
  CHECK(gq_used(store, std::to_string(gid)) == 0);
  CHECK(uq_used(store, "member1") == blob_n + 12);
  // 个人文件：本人可删；他人不可
  CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(idp),
             H("owner1"), "").status == 403);
  CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(idp),
             H("member1"), "").status == 200);

  // —— 配额：群主/管理员可设；成员 403；负数 400；受检扣费 60 进 50 拒 ——
  CHECK(http(port, "POST", "/files/manage/quota?target=" + gtarget,
             H("member1"), "{\"limit_bytes\":100}").status == 403);
  CHECK(http(port, "POST", "/files/manage/quota?target=" + gtarget,
             H("owner1"), "{\"limit_bytes\":-1}").status == 400);
  CHECK(http(port, "POST", "/files/manage/quota?target=" + gtarget,
             H("admin1"), "{\"limit_bytes\":100}").status == 200);
  const auto qg =
      http(port, "GET", "/files/quota?target=" + gtarget, H("member1"), "");
  CHECK(qg.status == 200);
  CHECK(jint(qg.body, "limit_bytes") == 100);
  const std::string b60(60, 'x');
  const std::string b50(50, 'o');
  const auto upq1 = http(port, "POST", "/files/upload?target=" + gtarget,
                         {{"Authorization", "Bearer " + tok["member1"]},
                          {"X-File-Name", "sixty.bin"}},
                         b60);
  CHECK(upq1.status == 200);
  const auto upq2 = http(port, "POST", "/files/upload?target=" + gtarget,
                         {{"Authorization", "Bearer " + tok["member1"]},
                          {"X-File-Name", "fifty.bin"}},
                         b50);
  CHECK(upq2.status == 413);
  CHECK(jstr(upq2.body, "error").find("配额不足") != std::string::npos);
  // 删 60 退费 → 50 可进（换群主传，个人配额独立不受 member1 账影响）
  const auto idq1 = jint(upq1.body, "id");
  CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(idq1),
             H("owner1"), "").status == 200);
  const auto upq3 = http(port, "POST", "/files/upload?target=" + gtarget,
                         {{"Authorization", "Bearer " + tok["owner1"]},
                          {"X-File-Name", "fifty.bin"}},
                         b50);
  CHECK(upq3.status == 200);
  const auto idq3 = jint(upq3.body, "id");
  CHECK(gq_used(store, std::to_string(gid)) == 50);

  // —— 隔离（R23-5 杀毒钩子落点）：读 403（显式拒）、秒传不命中、重传
  //     409 且字节保留（键仍被隔离行引用，引用计数不归零不删）——
  CHECK(store.set_file_status(idq3, ServerStore::FileStatus::Quarantine));
  const auto dlq = http(port, "GET",
                        "/files/download?id=" + std::to_string(idq3),
                        H("owner1"), "");
  CHECK(dlq.status == 403);
  CHECK(jstr(dlq.body, "error").find("deny:file-not-normal") !=
        std::string::npos);
  CHECK(http(port, "POST", "/files/upload?target=" + gtarget,
             {{"Authorization", "Bearer " + tok["owner1"]},
              {"X-File-Name", "fifty.bin"}},
             b50).status == 409);
  CHECK(fake->objects_.count("groups/" + std::to_string(gid) + "/" +
                             memex::server::sha256_hex(b50)) == 1);
  // 扣费照回滚：群账回到 50、owner 个人回到 50
  CHECK(gq_used(store, std::to_string(gid)) == 50);
  CHECK(uq_used(store, "owner1") == 50);

  // —— 落字节失败：502 且配额全回滚 ——
  fake->fail_next_put_ = true;
  const auto upf = http(port, "POST", "/files/upload?target=" + gtarget,
                        {{"Authorization", "Bearer " + tok["member1"]},
                         {"X-File-Name", "boom.bin"}},
                        std::string(30, 'z'));
  CHECK(upf.status == 502);
  const auto qg2 =
      http(port, "GET", "/files/quota?target=" + gtarget, H("member1"), "");
  CHECK(jint(qg2.body, "used_bytes") == 50);

  // —— R23-3 备忘录：本人建/查/改/删全链路（真 TCP）；他人 default-deny ——
  CHECK(http(port, "POST", "/files/memo", {},
             "{\"content\":\"x\"}").status == 401);
  const auto mc1 =
      http(port, "POST", "/files/memo", H("member1"),
           "{\"content\":\"first note\"}");
  CHECK(mc1.status == 200);
  const auto mid1 = jint(mc1.body, "id");
  CHECK(mid1 > 0);
  CHECK(http(port, "POST", "/files/memo", H("member1"), "{\"content\":\"second note\"}")
            .status == 200);
  // 空文 400；非法 JSON 400
  CHECK(http(port, "POST", "/files/memo", H("member1"), "{\"content\":\"\"}")
            .status == 400);
  CHECK(http(port, "POST", "/files/memo", H("member1"), "not-json").status == 400);
  // 单条读：本人 200；他人 403（default-deny）；不存在 404
  const auto mg =
      http(port, "GET", "/files/memo?id=" + std::to_string(mid1), H("member1"),
           "");
  CHECK(mg.status == 200);
  CHECK(jstr(mg.body, "content") == "first note");
  CHECK(http(port, "GET", "/files/memo?id=" + std::to_string(mid1), H("owner1"),
             "").status == 403);
  CHECK(http(port, "GET", "/files/memo?id=99999", H("member1"), "").status == 404);
  // 列表：本人两条；他人列表不串别人条目
  const auto mls = http(port, "GET", "/files/memo", H("member1"), "");
  CHECK(mls.status == 200);
  CHECK(mls.body.find("first note") != std::string::npos &&
        mls.body.find("second note") != std::string::npos);
  const auto mls_o = http(port, "GET", "/files/memo", H("owner1"), "");
  CHECK(mls_o.status == 200);
  CHECK(mls_o.body.find("first note") == std::string::npos);
  // CR-2 回归：≥20 位 limit 不得回绕成 SQLite LIMIT -1（无限制整表）——
  // 应回退默认 200、200 OK 且仍返回全部条目；10 位/非数字同样回退不报错。
  const auto ml_huge = http(
      port, "GET", "/files/memo?limit=99999999999999999999", H("member1"), "");
  CHECK(ml_huge.status == 200);
  CHECK(ml_huge.body.find("first note") != std::string::npos &&
        ml_huge.body.find("second note") != std::string::npos);
  CHECK(http(port, "GET", "/files/memo?limit=1234567890", H("member1"), "")
            .status == 200);
  CHECK(http(port, "GET", "/files/memo?limit=abc", H("member1"), "")
            .status == 200);
  // 编辑：带 id POST；改后内容回读一致；他人编辑 403
  CHECK(http(port, "POST", "/files/memo", H("member1"),
             "{\"id\":" + std::to_string(mid1) +
                 ",\"content\":\"first note v2\"}").status == 200);
  CHECK(jstr(http(port, "GET", "/files/memo?id=" + std::to_string(mid1),
                  H("member1"), "").body,
             "content") == "first note v2");
  CHECK(http(port, "POST", "/files/memo", H("owner1"),
             "{\"id\":" + std::to_string(mid1) +
                 ",\"content\":\"hijack\"}").status == 403);
  // 删除：他人 403；本人 200；删后查 404
  CHECK(http(port, "DELETE",
             "/files/memo?id=" + std::to_string(mid1 + 1), H("owner1"), "")
            .status == 403);
  CHECK(http(port, "DELETE", "/files/memo?id=" + std::to_string(mid1 + 1),
             H("member1"), "").status == 200);
  CHECK(http(port, "GET", "/files/memo?id=" + std::to_string(mid1 + 1),
             H("member1"), "").status == 404);

  // —— R23-3 收件箱（手机发自己=文件传输）：target=inbox 落 users/{uid}/、
  //     kind=inbox；同哈希个人空间是另一行；inbox 列表=文件+备忘录混排 ——
  const std::string inbox_blob = "from my phone\n"; // 14 字节
  const auto upi = http(port, "POST", "/files/upload?target=inbox",
                        {{"Authorization", "Bearer " + tok["member1"]},
                         {"X-File-Name", "phone.jpg"}},
                        inbox_blob);
  CHECK(upi.status == 200);
  const auto idi = jint(upi.body, "id");
  CHECK(idi > 0);
  // 对象键仍 users/{uid}/{hash}（收件箱共用个人前缀，kind 区分空间）
  CHECK(fake->objects_.count("users/member1/" +
                             memex::server::sha256_hex(inbox_blob)) == 1);
  // 只扣个人配额：此前 member1 个人账 12（群文件两份在先、private.txt
  // 删除退过费、idq1 删除退过费），加 14
  CHECK(uq_used(store, "member1") == blob_n + 14);
  // 同哈希进个人空间 = 新行（kind 入秒传键，跨空间不串用）
  const auto upi2 = http(port, "POST", "/files/upload?target=me",
                         {{"Authorization", "Bearer " + tok["member1"]},
                          {"X-File-Name", "same-bytes-personal.jpg"}},
                         inbox_blob);
  CHECK(upi2.status == 200);
  CHECK(jint(upi2.body, "id") != idi);
  CHECK(uq_used(store, "member1") == blob_n + 28);
  // 收件箱内重传 = 秒传命中（同 id、不再落字节）
  const auto upi3 = http(port, "POST", "/files/upload?target=inbox",
                         {{"Authorization", "Bearer " + tok["member1"]},
                          {"X-File-Name", "phone.jpg"}},
                         inbox_blob);
  CHECK(upi3.status == 200);
  CHECK(jint(upi3.body, "id") == idi);
  CHECK(upi3.body.find("\"second_transfer\":true") != std::string::npos);
  CHECK(fake->objects_.count("users/member1/" +
                             memex::server::sha256_hex(inbox_blob)) == 1);
  // 收件箱文件本人可读、字节一致
  const auto dli = http(port, "GET", "/files/download?id=" + std::to_string(idi),
                        H("member1"), "");
  CHECK(dli.status == 200);
  CHECK(dli.body == inbox_blob);

  // inbox 列表混排时间序：备忘录（updated_ms）与文件（upload_ts）统一倒序。
  // 用 sleep 拉开毫秒级时间差，顺序可断言：旧备忘录 < 收件箱文件 < 新备忘录
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto lmb =
      http(port, "POST", "/files/memo", H("member1"),
           "{\"content\":\"third note\"}");
  CHECK(lmb.status == 200);
  const auto lsi =
      http(port, "GET", "/files/list?target=inbox", H("member1"), "");
  CHECK(lsi.status == 200);
  const auto p_memo = lsi.body.find("\"type\":\"memo\"");
  const auto p_file = lsi.body.find("\"type\":\"file\"");
  CHECK(p_memo != std::string::npos && p_file != std::string::npos);
  CHECK(p_memo < p_file); // 新备忘录在前
  CHECK(jint(lsi.body, "id") == jint(lmb.body, "id")); // 首条=最新备忘录
  CHECK(lsi.body.find("phone.jpg") != std::string::npos);
  CHECK(lsi.body.find("first note v2") != std::string::npos);
  // 分页在混排后取窗：limit=1 只出首条（新备忘录）
  const auto lsi1 = http(port, "GET", "/files/list?target=inbox&limit=1",
                         H("member1"), "");
  CHECK(lsi1.status == 200);
  CHECK(lsi1.body.find("third note") != std::string::npos);
  CHECK(lsi1.body.find("phone.jpg") == std::string::npos);

  // me 列表只出个人空间文件：不含收件箱行、无混排类型字段
  const auto ls_me2 =
      http(port, "GET", "/files/list?target=me", H("member1"), "");
  CHECK(ls_me2.status == 200);
  CHECK(ls_me2.body.find("same-bytes-personal.jpg") != std::string::npos);
  CHECK(ls_me2.body.find("phone.jpg") == std::string::npos);
  CHECK(ls_me2.body.find("\"type\":") == std::string::npos);
  CHECK(ls_me2.body.find("first note") == std::string::npos);

  // 配额视角：target=inbox 走个人配额（与 me 同账）
  const auto qgi = http(port, "GET", "/files/quota?target=inbox", H("member1"),
                        "");
  CHECK(qgi.status == 200);
  CHECK(jint(qgi.body, "used_bytes") == uq_used(store, "member1"));

  // —— 无 Content-Length 头：GET/DELETE 放行（curl/浏览器/Qt QNAM 的
  //     无体请求口径，R23-3 走查实录锁回归）；POST 仍 400 ——
  CHECK(http(port, "GET", "/files/list?target=me", H("member1"), "", false)
            .status == 200);
  CHECK(http(port, "GET", "/files/memo", H("member1"), "", false).status == 200);
  CHECK(http(port, "DELETE", "/files/memo?id=99999", H("member1"), "", false)
            .status == 404);
  CHECK(http(port, "POST", "/files/memo", H("member1"), "{\"content\":\"x\"}",
             false).status == 400);

}

} // namespace memex::server::tests_api
