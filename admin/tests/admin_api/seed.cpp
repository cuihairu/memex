// LAY-009 拆分模块（造库种子数据）——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

int seed_db() {
  PHASE("seed-begin");
  tmp = std::filesystem::temp_directory_path() /
        ("memex-admin-test-" + std::to_string(::getpid()));
  std::filesystem::create_directories(tmp);
  db = (tmp / "memex.db").string();
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
    // 群组-1：群资料主题/目的（admin 台账同源回带腿）
    PHASE("seed-group-profile");
    CHECK(seed.group_set_profile(gid, "root", "季度盘点", "全量盘点迁移项"));
    // 群组-2：群记录导出留痕（定案③）——与检索/导出同表同口径，
    //   admin 后台可查（不新表；auditor 判权不变）
    {
      memex::server::AuditReadRow rec;
      rec.op_account = "m1";
      rec.action = "group_record_export";
      rec.filters = "{\"gid\":" + std::to_string(gid) +
                    ",\"from_ms\":1700000000000,\"to_ms\":1700000060000,"
                    "\"with_ai\":false}";
      rec.result_count = 7;
      rec.ts_ms = 1700000060000;
      CHECK(seed.add_audit_read(rec));
    }
    PHASE("seed-files");
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
    PHASE("seed-group-admin");
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
      // 任务类型腿：aud1 的一条引用「运维」类型的任务（删除引用拒证）
      CHECK(seed.task_create("aud1", "aud1", "盘点任务", "", 0,
                             1700000314000, "", "", "运维") > 0);
    }
    PHASE("seed-bizflow");
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
    PHASE("seed-branding");
    CHECK(seed.branding_set("Memex", "#1a73e8", "企业协作套件", 1700001100000) >
          0);
    CHECK(seed.branding_set_logo(png_bytes(1, 1), 1700001200000) > 0);
    CHECK(seed.branding_set_splash(png_bytes(2, 2), 1700001300000) > 0);
    CHECK(seed.emoji_add("m1", "ok", png_bytes(64, 64), 1700001400000) > 0);
    CHECK(seed.emoji_add("m1", "hi", png_bytes(64, 64), 1700001500000) > 0);
    const std::vector<unsigned char> av_bytes = png_bytes(32, 32);
    CHECK(seed.avatar_put("m1", 32, "image/png", av_bytes, 1700001600000));
    CHECK(seed.avatar_put("m1", 64, "image/png", av_bytes, 1700001600000));
    CHECK(seed.avatar_put("m1", 128, "image/png", av_bytes, 1700001600000));
    seed.close();
  }
  return 0;
}
