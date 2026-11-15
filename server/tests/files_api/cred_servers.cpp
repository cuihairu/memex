// test_files_api 拆分模块（LAY-005）：本套件场景体自原文件逐字搬移；
// 共享状态经 Fx 夹具传入（见 common.hpp）。套件按原序由 main 调用。
#include "common.hpp"

namespace memex::server::tests_api {

void cred_servers_suite(Fx& f) {
  auto H = [&f](const char* a) { return f.H(a); };
  auto& store = f.store;
  auto& io = f.io;
  auto& no_cred = f.no_cred;
  auto& port = f.port;
  auto& gid = f.gid;
  auto& gid2 = f.gid2;

  // —— R25-4 工具凭据面：服务端静态加密（AES-256-GCM，主密钥 SHA-256
  //     派生）、掩码元数据（回包绝不回显 value/sealed_hex）、未配主密钥
  //     503、store/cred 单元面（往返/错钥拒/篡改拒/覆盖/删/幽灵群） ——
  {
    // —— 单元面：cred 封装往返＋错钥/篡改拒；store 落库/覆盖/删/掩码 ——
    const std::string secret = "unit-test-cred-secret";
    const auto key = memex::server::derive_tool_cred_key(secret);
    CHECK(key.size() == 32);
    CHECK(memex::server::derive_tool_cred_key("").empty()); // 空=未启用
    const std::string plaintext = "sk-live-cred-0123456789";
    const auto sealed = memex::server::gcm_seal(key, plaintext);
    CHECK(!sealed.empty());
    CHECK(sealed.size() == (12 + plaintext.size() + 16) * 2); // hex(nonce||ct||tag)
    CHECK(sealed.find(plaintext) == std::string::npos); // 密文不含明文
    CHECK(memex::server::gcm_open(key, sealed) == plaintext); // 往返一致
    const auto wrong_key = memex::server::derive_tool_cred_key("other-secret");
    CHECK(memex::server::gcm_open(wrong_key, sealed).empty()); // 错钥拒
    std::string tampered = sealed;
    tampered[0] = tampered[0] == '0' ? '1' : '0';
    CHECK(memex::server::gcm_open(key, tampered).empty()); // 篡改拒
    // store 面：set→sealed 取回≠明文；覆盖；delete；list 掩码行
    CHECK(store.tool_cred_set(gid, "ci", sealed, "owner1", 1700000000000));
    const auto got = store.tool_cred_sealed(gid, "ci");
    CHECK(got.has_value());
    CHECK(got.value() == sealed);
    CHECK(got.value() != plaintext);
    CHECK(store.tool_cred_set(gid, "ci", sealed, "admin1", 1700000001000));
    CHECK(store.tool_cred_delete(gid, "ci"));
    CHECK(!store.tool_cred_sealed(gid, "ci").has_value());
    CHECK(!store.tool_cred_delete(gid, "ci")); // 再删=false
    CHECK(store.tool_cred_list(gid).empty());
    CHECK(!store.tool_cred_set(999999, "ci", sealed, "owner1", 1)); // 幽灵群
    // list 掩码行：多工具落库后只带 tool/updated_by/updated_ms（无密文）
    CHECK(store.tool_cred_set(gid, "ci", sealed, "owner1", 1700000000000));
    CHECK(store.tool_cred_set(gid, "pack", sealed, "admin1", 1700000001000));
    const auto metas = store.tool_cred_list(gid);
    CHECK(metas.size() == 2);
    CHECK(metas[0].tool == "ci" && metas[1].tool == "pack"); // ORDER BY tool
    CHECK(metas[0].updated_by == "owner1");
    CHECK(metas[1].updated_ms == 1700000001000);
    CHECK(store.tool_cred_delete(gid, "ci"));
    CHECK(store.tool_cred_delete(gid, "pack"));
  }

  // —— HTTP 面：配了主密钥的实例（会话按实例隔离，另换令牌） ——
  {
    memex::server::FileServer cred_files(io, store, nullptr, 0);
    cred_files.set_tool_cred_secret("http-test-secret");
    const std::uint16_t cport = cred_files.port();
    cred_files.start_accept();
    auto login = [&](const char* a) {
      const auto r = http(cport, "POST", "/files/session", {},
                          "{\"account\":\"" + std::string(a) +
                              "\",\"password\":\"pw-" + std::string(a) + "\"}");
      CHECK(r.status == 200);
      return jstr(r.body, "token");
    };
    const auto owner_tok = login("owner1");
    const auto member_tok = login("member1");
    CHECK(!owner_tok.empty() && !member_tok.empty());
    const auto Hc = [](const std::string& t) {
      return std::map<std::string, std::string>{
          {"Authorization", "Bearer " + t}};
    };
    const std::string gids = std::to_string(gid);
    // 未登录 401；成员 403（memo:config 仅群主/管理员）；缺 value 400；
    // 空 value 400；坏 gid 400；幽灵群 403（存在性不透）
    CHECK(http(cport, "POST", "/files/group-tools/credential", {},
                "{\"gid\":" + gids + ",\"tool\":\"ci\",\"value\":\"x\"}")
              .status == 401);
    CHECK(http(cport, "POST", "/files/group-tools/credential", Hc(member_tok),
                "{\"gid\":" + gids + ",\"tool\":\"ci\",\"value\":\"x\"}")
              .status == 403);
    CHECK(http(cport, "POST", "/files/group-tools/credential", Hc(owner_tok),
                "{\"gid\":" + gids + ",\"tool\":\"ci\"}").status == 400);
    CHECK(http(cport, "POST", "/files/group-tools/credential", Hc(owner_tok),
                "{\"gid\":" + gids + ",\"tool\":\"ci\",\"value\":\"\"}")
              .status == 400);
    CHECK(http(cport, "POST", "/files/group-tools/credential", Hc(owner_tok),
                "{\"gid\":0,\"tool\":\"ci\",\"value\":\"x\"}").status == 400);
    CHECK(http(cport, "POST", "/files/group-tools/credential", Hc(owner_tok),
                "{\"gid\":999999,\"tool\":\"ci\",\"value\":\"x\"}").status == 403);
    // 群主设置 200；回包不回显 value（明文/密文都不见）
    const std::string cred_value = "sk-http-secret-abcdef";
    const auto cs = http(cport, "POST", "/files/group-tools/credential",
                        Hc(owner_tok),
                        "{\"gid\":" + gids + ",\"tool\":\"ci\",\"value\":\"" +
                            cred_value + "\"}");
    CHECK(cs.status == 200);
    CHECK(cs.body.find("\"updated\":true") != std::string::npos);
    CHECK(cs.body.find(cred_value) == std::string::npos); // 明文不回显
    CHECK(cs.body.find("sealed") == std::string::npos); // 密文也不回显
    // 覆盖设置 200
    CHECK(http(cport, "POST", "/files/group-tools/credential", Hc(owner_tok),
                "{\"gid\":" + gids + ",\"tool\":\"ci\",\"value\":\"sk-new\"}")
              .status == 200);
    // 掩码列表：成员 403；群主 200 见 tool/updated_by/updated_ms，无 value
    CHECK(http(cport, "GET", "/files/group-tools/credentials?gid=" + gids,
                Hc(member_tok), "").status == 403);
    const auto cl = http(cport, "GET",
                         "/files/group-tools/credentials?gid=" + gids,
                         Hc(owner_tok), "");
    CHECK(cl.status == 200);
    CHECK(cl.body.find("\"tool\":\"ci\"") != std::string::npos);
    CHECK(cl.body.find("\"updated_by\":\"owner1\"") != std::string::npos);
    CHECK(cl.body.find(cred_value) == std::string::npos);
    CHECK(cl.body.find("sk-new") == std::string::npos);
    CHECK(cl.body.find("sealed") == std::string::npos);
    CHECK(cl.body.find("value") == std::string::npos);
    // 坏 gid 400
    CHECK(http(cport, "GET", "/files/group-tools/credentials?gid=x",
                Hc(owner_tok), "").status == 400);
    // 删除：成员 403；群主 200；再删 404
    CHECK(http(cport, "POST", "/files/group-tools/credential", Hc(member_tok),
                "{\"gid\":" + gids + ",\"tool\":\"ci\",\"op\":\"delete\"}")
              .status == 403);
    CHECK(http(cport, "POST", "/files/group-tools/credential", Hc(owner_tok),
                "{\"gid\":" + gids + ",\"tool\":\"ci\",\"op\":\"delete\"}")
              .status == 200);
    CHECK(http(cport, "POST", "/files/group-tools/credential", Hc(owner_tok),
                "{\"gid\":" + gids + ",\"tool\":\"ci\",\"op\":\"delete\"}")
              .status == 404);
    // 删后列表空
    CHECK(http(cport, "GET", "/files/group-tools/credentials?gid=" + gids,
                Hc(owner_tok), "").body.find("\"credentials\":[]") !=
              std::string::npos);

    // —— R26-4 服务器凭据面：目标机凭据只存服务端（同款加密）、掩码
    //    元数据走服务器列表（cred/cred_updated_by）、跨群 id 不给过 ——
    const auto key4 = memex::server::derive_tool_cred_key("http-test-secret");
    {
      // store 单元面：set 须服务器属该群（幽灵 id=false）→list 空
      CHECK(!store.server_cred_set(gid, 999999999, "aa", "owner1", 1));
      CHECK(!store.server_cred_set(gid2, 1, "aa", "owner1", 1));
      CHECK(store.server_cred_list(gid).empty());
    }
    const auto en4 = http(cport, "POST", "/files/group-servers/enroll",
                          Hc(owner_tok),
                          "{\"gid\":" + gids +
                              ",\"name\":\"cred-box\",\"host\":\"10.0.0.11\"}");
    CHECK(en4.status == 200);
    const std::int64_t csid = jint(en4.body, "id");
    // store 面：真服务器（id=csid 属 gid）→密文取回≠明文→gcm_open 往返
    CHECK(store.server_cred_set(gid, static_cast<std::uint64_t>(csid),
                                memex::server::gcm_seal(key4, "unit-srv-cred"),
                                "owner1", 1700000010000));
    const auto got_srv = store.server_cred_sealed(csid);
    CHECK(got_srv.has_value());
    CHECK(got_srv.value() != "unit-srv-cred");
    CHECK(memex::server::gcm_open(key4, got_srv.value()) == "unit-srv-cred");
    CHECK(store.server_cred_set(gid, static_cast<std::uint64_t>(csid),
                                memex::server::gcm_seal(key4, "unit-srv-2"),
                                "owner1", 1700000011000));
    // HTTP 面：未登录 401；缺 value 400；成员 403；跨群服务器 404
    CHECK(http(cport, "POST", "/files/group-servers/credential", {},
                "{\"gid\":" + gids + ",\"server_id\":" + std::to_string(csid) +
                    ",\"value\":\"x\"}")
              .status == 401);
    CHECK(http(cport, "POST", "/files/group-servers/credential",
                Hc(owner_tok),
                "{\"gid\":" + gids + ",\"server_id\":" + std::to_string(csid) +
                    "}")
              .status == 400);
    CHECK(http(cport, "POST", "/files/group-servers/credential",
                Hc(member_tok),
                "{\"gid\":" + gids + ",\"server_id\":" + std::to_string(csid) +
                    ",\"value\":\"x\"}")
              .status == 403);
    CHECK(http(cport, "POST", "/files/group-servers/credential",
                Hc(owner_tok),
                "{\"gid\":" + std::to_string(gid2) +
                    ",\"server_id\":" + std::to_string(csid) +
                    ",\"value\":\"x\"}")
              .status == 404);
    // 群主设置 200：回包无 value/sealed 回显
    const auto scs = http(cport, "POST", "/files/group-servers/credential",
                          Hc(owner_tok),
                          "{\"gid\":" + gids + ",\"server_id\":" +
                              std::to_string(csid) + ",\"value\":\"srv-live-1\"}");
    CHECK(scs.status == 200);
    CHECK(scs.body.find("\"updated\":true") != std::string::npos);
    CHECK(scs.body.find("srv-live-1") == std::string::npos);
    // 服务器列表掩码元数据：cred=true＋谁更新；明文/密文永不出门
    const auto sl = http(cport, "GET", "/files/group-servers/list?gid=" + gids,
                         Hc(owner_tok), "");
    CHECK(sl.status == 200);
    CHECK(sl.body.find("\"cred\":true") != std::string::npos);
    CHECK(sl.body.find("\"cred_updated_by\":\"owner1\"") != std::string::npos);
    CHECK(sl.body.find("srv-live-1") == std::string::npos);
    CHECK(sl.body.find("sealed") == std::string::npos);
    // 成员看列表见掩码态（无密文），但无权改（上面 403 已验）
    CHECK(http(cport, "GET", "/files/group-servers/list?gid=" + gids,
               Hc(member_tok), "")
              .body.find("\"cred\":true") != std::string::npos);
    // 删除：成员 403；群主 200；再删 404；列表翻回「未配置」
    CHECK(http(cport, "POST", "/files/group-servers/credential",
               Hc(member_tok),
               "{\"gid\":" + gids + ",\"server_id\":" + std::to_string(csid) +
                   ",\"op\":\"delete\"}")
              .status == 403);
    CHECK(http(cport, "POST", "/files/group-servers/credential",
               Hc(owner_tok),
               "{\"gid\":" + gids + ",\"server_id\":" + std::to_string(csid) +
                   ",\"op\":\"delete\"}")
              .status == 200);
    CHECK(http(cport, "POST", "/files/group-servers/credential",
               Hc(owner_tok),
               "{\"gid\":" + gids + ",\"server_id\":" + std::to_string(csid) +
                   ",\"op\":\"delete\"}")
              .status == 404);
    CHECK(http(cport, "GET", "/files/group-servers/list?gid=" + gids,
               Hc(owner_tok), "")
              .body.find("\"cred\":false") != std::string::npos);
    // 跨群 id 删不过；HTTP 删已净（再删=false、sealed 无）
    CHECK(!store.server_cred_delete(gid2, static_cast<std::uint64_t>(csid)));
    CHECK(!store.server_cred_delete(gid, static_cast<std::uint64_t>(csid)));
    CHECK(!store.server_cred_sealed(csid).has_value());
  }

  // —— HTTP 面：未配主密钥的实例——两路由一律 503（不静默存明文） ——
  // 两个临时面实例都活到 io 停摆之后：do_accept 以裸 this 链式再挂
  // accept，作用域内析构会让 io 线程在悬空 this 上跑完成回调（偶发段错误）
  {
    no_cred = std::make_unique<memex::server::FileServer>(io, store, nullptr, 0);
    const std::uint16_t nport = no_cred->port();
    no_cred->start_accept();
    const auto nt = http(nport, "POST", "/files/session", {},
                         "{\"account\":\"owner1\",\"password\":\"pw-owner1\"}");
    CHECK(nt.status == 200);
    const auto nh = std::map<std::string, std::string>{
        {"Authorization", "Bearer " + jstr(nt.body, "token")}};
    const std::string gids = std::to_string(gid);
    const auto ns = http(nport, "POST", "/files/group-tools/credential", nh,
                         "{\"gid\":" + gids + ",\"tool\":\"ci\",\"value\":\"x\"}");
    CHECK(ns.status == 503);
    CHECK(jstr(ns.body, "error").find("未启用") != std::string::npos);
    CHECK(http(nport, "GET", "/files/group-tools/credentials?gid=" + gids, nh,
                "").status == 503);
    // R26-4 服务器凭据同面：未配主密钥 503（不静默存明文）
    const auto nsc = http(nport, "POST", "/files/group-servers/credential", nh,
                          "{\"gid\":" + gids +
                              ",\"server_id\":1,\"value\":\"x\"}");
    CHECK(nsc.status == 503);
    CHECK(jstr(nsc.body, "error").find("未启用") != std::string::npos);
  }

  // —— R26-1 服务器 agent 面：登记判权＋令牌只存摘要＋心跳鉴权＋列表
  //    红绿灯（在线=last_seen 新鲜）——
  {
    const std::string gids = std::to_string(gid);
    const std::string enroll_body =
        "{\"gid\":" + gids + ",\"name\":\"web-1\",\"host\":\"10.0.0.1\"}";
    // 登记：未登录 401；缺 host 400；非管理员 403（外人/成员都摸不到）
    CHECK(http(port, "POST", "/files/group-servers/enroll", {}, enroll_body)
              .status == 401);
    CHECK(http(port, "POST", "/files/group-servers/enroll", H("owner1"),
               "{\"gid\":" + gids + ",\"name\":\"web-1\"}")
              .status == 400);
    for (const char* a : {"member1", "outsider"}) {
      CHECK(http(port, "POST", "/files/group-servers/enroll", H(a),
                 enroll_body).status == 403);
    }
    // 群主登记 200：回包带一次性令牌
    const auto en = http(port, "POST", "/files/group-servers/enroll",
                         H("owner1"), enroll_body);
    CHECK(en.status == 200);
    const std::string token = jstr(en.body, "token");
    CHECK(!token.empty());
    CHECK(jint(en.body, "id") > 0);
    // 令牌只存 SHA-256 摘要：摘要能开、明文开不了（库内永不见明文）
    CHECK(store
              .server_by_token_hash(memex::server::sha256_hex(token))
              .has_value());
    CHECK(!store.server_by_token_hash(token).has_value());
    // 心跳：错令牌 401；缺数值字段 400；正拍 200（心跳走令牌非人会话）
    const std::string hb_head =
        ",\"cpu_percent\":12.5,\"mem_used_mb\":1024,\"mem_total_mb\":2048,"
        "\"disk_used_mb\":20,\"disk_total_mb\":100,\"load1\":0.42";
    CHECK(http(port, "POST", "/files/group-servers/heartbeat", {},
               "{\"token\":\"bogus\"" + hb_head + "}").status == 401);
    CHECK(http(port, "POST", "/files/group-servers/heartbeat", {},
               "{\"token\":\"" + token +
                   ",\"cpu_percent\":1.0}").status == 400);
    CHECK(http(port, "POST", "/files/group-servers/heartbeat", {},
               "{\"token\":\"" + token + "\"" + hb_head + "}").status == 200);
    // 列表：非成员 403；成员 200 在线绿灯＋指标落账；令牌永不出现
    CHECK(http(port, "GET", "/files/group-servers/list?gid=" + gids,
               H("outsider"), "").status == 403);
    const auto sl = http(port, "GET",
                         "/files/group-servers/list?gid=" + gids,
                         H("member1"), "");
    CHECK(sl.status == 200);
    CHECK(sl.body.find("\"name\":\"web-1\"") != std::string::npos);
    CHECK(sl.body.find("\"online\":true") != std::string::npos);
    CHECK(sl.body.find("\"cpu_percent\":12.5") != std::string::npos);
    CHECK(sl.body.find("\"load1\":0.42") != std::string::npos);
    CHECK(sl.body.find("token") == std::string::npos);
    // 重登记=同行轮换令牌：id 稳定、老令牌 401、新令牌 200
    const auto en2 = http(port, "POST", "/files/group-servers/enroll",
                          H("owner1"), enroll_body);
    CHECK(en2.status == 200);
    CHECK(jint(en2.body, "id") == jint(en.body, "id"));
    CHECK(jstr(en2.body, "token") != token);
    CHECK(http(port, "POST", "/files/group-servers/heartbeat", {},
               "{\"token\":\"" + token + "\"" + hb_head + "}").status == 401);
    CHECK(http(port, "POST", "/files/group-servers/heartbeat", {},
               "{\"token\":\"" + jstr(en2.body, "token") + "\"" + hb_head +
                   "}").status == 200);
    // 幽灵群：memo:config 判权先于写入（不存在的群不给过）
    CHECK(http(port, "POST", "/files/group-servers/enroll", H("owner1"),
               "{\"gid\":999999999,\"name\":\"x\",\"host\":\"h\"}")
              .status == 403);
  }

  // —— R26-3 远程会话（SSH 起步）：短票签发/一次性兑现/收尾留痕/判权 ——
  {
    const std::string gids = std::to_string(gid);
    const auto en3 = http(port, "POST", "/files/group-servers/enroll",
                          H("owner1"),
                          "{\"gid\":" + gids +
                              ",\"name\":\"ssh-box\",\"host\":\"10.0.0.7\"}");
    CHECK(en3.status == 200);
    const std::int64_t sid = jint(en3.body, "id");
    // 签发：未登录 401；缺字段 400；非成员 403；协议 400；幽灵服务器 404
    CHECK(http(port, "POST", "/files/group-servers/session/request", {},
               "{\"gid\":" + gids + ",\"server_id\":" + std::to_string(sid) +
                   "}")
              .status == 401);
    CHECK(http(port, "POST", "/files/group-servers/session/request",
               H("member1"), "{\"gid\":" + gids + "}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-servers/session/request",
               H("outsider"),
               "{\"gid\":" + gids +
                   ",\"server_id\":" + std::to_string(sid) + "}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-servers/session/request",
               H("member1"),
               "{\"gid\":" + gids + ",\"server_id\":" + std::to_string(sid) +
                   ",\"protocol\":\"rdp\"}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-servers/session/request",
               H("member1"),
               "{\"gid\":" + gids + ",\"server_id\":999999999}")
              .status == 404);
    // 成员签发 200：一次性短票出门
    const auto rq =
        http(port, "POST", "/files/group-servers/session/request",
             H("member1"),
             "{\"gid\":" + gids + ",\"server_id\":" + std::to_string(sid) +
                 "}");
    CHECK(rq.status == 200);
    const std::string ticket = jstr(rq.body, "ticket");
    CHECK(!ticket.empty());
    const std::int64_t ssid = jint(rq.body, "session_id");
    CHECK(ssid > 0);
    // 兑现：错票 401；正票 200 回目标；重放 409（一次性）
    CHECK(http(port, "POST", "/files/group-servers/session/redeem", {},
               "{\"ticket\":\"bogus\"}").status == 401);
    const auto rd = http(port, "POST", "/files/group-servers/session/redeem",
                         {}, "{\"ticket\":\"" + ticket + "\"}");
    CHECK(rd.status == 200);
    CHECK(jstr(rd.body, "host") == "10.0.0.7");
    CHECK(jstr(rd.body, "protocol") == "ssh");
    CHECK(http(port, "POST", "/files/group-servers/session/redeem", {},
               "{\"ticket\":\"" + ticket + "\"}").status == 409);
    // 收尾：非本人 409（群主也关不了别人的会话）；本人 200；重复收尾 409
    CHECK(http(port, "POST", "/files/group-servers/session/close",
               H("owner1"),
               "{\"gid\":" + gids + ",\"session_id\":" +
                   std::to_string(ssid) + "}")
              .status == 409);
    CHECK(http(port, "POST", "/files/group-servers/session/close",
               H("member1"),
               "{\"gid\":" + gids + ",\"session_id\":" +
                   std::to_string(ssid) + "}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-servers/session/close",
               H("member1"),
               "{\"gid\":" + gids + ",\"session_id\":" +
                   std::to_string(ssid) + "}")
              .status == 409);
    // 留痕列表：非成员 403；成员见行（谁/连哪台/协议/已兑现/已收尾）；
    // 短票永不出现
    CHECK(http(port, "GET", "/files/group-servers/sessions?gid=" + gids,
               H("outsider"), "").status == 403);
    const auto ls = http(port, "GET",
                         "/files/group-servers/sessions?gid=" + gids,
                         H("member1"), "");
    CHECK(ls.status == 200);
    CHECK(ls.body.find("\"actor\":\"member1\"") != std::string::npos);
    CHECK(ls.body.find("\"host\":\"10.0.0.7\"") != std::string::npos);
    CHECK(ls.body.find("\"redeemed\":true") != std::string::npos);
    CHECK(ls.body.find("\"open\":false") != std::string::npos);
    CHECK(ls.body.find("ticket") == std::string::npos);
    // 另一笔进行中：未兑现的行明示（未使用短票）
    CHECK(http(port, "POST", "/files/group-servers/session/request",
               H("owner1"),
               "{\"gid\":" + gids + ",\"server_id\":" + std::to_string(sid) +
                   "}")
              .status == 200);
    const auto ls2 = http(port, "GET",
                          "/files/group-servers/sessions?gid=" + gids,
                          H("member1"), "");
    CHECK(ls2.body.find("\"redeemed\":false") != std::string::npos);
    CHECK(ls2.body.find("\"open\":true") != std::string::npos);
  }

}

} // namespace memex::server::tests_api
