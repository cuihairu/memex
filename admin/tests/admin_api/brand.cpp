// LAY-009 拆分模块（品牌与外观面（文本/素材/表情/头像））——场景体逐字搬移，公共面见 common.hpp。
#include "common.hpp"

void leg_brand() {
  // ㊸ 品牌与外观：品牌读回（文本三件＋素材有无/大小/版本）、文本保存
  //    （空串清空、accent 形态门、无可写字段 400、布尔形态门）、素材字节
  //    回读、上传（非 PNG 415／2049 宽 413／超 512KiB 413）、清除后字节
  //    面 404、表情包元数据清点（仅元数据无字节）、头像代读（三档/未配
  //    404/档位门 400/未知账号 404）；未登录 401、成员 403
  {
    const auto br = http("GET", "/api/admin/branding", {}, root_cookie);
    CHECK(br.code == 200);
    const auto jb = jbody(br.text);
    CHECK(jb["company_name"].asString() == "Memex");
    CHECK(jb["accent"].asString() == "#1a73e8");
    CHECK(jb["slogan"].asString() == "企业协作套件");
    CHECK(jb["has_logo"].asBool());
    CHECK(jb["has_splash"].asBool());
    CHECK(jb["logo_size"].asInt64() >= 24);
    CHECK(jb["version"].asInt64() > 0);
    // 素材字节回读（PNG magic 逐字对，长度与台账一致）
    const auto lg = http("GET", "/api/admin/branding/logo", {}, root_cookie);
    CHECK(lg.code == 200);
    CHECK(lg.text.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    CHECK(lg.text.size() == static_cast<std::size_t>(jb["logo_size"].asInt64()));
    const auto sp = http("GET", "/api/admin/branding/splash", {}, root_cookie);
    CHECK(sp.code == 200);
    CHECK(sp.text.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    // 文本保存：空串清标语（三件同提交，版本 bump 一次）
    const auto sv = http("POST", "/api/admin/branding",
                         R"({"company_name":"Memex","accent":"#1a73e8","slogan":""})",
                         root_cookie);
    CHECK(sv.code == 200);
    const std::int64_t v_after = jbody(sv.text)["version"].asInt64();
    CHECK(v_after > jb["version"].asInt64());
    const auto rb = http("GET", "/api/admin/branding", {}, root_cookie);
    CHECK(jbody(rb.text)["slogan"].asString().empty());
    CHECK(jbody(rb.text)["company_name"].asString() == "Memex");
    CHECK(jbody(rb.text)["version"].asInt64() == v_after);
    // 形态门与坏体
    CHECK(http("POST", "/api/admin/branding", R"({"accent":"1a73e8"})",
               root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/branding", R"({"accent":"#1a73e"})",
               root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/branding", R"({})", root_cookie).code == 400);
    CHECK(http("POST", "/api/admin/branding", "not json", root_cookie).code ==
          400);
    CHECK(http("POST", "/api/admin/branding", R"({"clear_logo":"true"})",
               root_cookie).code == 400);
    // 素材上传（原始字节体，content-type image/png）
    CHECK(http("POST", "/api/admin/branding/logo", test_png(4, 4), root_cookie,
               "image/png").code == 200);
    CHECK(http("POST", "/api/admin/branding/logo", "not a png", root_cookie,
               "image/png").code == 415);
    CHECK(http("POST", "/api/admin/branding/logo", test_png(2049, 1), root_cookie,
               "image/png").code == 413);
    CHECK(http("POST", "/api/admin/branding/logo", test_png(1, 1, 512 * 1024),
               root_cookie, "image/png").code == 413);
    CHECK(http("POST", "/api/admin/branding/splash", test_png(4, 4), root_cookie,
               "image/png").code == 200);
    // 清除（各 bump 一次）→ 字节面回 404
    const auto cl = http("POST", "/api/admin/branding", R"({"clear_logo":true})",
                         root_cookie);
    CHECK(cl.code == 200);
    const std::int64_t v_cl = jbody(cl.text)["version"].asInt64();
    CHECK(v_cl > v_after);
    CHECK(http("POST", "/api/admin/branding", R"({"clear_splash":true})",
               root_cookie).code == 200);
    const auto rb2 = http("GET", "/api/admin/branding", {}, root_cookie);
    CHECK(!jbody(rb2.text)["has_logo"].asBool());
    CHECK(!jbody(rb2.text)["has_splash"].asBool());
    CHECK(jbody(rb2.text)["logo_size"].asInt64() == 0);
    CHECK(http("GET", "/api/admin/branding/logo", {}, root_cookie).code ==
          404);
    CHECK(http("GET", "/api/admin/branding/splash", {}, root_cookie).code ==
          404);
    // 表情包元数据清点：ts 倒序、仅元数据（无字节/无内容键）
    const auto em = http("GET", "/api/admin/emoji?account=m1", {}, root_cookie);
    CHECK(em.code == 200);
    const auto je = jbody(em.text);
    CHECK(je["account"].asString() == "m1");
    CHECK(je["emoji"].size() == 2);
    CHECK(je["emoji"][0]["name"].asString() == "hi");
    CHECK(je["emoji"][1]["name"].asString() == "ok");
    CHECK(je["emoji"][0]["size"].asInt64() > 0);
    CHECK(je["emoji"][0]["ts_ms"].asInt64() > je["emoji"][1]["ts_ms"].asInt64());
    CHECK(!je["emoji"][0].isMember("bytes"));
    CHECK(!je["emoji"][0].isMember("content"));
    // 缺账号 400、未知账号 404
    CHECK(http("GET", "/api/admin/emoji", {}, root_cookie).code == 400);
    CHECK(http("GET", "/api/admin/emoji?account=ghost", {}, root_cookie).code ==
          404);
    // 头像代读（组织内互见口径）：三档可读、256 未配 404、档位门 400
    const auto av = http("GET", "/api/admin/avatar?account=m1&size=32", {},
                         root_cookie);
    CHECK(av.code == 200);
    CHECK(av.text.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=64", {}, root_cookie).code ==
          200);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=128", {}, root_cookie)
              .code == 200);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=256", {}, root_cookie)
              .code == 404);
    CHECK(http("GET", "/api/admin/avatar?account=m1", {}, root_cookie).code ==
          400);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=99", {}, root_cookie)
              .code == 400);
    CHECK(http("GET", "/api/admin/avatar?account=ghost&size=64", {}, root_cookie)
              .code == 404);
    // 未登录 401、成员 403（后台无代取字节/代删入口——只清点与代读）
    CHECK(http("GET", "/api/admin/branding").code == 401);
    CHECK(http("GET", "/api/admin/branding/logo").code == 401);
    CHECK(http("GET", "/api/admin/emoji?account=m1").code == 401);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=64").code == 401);
    CHECK(http("GET", "/api/admin/branding", {}, m1ck).code == 403);
    CHECK(http("POST", "/api/admin/branding", R"({"slogan":"x"})", m1ck).code ==
          403);
    CHECK(http("GET", "/api/admin/emoji?account=m1", {}, m1ck).code == 403);
    CHECK(http("GET", "/api/admin/avatar?account=m1&size=64", {}, m1ck).code ==
          403);
  }
}
