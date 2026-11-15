// LAY-009 拆分模块（群组管理面（台账/能力/CI/服务器/密码箱/备忘录/投票/群任务））——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

void leg_groups() {
  PHASE("leg-29");
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
      seen = g["owner"].asString() == "root" && g["members"].size() == 2 &&
             g["topic"].asString() == "季度盘点" &&
             g["purpose"].asString() == "全量盘点迁移项";
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
}
