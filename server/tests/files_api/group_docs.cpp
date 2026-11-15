// test_files_api 拆分模块（LAY-005）：本套件场景体自原文件逐字搬移；
// 共享状态经 Fx 夹具传入（见 common.hpp）。套件按原序由 main 调用。
#include "common.hpp"

namespace memex::server::tests_api {

void group_docs_suite(Fx& f) {
  auto H = [&f](const char* a) { return f.H(a); };
  auto& port = f.port;
  auto& gid = f.gid;
  auto& gid2 = f.gid2;
  auto& idp = f.idp;

  // —— R24-2 群备忘录：管理员维护默认、开放编辑开关、修订史回滚、搜索 ——
  {
    // 未登录 401；非成员 403（group-member 继承不命中）
    CHECK(http(port, "GET", "/files/group-memo/list?gid=" + std::to_string(gid),
               {}, "").status == 401);
    CHECK(http(port, "GET", "/files/group-memo/list?gid=" + std::to_string(gid),
               H("outsider"), "").status == 403);
    // 坏 gid 400
    CHECK(http(port, "GET", "/files/group-memo/list?gid=x", H("member1"), "")
              .status == 400);
    // 默认管理员维护：成员建 403；管理员建 200；缺 title 400
    CHECK(http(port, "POST", "/files/group-memo/save",
               H("member1"),
               "{\"gid\":" + std::to_string(gid) +
                   ",\"title\":\"t\",\"content\":\"c\"}").status == 403);
    CHECK(http(port, "POST", "/files/group-memo/save",
               H("admin1"),
               "{\"gid\":" + std::to_string(gid) + ",\"content\":\"c\"}")
              .status == 400);
    const auto mk = http(
        port, "POST", "/files/group-memo/save", H("admin1"),
        "{\"gid\":" + std::to_string(gid) +
            ",\"title\":\"测试环境\",\"content\":\"host=10.0.0.1 port=5432\"}");
    CHECK(mk.status == 200);
    const auto mid = jint(mk.body, "id");
    CHECK(mid > 0);
    // 列表：成员可读；open_edit=false 回真；标题命中
    const auto lst = http(
        port, "GET",
        "/files/group-memo/list?gid=" + std::to_string(gid), H("member1"), "");
    CHECK(lst.status == 200);
    CHECK(jstr(lst.body, "title") == "测试环境");
    CHECK(lst.body.find("\"open_edit\":false") != std::string::npos);
    // 编辑留痕：管理员改一次（修订史=首笔+编辑笔共 2）
    CHECK(http(port, "POST", "/files/group-memo/save", H("admin1"),
               "{\"gid\":" + std::to_string(gid) + ",\"id\":" +
                   std::to_string(mid) +
                   ",\"title\":\"测试环境\",\"content\":\"host=10.0.0.2\"}")
              .status == 200);
    const auto hist = http(
        port, "GET", "/files/group-memo/history?id=" + std::to_string(mid),
        H("member1"), "");
    CHECK(hist.status == 200);
    CHECK(jint(hist.body, "id") == mid);
    // 倒序：首笔在 updated 值语义上先落——revisions[0] 应是最新编辑笔
    CHECK(hist.body.find("host=10.0.0.2") != std::string::npos);
    CHECK(hist.body.find("host=10.0.0.1") != std::string::npos);
    // 开放编辑开关：成员设 403（memo:config 仅群主/管理员）；管理员开 200
    CHECK(http(port, "POST", "/files/group-memo/open-edit", H("member1"),
               "{\"gid\":" + std::to_string(gid) + ",\"open\":true}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-memo/open-edit", H("admin1"),
               "{\"gid\":" + std::to_string(gid) + ",\"open\":true}")
              .status == 200);
    // 开放后：成员可建/可改（留痕照记）；删仍归管理员（开放的是写不是删）
    const auto mk2 = http(
        port, "POST", "/files/group-memo/save", H("member1"),
        "{\"gid\":" + std::to_string(gid) +
            ",\"title\":\"值班表\",\"content\":\"周一 alice\"}");
    CHECK(mk2.status == 200);
    const auto mid2 = jint(mk2.body, "id");
    CHECK(mid2 > 0);
    CHECK(http(port, "POST", "/files/group-memo/save", H("member1"),
               "{\"gid\":" + std::to_string(gid) + ",\"id\":" +
                   std::to_string(mid) +
                   ",\"title\":\"测试环境\",\"content\":\"host=10.0.0.3\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-memo/delete", H("member1"),
               "{\"gid\":" + std::to_string(gid) + ",\"id\":" +
                   std::to_string(mid2) + "}").status == 403);
    // 回滚：成员回滚到首笔（content 回 host=10.0.0.1）——回滚即一次编辑落新笔
    const auto hist2 = http(
        port, "GET", "/files/group-memo/history?id=" + std::to_string(mid),
        H("admin1"), "");
    // 首笔 id：修订笔 JSON 键序 content 在前——首笔（数组最后一笔）content
    // 之后最近的 "id":N 即该笔 id
    const std::string first_rev_marker =
        "\"content\":\"host=10.0.0.1 port=5432\"";
    const auto fp = hist2.body.find(first_rev_marker);
    CHECK(fp != std::string::npos);
    std::int64_t first_rev = 0;
    if (fp != std::string::npos) {
      const std::size_t idp = hist2.body.find("\"id\":", fp);
      if (idp != std::string::npos) {
        first_rev = std::atoll(hist2.body.c_str() + idp + 5);
      }
    }
    CHECK(first_rev > 0);
    CHECK(http(port, "POST", "/files/group-memo/rollback", H("member1"),
               "{\"id\":" + std::to_string(mid) + ",\"revision_id\":" +
                   std::to_string(first_rev) + "}").status == 200);
    const auto after = http(
        port, "GET", "/files/group-memo/history?id=" + std::to_string(mid),
        H("member1"), "");
    // 回滚后新增一笔（editor=member1，content=首版全文）； revisions[0]
    // （倒序最新笔）即回滚笔——其 content 是修订笔数组里最先出现的 host=10.0.0.1
    CHECK(after.status == 200);
    const auto roll_pos = after.body.find(first_rev_marker);
    CHECK(roll_pos != std::string::npos);
    CHECK(after.body.find("\"editor\":\"member1\"", roll_pos) ==
          roll_pos + first_rev_marker.size() + 1);
    // 幽灵修订笔 404
    CHECK(http(port, "POST", "/files/group-memo/rollback", H("admin1"),
               "{\"id\":" + std::to_string(mid) + ",\"revision_id\":999999}")
              .status == 404);
    // 搜索：q=值班 命中条目2；q=不存在词 空
    const auto search = http(
        port, "GET",
        "/files/group-memo/list?gid=" + std::to_string(gid) + "&q=%E5%80%BC%E7%8F%AD",
        H("member1"), "");
    CHECK(search.status == 200);
    CHECK(search.body.find("值班表") != std::string::npos);
    CHECK(search.body.find("测试环境") == std::string::npos);
    // 关闭开放编辑：成员写回落 403
    CHECK(http(port, "POST", "/files/group-memo/open-edit", H("owner1"),
               "{\"gid\":" + std::to_string(gid) + ",\"open\":false}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-memo/save", H("member1"),
               "{\"gid\":" + std::to_string(gid) + ",\"id\":" +
                   std::to_string(mid) +
                   ",\"title\":\"测试环境\",\"content\":\"host=10.0.0.9\"}")
              .status == 403);
    // 管理员删除：条目+修订史连带（history 404）
    CHECK(http(port, "POST", "/files/group-memo/delete", H("admin1"),
               "{\"gid\":" + std::to_string(gid) + ",\"id\":" +
                   std::to_string(mid) + "}").status == 200);
    CHECK(http(port, "GET", "/files/group-memo/history?id=" +
                                 std::to_string(mid), H("admin1"), "")
              .status == 404);
  }

  // —— R24-3 群密码箱：全程密文（服务端不验 b64 内容）、授权名单、
  //     查看/复制留痕、重置/删除——解锁派生是客户端面，此处只验存取与判权 ——
  {
    const std::string salt = "czYwMDAwMDA=";
    const std::string wrap = "bm9uY2UxMjM0NTY3ODkwYWJjZGVmZ2hpamtsbW5vcA==";
    const std::string salt2 = "bmV3c2FsdA==";
    const std::string ct = "bm9uY2UtY2lwaGVydGV4dA==";
    const std::string nonce = "bm9uY2UxMjM0NTY3ODkw";
    const std::string gids = std::to_string(gid);
    // 未登录 401；非成员 403（存在性都不透）
    CHECK(http(port, "GET", "/files/group-vault/info?gid=" + gids, {}, "")
              .status == 401);
    CHECK(http(port, "GET", "/files/group-vault/info?gid=" + gids,
               H("outsider"), "").status == 403);
    // 未建箱 exists=false；坏 gid 400
    CHECK(http(port, "GET", "/files/group-vault/info?gid=x", H("member1"), "")
              .status == 400);
    const auto iv0 = http(port, "GET", "/files/group-vault/info?gid=" + gids,
                          H("member1"), "");
    CHECK(iv0.status == 200);
    CHECK(iv0.body.find("\"exists\":false") != std::string::npos);
    const std::string init_body = "{\"gid\":" + gids + ",\"kdf_salt\":\"" +
                                  salt + "\",\"kdf_iters\":600000,"
                                         "\"wrapped_dek\":\"" +
                                  wrap + "\"}";
    // 建箱：成员 403（仅群主/管理员）；迭代数 <10000 400；管理员可建；重复 409
    CHECK(http(port, "POST", "/files/group-vault/init", H("member1"),
               init_body).status == 403);
    CHECK(http(port, "POST", "/files/group-vault/init", H("admin1"),
               "{\"gid\":" + gids + ",\"kdf_salt\":\"" + salt +
                   "\",\"kdf_iters\":100,\"wrapped_dek\":\"" + wrap + "\"}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-vault/init", H("admin1"),
               init_body).status == 200);
    CHECK(http(port, "POST", "/files/group-vault/init", H("admin1"),
               init_body).status == 409);
    // info 建后带回盐/包裹块（授权成员解锁必需）
    const auto iv1 = http(port, "GET", "/files/group-vault/info?gid=" + gids,
                          H("member1"), "");
    CHECK(iv1.status == 200);
    CHECK(iv1.body.find("\"exists\":true") != std::string::npos);
    CHECK(iv1.body.find(salt) != std::string::npos);
    CHECK(iv1.body.find(wrap) != std::string::npos);
    // list：成员可读（名单空=全成员），掩码面——列表不带 secret 密文
    const auto lv = http(port, "GET", "/files/group-vault/list?gid=" + gids,
                         H("member1"), "");
    CHECK(lv.status == 200);
    CHECK(lv.body.find("\"secret_ct\"") == std::string::npos);
    // 条目维护：成员建 403（管理员维护，不开放成员写）；群主建 200
    const std::string entry =
        "{\"gid\":" + gids + ",\"name\":\"生产库\",\"account_name\":\"root\","
                             "\"secret_ct\":\"" +
        ct + "\",\"secret_nonce\":\"" + nonce + "\"}";
    CHECK(http(port, "POST", "/files/group-vault/save", H("member1"), entry)
              .status == 403);
    const auto sv = http(port, "POST", "/files/group-vault/save",
                         H("owner1"), entry);
    CHECK(sv.status == 200);
    const auto vid = jint(sv.body, "id");
    CHECK(vid > 0);
    // 改条目（带 id）：群主 200
    const std::string entry_upd =
        "{\"gid\":" + gids + ",\"id\":" + std::to_string(vid) +
        ",\"name\":\"生产库-主\",\"account_name\":\"root\","
        "\"secret_ct\":\"" +
        ct + "\",\"secret_nonce\":\"" + nonce + "\"}";
    CHECK(http(port, "POST", "/files/group-vault/save", H("owner1"),
               entry_upd).status == 200);
    // access：群主 reveal 200 带密文；成员 copy 200（默认全成员可解锁）；
    // 坏 action 400；幽灵条目 404
    const std::string acc_base = "{\"gid\":" + gids + ",\"id\":" +
                                 std::to_string(vid) + ",\"action\":\"";
    const auto ac1 = http(port, "POST", "/files/group-vault/access",
                          H("owner1"), acc_base + "reveal\"}");
    CHECK(ac1.status == 200);
    CHECK(ac1.body.find("\"secret_ct\":\"" + ct + "\"") != std::string::npos);
    CHECK(http(port, "POST", "/files/group-vault/access", H("member1"),
               acc_base + "copy\"}").status == 200);
    CHECK(http(port, "POST", "/files/group-vault/access", H("member1"),
               acc_base + "steal\"}").status == 400);
    CHECK(http(port, "POST", "/files/group-vault/access", H("member1"),
               "{\"gid\":" + gids + ",\"id\":999999,"
                                    "\"action\":\"reveal\"}")
              .status == 404);
    // 留痕：成员查审计拒（仅群主/管理员）；群主查见 reveal+copy 两行
    CHECK(http(port, "GET", "/files/group-vault/audit?gid=" + gids,
               H("member1"), "").status == 403);
    const auto au = http(port, "GET", "/files/group-vault/audit?gid=" + gids,
                         H("owner1"), "");
    CHECK(au.status == 200);
    CHECK(au.body.find("\"action\":\"copy\"") != std::string::npos);
    CHECK(au.body.find("\"action\":\"reveal\"") != std::string::npos);
    CHECK(au.body.find("\"actor\":\"member1\"") != std::string::npos);
    // 授权名单：管理员设拒（仅群主可收窄授权）；群主设 [admin1] 后
    // member1 不在名单拒、admin1 恒可（owner/admin 不受名单限）
    CHECK(http(port, "POST", "/files/group-vault/acl", H("admin1"),
               "{\"gid\":" + gids + ",\"accounts\":[\"member1\"]}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-vault/acl", H("owner1"),
               "{\"gid\":" + gids + ",\"accounts\":[\"admin1\"]}")
              .status == 200);
    CHECK(http(port, "GET", "/files/group-vault/list?gid=" + gids,
               H("member1"), "").status == 403);
    CHECK(http(port, "GET", "/files/group-vault/info?gid=" + gids,
               H("member1"), "").status == 403);
    CHECK(http(port, "GET", "/files/group-vault/list?gid=" + gids,
               H("admin1"), "").status == 200);
    // 空名单恢复全成员（共享本意默认）
    CHECK(http(port, "POST", "/files/group-vault/acl", H("owner1"),
               "{\"gid\":" + gids + ",\"accounts\":[]}").status == 200);
    CHECK(http(port, "GET", "/files/group-vault/list?gid=" + gids,
               H("member1"), "").status == 200);
    // 重置/重包裹：成员 403；群主换盐换包裹块 200 且 info 见新盐；
    // 无箱的 gid2 404
    CHECK(http(port, "POST", "/files/group-vault/rekey", H("member1"),
               init_body).status == 403);
    const std::string rekey_body = "{\"gid\":" + gids + ",\"kdf_salt\":\"" +
                                   salt2 + "\",\"kdf_iters\":720000,"
                                           "\"wrapped_dek\":\"" +
                                   wrap + "\"}";
    CHECK(http(port, "POST", "/files/group-vault/rekey", H("owner1"),
               rekey_body).status == 200);
    const auto iv2 = http(port, "GET", "/files/group-vault/info?gid=" + gids,
                          H("member1"), "");
    CHECK(iv2.body.find(salt2) != std::string::npos);
    CHECK(iv2.body.find("\"kdf_iters\":720000") != std::string::npos);
    CHECK(http(port, "POST", "/files/group-vault/rekey",
               H("owner1"),
               "{\"gid\":" + std::to_string(gid2) + ",\"kdf_salt\":\"" +
                   salt2 + "\",\"kdf_iters\":600000,\"wrapped_dek\":\"" +
                   wrap + "\"}")
              .status == 404);
    // 删条目恒归管理员：成员拒；群主删 200；再 access 404
    CHECK(http(port, "POST", "/files/group-vault/delete", H("member1"),
               "{\"gid\":" + gids + ",\"id\":" + std::to_string(vid) + "}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-vault/delete", H("owner1"),
               "{\"gid\":" + gids + ",\"id\":" + std::to_string(vid) + "}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-vault/access", H("owner1"),
               acc_base + "reveal\"}").status == 404);
  }

}

} // namespace memex::server::tests_api
