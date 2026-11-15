// LAY-009 拆分模块（归档检索与导出面（auditor 证））——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

void leg_archive() {
  PHASE("leg-14");
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

  PHASE("leg-15");
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
    // 群组-2 群记录导出留痕同表同口径（不新表；admin 后台可查）
    CHECK(reads.text.find("group_record_export") != std::string::npos);
  }

}
