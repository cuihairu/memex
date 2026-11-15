// test_files_api 拆分模块（LAY-005）：本套件场景体自原文件逐字搬移；
// 共享状态经 Fx 夹具传入（见 common.hpp）。套件按原序由 main 调用。
#include "common.hpp"

namespace memex::server::tests_api {

void tasks_suite(Fx& f) {
  auto H = [&f](const char* a) { return f.H(a); };
  auto& store = f.store;
  auto& port = f.port;

  // —— R27-6 任务类型/开始时间/状态（看板列语义）：类型清单（服务端
  //     配置）/建任务带 type+start_ms/未知类型 400/状态切换与 done 双写/
  //     开始到点提醒回执（独立位不与 deadline 互抢）/旧库 status 回填 ——
  {
    // 类型清单：初始四类、默认「任务」；未登录 401
    CHECK(http(port, "GET", "/files/task-types", {}, "").status == 401);
    const auto tt = http(port, "GET", "/files/task-types", H("member1"), "");
    CHECK(tt.status == 200);
    CHECK(tt.body.find("任务") != std::string::npos &&
          tt.body.find("需求") != std::string::npos &&
          tt.body.find("缺陷") != std::string::npos &&
          tt.body.find("其他") != std::string::npos);
    CHECK(tt.body.find("\"is_default\":true") != std::string::npos);
    // 建任务带 type+start_ms；未知类型 400（清单外不落库）
    const auto t6 = http(port, "POST", "/files/tasks", H("member1"),
                         "{\"title\":\"改造网关\",\"type\":\"需求\","
                         "\"start_ms\":1700001000000}");
    CHECK(t6.status == 200);
    const std::int64_t tid6 = jint(t6.body, "id");
    CHECK(http(port, "POST", "/files/tasks", H("member1"),
               "{\"title\":\"x\",\"type\":\"史诗\"}").status == 400);
    // 列表回读新字段
    const auto l6 = http(port, "GET", "/files/tasks", H("member1"), "");
    CHECK(l6.status == 200);
    CHECK(l6.body.find("\"type\":\"需求\"") != std::string::npos);
    CHECK(l6.body.find("\"status\":\"待办\"") != std::string::npos);
    CHECK(l6.body.find("\"start_ms\":1700001000000") != std::string::npos);
    // 状态切换判权：非清单主人 403；主人 待办→进行中（done 不动）
    CHECK(http(port, "POST", "/files/tasks/status", H("owner1"),
               "{\"id\":" + std::to_string(tid6) + ",\"status\":\"进行中\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/tasks/status", H("member1"),
               "{\"id\":" + std::to_string(tid6) + ",\"status\":\"进行中\"}")
              .status == 200);
    auto r6 = store.task_by_id(tid6);
    CHECK(r6.has_value() && r6->status == "进行中" && !r6->done);
    // 非法枚举 400（库层枚举校验；路由先拦）
    CHECK(http(port, "POST", "/files/tasks/status", H("member1"),
               "{\"id\":" + std::to_string(tid6) + ",\"status\":\"挂起\"}")
              .status == 400);
    // 进行中→完成：status 与 done 双写（done_ms 落笔）
    CHECK(http(port, "POST", "/files/tasks/status", H("member1"),
               "{\"id\":" + std::to_string(tid6) + ",\"status\":\"完成\"}")
              .status == 200);
    r6 = store.task_by_id(tid6);
    CHECK(r6.has_value() && r6->status == "完成" && r6->done &&
          r6->done_ms > 0);
    // 回退走 status：完成→待办（done 同步归零、done_ms 清零）
    CHECK(http(port, "POST", "/files/tasks/status", H("member1"),
               "{\"id\":" + std::to_string(tid6) + ",\"status\":\"待办\"}")
              .status == 200);
    r6 = store.task_by_id(tid6);
    CHECK(r6.has_value() && r6->status == "待办" && !r6->done &&
          r6->done_ms == 0);
    // 旧 done 路由仍活（双写另一方向）：勾完成→status=完成
    CHECK(http(port, "POST", "/files/tasks/done", H("member1"),
               "{\"id\":" + std::to_string(tid6) + ",\"done\":true}")
              .status == 200);
    r6 = store.task_by_id(tid6);
    CHECK(r6.has_value() && r6->status == "完成" && r6->done);
    // 开始到点提醒回执（kind=start）：独立落位，deadline 回执位不动；
    // 缺省 kind=deadline 口径照旧
    CHECK(http(port, "POST", "/files/tasks/reminded", H("member1"),
               "{\"id\":" + std::to_string(tid6) + ",\"kind\":\"start\"}")
              .status == 200);
    r6 = store.task_by_id(tid6);
    CHECK(r6.has_value() && r6->start_reminded_ms > 0 &&
          r6->reminded_ms == 0);
    CHECK(http(port, "POST", "/files/tasks/reminded", H("member1"),
               "{\"id\":" + std::to_string(tid6) + "}").status == 200);
    r6 = store.task_by_id(tid6);
    CHECK(r6.has_value() && r6->reminded_ms > 0);
    // 旧库回填：裸 sqlite 建 R27-1 形状任务表（无新列）→open 迁移→
    // status 按 done 回填（done=1→完成、done=0→待办）、type 落默认
    const std::string legacy_db6 =
        "memex_test_legacy6_" + std::to_string(::getpid()) + ".db";
    std::remove(legacy_db6.c_str());
    sqlite3* raw6 = nullptr;
    CHECK(sqlite3_open(legacy_db6.c_str(), &raw6) == SQLITE_OK);
    char* err6 = nullptr;
    sqlite3_exec(
        raw6,
        "CREATE TABLE accounts (account TEXT PRIMARY KEY,"
        " display_name TEXT NOT NULL, salt TEXT NOT NULL,"
        " digest TEXT NOT NULL, role TEXT NOT NULL DEFAULT 'member',"
        " created_ms INTEGER NOT NULL);"
        "CREATE TABLE tasks (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " owner TEXT NOT NULL, creator TEXT NOT NULL, title TEXT NOT NULL,"
        " note TEXT NOT NULL DEFAULT '', due_ms INTEGER NOT NULL DEFAULT 0,"
        " reminded_ms INTEGER NOT NULL DEFAULT 0,"
        " done INTEGER NOT NULL DEFAULT 0,"
        " done_ms INTEGER NOT NULL DEFAULT 0,"
        " created_ms INTEGER NOT NULL,"
        " provider TEXT NOT NULL DEFAULT '',"
        " ext_key TEXT NOT NULL DEFAULT '');"
        "INSERT INTO tasks(owner, creator, title, done, done_ms, created_ms)"
        " VALUES('legacy1','legacy1','老任务一',1,1700000900000,"
        "1700000800000);"
        "INSERT INTO tasks(owner, creator, title, done, created_ms)"
        " VALUES('legacy1','legacy1','老任务二',0,1700000800000);",
        nullptr, nullptr, &err6);
    CHECK(err6 == nullptr);
    sqlite3_close(raw6);
    memex::server::ServerStore lst6;
    CHECK(lst6.open(legacy_db6)); // ensure_schema 跑迁移＋回填
    const auto old1 = lst6.task_by_id(1);
    CHECK(old1.has_value() && old1->status == "完成" && old1->done &&
          old1->type == "任务" && old1->start_ms == 0);
    const auto old2 = lst6.task_by_id(2);
    CHECK(old2.has_value() && old2->status == "待办" && !old2->done);
    CHECK(lst6.task_types().size() == 4); // 初始四类随 open 播种
    std::remove(legacy_db6.c_str());
  }

  // —— 二期·审批（请假起步）：固定模板白名单/审批人=直属上级（无上级
  //     org-admin 兜底）/四态流转/判权先于状态/全程留痕 ——
  {
    // 未登录 401
    CHECK(http(port, "GET", "/files/approvals", {}, "").status == 401);
    // 白名单外类型拒；合法类型 200
    CHECK(http(port, "POST", "/files/approvals", H("member1"),
               "{\"type\":\"探亲\"}").status == 400);
    const auto a1 = http(port, "POST", "/files/approvals", H("member1"),
                         "{\"type\":\"年假\",\"from\":\"2026-10-12\","
                         "\"to\":\"2026-10-14\",\"reason\":\"家里有事\"}");
    CHECK(a1.status == 200);
    const std::int64_t aid = jint(a1.body, "id");
    // 审批人=直属上级：reporting_set member1→owner1；owner1 待决可见，
    // 无关者 admin1 列表不见该行（判权不过的行不出现=不泄露存在性）
    CHECK(store.reporting_set("member1", "owner1", 1));
    const auto lp = http(port, "GET", "/files/approvals", H("owner1"), "");
    CHECK(lp.status == 200);
    CHECK(lp.body.find("\"applicant\":\"member1\"") != std::string::npos);
    const auto ln = http(port, "GET", "/files/approvals", H("admin1"), "");
    CHECK(ln.body.find("\"applicant\":\"member1\"") == std::string::npos);
    // 判权先于状态检查：admin1 对存在申请同口径 403（与不存在者一致）
    CHECK(http(port, "POST", "/files/approvals/decide", H("admin1"),
               "{\"id\":" + std::to_string(aid) + ",\"approved\":true}")
              .status == 403);
    // 自建自审不成立：member1 决自己的申请 403（申请人≠审批人写死）
    CHECK(http(port, "POST", "/files/approvals/decide", H("member1"),
               "{\"id\":" + std::to_string(aid) + ",\"approved\":true}")
              .status == 403);
    // 直属上级批准（带意见）；重复决 409（只有 pending 可决）
    CHECK(http(port, "POST", "/files/approvals/decide", H("owner1"),
               "{\"id\":" + std::to_string(aid) +
                   ",\"approved\":true,\"note\":\"同意\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/approvals/decide", H("owner1"),
               "{\"id\":" + std::to_string(aid) + ",\"approved\":true}")
              .status == 409);
    const auto arow = store.approval_by_id(aid);
    CHECK(arow.has_value() && arow->status == "approved" &&
          arow->decider == "owner1" && arow->decision_note == "同意" &&
          arow->decided_ms > 0);
    // 撤回：申请人专属且仅 pending——已决申请撤回 409；pending 新申请
    // 他人撤 403、自己撤 200；撤后再决 409
    const auto a2 = http(port, "POST", "/files/approvals", H("member1"),
                         "{\"type\":\"调休\"}");
    CHECK(a2.status == 200);
    const std::int64_t a2id = jint(a2.body, "id");
    CHECK(http(port, "POST", "/files/approvals/withdraw", H("owner1"),
               "{\"id\":" + std::to_string(a2id) + "}").status == 403);
    CHECK(http(port, "POST", "/files/approvals/withdraw", H("member1"),
               "{\"id\":" + std::to_string(a2id) + "}").status == 200);
    CHECK(http(port, "POST", "/files/approvals/decide", H("owner1"),
               "{\"id\":" + std::to_string(a2id) + ",\"approved\":false}")
              .status == 409);
    CHECK(store.approval_by_id(a2id)->status == "withdrawn");
    // org-admin 兜底：无直属上级申请人 outsider → 授 admin1 org-admin
    // 后可见可决；有上级的申请 admin1 仍不可决（直属上级优先写死）
    const auto a3 = http(port, "POST", "/files/approvals", H("outsider"),
                         "{\"type\":\"病假\"}");
    CHECK(a3.status == 200);
    const std::int64_t a3id = jint(a3.body, "id");
    CHECK(store.role_grant("admin1", "org-admin", "", 0, 0, "admin1", 1) > 0);
    const auto lo = http(port, "GET", "/files/approvals", H("admin1"), "");
    CHECK(lo.body.find("\"applicant\":\"outsider\"") != std::string::npos);
    CHECK(http(port, "POST", "/files/approvals/decide", H("admin1"),
               "{\"id\":" + std::to_string(a3id) + ",\"approved\":true}")
              .status == 200);
    // member1 有直属上级（owner1），admin1 即便 org-admin 也只 403
    const auto a4 = http(port, "POST", "/files/approvals", H("member1"),
                         "{\"type\":\"事假\"}");
    CHECK(http(port, "POST", "/files/approvals/decide", H("admin1"),
               "{\"id\":" + std::to_string(jint(a4.body, "id")) +
                   ",\"approved\":true}")
              .status == 403);
    // 我申请的列表回带终态与审批人（留痕可对账）
    const auto lm = http(port, "GET", "/files/approvals", H("member1"), "");
    CHECK(lm.body.find("\"decider\":\"owner1\"") != std::string::npos);
    CHECK(lm.body.find("withdrawn") != std::string::npos);
  }

  // —— 二期·日报周报：个人日报台账（当日重复=upsert 更新）/直属上级
  //     可看下属（org-admin 不兜底）/团队聚合反查/转岗即时断权 ——
  {
    // 未登录 401；缺 content 400；坏 date 400（归属日须 YYYY-MM-DD）
    CHECK(http(port, "GET", "/files/reports", {}, "").status == 401);
    CHECK(http(port, "POST", "/files/reports", H("member1"),
               "{\"date\":\"2026-10-08\"}").status == 400);
    CHECK(http(port, "POST", "/files/reports", H("member1"),
               "{\"date\":\"10月8日\",\"content\":\"x\"}").status == 400);
    // 写＋当日重复提交=更新（同 id、content 覆盖、created_ms 不动）
    const auto r1 = http(port, "POST", "/files/reports", H("member1"),
                         "{\"date\":\"2026-10-08\",\"content\":\"v1\"}");
    CHECK(r1.status == 200);
    const std::int64_t rid = jint(r1.body, "id");
    CHECK(rid > 0);
    const auto r2 = http(port, "POST", "/files/reports", H("member1"),
                         "{\"date\":\"2026-10-08\",\"content\":\"v2\"}");
    CHECK(r2.status == 200 && jint(r2.body, "id") == rid);
    const auto mrows = store.reports_of("member1");
    CHECK(mrows.size() == 1 && mrows[0].content == "v2" &&
          mrows[0].created_ms > 0 && mrows[0].updated_ms >= mrows[0].created_ms);
    // 自己看自己（列表回带）
    const auto mine = http(port, "GET", "/files/reports", H("member1"), "");
    CHECK(mine.status == 200);
    CHECK(mine.body.find("\"date\":\"2026-10-08\"") != std::string::npos);
    CHECK(mine.body.find("v2") != std::string::npos);
    // 直属上级可看下属（审批段已设 member1→owner1 汇报线）
    const auto sub = http(port, "GET",
                          "/files/reports/read?author=member1",
                          H("owner1"), "");
    CHECK(sub.status == 200);
    CHECK(sub.body.find("v2") != std::string::npos);
    // org-admin 不兜底（设计口径日报只对直属上级开放；admin1 在审批段
    // 已授 org-admin）
    CHECK(http(port, "GET", "/files/reports/read?author=member1",
               H("admin1"), "").status == 403);
    // 无关系他人与幽灵账号同口径 403（不泄露存在性）；缺 author 400
    CHECK(http(port, "GET", "/files/reports/read?author=owner1",
               H("member1"), "").status == 403);
    CHECK(http(port, "GET", "/files/reports/read?author=ghost",
               H("owner1"), "").status == 403);
    CHECK(http(port, "GET", "/files/reports/read", H("owner1"), "").status ==
          400);
    // 团队聚合反查：owner1 的直接下属=member1（reports 随行回带）；
    // member1 无下属=空 team
    const auto team = http(port, "GET", "/files/reports/team", H("owner1"),
                           "");
    CHECK(team.status == 200);
    CHECK(team.body.find("\"author\":\"member1\"") != std::string::npos);
    const auto team_empty = http(port, "GET", "/files/reports/team",
                                 H("member1"), "");
    CHECK(team_empty.status == 200);
    CHECK(team_empty.body.find("\"author\":\"member1\"") == std::string::npos);
    // 转岗即时断权：汇报线清掉后直属上级立即看不了（现查现裁）
    CHECK(store.reporting_clear("member1"));
    CHECK(http(port, "GET", "/files/reports/read?author=member1",
               H("owner1"), "").status == 403);
  }

  // —— 二期·会话审计：在线查阅（auditor 持证，admin 不自动可读）＋
  //     查阅日志落库（谁/条件/命中几条）＋被拒尝试留痕（可对账）——
  {
    // 归档夹具：直插两条协作消息（T2.3 面；检索按关键词命中）
    const std::int64_t t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now()
                                   .time_since_epoch())
                               .count();
    CHECK(store.store_message("audit-m1", "member1", "owner1", 1,
                              "含审计暗号XYZ的正文", t0));
    CHECK(store.store_message("audit-m2", "owner1", "member1", 1,
                              "普通回复", t0 + 1));
    // 未登录 401
    CHECK(http(port, "POST", "/files/audit/search", {},
               "{\"keyword\":\"审计暗号\"}").status == 401);
    // 无 auditor 有效角色拒 403（被拒尝试已留痕——日志腿稍后对账）
    const auto denied = http(port, "POST", "/files/audit/search",
                             H("member1"), "{\"keyword\":\"审计暗号\"}");
    CHECK(denied.status == 403);
    CHECK(denied.body.find("被拒尝试已留痕") != std::string::npos);
    // 查阅日志同样持证：无角色查日志 403
    CHECK(http(port, "GET", "/files/audit/reads", H("member1"), "").status ==
          403);
    // 授 owner1 全局 auditor（时间窗多笔授权共存：平台段那笔已过期）
    CHECK(store.role_grant("owner1", "auditor", "", 0, 0, "owner1", 1) > 0);
    // 持证检索：关键词命中；admin/auditor 分立（member1 纵有 admin 也拒，
    // owner1 持 auditor 过）
    const auto hit = http(port, "POST", "/files/audit/search", H("owner1"),
                          "{\"keyword\":\"审计暗号\"}");
    CHECK(hit.status == 200);
    CHECK(hit.body.find("audit-m1") != std::string::npos);
    CHECK(hit.body.find("审计暗号XYZ") != std::string::npos);
    CHECK(hit.body.find("audit-m2") == std::string::npos); // 条件 AND 命中面
    // 空条件=全量；账号过滤收发双侧
    const auto all = http(port, "POST", "/files/audit/search", H("owner1"),
                          "{}");
    CHECK(all.status == 200);
    CHECK(all.body.find("audit-m1") != std::string::npos &&
          all.body.find("audit-m2") != std::string::npos);
    // 查阅日志（台账自阅）：member1 的被拒（audit.denied）与 owner1 的
    // 检索（audit.message.search）都在、含条件摘要与命中数
    const auto reads = http(port, "GET", "/files/audit/reads", H("owner1"),
                            "");
    CHECK(reads.status == 200);
    CHECK(reads.body.find("audit.denied") != std::string::npos);
    CHECK(reads.body.find("audit.message.search") != std::string::npos);
    CHECK(reads.body.find("账号=member1") == std::string::npos); // 被拒无账号条件
    CHECK(reads.body.find("关键词=审计暗号") != std::string::npos);
  }

}

} // namespace memex::server::tests_api
