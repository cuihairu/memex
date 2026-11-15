// test_files_api 拆分模块（LAY-005）：本套件场景体自原文件逐字搬移；
// 共享状态经 Fx 夹具传入（见 common.hpp）。套件按原序由 main 调用。
#include "common.hpp"

namespace memex::server::tests_api {

void audit_brand_suite(Fx& f) {
  auto H = [&f](const char* a) { return f.H(a); };
  auto& store = f.store;
  auto& port = f.port;

  // —— 二期·审计上下文/撤回事件/导出 与 信箱未读（gate 拒＋留痕＋400 分支）——
  {
    // 信箱基线：此前各端点（审批/日报/群事件）已给 member1 自动入箱若干条，
    // 断言一律相对基线（增量口径），不写绝对数
    const auto mb0 = http(port, "POST", "/files/mailbox/unread", H("member1"), "");
    CHECK(mb0.status == 200);
    const std::int64_t base_all = jint(mb0.body, "all");
    const std::int64_t base_warn = jint(mb0.body, "warning");
    const std::int64_t base_note = jint(mb0.body, "notice");
    const std::int64_t base_ann = jint(mb0.body, "announce");

    // 种子：同会话四条协作文本（type=10=text），ctx-2 由 owner1 撤回；
    // 信箱给 member1 两条未读（warning/notice 各一）——owner1 在上段已授 auditor
    CHECK(store.store_message("ctx-1", "owner1", "member1", 10, "第一条", 1000));
    CHECK(store.store_message("ctx-2", "owner1", "member1", 10, "第二条", 2000));
    CHECK(store.store_message("ctx-3", "owner1", "member1", 10, "第三条", 3000));
    CHECK(store.store_message("ctx-4", "owner1", "member1", 10, "第四条", 4000));
    CHECK(store.recall_message("ctx-2", "owner1", 2500));
    CHECK(store.mailbox_add("warning", "磁盘告警", "空间低于 10%", "ops",
                            "ref-w", "warn-1", {"member1"}, 5000) > 0);
    CHECK(store.mailbox_add("notice", "版本提示", "新版本就绪", "updater",
                            "ref-n", "note-1", {"member1"}, 6000) > 0);

    // —— GET /files/audit/context：窗口命中＋撤回标记＋空 msg_id 400＋门拒 ——
    CHECK(http(port, "GET", "/files/audit/context?msg_id=", H("owner1"), "")
              .status == 400);
    const auto ctx = http(port, "GET",
                          "/files/audit/context?msg_id=ctx-2&span=1",
                          H("owner1"), "");
    CHECK(ctx.status == 200);
    CHECK(ctx.body.find("\"msg_id\":\"ctx-2\"") != std::string::npos);
    CHECK(ctx.body.find("\"text\":\"第一条\"") != std::string::npos);
    CHECK(ctx.body.find("\"text\":\"第二条\"") != std::string::npos);
    CHECK(ctx.body.find("\"text\":\"第三条\"") != std::string::npos);
    CHECK(ctx.body.find("\"type\":\"text\"") != std::string::npos);
    CHECK(ctx.body.find("\"recalled\":true") != std::string::npos); // ctx-2 撤回
    CHECK(ctx.body.find("第四条") == std::string::npos); // span=1 窗口外
    CHECK(http(port, "GET", "/files/audit/context?msg_id=ctx-1",
               H("member1"), "")
              .status == 403); // 无 auditor 角色拒

    // —— GET /files/audit/recall-event：撤回事件/未撤回/400/门拒 ——
    CHECK(http(port, "GET", "/files/audit/recall-event?msg_id=", H("owner1"),
               "")
              .status == 400);
    const auto rev = http(port, "GET", "/files/audit/recall-event?msg_id=ctx-2",
                          H("owner1"), "");
    CHECK(rev.status == 200);
    CHECK(rev.body.find("\"exists\":true") != std::string::npos);
    CHECK(jstr(rev.body, "by_account") == "owner1");
    CHECK(jint(rev.body, "ts_ms") == 2500);
    const auto rev_no = http(port, "GET",
                             "/files/audit/recall-event?msg_id=ctx-3",
                             H("owner1"), "");
    CHECK(rev_no.status == 200);
    CHECK(rev_no.body.find("\"exists\":false") != std::string::npos);
    CHECK(http(port, "GET", "/files/audit/recall-event?msg_id=ctx-1",
               H("member1"), "")
              .status == 403);

    // —— POST /files/audit/export：坏 JSON 400／关键词命中＋operator／门拒 ——
    CHECK(http(port, "POST", "/files/audit/export", H("owner1"), "not-json")
              .status == 400);
    const auto exp = http(port, "POST", "/files/audit/export", H("owner1"),
                          "{\"account\":\"member1\",\"keyword\":\"二条\","
                          "\"limit\":5}");
    CHECK(exp.status == 200);
    CHECK(jstr(exp.body, "operator") == "owner1");
    CHECK(exp.body.find("\"msg_id\":\"ctx-2\"") != std::string::npos);
    CHECK(exp.body.find("\"text\":\"第二条\"") != std::string::npos);
    CHECK(exp.body.find("exported_ms") != std::string::npos);
    CHECK(http(port, "POST", "/files/audit/export", H("member1"),
               "{\"account\":\"owner1\"}")
              .status == 403);

    // 留痕：context/recall/export 三动作均落 audit_reads（member1 被拒另记 denied）
    const auto reads2 = http(port, "GET", "/files/audit/reads", H("owner1"), "");
    CHECK(reads2.status == 200);
    CHECK(reads2.body.find("audit.message.context") != std::string::npos);
    CHECK(reads2.body.find("audit.recall.event") != std::string::npos);
    CHECK(reads2.body.find("audit.message.export") != std::string::npos);

    // —— POST /files/mailbox/unread：增量对齐种子；全部已读后归零 ——
    const auto mb = http(port, "POST", "/files/mailbox/unread", H("member1"), "");
    CHECK(mb.status == 200);
    CHECK(jint(mb.body, "all") == base_all + 2);
    CHECK(jint(mb.body, "warning") == base_warn + 1);
    CHECK(jint(mb.body, "notice") == base_note + 1);
    CHECK(jint(mb.body, "announce") == base_ann);
    const auto rd = http(port, "POST", "/files/mailbox/read", H("member1"),
                         "{\"all\":true}");
    CHECK(rd.status == 200);
    CHECK(jint(rd.body, "marked") == base_all + 2);
    const auto mb2 = http(port, "POST", "/files/mailbox/unread", H("member1"), "");
    CHECK(mb2.status == 200);
    CHECK(jint(mb2.body, "all") == 0);
  }

  // —— 二期·办公室位置图：自楼层可见（同层互见）/org-admin 编辑/一人
  //     一工位/越界坐标夹回 ——
  {
    // 未登录 401；编辑权 org-admin（组织级面）：member1 建座 403
    CHECK(http(port, "GET", "/files/office-map", {}, "").status == 401);
    CHECK(http(port, "POST", "/files/office-map/seat", H("member1"),
               "{\"floor\":\"3F\",\"label\":\"A-01\",\"x\":0.2,\"y\":0.3}")
              .status == 403);
    // admin1（审批段已授 org-admin）建三座（3F 两座、2F 一座）
    const auto s1 = http(port, "POST", "/files/office-map/seat", H("admin1"),
                         "{\"floor\":\"3F\",\"label\":\"A-01\",\"x\":0.2,"
                         "\"y\":0.3}");
    CHECK(s1.status == 200);
    const std::int64_t seat1 = jint(s1.body, "id");
    const auto s2 = http(port, "POST", "/files/office-map/seat", H("admin1"),
                         "{\"floor\":\"3F\",\"label\":\"A-02\",\"x\":0.4,"
                         "\"y\":0.3}");
    CHECK(s2.status == 200);
    const std::int64_t seat2 = jint(s2.body, "id");
    CHECK(http(port, "POST", "/files/office-map/seat", H("admin1"),
               "{\"floor\":\"2F\",\"label\":\"B-01\",\"x\":0.5,\"y\":0.5}")
              .status == 200);
    // 拖拽改位=同 (floor,label) upsert 覆盖坐标；越界夹回 0~1（9→1、-3→0）
    CHECK(http(port, "POST", "/files/office-map/seat", H("admin1"),
               "{\"floor\":\"3F\",\"label\":\"A-01\",\"x\":9,\"y\":-3}")
              .status == 200);
    // 绑定：幽灵 409；绑定后 member1 自楼层互见（3F 两座、2F 不出现）
    CHECK(http(port, "POST", "/files/office-map/bind", H("admin1"),
               "{\"id\":" + std::to_string(seat1) +
                   ",\"account\":\"ghost\"}")
              .status == 409);
    CHECK(http(port, "POST", "/files/office-map/bind", H("admin1"),
               "{\"id\":" + std::to_string(seat1) +
                   ",\"account\":\"member1\"}")
              .status == 200);
    const auto m1 = http(port, "GET", "/files/office-map", H("member1"), "");
    CHECK(m1.status == 200);
    CHECK(m1.body.find("\"floor\":\"3F\"") != std::string::npos);
    CHECK(m1.body.find("A-02") != std::string::npos);
    CHECK(m1.body.find("B-01") == std::string::npos); // 只见自楼层
    CHECK(m1.body.find("\"can_manage\":false") != std::string::npos);
    CHECK(m1.body.find("\"x\":1.0") != std::string::npos); // 越界夹回
    // 无工位者=空图（floor 空串，不造楼层）
    const auto m2 = http(port, "GET", "/files/office-map", H("owner1"), "");
    CHECK(m2.status == 200);
    CHECK(m2.body.find("\"floor\":\"\"") != std::string::npos);
    // org-admin 跨层查看 ?floor=；can_manage=true
    const auto m3 = http(port, "GET", "/files/office-map?floor=2F",
                         H("admin1"), "");
    CHECK(m3.status == 200);
    CHECK(m3.body.find("\"floor\":\"2F\"") != std::string::npos);
    CHECK(m3.body.find("B-01") != std::string::npos);
    CHECK(m3.body.find("\"can_manage\":true") != std::string::npos);
    // 一人一工位：member1 已占 A-01，直接绑 A-02 拒 409（换座先解绑，
    // 不静默顶替）；解绑后可绑
    CHECK(http(port, "POST", "/files/office-map/bind", H("admin1"),
               "{\"id\":" + std::to_string(seat2) +
                   ",\"account\":\"member1\"}")
              .status == 409);
    CHECK(http(port, "POST", "/files/office-map/bind", H("admin1"),
               "{\"id\":" + std::to_string(seat1) + ",\"account\":\"\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/office-map/bind", H("admin1"),
               "{\"id\":" + std::to_string(seat2) +
                   ",\"account\":\"member1\"}")
              .status == 200);
    // 删座：不存在 404
    CHECK(http(port, "POST", "/files/office-map/seat", H("admin1"),
               "{\"remove\":true,\"id\":999999}")
              .status == 404);
  }

  // —— 品牌物料：读三免鉴权（登录窗鉴权前显示）/写三判权 org-admin/
  //     PNG magic·大小·尺寸校验/version 单调（设计稿 品牌物料.md §6）——
  {
    // 免鉴权 GET：未配=默认全空 version 0（此段须先于任何写腿）
    const auto b0 = http(port, "GET", "/files/branding", {}, "");
    CHECK(b0.status == 200);
    CHECK(b0.body.find("\"company_name\":\"\"") != std::string::npos);
    CHECK(b0.body.find("\"version\":0") != std::string::npos);
    CHECK(b0.body.find("\"has_logo\":false") != std::string::npos);
    CHECK(http(port, "GET", "/files/branding/logo", {}, "").status == 404);
    // 写判权：未登录 401；非 org-admin 403；org-admin 200 且 version+1
    CHECK(http(port, "POST", "/files/branding", {},
               "{\"company_name\":\"甲乙丙\"}")
              .status == 401);
    CHECK(http(port, "POST", "/files/branding", H("member1"),
               "{\"company_name\":\"甲乙丙\"}")
              .status == 403);
    const auto bs =
        http(port, "POST", "/files/branding", H("admin1"),
             "{\"company_name\":\"甲乙丙有限公司\",\"accent\":\"#1a2b3c\","
             "\"slogan\":\"高效协作\"}");
    CHECK(bs.status == 200);
    CHECK(jint(bs.body, "version") == 1);
    // accent 坏形态 400（version 不动）
    CHECK(http(port, "POST", "/files/branding", H("admin1"),
               "{\"accent\":\"blue\"}")
              .status == 400);
    CHECK(http(port, "POST", "/files/branding", H("admin1"),
               "{\"accent\":\"#12345\"}")
              .status == 400);
    // 素材校验：非 PNG 415；空 body 400；合法 PNG（最小 IHDR 头）200
    const std::string not_png = "GIF89a-not-a-png-at-all";
    CHECK(http(port, "POST", "/files/branding/logo", H("admin1"), not_png)
              .status == 415);
    CHECK(http(port, "POST", "/files/branding/logo", H("admin1"), "")
              .status == 400);
    std::string png;
    png += std::string("\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8); // magic
    png += std::string("\x00\x00\x00\x0DIHDR", 8);             // IHDR
    png += std::string("\x00\x00\x02\x00", 4);                 // 宽 512
    png += std::string("\x00\x00\x02\x00", 4);                 // 高 512
    png += std::string("\x08\x06\x00\x00\x00", 5);             // 位深色型等
    CHECK(http(port, "POST", "/files/branding/splash", H("member1"), png)
              .status == 403);
    const auto bl =
        http(port, "POST", "/files/branding/logo", H("admin1"), png);
    CHECK(bl.status == 200);
    CHECK(jint(bl.body, "version") == 2); // 素材写也 version+1
    // 尺寸超限（宽 4097>2048）413；超大（>512KiB）413
    std::string big = png;
    big[16] = '\x00';
    big[17] = '\x00';
    big[18] = '\x10';
    big[19] = '\x01'; // 宽 4097
    CHECK(http(port, "POST", "/files/branding/logo", H("admin1"), big)
              .status == 413);
    const std::string huge(512 * 1024 + 1, '\0');
    const std::string huge_png =
        std::string("\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8) + huge;
    CHECK(http(port, "POST", "/files/branding/logo", H("admin1"), huge_png)
              .status == 413);
    // 免鉴权 GET 回已配值与字节面（logo 字节原样、splash 未配 404）
    const auto b1 = http(port, "GET", "/files/branding", {}, "");
    CHECK(b1.status == 200);
    CHECK(b1.body.find("\"company_name\":\"甲乙丙有限公司\"") !=
          std::string::npos);
    CHECK(b1.body.find("\"accent\":\"#1a2b3c\"") != std::string::npos);
    CHECK(b1.body.find("\"slogan\":\"高效协作\"") != std::string::npos);
    CHECK(b1.body.find("\"has_logo\":true") != std::string::npos);
    CHECK(b1.body.find("\"version\":2") != std::string::npos);
    const auto glogo = http(port, "GET", "/files/branding/logo", {}, "");
    CHECK(glogo.status == 200);
    CHECK(glogo.body == png); // 字节面原样
    CHECK(http(port, "GET", "/files/branding/splash", {}, "").status == 404);
    // 清空（显式空串）与清除素材：version 单调递增、clear 后 404
    CHECK(http(port, "POST", "/files/branding", H("admin1"),
               "{\"slogan\":\"\"}")
              .status == 200);
    CHECK(jint(http(port, "GET", "/files/branding", {}, "").body,
              "version") == 3);
    CHECK(http(port, "GET", "/files/branding", {}, "").body.find(
              "\"slogan\":\"\"") != std::string::npos);
    // 素材清除路由（品牌设置页「清除」按钮所需，store 六方法全暴露）：
    // 布尔门 400／member 403／清除 200 version+1／has_logo 回 false 且
    // 字节面 404／空对象 400（无可写品牌字段）；素材覆盖=重传照旧
    CHECK(http(port, "POST", "/files/branding", H("admin1"),
               "{\"clear_logo\":\"yes\"}").status == 400);
    CHECK(http(port, "POST", "/files/branding", H("member1"),
               "{\"clear_splash\":true}").status == 403);
    CHECK(http(port, "POST", "/files/branding", H("admin1"),
               "{\"clear_logo\":true}").status == 200);
    CHECK(jint(http(port, "GET", "/files/branding", {}, "").body,
              "version") == 4);
    CHECK(http(port, "GET", "/files/branding", {}, "").body.find(
              "\"has_logo\":false") != std::string::npos);
    CHECK(http(port, "GET", "/files/branding/logo", {}, "").status == 404);
    CHECK(http(port, "POST", "/files/branding", H("admin1"), "{}").status ==
          400);
  }

  // —— 二期 远程协助：协议面＋媒体中继（模型层平台-11 在 store：
  //     五态/consent 红线/audit 红线/部门开关默认禁；此处验路由判权、
  //     状态门与媒体回环）——
  {
    // 未登录 401
    CHECK(http(port, "POST", "/files/assist/request", {},
               "{\"target\":\"member1\",\"perms\":[\"view\"]}").status == 401);
    // 部门放行开关默认禁（白名单口径）：默认全局行禁 → 403
    CHECK(http(port, "POST", "/files/assist/request", H("owner1"),
               "{\"target\":\"member1\",\"perms\":[\"view\"]}").status ==
          403);
    // 开全局放行（直调 store；CLI assist policy set 同款）
    CHECK(store.assist_policy_set(true, "", "admin1", 1700000001000));
    // 形态门：自协助 400／幽灵受控方 404／非法权限名 400
    CHECK(http(port, "POST", "/files/assist/request", H("owner1"),
               "{\"target\":\"owner1\",\"perms\":[\"view\"]}").status == 400);
    CHECK(http(port, "POST", "/files/assist/request", H("owner1"),
               "{\"target\":\"ghost\",\"perms\":[\"view\"]}").status == 404);
    CHECK(http(port, "POST", "/files/assist/request", H("owner1"),
               "{\"target\":\"member1\",\"perms\":[\"root\"]}").status == 400);
    // 发起 200：会话 id ra- 前缀（store 层生成）
    const auto req = http(port, "POST", "/files/assist/request", H("owner1"),
                          "{\"target\":\"member1\",\"perms\":[\"view\","
                          "\"mouse\",\"keyboard\"]}");
    CHECK(req.status == 200);
    const std::string sid = jstr(req.body, "id");
    CHECK(sid.rfind("ra-", 0) == 0);
    // consent 红线：批/拒只属受控方本人——发起方自批 403
    CHECK(http(port, "POST", "/files/assist/respond", H("owner1"),
               "{\"id\":\"" + sid + "\",\"approve\":true}")
              .status == 403);
    // 实批 ⊆ 申请（可缩不可扩）：clipboard 不在申请集 → 409
    CHECK(http(port, "POST", "/files/assist/respond", H("member1"),
               "{\"id\":\"" + sid +
                   "\",\"approve\":true,\"perms\":[\"view\",\"clipboard\"]}")
              .status == 409);
    // 缩权批准：只给 view+mouse（keyboard 留作未批腿）
    CHECK(http(port, "POST", "/files/assist/respond", H("member1"),
               "{\"id\":\"" + sid +
                   "\",\"approve\":true,\"perms\":[\"view\",\"mouse\"]}")
              .status == 200);
    // 媒体状态门：未 start 推帧 409
    CHECK(http(port, "POST", "/files/assist/frame", H("member1"),
               "{\"id\":\"" + sid + "\",\"jpeg_b64\":\"ZmFrZQ==\",\"seq\":1}")
              .status == 409);
    // 非当事方 start 403；发起方 start 200；重复 start 409
    CHECK(http(port, "POST", "/files/assist/start", H("admin1"),
               "{\"id\":\"" + sid + "\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/assist/start", H("owner1"),
               "{\"id\":\"" + sid + "\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/assist/start", H("owner1"),
               "{\"id\":\"" + sid + "\"}")
              .status == 409);
    // 推帧=受控方专属：发起方推 403；受控方推 200（seq 回带）
    CHECK(http(port, "POST", "/files/assist/frame", H("owner1"),
               "{\"id\":\"" + sid + "\",\"jpeg_b64\":\"ZnJvbV9oZWxwZXI=\","
               "\"seq\":9}")
              .status == 403);
    const auto pf = http(port, "POST", "/files/assist/frame", H("member1"),
                         "{\"id\":\"" + sid +
                             "\",\"jpeg_b64\":\"ZmFrZQ==\",\"seq\":7}");
    CHECK(pf.status == 200);
    CHECK(jint(pf.body, "seq") == 7);
    // 拉帧=发起方专属且须 view：受控方拉 403；发起方拉回环同 seq 同帧
    CHECK(http(port, "GET", "/files/assist/frame?id=" + sid, H("member1"),
               "")
              .status == 403);
    const auto gf = http(port, "GET", "/files/assist/frame?id=" + sid,
                         H("owner1"), "");
    CHECK(gf.status == 200);
    CHECK(jint(gf.body, "seq") == 7);
    CHECK(gf.body.find("ZmFrZQ==") != std::string::npos);
    // 输入权限位：keyboard 未批 → key 403；mouse 已批 → click 200；
    // 受控方发输入=侧别 403
    CHECK(http(port, "POST", "/files/assist/input", H("owner1"),
               "{\"id\":\"" + sid + "\",\"kind\":\"key\",\"key\":\"a\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/assist/input", H("owner1"),
               "{\"id\":\"" + sid +
                   "\",\"kind\":\"mouse_click\",\"x\":0.5,\"y\":0.25}")
              .status == 200);
    CHECK(http(port, "POST", "/files/assist/input", H("member1"),
               "{\"id\":\"" + sid + "\",\"kind\":\"mouse_click\",\"x\":0.1,"
               "\"y\":0.1}")
              .status == 403);
    // 取走即清（FIFO 不重投）：发起方取输入=侧别 403；受控方取见一笔，
    // 再取空
    CHECK(http(port, "GET", "/files/assist/input?id=" + sid, H("owner1"), "")
              .status == 403);
    const auto gi = http(port, "GET", "/files/assist/input?id=" + sid,
                         H("member1"), "");
    CHECK(gi.status == 200);
    CHECK(gi.body.find("mouse_click") != std::string::npos);
    CHECK(gi.body.find("\"x\":0.5") != std::string::npos); // 原样转发带坐标
    const auto gi2 = http(port, "GET", "/files/assist/input?id=" + sid,
                          H("member1"), "");
    CHECK(gi2.status == 200);
    CHECK(gi2.body.find("mouse_click") == std::string::npos);
    // 台账（过程持续可见）：双方视角都见会话行（状态 active）
    const auto ls = http(port, "GET", "/files/assist/sessions", H("owner1"),
                         "");
    CHECK(ls.status == 200);
    CHECK(ls.body.find(sid) != std::string::npos);
    CHECK(ls.body.find("\"active\"") != std::string::npos);
    // 审计链当事方可读、非当事方 403
    const auto aud = http(port, "POST", "/files/assist/audit", H("member1"),
                          "{\"id\":\"" + sid + "\"}");
    CHECK(aud.status == 200);
    CHECK(aud.body.find("\"request\"") != std::string::npos);
    CHECK(aud.body.find("\"approve\"") != std::string::npos);
    CHECK(aud.body.find("\"start\"") != std::string::npos);
    CHECK(http(port, "POST", "/files/assist/audit", H("admin1"),
               "{\"id\":\"" + sid + "\"}")
              .status == 403);
    // 受控方 end=撤权即时生效：媒体槽即擦（再拉帧 409 非进行中）、
    // 重复 end 409
    CHECK(http(port, "POST", "/files/assist/end", H("member1"),
               "{\"id\":\"" + sid + "\",\"reason\":\"收工\"}")
              .status == 200);
    CHECK(http(port, "GET", "/files/assist/frame?id=" + sid, H("owner1"), "")
              .status == 409);
    CHECK(http(port, "POST", "/files/assist/end", H("member1"),
               "{\"id\":\"" + sid + "\"}")
              .status == 409);
    // deny 终态腿：新会话被拒后再 respond 409
    const auto req2 = http(port, "POST", "/files/assist/request", H("owner1"),
                           "{\"target\":\"admin1\",\"perms\":[\"view\"]}");
    CHECK(req2.status == 200);
    const std::string sid2 = jstr(req2.body, "id");
    CHECK(http(port, "POST", "/files/assist/respond", H("admin1"),
               "{\"id\":\"" + sid2 + "\",\"approve\":false}")
              .status == 200);
    // 终态守卫：denied 后带合法实批再批 → 409（空 perms 则先被形态门
    // 400 拒——批准须非空子集，与 store 口径一致）
    CHECK(http(port, "POST", "/files/assist/respond", H("admin1"),
               "{\"id\":\"" + sid2 + "\",\"approve\":true,\"perms\":[]}")
              .status == 400);
    CHECK(http(port, "POST", "/files/assist/respond", H("admin1"),
               "{\"id\":\"" + sid2 +
                   "\",\"approve\":true,\"perms\":[\"view\"]}")
              .status == 409);
    // 台账收口：终态 closed 与 denied 都在
    const auto ls2 = http(port, "GET", "/files/assist/sessions", H("owner1"),
                          "");
    CHECK(ls2.status == 200);
    CHECK(ls2.body.find("\"closed\"") != std::string::npos);
    CHECK(ls2.body.find("\"denied\"") != std::string::npos);
  }

}

} // namespace memex::server::tests_api
