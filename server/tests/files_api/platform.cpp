// test_files_api 拆分模块（LAY-005）：本套件场景体自原文件逐字搬移；
// 共享状态经 Fx 夹具传入（见 common.hpp）。套件按原序由 main 调用。
#include "common.hpp"

namespace memex::server::tests_api {

void platform_suite(Fx& f) {
  auto H = [&f](const char* a) { return f.H(a); };
  auto& store = f.store;
  auto& io = f.io;
  auto& bare = f.bare;
  auto& port = f.port;
  auto& tok = f.tok;
  auto& gid2 = f.gid2;

  // —— 平台-1 数据过滤：Server Query → Authorization Filter → Allowed
  //     Records（单元面：冲突规则与单点裁决同源、理由与序保持；HTTP 面：
  //     群文件列表行级隐没非 normal 行）——
  {
    // 单元面：独立小规则集（deny 某行 / allow 主人 / 默认拒）
    memex::server::AuthorizationService faz;
    faz.add_rule(memex::server::RuleEffect::ExplicitDeny, "deny-f2",
                 [](const memex::server::AuthzQuery& q) {
                   return q.resource.find("/file:2") != std::string::npos;
                 });
    faz.add_rule(memex::server::RuleEffect::ExplicitAllow, "allow-owner",
                 [](const memex::server::AuthzQuery& q) {
                   return q.context == "owner";
                 });
    const auto hits = faz.filter_allowed(
        4, [](std::size_t i) {
          return memex::server::AuthzQuery{"alice", "file:read",
                                           "group:9/file:" +
                                               std::to_string(i + 1),
                                           "owner"};
        });
    // 行 2 被显式拒（Deny 压 Allow）；其余放行且理由=命中规则名、序保持
    CHECK(hits.size() == 3);
    CHECK(hits[0].index == 0 && hits[0].reason == "allow:allow-owner");
    CHECK(hits[1].index == 2 && hits[1].reason == "allow:allow-owner");
    CHECK(hits[2].index == 3 && hits[2].reason == "allow:allow-owner");
    // 全未命中=默认拒：context 不匹配 → 颗粒无存
    CHECK(faz.filter_allowed(2, [](std::size_t) {
            return memex::server::AuthzQuery{"alice", "file:read",
                                             "group:9/file:1", "other"};
          }).empty());
    // 单点 authorize 与过滤同源：同查询同结果
    CHECK(!faz.authorize({"alice", "file:read", "group:9/file:2", "owner"})
               .allowed);
    CHECK(faz.authorize({"alice", "file:read", "group:9/file:1", "owner"})
              .allowed);
  }
  // HTTP 面：gid2 传两行、隔离其一 → 群列表只余 normal 行（群主同滤——
  // 显式拒先于管理允许）；成员/群主同见
  {
    const std::string g2 = "group:" + std::to_string(gid2);
    const auto upa = http(port, "POST", "/files/upload?target=" + g2,
                          {{"Authorization", "Bearer " + tok["owner1"]},
                           {"X-File-Name", "keep.bin"}},
                          std::string("keep-bytes"));
    CHECK(upa.status == 200);
    const auto upb = http(port, "POST", "/files/upload?target=" + g2,
                          {{"Authorization", "Bearer " + tok["owner1"]},
                           {"X-File-Name", "gone.bin"}},
                          std::string("gone-bytes"));
    CHECK(upb.status == 200);
    const auto id_keep = jint(upa.body, "id");
    const auto id_gone = jint(upb.body, "id");
    CHECK(store.set_file_status(id_gone,
                                ServerStore::FileStatus::Quarantine));
    for (const char* who : {"owner1", "member1"}) {
      const auto ls = http(port, "GET", "/files/list?target=" + g2, H(who),
                           "");
      CHECK(ls.status == 200);
      CHECK(ls.body.find("\"id\":" + std::to_string(id_keep)) !=
            std::string::npos);
      // 隔离行连元数据一起隐没（群主也不例外的口径=file-not-normal 拒
      // 先于一切允许，冲突规则写死）
      CHECK(ls.body.find("\"id\":" + std::to_string(id_gone)) ==
            std::string::npos);
    }
  }

  // —— 平台-2 Identity 模型补全：Credential 独立判登、老库迁移、
  //     Session 台账（签发落行/登出落理由/令牌即刻失效）、IdentityBinding ——
  {
    // 凭据独立：create_account 双写 credentials（判登唯一来源）
    const auto cred = store.find_credential("owner1", "password");
    CHECK(cred.has_value());
    CHECK(!cred->disabled);
    CHECK(!cred->salt_hex.empty() && !cred->digest_hex.empty());
    CHECK(!store.find_credential("ghost", "password").has_value());
    // Session 台账：签发落行（scope=internal、进行中）→登出落理由→
    // 同令牌立即失效→再登出 401
    const auto lg = http(port, "POST", "/files/session", {},
                         "{\"account\":\"owner1\",\"password\":\"pw-owner1\"}");
    CHECK(lg.status == 200);
    const std::string lt = jstr(lg.body, "token");
    const auto Hl = std::map<std::string, std::string>{
        {"Authorization", "Bearer " + lt}};
    const auto open_rows = store.session_list("owner1");
    CHECK(!open_rows.empty());
    CHECK(open_rows.front().logged_out_ms == 0);
    CHECK(open_rows.front().logout_reason.empty());
    CHECK(open_rows.front().scope == "internal");
    CHECK(http(port, "POST", "/files/logout", Hl, "").status == 200);
    CHECK(http(port, "GET", "/files/list?target=me", Hl, "").status == 401);
    CHECK(http(port, "POST", "/files/logout", Hl, "").status == 401);
    const auto closed = store.session_list("owner1");
    CHECK(!closed.empty());
    CHECK(closed.front().logout_reason == "user_logout");
    CHECK(closed.front().logged_out_ms > 0);
    // 老库迁移：accounts 内联口令 → credentials（open 幂等迁、字段原样）
    const std::string legacy_db =
        "memex_test_legacy_" + std::to_string(::getpid()) + ".db";
    std::remove(legacy_db.c_str());
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open(legacy_db.c_str(), &raw) == SQLITE_OK);
    char* err = nullptr;
    sqlite3_exec(
        raw,
        "CREATE TABLE accounts (account TEXT PRIMARY KEY,"
        " display_name TEXT NOT NULL, salt TEXT NOT NULL,"
        " digest TEXT NOT NULL, role TEXT NOT NULL DEFAULT 'member',"
        " created_ms INTEGER NOT NULL);"
        "INSERT INTO accounts VALUES('legacy1','老号','saltyhex',"
        "'deadbeef','member',1700000000000);",
        nullptr, nullptr, &err);
    CHECK(err == nullptr);
    sqlite3_close(raw);
    memex::server::ServerStore lst;
    CHECK(lst.open(legacy_db)); // ensure_schema 跑迁移
    const auto lc = lst.find_credential("legacy1", "password");
    CHECK(lc.has_value());
    CHECK(lc->salt_hex == "saltyhex" && lc->digest_hex == "deadbeef");
    CHECK(lst.identity_list().empty());
    std::remove(legacy_db.c_str());
    // IdentityBinding（store 面）：幽灵账号拒；绑定→查→唯一→列→解绑→无
    CHECK(store.identity_bind("ghost", "corp-sso", "u1", 1) == 0);
    const auto bid = store.identity_bind("owner1", "corp-sso", "alice@corp", 2);
    CHECK(bid > 0);
    const auto bf = store.identity_find("corp-sso", "alice@corp");
    CHECK(bf.has_value());
    CHECK(bf->account == "owner1");
    CHECK(store.identity_bind("member1", "corp-sso", "alice@corp", 3) == 0);
    CHECK(store.identity_list("owner1").size() == 1);
    CHECK(store.identity_unbind(bid));
    CHECK(!store.identity_find("corp-sso", "alice@corp").has_value());
    CHECK(!store.identity_unbind(bid));

    // —— 平台-3 组织三关联：多部门归属／带时间窗追加授权／汇报线 ——
    // Membership：一人多部门；重复入拒；幽灵账号/幽灵部门拒；移除落净
    const int d1 = store.ensure_department_path("公司/研发部");
    const int d2 = store.ensure_department_path("公司/平台组");
    CHECK(d1 > 0 && d2 > 0);
    CHECK(!store.membership_add("ghost", d1, 1)); // 幽灵账号
    CHECK(!store.membership_add("owner1", 999999, 1)); // 幽灵部门
    CHECK(store.membership_add("owner1", d1, 1700000100000));
    CHECK(store.membership_add("owner1", d2, 1700000100001)); // 第二部门
    CHECK(!store.membership_add("owner1", d1, 2)); // 唯一约束（已入）
    const auto mems = store.memberships_of("owner1");
    CHECK(mems.size() == 2);
    CHECK(mems[0].department_id == d1 && mems[1].department_id == d2);
    CHECK(mems[0].created_ms == 1700000100000);
    CHECK(store.membership_remove("owner1", d1));
    CHECK(!store.membership_remove("owner1", d1)); // 再移=false
    CHECK(store.memberships_of("owner1").size() == 1);
    CHECK(store.membership_remove("owner1", d2)); // 清场

    // RoleAssignment：基础角色（accounts.role）∪ 窗内授权；0 端点=不开窗
    CHECK(store.role_grant("ghost", "admin", "", 0, 0, "root", 1) == 0);
    CHECK(store.role_grant("owner1", "", "", 0, 0, "root", 1) == 0);
    CHECK(store.role_grant("owner1", "admin", "", 200, 100, "root", 1) ==
          0); // 时间窗倒置
    const std::int64_t now3 = 1700000200000;
    const auto base_row3 = store.find_account("owner1");
    CHECK(base_row3.has_value());
    const std::string base3 = base_row3->role; // 基础角色（accounts.role）
    const auto rid =
        store.role_grant("owner1", "admin", "", 0, 0, "admin1", 1);
    CHECK(rid > 0); // 无窗=恒生效
    CHECK(store.effective_roles("owner1", now3).size() >= 1);
    {
      const auto er = store.effective_roles("owner1", now3);
      CHECK(std::find(er.begin(), er.end(), "admin") != er.end());
      CHECK(std::find(er.begin(), er.end(), base3) != er.end()); // 基础仍在
    }
    const auto rid2 =
        store.role_grant("owner1", "auditor", "公司/研发部", now3 + 60000,
                         now3 + 120000, "admin1", 2); // 临时代理=时间窗
    CHECK(rid2 > 0);
    {
      const auto before = store.effective_roles("owner1", now3);
      CHECK(std::find(before.begin(), before.end(), "auditor") ==
            before.end()); // 未生效不含
      const auto in_win =
          store.effective_roles("owner1", now3 + 60000);
      CHECK(std::find(in_win.begin(), in_win.end(), "auditor") !=
            in_win.end()); // 生效含（from<=at）
      const auto last_ms =
          store.effective_roles("owner1", now3 + 119999);
      CHECK(std::find(last_ms.begin(), last_ms.end(), "auditor") !=
            last_ms.end());
      const auto expired = store.effective_roles("owner1", now3 + 120000);
      CHECK(std::find(expired.begin(), expired.end(), "auditor") ==
            expired.end()); // 过期不含（until 闭开区间）
    }
    const auto ledger = store.role_assignments("owner1");
    CHECK(ledger.size() == 2); // 台账含过期行（不随窗蒸发）
    CHECK(ledger[1].role == "auditor" && ledger[1].scope == "公司/研发部");
    CHECK(ledger[1].granted_by == "admin1");
    CHECK(store.role_revoke(rid2));
    CHECK(!store.role_revoke(rid2)); // 再撤=false
    {
      const auto after = store.effective_roles("owner1", now3 + 60000);
      CHECK(std::find(after.begin(), after.end(), "auditor") == after.end());
    }
    // 可见性 admin 判定走有效角色：member 授 admin 即得管理员视野
    CHECK(store.set_visibility("member", "outsider", true, false, ""));
    {
      const auto as_member = store.visible_members("member1");
      bool sees = false;
      for (const auto& m : as_member)
        if (m.account == "outsider") sees = true;
      CHECK(!sees); // 隐藏行对普通成员隐没
    }
    const auto mid =
        store.role_grant("member1", "admin", "", 0, 0, "admin1", 3);
    CHECK(mid > 0);
    {
      const auto as_admin = store.visible_members("member1");
      bool sees = false;
      for (const auto& m : as_admin)
        if (m.account == "outsider") sees = true;
      CHECK(sees); // 追加授权即得管理员视野
    }
    CHECK(store.role_revoke(mid));
    {
      const auto as_member2 = store.visible_members("member1");
      bool sees = false;
      for (const auto& m : as_member2)
        if (m.account == "outsider") sees = true;
      CHECK(!sees); // 撤销即收回
    }
    CHECK(store.set_visibility("member", "outsider", false, false, "")); // 清场

    // set_member_profile 双写：dept→memberships replace-set＋镜像列；
    // manager→reporting 落行＋镜像列（单值视图语义不变）
    CHECK(store.set_member_profile("member1", d1, "工程师", "admin1"));
    {
      const auto ms = store.memberships_of("member1");
      CHECK(ms.size() == 1 && ms[0].department_id == d1); // 权威表落行
      const auto p = store.member_profile("member1");
      CHECK(p.has_value());
      CHECK(p->department_path == "公司/研发部"); // 镜像列可读
      CHECK(p->title == "工程师" && p->manager == "admin1");
    }
    CHECK(store.manager_chain("member1").front() == "admin1");
    CHECK(store.set_member_profile("member1", -1, "", "")); // 清场=replace-set 空
    CHECK(store.memberships_of("member1").empty());
    CHECK(store.set_member_profile("owner1", -1, "", "")); // 建档（镜像落点）

    // ReportingLine：自为上级/幽灵/成环拒；权威表落行＋镜像同步；
    // 链路沿新表逐级上溯
    CHECK(!store.reporting_set("ghost", "member1", 1));
    CHECK(!store.reporting_set("owner1", "ghost", 1));
    CHECK(!store.reporting_set("owner1", "owner1", 1)); // 自为上级
    CHECK(store.reporting_set("owner1", "member1", 1700000300000));
    CHECK(!store.reporting_set("member1", "owner1", 1)); // 会成环
    {
      const auto p = store.member_profile("owner1");
      CHECK(p.has_value());
      CHECK(p->manager == "member1"); // 镜像列同步
    }
    CHECK(store.manager_chain("owner1").front() == "member1");
    CHECK(store.reporting_set("member1", "admin1", 1700000300001));
    const auto chain3 = store.manager_chain("owner1");
    CHECK(chain3.size() == 2); // member1 → admin1
    CHECK(chain3[0] == "member1" && chain3[1] == "admin1");
    CHECK(store.reporting_clear("member1"));
    CHECK(store.manager_chain("owner1").size() == 1);
    {
      const auto p = store.member_profile("owner1");
      CHECK(p.has_value() && p->manager == "member1");
    }
    CHECK(store.reporting_clear("owner1")); // 清场
    {
      const auto p = store.member_profile("owner1");
      CHECK(p.has_value() && p->manager.empty()); // 镜像列同步清除
    }
    CHECK(store.manager_chain("owner1").empty());

    // 老库迁移（幂等）：member_profiles 单列部门/上级 → 三关联权威表
    const std::string legacy_db3 =
        "memex_test_legacy3_" + std::to_string(::getpid()) + ".db";
    std::remove(legacy_db3.c_str());
    sqlite3* raw3 = nullptr;
    CHECK(sqlite3_open(legacy_db3.c_str(), &raw3) == SQLITE_OK);
    char* err3 = nullptr;
    sqlite3_exec(
        raw3,
        "CREATE TABLE accounts (account TEXT PRIMARY KEY,"
        " display_name TEXT NOT NULL, salt TEXT NOT NULL,"
        " digest TEXT NOT NULL, role TEXT NOT NULL DEFAULT 'member',"
        " created_ms INTEGER NOT NULL);"
        "CREATE TABLE member_profiles (account TEXT PRIMARY KEY,"
        " department_id INTEGER, title TEXT NOT NULL DEFAULT '',"
        " manager TEXT NOT NULL DEFAULT '', updated_ms INTEGER NOT NULL);"
        "INSERT INTO accounts VALUES('old1','旧号','s','d','member',"
        "1700000000000);"
        "INSERT INTO accounts VALUES('old2','旧号2','s','d','member',"
        "1700000000000);"
        "INSERT INTO member_profiles VALUES('old1',7,'工程师','old2',"
        "1700000001000);",
        nullptr, nullptr, &err3);
    CHECK(err3 == nullptr);
    sqlite3_close(raw3);
    {
      memex::server::ServerStore lst3;
      CHECK(lst3.open(legacy_db3)); // ensure_schema 迁移单值列进三关联
      const auto migs = lst3.memberships_of("old1");
      CHECK(migs.size() == 1 && migs[0].department_id == 7);
      CHECK(lst3.manager_chain("old1").size() == 1);
      // 迁移不镜改原行（单值视图仍可读）
      CHECK(lst3.role_assignments().empty()); // 授权台账不动老数据
    }
    std::remove(legacy_db3.c_str());
  }

  // —— 存储未配置：面在、字节面 503、元数据面照常 ——
  {
    bare = std::make_unique<memex::server::FileServer>(io, store, nullptr, 0);
    const std::uint16_t bport = bare->port();
    bare->start_accept();
    const auto bt =
        http(bport, "POST", "/files/session", {},
             "{\"account\":\"member1\",\"password\":\"pw-member1\"}");
    CHECK(bt.status == 200);
    const auto btok = jstr(bt.body, "token");
    CHECK(!btok.empty());
    CHECK(http(bport, "POST", "/files/upload?target=me",
               {{"Authorization", "Bearer " + btok}, {"X-File-Name", "x.txt"}},
               "abc").status == 503);
    // 元数据面（列表）不依赖存储
    CHECK(http(bport, "GET", "/files/list?target=me",
               {{"Authorization", "Bearer " + btok}}, "").status == 200);
  }

  // —— R27-1 个人任务清单：自建/分配（权限模型「谁能分配」判权）/
  //     完成（归清单主人）/提醒回执/撤回（主人或分配人）——
  {
    // 未登录 401
    CHECK(http(port, "GET", "/files/tasks", {}, "").status == 401);
    // 自建 200；空标题 400
    CHECK(http(port, "POST", "/files/tasks", H("owner1"),
               "{\"title\":\"给自己排个活\",\"note\":\"备注\"}").status == 200);
    CHECK(http(port, "POST", "/files/tasks", H("owner1"), "{\"title\":\"\"}")
              .status == 400);
    // 无关系/幽灵分配默认拒（task-assign 未命中 → default-deny；判权
    // 先于存在性检查——不向调用方泄露账号存在性）
    const auto td = http(port, "POST", "/files/tasks", H("member1"),
                         "{\"title\":\"x\",\"assignee\":\"outsider\"}");
    CHECK(td.status == 403);
    CHECK(jstr(td.body, "error").find("default-deny") != std::string::npos);
    CHECK(http(port, "POST", "/files/tasks", H("owner1"),
               "{\"title\":\"x\",\"assignee\":\"ghost\"}").status == 403);
    // 同群分配放行（owner1/member1 同在 gid 群——现查现裁）
    const auto ta = http(port, "POST", "/files/tasks", H("owner1"),
                         "{\"title\":\"帮我看下机器\",\"assignee\":\"member1\"}");
    CHECK(ta.status == 200);
    const std::int64_t tid = jint(ta.body, "id");
    // 同部门分配放行（department_path 相同且非空）
    const int cap_dept = store.ensure_department_path("公司/任务面");
    CHECK(cap_dept > 0);
    CHECK(store.set_member_profile("admin1", cap_dept, "工程师", ""));
    CHECK(store.set_member_profile("outsider", cap_dept, "工程师", ""));
    CHECK(http(port, "POST", "/files/tasks", H("admin1"),
               "{\"title\":\"同部门可派\",\"assignee\":\"outsider\"}")
              .status == 200);
    // 完成/提醒归清单主人：member1 勾完成与回执 200；分配人 owner1 403
    CHECK(http(port, "POST", "/files/tasks/done", H("member1"),
               "{\"id\":" + std::to_string(tid) + ",\"done\":true}")
              .status == 200);
    CHECK(http(port, "POST", "/files/tasks/done", H("owner1"),
               "{\"id\":" + std::to_string(tid) + ",\"done\":true}")
              .status == 403);
    CHECK(http(port, "POST", "/files/tasks/reminded", H("member1"),
               "{\"id\":" + std::to_string(tid) + "}").status == 200);
    CHECK(http(port, "POST", "/files/tasks/reminded", H("owner1"),
               "{\"id\":" + std::to_string(tid) + "}").status == 403);
    const auto trow = store.task_by_id(tid);
    CHECK(trow.has_value() && trow->done && trow->reminded_ms > 0);
    // 列表：主人清单带分派来源（creator），分配人见 assigned_by_me
    const auto lt = http(port, "GET", "/files/tasks", H("member1"), "");
    CHECK(lt.status == 200);
    CHECK(lt.body.find("帮我看下机器") != std::string::npos);
    CHECK(lt.body.find("\"creator\":\"owner1\"") != std::string::npos);
    const auto la = http(port, "GET", "/files/tasks", H("owner1"), "");
    CHECK(la.status == 200);
    CHECK(la.body.find("assigned_by_me") != std::string::npos);
    CHECK(la.body.find("帮我看下机器") != std::string::npos);
    // 撤回：无关第三者 403；分配人可撤自己派的；删后再删 404
    CHECK(http(port, "POST", "/files/tasks/delete", H("admin1"),
               "{\"id\":" + std::to_string(tid) + "}").status == 403);
    CHECK(http(port, "POST", "/files/tasks/delete", H("owner1"),
               "{\"id\":" + std::to_string(tid) + "}").status == 200);
    CHECK(http(port, "POST", "/files/tasks/delete", H("owner1"),
               "{\"id\":" + std::to_string(tid) + "}").status == 404);
  }

}

} // namespace memex::server::tests_api
