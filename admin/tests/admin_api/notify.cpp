// LAY-009 拆分模块（通知与集成面（webhook/bot/网关/偏好盘点））——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

void leg_notify() {
  PHASE("leg-20");
  // ⑳ 通知与集成·webhook：create token 仅此一次（明文不再出现在任何
  //    list 应答）、目标校验（不存在 404）、member 越权 403、吊销二次 404
  std::string whk_token;
  {
    // 目标不存在 → 404（与 CLI create 建即校验同口径）
    CHECK(http("POST", "/api/admin/webhooks",
               R"({"target":"nosuch"})", root_cookie).code == 404);
    // 缺 target → 400
    CHECK(http("POST", "/api/admin/webhooks", "{}", root_cookie).code == 400);
    const auto r1 = http("POST", "/api/admin/webhooks",
                         R"({"target":"m1","name":"个人hook"})", root_cookie);
    CHECK(r1.code == 200);
    whk_token = jbody(r1.text)["token"].asString();
    CHECK(whk_token.rfind("whk_", 0) == 0);
    CHECK(jbody(r1.text)["id"].asInt64() > 0);
    Json::Value grp;
    grp["target"] = "group:" + std::to_string(gid);
    Json::StreamWriterBuilder wbg;
    const auto r2 = http("POST", "/api/admin/webhooks",
                         Json::writeString(wbg, grp), root_cookie);
    CHECK(r2.code == 200);
    // list：摘要前缀 12 位在台账，明文 token 不再出现在任何应答
    const auto l = http("GET", "/api/admin/webhooks", {}, root_cookie);
    CHECK(l.code == 200);
    CHECK(l.text.find(whk_token) == std::string::npos);
    const Json::Value hooks = jbody(l.text)["webhooks"];
    CHECK(hooks.size() == 2);
    CHECK(hooks[0]["token_prefix"].asString().size() == 12);
    // member 越权：读写都 403（org-admin 面口径）
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(http("GET", "/api/admin/webhooks", {}, lr.set_cookie).code == 403);
    CHECK(http("POST", "/api/admin/webhooks",
               R"({"target":"m2"})", lr.set_cookie).code == 403);
    // 吊销：一次成功，二次 404（行已吊销，与 roles/revoke 同口径）
    Json::Value rev;
    rev["id"] = hooks[0]["id"];
    Json::StreamWriterBuilder wb;
    CHECK(http("POST", "/api/admin/webhooks/revoke",
               Json::writeString(wb, rev), root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/webhooks/revoke",
               Json::writeString(wb, rev), root_cookie).code == 404);
  }

  PHASE("leg-21");
  // ㉑ bot：add token 仅此一次、':' 名 400、重名 409、禁用回读、移除二次 404
  {
    CHECK(http("POST", "/api/admin/bots", "{}", root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/bots", R"({"name":"a:b"})",
               root_cookie).code == 400);
    const auto r = http("POST", "/api/admin/bots",
                        R"({"name":"testbot"})", root_cookie);
    CHECK(r.code == 200);
    const std::string bot_token = jbody(r.text)["token"].asString();
    CHECK(bot_token.rfind("bot_", 0) == 0);
    // 重名 409
    CHECK(http("POST", "/api/admin/bots", R"({"name":"testbot"})",
               root_cookie).code == 409);
    const auto l = http("GET", "/api/admin/bots", {}, root_cookie);
    CHECK(l.code == 200);
    CHECK(l.text.find(bot_token) == std::string::npos); // 明文不再出现
    const Json::Value bots = jbody(l.text)["bots"];
    CHECK(bots.size() == 1);
    CHECK(bots[0]["full_name"].asString() == "bot:testbot");
    CHECK(bots[0]["created_by"].asString() == "root"); // 登记者=会话身份留痕
    CHECK(bots[0]["token_prefix"].asString().size() == 12);
    // 禁用→回读 disabled=true→启用
    CHECK(http("POST", "/api/admin/bots/disabled",
               R"({"name":"testbot","disabled":true})", root_cookie).code == 200);
    const Json::Value after =
        jbody(http("GET", "/api/admin/bots", {}, root_cookie).text)["bots"];
    CHECK(after[0]["disabled"].asBool() == true);
    CHECK(http("POST", "/api/admin/bots/disabled",
               R"({"name":"testbot","disabled":false})", root_cookie).code == 200);
    // 移除（连带退群/白名单清理在 store 层）；再移除 404
    CHECK(http("POST", "/api/admin/bots/remove",
               R"({"name":"testbot"})", root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/bots/remove",
               R"({"name":"testbot"})", root_cookie).code == 404);
  }

  PHASE("leg-22");
  // ㉒ 模型网关端点：api_key 只写不回显（list 应答无原文）、http:// 与
  //    ':' 名校验、重名 409、停用回读、移除二次 404
  {
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"local-ep","base_url":"http://127.0.0.1:9100","api_key":"sk-secret-123","model":"qwen-local","is_local":true})",
               root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"ext-ep","base_url":"http://10.0.0.9:8000/v1"})",
               root_cookie).code == 200);
    // 重名 409；非 http:// 400；':' 名 400；缺 base_url 400
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"local-ep","base_url":"http://x:1"})",
               root_cookie).code == 409);
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"bad","base_url":"https://x:1"})",
               root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"a:b","base_url":"http://x:1"})",
               root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/model/endpoints",
               R"({"name":"nohost"})", root_cookie).code == 400);
    const auto l = http("GET", "/api/admin/model/endpoints", {}, root_cookie);
    CHECK(l.code == 200);
    CHECK(l.text.find("sk-secret-123") == std::string::npos); // 密钥不回显
    const Json::Value eps = jbody(l.text)["endpoints"];
    CHECK(eps.size() == 2);
    CHECK(eps[0]["api_key_set"].asBool() == true);  // 只出有无布尔
    CHECK(eps[1]["api_key_set"].asBool() == false); // 未配 key 端点
    CHECK(eps[0]["is_local"].asBool() == true);
    CHECK(eps[0]["created_by"].asString() == "root");
    // 停用→回读 enabled=false
    CHECK(http("POST", "/api/admin/model/endpoints/enabled",
               R"({"name":"ext-ep","enabled":false})", root_cookie).code == 200);
    const Json::Value after =
        jbody(http("GET", "/api/admin/model/endpoints", {}, root_cookie)
                  .text)["endpoints"];
    bool ext_disabled = false;
    for (const auto& e : after) {
      if (e["name"].asString() == "ext-ep")
        ext_disabled = !e["enabled"].asBool();
    }
    CHECK(ext_disabled);
    // 移除与二次移除
    CHECK(http("POST", "/api/admin/model/endpoints/remove",
               R"({"name":"ext-ep"})", root_cookie).code == 200);
    CHECK(http("POST", "/api/admin/model/endpoints/remove",
               R"({"name":"ext-ep"})", root_cookie).code == 404);
    // 调用审计（只读）：空台账 200
    CHECK(http("GET", "/api/admin/model/calls", {}, root_cookie).code == 200);
  }

  PHASE("leg-23");
  // ㉓ 三级通知偏好只读盘点：全局计数对账＋群/个人分档（群名解析）＋
  //    member 越权 403；纯读不改（台账现值聚合，无写入口）
  {
    const auto r = http("GET", "/api/admin/notify-overview", {}, root_cookie);
    CHECK(r.code == 200);
    const Json::Value ov = jbody(r.text);
    CHECK(ov["global"]["webhooks_total"].asInt64() == 2);  // 吊销行也在台账
    CHECK(ov["global"]["webhooks_active"].asInt64() == 1); // 一行已吊销
    CHECK(ov["global"]["bots_total"].asInt64() == 0);      // ㉑ 已移除
    CHECK(ov["global"]["endpoints_total"].asInt64() == 1); // ㉒ 移除 ext-ep
    CHECK(ov["global"]["endpoints_local"].asInt64() == 1);
    CHECK(ov["group_scope"].size() == 1);
    CHECK(ov["group_scope"][0]["target"].asString() ==
          "group:" + std::to_string(gid));
    CHECK(ov["group_scope"][0]["group_name"].asString() == "盘点群");
    CHECK(ov["group_scope"][0]["revoked"].asBool() == false);
    CHECK(ov["personal_scope"].size() == 1);
    CHECK(ov["personal_scope"][0]["target"].asString() == "m1");
    CHECK(ov["personal_scope"][0]["revoked"].asBool() == true);
    // note 如实口径（客户端本机偏好不集中存储）
    CHECK(std::string(ov["note"].asString()).find("只读") != std::string::npos);
    const auto lr = http("POST", "/api/login",
                         R"({"account":"m1","password":"m1-pw"})");
    CHECK(http("GET", "/api/admin/notify-overview", {}, lr.set_cookie).code ==
          403);
  }

}
