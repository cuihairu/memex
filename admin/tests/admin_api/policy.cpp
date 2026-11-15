// LAY-009 拆分模块（策略下发与留存审批面）——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

void leg_policy() {
  PHASE("leg-12");
  // ⑫ 策略下发：全局行写入＋列表回读
  {
    CHECK(http("POST", "/api/admin/policy",
               R"({"department_path":"","allow_anonymous":true,"allow_cross_state":false,"new_device_approval":true,"allow_cross_dept_file":true,"allow_forward_file":false})",
               root_cookie).code == 200);
    const auto l = http("GET", "/api/admin/policy", {}, root_cookie);
    CHECK(l.code == 200);
    const Json::Value row = jbody(l.text)["rows"][0];
    CHECK(row["allow_cross_state"].asBool() == false);
    CHECK(row["new_device_approval"].asBool() == true);
    CHECK(row["allow_forward_file"].asBool() == false);
  }

  PHASE("leg-13");
  // ⑬ 留存：配置 30 天；purge 双人审批红线（自审 409、缺理由 400、合规 200）
  {
    CHECK(http("POST", "/api/admin/retention",
               R"({"days":30,"department_path":""})", root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/retention",
               R"({"days":45,"department_path":""})", root_cookie).code == 409);
    // 缺理由 400
    CHECK(http("POST", "/api/admin/retention/purge",
               R"({"before_ms":1800000000000,"approved_by":"aud1"})", root_cookie).code == 400);
    // 自审 409（approved_by==purged_by=root）
    CHECK(http("POST", "/api/admin/retention/purge",
               R"({"before_ms":1800000000000,"reason":"r","approved_by":"root"})",
               root_cookie).code == 409);
    // 合规：before_ms 取早于 msg-1（1700000000000）的时刻——双人审批与
    // 台账落库走通、回执 id>0，且不清掉后续检索/导出腿要用的归档行
    const auto r = http("POST", "/api/admin/retention/purge",
                        R"({"before_ms":1600000000000,"reason":"过期清理","approved_by":"aud1","policy_days":30})",
                        root_cookie);
    CHECK(r.code == 200);
    CHECK(jbody(r.text)["id"].asInt64() > 0);
  }

}
