// LAY-009 拆分模块（业务流程面（审批/日报/位置图/远程协助/任务备忘录））——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

void leg_bizflow() {
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
    PHASE("leg-10");
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
}
