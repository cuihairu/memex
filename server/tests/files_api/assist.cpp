// test_files_api 拆分模块（LAY-005）：本套件场景体自原文件逐字搬移；
// 共享状态经 Fx 夹具传入（见 common.hpp）。套件按原序由 main 调用。
#include "common.hpp"

namespace memex::server::tests_api {

void assist_suite(Fx& f) {
  auto H = [&f](const char* a) { return f.H(a); };
  auto& store = f.store;
  auto& files = *f.files;
  auto& port = f.port;
  auto& tok = f.tok;
  auto& gid = f.gid;
  auto& gid2 = f.gid2;

  // —— R27-2 外部任务登记：provider/ext_key 成对、个人登记不转派、
  //     列表回带引用（详情 URL 由客户端 SPI 解析，服务端只存引用）——
  {
    // 成对缺失各拒
    CHECK(http(port, "POST", "/files/tasks", H("owner1"),
               "{\"title\":\"外部缺键\",\"provider\":\"url\"}").status == 400);
    CHECK(http(port, "POST", "/files/tasks", H("owner1"),
               "{\"title\":\"外部缺provider\",\"ext_key\":"
               "\"https://a.example/1\"}").status == 400);
    // 直通链接登记 200（外部条目落自己清单）
    const auto ex = http(port, "POST", "/files/tasks", H("owner1"),
                         "{\"title\":\"跟踪上游缺陷\",\"provider\":\"url\","
                         "\"ext_key\":\"https://github.com/x/y/issues/9\"}");
    CHECK(ex.status == 200);
    // 外部任务不转派（分配语义只对本地任务定义）
    CHECK(http(port, "POST", "/files/tasks", H("owner1"),
               "{\"title\":\"外部转派拒\",\"provider\":\"url\",\"ext_key\":"
               "\"https://a.example/2\",\"assignee\":\"member1\"}")
              .status == 400);
    // 列表回带 provider/ext_key 引用
    const auto le = http(port, "GET", "/files/tasks", H("owner1"), "");
    CHECK(le.status == 200);
    CHECK(le.body.find("\"provider\":\"url\"") != std::string::npos);
    CHECK(le.body.find("https://github.com/x/y/issues/9") !=
          std::string::npos);
    // 外部条目完成勾选照主人语义（memex 本地进度标记，不回写外部）
    const std::int64_t exid = jint(ex.body, "id");
    CHECK(http(port, "POST", "/files/tasks/done", H("owner1"),
               "{\"id\":" + std::to_string(exid) + ",\"done\":true}")
              .status == 200);
    // 本地任务不受影响：无 provider 字段照常建
    CHECK(http(port, "POST", "/files/tasks", H("owner1"),
               "{\"title\":\"本地照常\"}").status == 200);
  }

  // —— 平台-12 权限模型接线：群能力面统一门（未配置=现行允许、配置禁即
  //     拒；停用即 403 deny:capability-<名>，重启用即恢复）——
  {
    const std::string gids = std::to_string(gid);
    const std::string gid2s = std::to_string(gid2);
    std::int64_t cap_ts = 1700000000000;
    // memo：读/写双路由都挡
    CHECK(store.group_capability_set(gid, "memo", false, "owner1", cap_ts++));
    const auto cm = http(port, "GET", "/files/group-memo/list?gid=" + gids,
                         H("member1"), "");
    CHECK(cm.status == 403);
    CHECK(jstr(cm.body, "error").find("deny:capability-memo") !=
          std::string::npos);
    CHECK(http(port, "POST", "/files/group-memo/save", H("admin1"),
               "{\"gid\":" + gids +
                   ",\"title\":\"t\",\"content\":\"c\"}").status == 403);
    CHECK(store.group_capability_set(gid, "memo", true, "owner1", cap_ts++));
    CHECK(http(port, "GET", "/files/group-memo/list?gid=" + gids,
               H("member1"), "").status == 200);
    // vault：初始化前 info 也挡（面级停用，不只挡已建箱）
    CHECK(store.group_capability_set(gid, "vault", false, "owner1", cap_ts++));
    const auto cv = http(port, "GET", "/files/group-vault/info?gid=" + gids,
                         H("member1"), "");
    CHECK(cv.status == 403);
    CHECK(jstr(cv.body, "error").find("deny:capability-vault") !=
          std::string::npos);
    CHECK(store.group_capability_set(gid, "vault", true, "owner1", cap_ts++));
    CHECK(http(port, "GET", "/files/group-vault/info?gid=" + gids,
               H("member1"), "").status == 200);
    // tools：tools／ci／pack 三族同门（群工具面=CI/CD、打包、配置导出）
    CHECK(store.group_capability_set(gid, "tools", false, "owner1", cap_ts++));
    const auto ct = http(port, "GET", "/files/group-tools/list?gid=" + gids,
                         H("member1"), "");
    CHECK(ct.status == 403);
    CHECK(jstr(ct.body, "error").find("deny:capability-tools") !=
          std::string::npos);
    CHECK(http(port, "GET", "/files/group-ci/list?gid=" + gids, H("member1"),
               "").status == 403);
    CHECK(http(port, "GET", "/files/group-pack/list?gid=" + gids,
               H("member1"), "").status == 403);
    CHECK(store.group_capability_set(gid, "tools", true, "owner1", cap_ts++));
    CHECK(http(port, "GET", "/files/group-tools/list?gid=" + gids,
               H("member1"), "").status == 200);
    // server_tools：列表（成员读）与登记（管理写）同门
    CHECK(store.group_capability_set(gid, "server_tools", false, "owner1",
                                     cap_ts++));
    const auto cs =
        http(port, "GET", "/files/group-servers/list?gid=" + gids,
             H("member1"), "");
    CHECK(cs.status == 403);
    CHECK(jstr(cs.body, "error").find("deny:capability-server_tools") !=
          std::string::npos);
    CHECK(http(port, "POST", "/files/group-servers/enroll", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"cap-1\",\"host\":\"10.0.0.9\"}").status == 403);
    CHECK(store.group_capability_set(gid, "server_tools", true, "owner1",
                                     cap_ts++));
    CHECK(http(port, "GET", "/files/group-servers/list?gid=" + gids,
               H("member1"), "").status == 200);
    // files：上传/列表/下载/删除四动词（gid2 独立群，探针文件用毕即清）
    const auto up_cap = http(
        port, "POST", "/files/upload?target=group:" + gid2s,
        {{"Authorization", "Bearer " + tok.at("member1")},
         {"X-File-Name", "cap-probe.txt"}},
        "cap-bytes");
    CHECK(up_cap.status == 200);
    const std::int64_t id_cap = jint(up_cap.body, "id");
    CHECK(store.group_capability_set(gid2, "files", false, "owner1",
                                     cap_ts++));
    const auto uf = http(
        port, "POST", "/files/upload?target=group:" + gid2s,
        {{"Authorization", "Bearer " + tok.at("member1")},
         {"X-File-Name", "cap-denied.txt"}},
        "nope");
    CHECK(uf.status == 403);
    CHECK(jstr(uf.body, "error").find("deny:capability-files") !=
          std::string::npos);
    CHECK(http(port, "GET", "/files/list?target=group:" + gid2s,
               H("member1"), "").status == 403);
    CHECK(http(port, "GET", "/files/download?id=" + std::to_string(id_cap),
               H("member1"), "").status == 403);
    CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(id_cap),
               H("owner1"), "").status == 403);
    CHECK(store.group_capability_set(gid2, "files", true, "owner1", cap_ts++));
    const auto up_ok = http(
        port, "POST", "/files/upload?target=group:" + gid2s,
        {{"Authorization", "Bearer " + tok.at("member1")},
         {"X-File-Name", "cap-recovered.txt"}},
        "back");
    CHECK(up_ok.status == 200);
    CHECK(http(port, "POST",
               "/files/manage/delete?id=" + std::to_string(id_cap),
               H("owner1"), "").status == 200);
    CHECK(http(port, "POST",
               "/files/manage/delete?id=" + std::to_string(jint(up_ok.body, "id")),
               H("owner1"), "").status == 200);
  }

  // —— 二期群工具三件：投票（记名单选/改票覆盖/发起人或群主·管理员关票）
  //     ＋接龙（一人一条 upsert）＋群任务（认领制/done 终态留痕）——
  //     判权全循 file:read 群继承（非成员 403 不透存在性），资源号幽灵 404
  {
    const std::string gids = std::to_string(gid);
    // 结果卡片回群面（R26）：注桩捕获（main 接线 deliver_group_text＝
    // TEXT 群消息，测试注桩验卡片文本；缺省未设＝只落账不回群——本段
    // 之前所有 close 腿未设回调照常跑＝不崩口径隐式覆盖）
    std::vector<std::string> cards;
    files.set_group_text([&cards](const std::string& t, const std::string& f,
                                  const std::string& x) {
      cards.push_back(t + "|" + f + "|" + x);
    });
    // 投票建：未登录 401；非成员 403；选项<2 400；建 200
    CHECK(http(port, "POST", "/files/group-polls", {},
               "{\"gid\":" + gids + ",\"topic\":\"午饭\",\"options\":[\"A\",\"B\"]}")
              .status == 401);
    CHECK(http(port, "POST", "/files/group-polls", H("outsider"),
               "{\"gid\":" + gids + ",\"topic\":\"午饭\",\"options\":[\"A\",\"B\"]}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-polls", H("owner1"),
               "{\"gid\":" + gids + ",\"topic\":\"午饭\",\"options\":[\"只一个\"]}")
              .status == 400);
    const auto pc = http(port, "POST", "/files/group-polls", H("owner1"),
                         "{\"gid\":" + gids + ",\"topic\":\"午饭去哪\","
                         "\"options\":[\"面馆\",\"食堂\",\"外卖\"]}");
    CHECK(pc.status == 200);
    const std::int64_t poll1 = jint(pc.body, "poll_id");
    CHECK(poll1 > 0);
    // 列表（成员 200）；outsider 403
    CHECK(http(port, "GET", "/files/group-polls?gid=" + gids, H("outsider"),
               "").status == 403);
    const auto pl0 =
        http(port, "GET", "/files/group-polls?gid=" + gids, H("member1"), "");
    CHECK(pl0.status == 200);
    CHECK(pl0.body.find("\"counts\":[0,0,0]") != std::string::npos);
    // 投票：越界 400；幽灵票 404；改票覆盖（2 再改 1）；他人票不动
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll1) +
                   ",\"choice\":9}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":999999,\"choice\":1}")
              .status == 404);
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll1) +
                   ",\"choice\":2}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll1) +
                   ",\"choice\":1}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-polls/vote", H("owner1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll1) +
                   ",\"choice\":2}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-polls/vote", H("outsider"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll1) +
                   ",\"choice\":1}")
              .status == 403);
    // 列表统计：counts=[1,2,0]（member1 改票后 1 号、owner1+member1 原 2 号
    // 中 member1 移走→2 号只 owner1 与 member1=1+1）——精确断言；
    // 记名台账：member1 恰一条且 choice=1
    const auto pl1 =
        http(port, "GET", "/files/group-polls?gid=" + gids, H("member1"), "");
    CHECK(pl1.status == 200);
    CHECK(pl1.body.find("\"counts\":[1,1,0]") != std::string::npos);
    CHECK(pl1.body.find("\"account\":\"owner1\",\"choice\":2") !=
          std::string::npos);
    const auto pos_mv1 = pl1.body.find("\"account\":\"member1\",\"choice\":1");
    const auto pos_mv2 = pl1.body.find("\"account\":\"member1\",\"choice\":2");
    CHECK(pos_mv1 != std::string::npos);
    CHECK(pos_mv2 == std::string::npos); // 改票覆盖：旧票不留
    // 关票三腿：无关者（成员但非发起人非群主·管理员）403；发起人 200；
    // 重复关 409；关后投票 409
    CHECK(http(port, "POST", "/files/group-polls/close", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll1) + "}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-polls/close", H("owner1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll1) + "}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-polls/close", H("owner1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll1) + "}")
              .status == 409);
    // 结果卡片回群（R26）：显式关票发卡（counts/台账与列表页同口径）；
    // 重复关 409 不重复发（size 仍 1）
    CHECK(cards.size() == 1);
    {
      const auto& card = cards.back();
      CHECK(card.find("group:" + gids + "|owner1|") == 0);
      CHECK(card.find("【投票结果】#" + std::to_string(poll1) + " 午饭去哪") !=
            std::string::npos);
      CHECK(card.find("面馆×1") != std::string::npos);
      CHECK(card.find("食堂×1") != std::string::npos);
      CHECK(card.find("外卖×0") != std::string::npos);
      CHECK(card.find("member1→1") != std::string::npos);
      CHECK(card.find("owner1→2") != std::string::npos);
      CHECK(card.find("已截止") != std::string::npos);
    }
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll1) +
                   ",\"choice\":3}")
              .status == 409);
    // 截止自动关票（惰性判定）：建带近秒截止的票→未到点可投→到点投票
    // 409（改票同拒）→列表 status=closed 且 closed 库值不回写→发起人
    // close 补写留痕 200→重复关 409→close 后投票仍 409
    const std::int64_t now0 =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const auto pd = http(port, "POST", "/files/group-polls", H("owner1"),
                         "{\"gid\":" + gids + ",\"topic\":\"快截票\","
                         "\"options\":[\"甲\",\"乙\"],\"deadline_ms\":" +
                             std::to_string(now0 + 2000) + "}");
    CHECK(pd.status == 200);
    const std::int64_t poll2 = jint(pd.body, "poll_id");
    CHECK(poll2 > 0);
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll2) +
                   ",\"choice\":1}")
              .status == 200); // 未到点可投
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count() < now0 + 2100) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto vexp =
        http(port, "POST", "/files/group-polls/vote", H("member1"),
             "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll2) +
                 ",\"choice\":2}"); // 到点改票同拒
    CHECK(vexp.status == 409);
    CHECK(vexp.body.find("已到截止时间") != std::string::npos);
    const auto pl2 =
        http(port, "GET", "/files/group-polls?gid=" + gids, H("member1"), "");
    CHECK(pl2.status == 200);
    CHECK(pl2.body.find("\"status\":\"closed\"") != std::string::npos);
    CHECK(pl2.body.find("\"closed\":false") != std::string::npos); // 惰性判定不回写
    CHECK(http(port, "POST", "/files/group-polls/close", H("owner1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll2) + "}")
              .status == 200); // 到点后手动关票补写留痕
    // 到点补写关票同发卡（size 2）
    CHECK(cards.size() == 2);
    CHECK(cards.back().find("【投票结果】#" + std::to_string(poll2) +
                            " 快截票") != std::string::npos);
    CHECK(cards.back().find("甲×1") != std::string::npos);
    CHECK(cards.back().find("member1→1") != std::string::npos);
    CHECK(http(port, "POST", "/files/group-polls/close", H("owner1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll2) + "}")
              .status == 409);
    CHECK(http(port, "POST", "/files/group-polls/vote", H("owner1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll2) +
                   ",\"choice\":1}")
              .status == 409);
    // 匿名单选：建→投→counts 照常、votes 台账不回 voter 身份（全局唯一
    // 空台账段）→匿名改票 200＝upsert 靠库内 account 留痕（展示匿名口径）
    const auto pa = http(port, "POST", "/files/group-polls", H("owner1"),
                         "{\"gid\":" + gids + ",\"topic\":\"匿名评优\","
                         "\"options\":[\"甲\",\"乙\"],\"anonymous\":true}");
    CHECK(pa.status == 200);
    const std::int64_t poll3 = jint(pa.body, "poll_id");
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll3) +
                   ",\"choice\":1}")
              .status == 200);
    const auto pl3 =
        http(port, "GET", "/files/group-polls?gid=" + gids, H("member1"), "");
    CHECK(pl3.status == 200);
    CHECK(pl3.body.find("\"anonymous\":true") != std::string::npos);
    CHECK(pl3.body.find("\"votes\":[]") != std::string::npos);
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll3) +
                   ",\"choice\":2}")
              .status == 200); // 匿名改票：库内留 account 供 upsert
    // 匿名单选发多位集（3=bit0+bit1）→单选模式 400
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll3) +
                   ",\"choice\":3}")
              .status == 400);
    // 匿名票关票发卡：只报参与人数不回 voter 身份（展示匿名口径）
    CHECK(http(port, "POST", "/files/group-polls/close", H("owner1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll3) + "}")
              .status == 200);
    CHECK(cards.size() == 3);
    {
      const auto& card = cards.back();
      CHECK(card.find("【投票结果】#" + std::to_string(poll3) + " 匿名评优") !=
            std::string::npos);
      CHECK(card.find("匿名投票，共 1 人参与") != std::string::npos);
      CHECK(card.find("member1→") == std::string::npos); // 台账不回 voter
    }
    // 多选：建→投位集 5（选 1+3）→counts 位展开→改投 2 覆盖→counts
    // 此消彼长→位集越界 400→零位集 400
    const auto pm = http(port, "POST", "/files/group-polls", H("owner1"),
                         "{\"gid\":" + gids + ",\"topic\":\"多选征询\","
                         "\"options\":[\"甲\",\"乙\",\"丙\"],\"multi\":true}");
    CHECK(pm.status == 200);
    const std::int64_t poll4 = jint(pm.body, "poll_id");
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll4) +
                   ",\"choice\":5}")
              .status == 200);
    const auto pl4a =
        http(port, "GET", "/files/group-polls?gid=" + gids, H("member1"), "");
    CHECK(pl4a.status == 200);
    CHECK(pl4a.body.find("\"multi\":true") != std::string::npos);
    CHECK(pl4a.body.find("\"counts\":[1,0,1]") != std::string::npos);
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll4) +
                   ",\"choice\":2}")
              .status == 200); // 改投=整集覆盖
    const auto pl4b =
        http(port, "GET", "/files/group-polls?gid=" + gids, H("member1"), "");
    CHECK(pl4b.body.find("\"counts\":[0,1,0]") != std::string::npos);
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll4) +
                   ",\"choice\":8}")
              .status == 400); // bit3 越界（n=3）
    CHECK(http(port, "POST", "/files/group-polls/vote", H("member1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll4) +
                   ",\"choice\":0}")
              .status == 400);
    // 多选关票发卡：counts 位展开＋台账位集展开（改投后 2=单 bit 无加号）
    CHECK(http(port, "POST", "/files/group-polls/close", H("owner1"),
               "{\"gid\":" + gids + ",\"poll_id\":" + std::to_string(poll4) + "}")
              .status == 200);
    CHECK(cards.size() == 4);
    {
      const auto& card = cards.back();
      CHECK(card.find("【投票结果】#" + std::to_string(poll4) + " 多选征询") !=
            std::string::npos);
      CHECK(card.find("乙×1") != std::string::npos);
      CHECK(card.find("甲×0") != std::string::npos);
      CHECK(card.find("member1→2") != std::string::npos);
    }
    // 接龙：建（title 空 400/非成员 403/建 200）；加入与更新（一人一条
    // upsert：更新自己条目他人不动）；群主可关非本人发起；关后加入 409
    CHECK(http(port, "POST", "/files/group-chains", H("member1"),
               "{\"gid\":" + gids + ",\"title\":\"\"}").status == 400);
    CHECK(http(port, "POST", "/files/group-chains", H("outsider"),
               "{\"gid\":" + gids + ",\"title\":\"晚点接龙\"}").status == 403);
    const auto cc = http(port, "POST", "/files/group-chains", H("member1"),
                         "{\"gid\":" + gids + ",\"title\":\"晚饭接龙\","
                         "\"format_hint\":\"姓名+几点\"}");
    CHECK(cc.status == 200);
    const std::int64_t chain1 = jint(cc.body, "chain_id");
    CHECK(chain1 > 0);
    // 格式校验（hint「姓名+几点」=2 段）：单段/三段 400 文案明示
    {
      const auto cj = http(port, "POST", "/files/group-chains/join",
                           H("admin1"),
                           "{\"gid\":" + gids +
                               ",\"chain_id\":" + std::to_string(chain1) +
                               ",\"content\":\"只有一段\"}");
      CHECK(cj.status == 400);
      CHECK(cj.body.find("分 2 段") != std::string::npos);
    }
    CHECK(http(port, "POST", "/files/group-chains/join", H("admin1"),
               "{\"gid\":" + gids + ",\"chain_id\":" + std::to_string(chain1) +
                   ",\"content\":\"a+b+c\"}")
              .status == 400); // 三段≠2 段
    CHECK(http(port, "POST", "/files/group-chains/join", H("admin1"),
               "{\"gid\":" + gids + ",\"chain_id\":" + std::to_string(chain1) +
                   ",\"content\":\"admin1+19点\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-chains/join", H("member1"),
               "{\"gid\":" + gids + ",\"chain_id\":" + std::to_string(chain1) +
                   ",\"content\":\"member1+18点半\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-chains/join", H("admin1"),
               "{\"gid\":" + gids + ",\"chain_id\":" + std::to_string(chain1) +
                   ",\"content\":\"admin1+改 20点\"}")
              .status == 200);
    const auto cl =
        http(port, "GET", "/files/group-chains?gid=" + gids, H("member1"), "");
    CHECK(cl.status == 200);
    CHECK(cl.body.find("\"account\":\"admin1\",\"content\":\"admin1+改 20点\"") !=
          std::string::npos);
    CHECK(cl.body.find("admin1+19点") == std::string::npos); // 旧条目被覆盖
    CHECK(cl.body.find("\"account\":\"member1\",\"content\":\"member1+18点半\"") !=
          std::string::npos);
    CHECK(cl.body.find("晚饭接龙") != std::string::npos);
    CHECK(cl.body.find("只有一段") == std::string::npos); // 格式拒，未落地
    // 无 hint 接龙：自由文本不做段数校验（向后兼容）
    const auto cc2 = http(port, "POST", "/files/group-chains", H("admin1"),
                          "{\"gid\":" + gids + ",\"title\":\"自由接龙\"}");
    CHECK(cc2.status == 200);
    CHECK(http(port, "POST", "/files/group-chains/join", H("admin1"),
               "{\"gid\":" + gids +
                   ",\"chain_id\":" + std::to_string(jint(cc2.body, "chain_id")) +
                   ",\"content\":\"自由格式一条\"}")
              .status == 200);
    // 群主关（非发起人仍可关=群主/管理员口径）；关后加入 409；重复关 409
    CHECK(http(port, "POST", "/files/group-chains/close", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"chain_id\":" + std::to_string(chain1) + "}")
              .status == 200);
    // 接龙关发卡：条目全量明细（upsert 后终态）＋已截止
    CHECK(cards.size() == 5);
    {
      const auto& card = cards.back();
      CHECK(card.find("group:" + gids + "|owner1|") == 0);
      CHECK(card.find("【接龙结果】#" + std::to_string(chain1) + " 晚饭接龙") !=
            std::string::npos);
      CHECK(card.find("admin1：admin1+改 20点") != std::string::npos);
      CHECK(card.find("member1：member1+18点半") != std::string::npos);
      CHECK(card.find("已截止") != std::string::npos);
    }
    CHECK(http(port, "POST", "/files/group-chains/join", H("member1"),
               "{\"gid\":" + gids + ",\"chain_id\":" + std::to_string(chain1) +
                   ",\"content\":\"迟到的\"}")
              .status == 409);
    CHECK(http(port, "POST", "/files/group-chains/close", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"chain_id\":" + std::to_string(chain1) + "}")
              .status == 409);
    // 群任务：幽灵负责人 404；待认领建 200；指定负责人建 200
    CHECK(http(port, "POST", "/files/group-tasks", H("owner1"),
               "{\"gid\":" + gids + ",\"title\":\"换投影仪\","
               "\"assignee\":\"outsider\"}")
              .status == 404);
    const auto tc1 = http(port, "POST", "/files/group-tasks", H("owner1"),
                          "{\"gid\":" + gids + ",\"title\":\"会议室白板笔补货\"}");
    CHECK(tc1.status == 200);
    const std::int64_t task1 = jint(tc1.body, "task_id");
    const auto tc2 =
        http(port, "POST", "/files/group-tasks", H("owner1"),
             "{\"gid\":" + gids + ",\"title\":\"周报汇总\",\"assignee\":\"member1\"}");
    CHECK(tc2.status == 200);
    const std::int64_t task2 = jint(tc2.body, "task_id");
    const auto tc3 =
        http(port, "POST", "/files/group-tasks", H("admin1"),
             "{\"gid\":" + gids + ",\"title\":\"账目核对\",\"assignee\":\"admin1\"}");
    CHECK(tc3.status == 200);
    const std::int64_t task3 = jint(tc3.body, "task_id");
    // 认领：todo 且无人认领才可占；占位 409；幽灵 404
    CHECK(http(port, "POST", "/files/group-tasks/claim", H("member1"),
               "{\"gid\":" + gids +
                   ",\"task_id\":" + std::to_string(task1) + "}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-tasks/claim", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"task_id\":" + std::to_string(task1) + "}")
              .status == 409);
    CHECK(http(port, "POST", "/files/group-tasks/claim", H("member1"),
               "{\"gid\":" + gids + ",\"task_id\":999999}")
              .status == 404);
    CHECK(http(port, "POST", "/files/group-tasks/claim", H("member1"),
               "{\"gid\":" + gids +
                   ",\"task_id\":" + std::to_string(task2) + "}")
              .status == 409); // 已有负责人=占位
    // 完成：无关者 403（member1 对 admin1 的任务非负责人/创建者/群主·管理员）；
    // 负责人 200；群主可代（owner1 对 admin1 的任务）；重复完成 409
    CHECK(http(port, "POST", "/files/group-tasks/done", H("member1"),
               "{\"gid\":" + gids +
                   ",\"task_id\":" + std::to_string(task3) + "}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-tasks/done", H("member1"),
               "{\"gid\":" + gids +
                   ",\"task_id\":" + std::to_string(task2) + "}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-tasks/done", H("member1"),
               "{\"gid\":" + gids +
                   ",\"task_id\":" + std::to_string(task2) + "}")
              .status == 409);
    CHECK(http(port, "POST", "/files/group-tasks/done", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"task_id\":" + std::to_string(task3) + "}")
              .status == 200);
    // 列表终态留痕：done 行带 done_by；claimed_ms 留痕；todo 行仍在
    const auto tl =
        http(port, "GET", "/files/group-tasks?gid=" + gids, H("member1"), "");
    CHECK(tl.status == 200);
    CHECK(tl.body.find("\"title\":\"周报汇总\"") != std::string::npos);
    CHECK(tl.body.find("\"status\":\"done\"") != std::string::npos);
    CHECK(tl.body.find("\"done_by\":\"member1\"") != std::string::npos);
    CHECK(tl.body.find("\"done_by\":\"owner1\"") != std::string::npos);
    CHECK(tl.body.find("\"status\":\"todo\"") != std::string::npos);
    // task1 已被认领：列表中不应再有无人认领行（认领留痕=assignee 落名）
    CHECK(tl.body.find("\"assignee\":\"\"") == std::string::npos);
  }

  // —— R27-7 领导视角：下属=直接下属（只一级跨级关闭）/az 写死只有直属
  //     上级可读（平级/跨级/org-admin 一律不可见）/负荷三数口径写死
  //    （未完成=status≠完成、已逾期=due 已过未完成、7 天内到期）——
  {
    // 干净链路：lead7（领导）← sub7（下属）← admin1（跨级上级）；
    // admin2=有 admin 角色但无任何下属（org-admin 无兜底腿）
    CHECK(store.create_account("lead7", "pw-lead7", "lead7"));
    CHECK(store.create_account("sub7", "pw-sub7", "sub7"));
    CHECK(store.create_account("admin2", "pw-admin2", "admin2", "admin"));
    CHECK(store.reporting_set("sub7", "lead7", 1700001400000));
    CHECK(store.reporting_set("lead7", "admin1", 1700001400001));
    for (const char* a : {"lead7", "sub7", "admin2"}) {
      const auto r = http(port, "POST", "/files/session", {},
                          "{\"account\":\"" + std::string(a) +
                              "\",\"password\":\"pw-" + std::string(a) +
                              "\"}");
      CHECK(r.status == 200);
      tok[a] = jstr(r.body, "token");
      CHECK(!tok[a].empty());
    }
    // 未登录 401
    CHECK(http(port, "GET", "/files/tasks/team", {}, "").status == 401);
    // 播 sub7 的任务面（现在时锚定：逾期=昨天、临期=3 天后、远期=30 天后）
    const auto now7 = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    const auto day = 86400000LL;
    CHECK(http(port, "POST", "/files/tasks", H("sub7"),
               "{\"title\":\"泳道待办\"}").status == 200);
    CHECK(http(port, "POST", "/files/tasks", H("sub7"),
               "{\"title\":\"泳道逾期\",\"due_ms\":" +
                   std::to_string(now7 - day) + "}")
              .status == 200);
    CHECK(http(port, "POST", "/files/tasks", H("sub7"),
               "{\"title\":\"泳道临期\",\"due_ms\":" +
                   std::to_string(now7 + 3 * day) + "}")
              .status == 200);
    const auto t7far = http(port, "POST", "/files/tasks", H("sub7"),
                            "{\"title\":\"泳道远期\",\"due_ms\":" +
                                std::to_string(now7 + 30 * day) + "}");
    CHECK(t7far.status == 200);
    const auto t7done = http(port, "POST", "/files/tasks", H("sub7"),
                             "{\"title\":\"泳道已完成\"}");
    CHECK(t7done.status == 200);
    // 完成腿：done 双写后不计入任何口径
    CHECK(http(port, "POST", "/files/tasks/done", H("sub7"),
               "{\"id\":" + std::to_string(jint(t7done.body, "id")) +
                   ",\"done\":true}")
              .status == 200);
    // 直属上级 GET 200：恰一条泳道（自己的任务不进自己的视角）
    const auto tm7 = http(port, "GET", "/files/tasks/team", H("lead7"), "");
    CHECK(tm7.status == 200);
    CHECK(tm7.body.find("\"account\":\"sub7\"") != std::string::npos);
    CHECK(tm7.body.find("\"account\":\"lead7\"") == std::string::npos);
    // 负荷三数口径：未完成=4（含待办+进行中+逾期+远期）、逾期=1、
    // 7 天内到期=1（30 天远期不入窗）；已完成行在 tasks 里但不计数
    CHECK(tm7.body.find("\"open\":4") != std::string::npos);
    CHECK(tm7.body.find("\"overdue\":1") != std::string::npos);
    CHECK(tm7.body.find("\"due_soon\":1") != std::string::npos);
    CHECK(tm7.body.find("泳道已完成") != std::string::npos);
    // 口径跟 status 走：待办→进行中仍计未完成；进行中→完成则退出
    CHECK(http(port, "POST", "/files/tasks/status", H("sub7"),
               "{\"id\":" + std::to_string(jint(t7far.body, "id")) +
                   ",\"status\":\"进行中\"}")
              .status == 200);
    auto tm7b = http(port, "GET", "/files/tasks/team", H("lead7"), "");
    CHECK(tm7b.status == 200);
    CHECK(tm7b.body.find("\"open\":4") != std::string::npos);
    CHECK(http(port, "POST", "/files/tasks/status", H("sub7"),
               "{\"id\":" + std::to_string(jint(t7far.body, "id")) +
                   ",\"status\":\"完成\"}")
              .status == 200);
    tm7b = http(port, "GET", "/files/tasks/team", H("lead7"), "");
    CHECK(tm7b.status == 200);
    CHECK(tm7b.body.find("\"open\":3") != std::string::npos);
    CHECK(tm7b.body.find("\"due_soon\":1") != std::string::npos); // 临期仍在
    // 跨级关闭：admin1 是 lead7 的直属上级（看得见 lead7 泳道），
    // 但 sub7 是跨级——lead7 泳道在、sub7 泳道与其任务不在
    const auto ta7 = http(port, "GET", "/files/tasks/team", H("admin1"), "");
    CHECK(ta7.status == 200);
    CHECK(ta7.body.find("\"account\":\"lead7\"") != std::string::npos);
    CHECK(ta7.body.find("\"account\":\"sub7\"") == std::string::npos);
    CHECK(ta7.body.find("泳道逾期") == std::string::npos);
    // org-admin 无兜底：admin2 有 admin 角色但无任何下属=空 team
    const auto tm7a = http(port, "GET", "/files/tasks/team", H("admin2"), "");
    CHECK(tm7a.status == 200);
    CHECK(tm7a.body.find("\"account\"") == std::string::npos);
    // 无下属账号（owner1）=200 空 team；平级（admin2 看不到 sub7）
    const auto tm7o = http(port, "GET", "/files/tasks/team", H("owner1"), "");
    CHECK(tm7o.status == 200);
    CHECK(tm7o.body.find("\"account\"") == std::string::npos);
    // 清场：汇报线拆除，不留到后续腿
    CHECK(store.reporting_clear("sub7"));
    CHECK(store.reporting_clear("lead7"));
  }

}

} // namespace memex::server::tests_api
