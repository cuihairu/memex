// test_files_api 拆分模块（LAY-005）：本套件场景体自原文件逐字搬移；
// 共享状态经 Fx 夹具传入（见 common.hpp）。套件按原序由 main 调用。
#include "common.hpp"

namespace memex::server::tests_api {

void tools_suite(Fx& f) {
  auto H = [&f](const char* a) { return f.H(a); };
  auto& store = f.store;
  auto& files = *f.files;
  auto& port = f.port;
  auto& gid = f.gid;
  auto& gid2 = f.gid2;

  // —— R25-1 群工具框架：白名单配置（memo:config=群主/管理员）、
  //     成员可读清单、动作代理调用（stub 回显＋谁/何时/动作/参数留痕）、
  //     审计管理面——入群即授权/退群即失（file:read 群继承） ——
  {
    const std::string gids = std::to_string(gid);
    // config：未登录 401；普通成员 403；缺字段/坏 actions 400；
    // 幽灵群不透存在性（owner1 也 403）
    CHECK(http(port, "POST", "/files/group-tools/config", {},
               "{\"gid\":" + gids + ",\"tool\":\"ci\",\"actions\":[]}")
              .status == 401);
    CHECK(http(port, "POST", "/files/group-tools/config", H("member1"),
               "{\"gid\":" + gids + ",\"tool\":\"ci\",\"actions\":[]}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids + ",\"tool\":\"ci\"}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":0,\"tool\":\"ci\",\"actions\":[]}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids + ",\"tool\":\"\",\"actions\":[]}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"ci\",\"actions\":[\"deploy\",42]}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":999999,\"tool\":\"ci\",\"actions\":[]}")
              .status == 403);
    // 管理员可配置；upsert 覆盖（再设即替换旧清单）
    CHECK(http(port, "POST", "/files/group-tools/config", H("admin1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"ci\",\"actions\":[\"deploy\",\"rollback\"]}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"ci\",\"actions\":[\"deploy\",\"rollback\","
                   "\"status\"]}")
              .status == 200);
    // list：未登录 401；非成员 403；坏 gid 400；成员见最新清单（覆盖生效）
    CHECK(http(port, "GET", "/files/group-tools/list?gid=" + gids, {}, "")
              .status == 401);
    CHECK(http(port, "GET", "/files/group-tools/list?gid=" + gids,
               H("outsider"), "").status == 403);
    CHECK(http(port, "GET", "/files/group-tools/list?gid=x", H("member1"), "")
              .status == 400);
    const auto lt = http(port, "GET",
                         "/files/group-tools/list?gid=" + gids, H("member1"),
                         "");
    CHECK(lt.status == 200);
    CHECK(lt.body.find("\"tool\":\"ci\"") != std::string::npos);
    CHECK(lt.body.find("\"deploy\"") != std::string::npos);
    CHECK(lt.body.find("\"rollback\"") != std::string::npos);
    CHECK(lt.body.find("\"status\"") != std::string::npos);
    // call：字段校验（params 须对象）400；非成员 403；幽灵群 403；
    // 未配置工具 404（gid2 有成员无配置）
    CHECK(http(port, "POST", "/files/group-tools/call", H("member1"),
               "{\"gid\":" + gids + ",\"tool\":\"ci\",\"action\":\"deploy\","
                                    "\"params\":[1]}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-tools/call", H("outsider"),
               "{\"gid\":" + gids + ",\"tool\":\"ci\",\"action\":\"deploy\","
                                    "\"params\":{}}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-tools/call", H("owner1"),
               "{\"gid\":999999,\"tool\":\"ci\",\"action\":\"deploy\","
               "\"params\":{}}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-tools/call", H("member1"),
               "{\"gid\":" + std::to_string(gid2) +
                   ",\"tool\":\"nope\",\"action\":\"deploy\",\"params\":{}}")
              .status == 404);
    // 白名单外 403；白名单内 200 且 stub 回显带参数
    CHECK(http(port, "POST", "/files/group-tools/call", H("member1"),
               "{\"gid\":" + gids + ",\"tool\":\"ci\",\"action\":\"hack\","
                                    "\"params\":{}}")
              .status == 403);
    const auto c1 = http(port, "POST", "/files/group-tools/call", H("member1"),
                         "{\"gid\":" + gids +
                             ",\"tool\":\"ci\",\"action\":\"deploy\","
                             "\"params\":{\"env\":\"prod\",\"rev\":\"a1b2\"}}");
    CHECK(c1.status == 200);
    CHECK(c1.body.find("\"stub\":true") != std::string::npos);
    CHECK(c1.body.find("\"env\":\"prod\"") != std::string::npos);
    // 再调一笔（供审计倒序断言：后调的在前）
    CHECK(http(port, "POST", "/files/group-tools/call", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"ci\",\"action\":\"status\",\"params\":{}}")
              .status == 200);
    // audit：成员 403（管理面）；群主见两行且倒序（status 在 deploy 前）、
    // 行带 actor/params/结果
    CHECK(http(port, "GET", "/files/group-tools/audit?gid=" + gids,
               H("member1"), "").status == 403);
    const auto ta = http(port, "GET",
                         "/files/group-tools/audit?gid=" + gids, H("owner1"),
                         "");
    CHECK(ta.status == 200);
    CHECK(ta.body.find("\"actor\":\"member1\"") != std::string::npos);
    CHECK(ta.body.find("\"rev\":\"a1b2\"") != std::string::npos);
    const auto pos_status = ta.body.find("\"action\":\"status\"");
    const auto pos_deploy = ta.body.find("\"action\":\"deploy\"");
    CHECK(pos_status != std::string::npos && pos_deploy != std::string::npos);
    CHECK(pos_status < pos_deploy); // id DESC：最新在前
  }

  // —— R25-2 CI/CD 工具：流水线定义（memo:config）、红绿灯列表（成员）、
  //     触发走 R25-1 白名单闸＋谁触发留痕＋结果卡片回群（注桩捕获） ——
  {
    const std::string gids = std::to_string(gid);
    // 结果卡片回群面：注桩捕获（main 接线 deliver_notice，测试注桩验卡片）
    std::vector<std::string> notices;
    files.set_notice([&notices](const std::string& t, const std::string& ti,
                                const std::string& c, int u) {
      notices.push_back(t + "|" + ti + "|" + c + "|" + std::to_string(u));
    });
    // 流水线定义：未登录 401；成员 403；缺字段/name 空 400；管理员可建；
    // 幽灵群 403（存在性不透）
    CHECK(http(port, "POST", "/files/group-ci/pipeline", {},
               "{\"gid\":" + gids + ",\"name\":\"dev\"}")
              .status == 401);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("member1"),
               "{\"gid\":" + gids + ",\"name\":\"dev\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("owner1"),
               "{\"gid\":" + gids + "}").status == 400);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("owner1"),
               "{\"gid\":" + gids + ",\"name\":\"\"}").status == 400);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("admin1"),
               "{\"gid\":" + gids + ",\"name\":\"dev\","
                             "\"description\":\"主构建\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("owner1"),
               "{\"gid\":999999,\"name\":\"dev\"}").status == 403);
    // 列表（红绿灯面）：非成员 403；成员见定义且未跑时无 last_status
    CHECK(http(port, "GET", "/files/group-ci/list?gid=" + gids,
               H("outsider"), "").status == 403);
    const auto l0 = http(port, "GET",
                         "/files/group-ci/list?gid=" + gids, H("member1"), "");
    CHECK(l0.status == 200);
    CHECK(l0.body.find("\"name\":\"dev\"") != std::string::npos);
    CHECK(l0.body.find("last_status") == std::string::npos);
    // 触发：白名单未开放 403（ci 工具尚未配置）；非成员 403；幽灵流水线
    // 404（先开白名单）；stub 默认成功；params fail=true 出红
    CHECK(http(port, "POST", "/files/group-ci/trigger", H("member1"),
               "{\"gid\":" + gids + ",\"pipeline\":\"dev\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"ci\",\"actions\":[\"trigger\"]}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-ci/trigger", H("outsider"),
               "{\"gid\":" + gids + ",\"pipeline\":\"dev\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-ci/trigger", H("member1"),
               "{\"gid\":" + gids + ",\"pipeline\":\"ghost\"}")
              .status == 404);
    const auto t1 = http(port, "POST", "/files/group-ci/trigger",
                         H("member1"),
                         "{\"gid\":" + gids + ",\"pipeline\":\"dev\"}");
    CHECK(t1.status == 200);
    CHECK(t1.body.find("\"status\":\"success\"") != std::string::npos);
    CHECK(t1.body.find("\"actor\":\"member1\"") != std::string::npos);
    CHECK(jint(t1.body, "run_id") > 0);
    const auto t2 = http(port, "POST", "/files/group-ci/trigger", H("owner1"),
                         "{\"gid\":" + gids + ",\"pipeline\":\"dev\","
                                              "\"params\":{\"fail\":true}}");
    CHECK(t2.status == 200);
    CHECK(t2.body.find("\"status\":\"failed\"") != std::string::npos);
    // 红绿灯翻转：最近一笔失败→last_status=failed（绿翻红）
    const auto l1 = http(port, "GET",
                         "/files/group-ci/list?gid=" + gids, H("member1"), "");
    CHECK(l1.body.find("\"last_status\":\"failed\"") != std::string::npos);
    CHECK(l1.body.find("\"last_actor\":\"owner1\"") != std::string::npos);
    // 结果卡片回群：两笔触发两卡片；后笔失败在标题（红绿灯语义）
    CHECK(notices.size() == 2);
    CHECK(notices[0].find("group:" + gids) != std::string::npos);
    CHECK(notices[0].find("构建成功：dev") != std::string::npos);
    CHECK(notices[1].find("构建失败：dev") != std::string::npos);
    // run 历史：成员可读、id DESC（后触发的 failed 在前）、谁触发可回溯
    const auto rs = http(port, "GET",
                         "/files/group-ci/runs?gid=" + gids, H("member1"), "");
    CHECK(rs.status == 200);
    const auto pos_fail = rs.body.find("\"status\":\"failed\"");
    const auto pos_ok = rs.body.find("\"status\":\"success\"");
    CHECK(pos_fail != std::string::npos && pos_ok != std::string::npos);
    CHECK(pos_fail < pos_ok);
    CHECK(rs.body.find("\"actor\":\"member1\"") != std::string::npos);
    // 过滤：?pipeline=ghost 空集；坏 gid 400
    CHECK(http(port, "GET",
               "/files/group-ci/runs?gid=" + gids + "&pipeline=ghost",
               H("member1"), "").body.find("\"runs\":[]") !=
            std::string::npos);
    CHECK(http(port, "GET", "/files/group-ci/runs?gid=x", H("member1"), "")
              .status == 400);
    // 触发留痕进 R25-1 工具审计（tool=ci action=trigger 两行）
    const auto ca = http(port, "GET",
                         "/files/group-tools/audit?gid=" + gids, H("owner1"),
                         "");
    CHECK(ca.body.find("\"tool\":\"ci\"") != std::string::npos);
    CHECK(ca.body.find("\"action\":\"trigger\"") != std::string::npos);
    // 流水线删除：成员 403；管理员删 200；再触发 404
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("member1"),
               "{\"gid\":" + gids + ",\"name\":\"dev\",\"op\":\"delete\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("admin1"),
               "{\"gid\":" + gids + ",\"name\":\"dev\",\"op\":\"delete\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-ci/trigger", H("member1"),
               "{\"gid\":" + gids + ",\"pipeline\":\"dev\"}")
              .status == 404);
  }

  // —— R25-3 打包工具＋配置导出（首批动作类示例）——
  {
    const std::string gids = std::to_string(gid);
    // 打包卡片回群面：换新注桩捕获
    std::vector<std::string> notices;
    files.set_notice([&notices](const std::string& t, const std::string& ti,
                                const std::string& c, int u) {
      notices.push_back(t + "|" + ti);
    });
    // 打包：未登录 401；缺字段 400；非成员 403；白名单未开 403（成员也拒）
    CHECK(http(port, "POST", "/files/group-pack/build", {},
               "{\"gid\":" + gids + ",\"name\":\"app\",\"version\":\"1.0\"}")
              .status == 401);
    CHECK(http(port, "POST", "/files/group-pack/build", H("member1"),
               "{\"gid\":" + gids + ",\"name\":\"app\"}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-pack/build", H("outsider"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-pack/build", H("member1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\"}")
              .status == 403);
    // 开白名单（pack/build）→成员可打包（入群即授权）；同款重复=覆盖台账
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"pack\",\"actions\":[\"build\"]}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-pack/build", H("member1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\","
                   "\"note\":\"首包\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-pack/build", H("member1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\","
                   "\"note\":\"重打\"}")
              .status == 200);
    // 卡片回群：两笔两卡片，标题带产物名与版本
    CHECK(notices.size() == 2);
    CHECK(notices[1].find("打包完成：app 1.0") != std::string::npos);
    // 台账：非成员 403；成员见一条（同款覆盖）且 created_by=member1
    CHECK(http(port, "GET", "/files/group-pack/list?gid=" + gids,
               H("outsider"), "").status == 403);
    const auto pl = http(port, "GET",
                         "/files/group-pack/list?gid=" + gids, H("member1"),
                         "");
    CHECK(pl.status == 200);
    CHECK(pl.body.find("\"name\":\"app\"") != std::string::npos);
    CHECK(pl.body.find("\"note\":\"重打\"") != std::string::npos);
    CHECK(pl.body.find("\"created_by\":\"member1\"") != std::string::npos);
    // 删除恒归管理员：成员 403；幽灵产物 404；管理员删 200
    CHECK(http(port, "POST", "/files/group-pack/delete", H("member1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-pack/delete", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"ghost\",\"version\":\"9.9\"}")
              .status == 404);
    CHECK(http(port, "POST", "/files/group-pack/delete", H("admin1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\"}")
              .status == 200);
    // 配置导出：管理面——成员 403；坏 gid 400；群主 200 快照带成员角色/
    // 工具白名单/备忘录开关/密码箱存在性；密文面永不进导出
    CHECK(http(port, "GET", "/files/group-export?gid=" + gids, H("member1"),
               "").status == 403);
    CHECK(http(port, "GET", "/files/group-export?gid=x", H("owner1"), "")
              .status == 400);
    const auto ex = http(port, "GET",
                         "/files/group-export?gid=" + gids, H("owner1"), "");
    CHECK(ex.status == 200);
    CHECK(ex.body.find("\"owner\":\"owner1\"") != std::string::npos);
    CHECK(ex.body.find("\"account\":\"member1\"") != std::string::npos);
    CHECK(ex.body.find("\"role\":\"admin\"") != std::string::npos);
    CHECK(ex.body.find("\"tool\":\"ci\"") != std::string::npos);
    CHECK(ex.body.find("\"tool\":\"pack\"") != std::string::npos);
    CHECK(ex.body.find("\"exists\":true") != std::string::npos);
    // 密文不进导出：包裹块/条目密文键一律不在快照
    CHECK(ex.body.find("wrapped_dek") == std::string::npos);
    CHECK(ex.body.find("secret_ct") == std::string::npos);
    CHECK(ex.body.find("kdf_salt") == std::string::npos);
    // 导出=敏感动作：进工具留痕（tool=export）
    const auto ea = http(port, "GET",
                         "/files/group-tools/audit?gid=" + gids, H("owner1"),
                         "");
    CHECK(ea.body.find("\"tool\":\"export\"") != std::string::npos);
  }

  // —— 群组-2 群记录导出（Markdown 三段定案④／权限定案②全员＝本群成员／
  //     定案③ audit_reads 恰一行／AI 档降级注明／归档只读副本口径）——
  {
    const auto g2 = store.create_group("records", "owner1",
                                       {"owner1", "admin1", "member1"});
    CHECK(g2 > 0);
    CHECK(store.group_set_role(g2, "admin1", "admin"));
    const std::string g2s = std::to_string(g2);
    const std::string g2t = "group:" + g2s;
    // 固定时刻的消息序列：升序时刻即升序 id（导出升序分页的页序基准）；
    // 含一条撤回（撤回标记可见）与一条区间外（不该进导出）
    const std::int64_t base = 1700000000000LL;
    CHECK(store.store_message("rec-1", "owner1", g2t, 10, "第一条记录", base + 1000));
    CHECK(store.store_message("rec-2", "member1", g2t, 10, "第二条记录", base + 2000));
    CHECK(store.store_message("rec-3", "owner1", g2t, 10, "第三条记录", base + 3000));
    CHECK(store.recall_message("rec-3", "owner1", base + 3500));
    CHECK(store.store_message("rec-4", "member1", g2t, 10, "第四条记录", base + 4000));
    CHECK(store.store_message("rec-5", "owner1", g2t, 10, "第五条记录", base + 5000));
    CHECK(store.store_message("rec-out", "owner1", g2t, 10, "区间外消息", base - 60000));
    const std::string from = std::to_string(base - 1000);
    const std::string to = std::to_string(base + 6000);
    const std::string exp = "/files/group-record-export?gid=" + g2s +
                            "&from=" + from + "&to=" + to;
    // 归档基线（副本口径：导出前后逐位一致——R3 只读断言）
    const auto archive_snap = [&]() {
      memex::server::MessageSearch qs;
      qs.account = "owner1";
      qs.peer = g2t;
      qs.limit = 1000;
      std::vector<std::pair<std::string, std::string> > v;
      for (const auto& m : store.search_messages(qs)) {
        v.emplace_back(m.msg_id, m.text);
      }
      std::sort(v.begin(), v.end());
      return v;
    };
    const auto before = archive_snap();
    CHECK(before.size() == 6);
    // 参与人明细（角色＋加入时间；导出「参与人」段同源）
    const auto parts = store.group_members_detail(g2);
    CHECK(parts.size() == 3);
    bool saw_member1 = false;
    for (const auto& p : parts) {
      if (p.account == "member1") {
        saw_member1 = true;
        CHECK(p.role == "member");
        CHECK(p.joined_ms > 0);
      }
    }
    CHECK(saw_member1);
    // 权限：未带令牌 401；坏 gid 400；幽灵群 404；非群成员 403（定案②）
    CHECK(http(port, "GET", exp, {}, "").status == 401);
    CHECK(http(port, "GET",
               "/files/group-record-export?gid=x&from=" + from + "&to=" + to,
               H("owner1"), "").status == 400);
    CHECK(http(port, "GET",
               "/files/group-record-export?gid=999999&from=" + from + "&to=" + to,
               H("owner1"), "").status == 404);
    CHECK(http(port, "GET", exp, H("outsider"), "").status == 403);
    // 本群普通成员 200（定案②全员＝本群成员，非仅群主/管理员）
    const auto r1 = http(port, "GET", exp, H("member1"), "");
    CHECK(r1.status == 200);
    const std::string md1 = jstr(r1.body, "md");
    CHECK(md1.find("# 群记录：records") != std::string::npos);
    CHECK(md1.find("## 参与人") != std::string::npos);
    CHECK(md1.find("## 时间范围") != std::string::npos);
    CHECK(md1.find("## 聊天记录") != std::string::npos);
    // 参与人：名单＋角色＋加入时间
    CHECK(md1.find("owner1（owner）") != std::string::npos);
    CHECK(md1.find("admin1（admin）") != std::string::npos);
    CHECK(md1.find("member1（member）") != std::string::npos);
    CHECK(md1.find("加入于 ") != std::string::npos);
    CHECK(md1.find("离开时间不含") != std::string::npos);
    // 时间范围：建群时间（groups.created_ms 真值，与导出同一时间格式）＋
    // 所选区间
    const auto g2info = store.group_info(g2);
    CHECK(g2info.has_value());
    std::time_t ct = static_cast<std::time_t>(g2info->created_ms / 1000);
    std::tm tmv{};
    localtime_r(&ct, &tmv);
    char cb[32];
    std::strftime(cb, sizeof(cb), "%Y-%m-%d %H:%M:%S", &tmv);
    CHECK(md1.find(std::string("建群时间：") + cb) != std::string::npos);
    CHECK(md1.find("导出区间：") != std::string::npos);
    // 聊天记录：发送人＋时间戳；撤回行带标记；区间外不进导出
    CHECK(md1.find("owner1: 第一条记录") != std::string::npos);
    CHECK(md1.find("member1: 第二条记录") != std::string::npos);
    CHECK(md1.find("（撤回）") != std::string::npos);
    CHECK(md1.find("区间外消息") == std::string::npos);
    CHECK(jint(r1.body, "count") == 5);
    CHECK(r1.body.find("\"has_more\":false") != std::string::npos);
    CHECK(r1.body.find("\"truncated\":false") != std::string::npos);
    // AI 档降级：ai_enabled=false＋「AI 未启用」注明，无「总结」章节占位
    CHECK(r1.body.find("\"ai_enabled\":false") != std::string::npos);
    CHECK(md1.find("AI 未启用") == std::string::npos); // 未勾选 AI＝不加注明
    const auto ra = http(port, "GET", exp + "&with_ai=1", H("admin1"), "");
    CHECK(ra.status == 200);
    CHECK(ra.body.find("\"ai_enabled\":false") != std::string::npos);
    CHECK(jstr(ra.body, "md").find("AI 未启用") != std::string::npos);
    CHECK(jstr(ra.body, "md").find("## 总结") == std::string::npos);
    // 定案③：导出留痕（谁/何时/哪个群/区间/条数/是否含 AI 总结；不新表）
    const auto export_rows = [&]() {
      std::vector<memex::server::AuditReadRow> v;
      for (const auto& a : store.audit_reads(200)) {
        if (a.action == "group_record_export") v.push_back(a);
      }
      return v;
    };
    {
      const auto rows = export_rows();
      CHECK(rows.size() == 2);
      bool saw_ai = false, saw_plain = false;
      for (const auto& a : rows) {
        CHECK(a.op_account == "member1" || a.op_account == "admin1");
        CHECK(a.filters.find("\"gid\":" + g2s) != std::string::npos);
        CHECK(a.filters.find("\"from_ms\":" + from) != std::string::npos);
        CHECK(a.filters.find("\"to_ms\":" + to) != std::string::npos);
        CHECK(a.result_count == 5);
        CHECK(a.ts_ms > 0);
        if (a.filters.find("\"with_ai\":true") != std::string::npos) {
          saw_ai = true;
        } else {
          saw_plain = true;
        }
      }
      CHECK(saw_ai && saw_plain); // AI 标记可查（定案③）
    }
    // 分页：升序页序无重无漏（keyset 游标续传；续页不带三段头）
    const auto fetch_page = [&](const std::string& acct, int page_size,
                                int fetched, int cap, std::int64_t after_id) {
      return http(port, "GET",
                  exp + "&page_size=" + std::to_string(page_size) +
                      "&cap=" + std::to_string(cap) + "&fetched=" +
                          std::to_string(fetched) + "&after_id=" +
                          std::to_string(after_id),
                  H(acct.c_str()), "");
    };
    const auto p1 = fetch_page("member1", 2, 0, 10000, 0);
    CHECK(p1.status == 200);
    CHECK(jint(p1.body, "count") == 2);
    CHECK(p1.body.find("\"has_more\":true") != std::string::npos);
    const std::string md_a = jstr(p1.body, "md");
    CHECK(md_a.find("## 参与人") != std::string::npos);
    CHECK(md_a.find("第一条记录") != std::string::npos);
    CHECK(md_a.find("第二条记录") != std::string::npos);
    CHECK(md_a.find("第三条记录") == std::string::npos);
    const std::int64_t nid1 = std::atoll(jstr(p1.body, "next_after_id").c_str());
    CHECK(nid1 > 0);
    const auto p2 = fetch_page("member1", 2, 2, 10000, nid1);
    CHECK(jint(p2.body, "count") == 2);
    CHECK(p2.body.find("\"has_more\":true") != std::string::npos);
    const std::string md_b = jstr(p2.body, "md");
    CHECK(md_b.find("## 参与人") == std::string::npos); // 续页只带聊天记录行
    CHECK(md_b.find("第三条记录") != std::string::npos);
    CHECK(md_b.find("第四条记录") != std::string::npos);
    CHECK(md_b.find("第一条记录") == std::string::npos);
    const std::int64_t nid2 = std::atoll(jstr(p2.body, "next_after_id").c_str());
    CHECK(nid2 > nid1);
    const auto p3 = fetch_page("member1", 2, 4, 10000, nid2);
    CHECK(jint(p3.body, "count") == 1);
    CHECK(p3.body.find("\"has_more\":false") != std::string::npos);
    const std::string md_c = jstr(p3.body, "md");
    CHECK(md_c.find("第五条记录") != std::string::npos);
    CHECK(md_c.find("第二条记录") == std::string::npos); // 无重无漏
    // 截断：单次上限触顶→truncated（客户端据此停止；不静默截尾）
    const auto t1 = fetch_page("member1", 2, 0, 3, 0);
    CHECK(jint(t1.body, "count") == 2);
    CHECK(t1.body.find("\"has_more\":true") != std::string::npos);
    const std::int64_t tnid = std::atoll(jstr(t1.body, "next_after_id").c_str());
    const auto t2 = fetch_page("member1", 2, 2, 3, tnid);
    CHECK(jint(t2.body, "count") == 1);
    CHECK(t2.body.find("\"truncated\":true") != std::string::npos);
    CHECK(t2.body.find("\"has_more\":false") != std::string::npos);
    CHECK(export_rows().size() == 4); // 两次单页＋两次分页导出各一行
    // 退群即失权：同请求 403（不参与审计）
    CHECK(store.group_leave(g2, "member1"));
    CHECK(http(port, "GET", exp, H("member1"), "").status == 403);
    CHECK(export_rows().size() == 4);
    CHECK(store.group_members_detail(g2).size() == 2); // 退群即摘行
    // 归档只读：导出前后逐位一致（R3 副本口径）
    CHECK(archive_snap() == before);
  }


}

} // namespace memex::server::tests_api
