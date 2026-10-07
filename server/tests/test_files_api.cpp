// R23-2 文件面验收（真实走查）：HTTP 全链路（真 TCP）× 群主/管理员/成员/
// 非成员四视角 × 读/列/传/删/置顶/配额。对象层用内存 Fake（铁律考点：
// 权限全在元数据层判、字节全过本面；S3 真容器腿在 test_s3_e2e）。
// 覆盖：会话令牌（错口令 401）、上传（秒传复用不扣费/换目标新行/两级
// 扣费/413 配额拒/502 落字节失败回滚/409 隔离冲突且字节保留）、下载
//（403 显式拒隔离、个人仅本人）、列表（置顶优先/分页）、管理（成员不可
// 删群文件、管理员/群主可管、配额任免权）、删除退费与对象引用计数。
#include <asio.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cred.hpp"
#include "files_server.hpp"
#include "storage.hpp"
#include "store.hpp"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ':' << __LINE__ << ' ' << #cond    \
                << '\n';                                                     \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

using memex::server::ServerStore;
using memex::server::S3Storage;

// —— 内存对象存储 Fake：全接口落地，可注入 put 失败 ——
class FakeS3 final : public S3Storage {
 public:
  bool create_bucket() override { return true; }
  bool put_object(const std::string& key, const std::string& body,
                  std::string* etag_out) override {
    if (fail_next_put_) {
      fail_next_put_ = false;
      return false;
    }
    objects_[key] = body;
    if (etag_out) *etag_out = "\"fake\"";
    return true;
  }
  bool get_object(const std::string& key, std::string* body_out) override {
    const auto it = objects_.find(key);
    if (it == objects_.end()) return false;
    *body_out = it->second;
    return true;
  }
  bool delete_object(const std::string& key) override {
    return objects_.erase(key) > 0;
  }
  std::vector<std::string> list_objects(const std::string& prefix,
                                        int max_keys) override {
    std::vector<std::string> out;
    for (const auto& [k, v] : objects_) {
      (void)v;
      if (k.rfind(prefix, 0) == 0 && static_cast<int>(out.size()) < max_keys) {
        out.push_back(k);
      }
    }
    return out;
  }
  std::optional<std::string> create_multipart_upload(
      const std::string&) override {
    return "fake-upload-id";
  }
  std::optional<std::string> upload_part(const std::string&,
                                         const std::string&, int,
                                         const std::string&) override {
    return "\"fake-part\"";
  }
  std::optional<memex::server::CompleteUploadResult> complete_multipart_upload(
      const std::string& key, const std::string&,
      const std::vector<memex::server::UploadPartResult>&) override {
    memex::server::CompleteUploadResult r;
    r.bucket = "fake";
    r.key = key;
    return r;
  }
  bool abort_multipart_upload(const std::string&,
                              const std::string&) override {
    return true;
  }
  std::string presign_put(const std::string&, int) override { return ""; }
  std::string presign_get(const std::string&, int) override { return ""; }
  bool head_object(const std::string& key, std::int64_t* size_out,
                   std::string*) override {
    const auto it = objects_.find(key);
    if (it == objects_.end()) return false;
    if (size_out) *size_out = static_cast<std::int64_t>(it->second.size());
    return true;
  }
  bool delete_objects(const std::vector<std::string>& keys) override {
    for (const auto& k : keys) objects_.erase(k);
    return true;
  }

  bool fail_next_put_ = false;
  std::map<std::string, std::string> objects_;
};

// —— 阻塞 HTTP 客户端（真 TCP；服务端 Connection: close，读到对端关）——
struct HttpReply {
  int status{0};
  std::map<std::string, std::string> headers; // 小写键
  std::string body;
};

HttpReply http(std::uint16_t port, const std::string& method,
               const std::string& path,
               const std::map<std::string, std::string>& headers,
               const std::string& body, bool send_content_length = true) {
  HttpReply rep;
  asio::io_context io;
  asio::ip::tcp::socket s(io);
  s.connect({asio::ip::make_address("127.0.0.1"), port});
  std::string req = method + " " + path + " HTTP/1.1\r\n";
  if (send_content_length)
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  for (const auto& [k, v] : headers) req += k + ": " + v + "\r\n";
  req += "\r\n" + body;
  asio::write(s, asio::buffer(req));
  asio::error_code ec;
  std::string raw;
  char buf[8192];
  for (;;) {
    const std::size_t n = s.read_some(asio::buffer(buf), ec);
    if (ec) break;
    raw.append(buf, n);
  }
  const std::size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string::npos) return rep;
  std::istringstream in(raw.substr(0, head_end));
  std::string line;
  std::getline(in, line); // 状态行
  if (line.rfind("HTTP/1.1 ", 0) == 0) rep.status = std::atoi(line.c_str() + 9);
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string key = line.substr(0, colon);
      for (auto& c : key)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      rep.headers[key] = line.substr(colon + 2);
    }
  }
  rep.body = raw.substr(head_end + 4);
  return rep;
}

// JSON 取字段（nlohmann 对象键字典序，子串检索取首个即首条记录的字段）
std::string jstr(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto pos = j.find(k);
  if (pos == std::string::npos) return "";
  std::size_t i = pos + k.size();
  while (i < j.size() && (j[i] == ' ' || j[i] == '"')) ++i;
  std::string out;
  while (i < j.size() && j[i] != '"') out += j[i++];
  return out;
}
std::int64_t jint(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto pos = j.find(k);
  if (pos == std::string::npos) return 0;
  return std::atoll(j.c_str() + pos + k.size());
}

// 配额账本直查（走查断言用；空行视为失败防 optional 解引用 UB）
std::int64_t gq_used(ServerStore& s, const std::string& gid) {
  const auto q = s.get_group_quota(gid);
  CHECK(q.has_value());
  return q.has_value() ? q->used_bytes : -1;
}
std::int64_t uq_used(ServerStore& s, const std::string& uid) {
  const auto q = s.get_user_quota(uid);
  CHECK(q.has_value());
  return q.has_value() ? q->used_bytes : -1;
}

} // namespace

int main() {
  asio::io_context io;
  ServerStore store;
  CHECK(store.open(":memory:"));
  for (const char* a : {"owner1", "admin1", "member1", "outsider"}) {
    CHECK(store.create_account(a, "pw-" + std::string(a), a));
  }
  // 群 g1：owner1 群主，admin1 授管理员，member1 普通（角色走 store 层）
  const auto gid = store.create_group("dev", "owner1",
                                      {"owner1", "admin1", "member1"});
  CHECK(gid > 0);
  CHECK(store.group_set_role(gid, "admin1", "admin"));
  const std::string gtarget = "group:" + std::to_string(gid);

  auto fake = std::make_shared<FakeS3>();
  memex::server::FileServer files(io, store, fake, 0);
  const std::uint16_t port = files.port();
  files.start_accept();
  std::thread th([&io] { io.run(); });

  // —— 健康探针：无鉴权 GET 恒 200（容器 HEALTHCHECK 口径），不动方法/路径
  //    语义（POST /files/health 仍 404）——
  {
    const auto h = http(port, "GET", "/files/health", {}, "");
    CHECK(h.status == 200);
    CHECK(h.body.find("\"ok\":true") != std::string::npos);
    CHECK(http(port, "POST", "/files/health", {}, "").status == 404);
  }

  // —— 会话令牌：错口令/未知账号 401；四人各换 token ——
  CHECK(http(port, "POST", "/files/session", {},
             "{\"account\":\"member1\",\"password\":\"wrong\"}").status == 401);
  CHECK(http(port, "POST", "/files/session", {},
             "{\"account\":\"ghost\",\"password\":\"pw-member1\"}").status == 401);
  std::map<std::string, std::string> tok;
  for (const char* a : {"owner1", "admin1", "member1", "outsider"}) {
    const auto r = http(port, "POST", "/files/session", {},
                        "{\"account\":\"" + std::string(a) +
                            "\",\"password\":\"pw-" + std::string(a) + "\"}");
    CHECK(r.status == 200);
    tok[a] = jstr(r.body, "token");
    CHECK(!tok[a].empty());
  }
  const auto H = [&tok](const char* a) {
    return std::map<std::string, std::string>{
        {"Authorization", "Bearer " + tok.at(a)}};
  };

  // —— 上传：群成员可传；两级扣费（群+传者个人）；对象键=groups/{gid}/{hash} ——
  const std::string blob = "hello R23-2\n"; // 12 字节
  const auto blob_n = static_cast<std::int64_t>(blob.size());
  const auto up1 = http(port, "POST", "/files/upload?target=" + gtarget,
                        {{"Authorization", "Bearer " + tok["member1"]},
                         {"X-File-Name", "a.txt"}},
                        blob);
  CHECK(up1.status == 200);
  const auto id1 = jint(up1.body, "id");
  CHECK(id1 > 0);
  CHECK(up1.body.find("\"second_transfer\":false") != std::string::npos);
  const std::string gkey =
      "groups/" + std::to_string(gid) + "/" + memex::server::sha256_hex(blob);
  CHECK(fake->objects_.count(gkey) == 1);
  CHECK(gq_used(store, std::to_string(gid)) == blob_n);
  CHECK(uq_used(store, "member1") == blob_n);

  // —— 无令牌 401；非成员默认拒（default-deny，白名单口径）——
  CHECK(http(port, "POST", "/files/upload?target=" + gtarget,
             {{"X-File-Name", "x.txt"}}, blob).status == 401);
  const auto up_out = http(port, "POST", "/files/upload?target=" + gtarget,
                           {{"Authorization", "Bearer " + tok["outsider"]},
                            {"X-File-Name", "x.txt"}},
                           blob);
  CHECK(up_out.status == 403);
  CHECK(jstr(up_out.body, "error").find("default-deny") != std::string::npos);

  // —— 秒传：同属主+同哈希+同归属复用（同 id、不扣费、不再落字节）——
  const auto up2 = http(port, "POST", "/files/upload?target=" + gtarget,
                        {{"Authorization", "Bearer " + tok["member1"]},
                         {"X-File-Name", "b.txt"}},
                        blob);
  CHECK(up2.status == 200);
  CHECK(jint(up2.body, "id") == id1);
  CHECK(up2.body.find("\"second_transfer\":true") != std::string::npos);
  CHECK(gq_used(store, std::to_string(gid)) == blob_n);
  CHECK(fake->objects_.size() == 1);

  // —— 换目标＝新文件行（新对象键、两级再扣费）——
  const auto gid2 = store.create_group("dev2", "owner1", {"owner1", "member1"});
  CHECK(gid2 > 0);
  const auto up3 =
      http(port, "POST", "/files/upload?target=group:" + std::to_string(gid2),
           {{"Authorization", "Bearer " + tok["member1"]},
            {"X-File-Name", "a.txt"}},
           blob);
  CHECK(up3.status == 200);
  CHECK(jint(up3.body, "id") != id1);
  CHECK(gq_used(store, std::to_string(gid2)) == blob_n);
  CHECK(uq_used(store, "member1") == 2 * blob_n);

  // —— 下载：成员可读、字节一致、X-File-Name 回原名；非成员 403；未知 id 404 ——
  const auto dl1 = http(port, "GET",
                        "/files/download?id=" + std::to_string(id1),
                        H("member1"), "");
  CHECK(dl1.status == 200);
  CHECK(dl1.body == blob);
  CHECK(dl1.headers.at("x-file-name") == "a.txt");
  CHECK(http(port, "GET", "/files/download?id=" + std::to_string(id1),
             H("outsider"), "").status == 403);
  CHECK(http(port, "GET", "/files/download?id=99999", H("member1"), "")
            .status == 404);

  // —— 个人文件：仅本人可读；他人（含群主）403；只扣个人配额 ——
  const auto up_p = http(port, "POST", "/files/upload?target=me",
                         {{"Authorization", "Bearer " + tok["member1"]},
                          {"X-File-Name", "private.txt"}},
                         "my own bytes"); // 12 字节
  CHECK(up_p.status == 200);
  const auto idp = jint(up_p.body, "id");
  CHECK(http(port, "GET", "/files/download?id=" + std::to_string(idp),
             H("owner1"), "").status == 403);
  CHECK(http(port, "GET", "/files/download?id=" + std::to_string(idp),
             H("member1"), "").status == 200);
  CHECK(uq_used(store, "member1") == 2 * blob_n + 12);
  // 列表隔离：本人 me 列表恰该行；他人 me 列表（=查看者本人空间）不含他人文件
  const auto ls_me =
      http(port, "GET", "/files/list?target=me", H("member1"), "");
  CHECK(ls_me.status == 200);
  CHECK(jint(ls_me.body, "id") == idp);
  const auto ls_owner_me =
      http(port, "GET", "/files/list?target=me", H("owner1"), "");
  CHECK(ls_owner_me.status == 200);
  CHECK(ls_owner_me.body.find("private.txt") == std::string::npos);

  // —— 置顶：成员 403；管理员/群主可置；列表置顶行居首、pin 回真 ——
  const std::string pin_path =
      "/files/manage/pin?id=" + std::to_string(id1) + "&pin=1";
  CHECK(http(port, "POST", pin_path, H("member1"), "").status == 403);
  CHECK(http(port, "POST", pin_path, H("admin1"), "").status == 200);
  CHECK(http(port, "POST", pin_path, H("owner1"), "").status == 200);
  const auto ls_g =
      http(port, "GET", "/files/list?target=" + gtarget, H("member1"), "");
  CHECK(ls_g.status == 200);
  CHECK(jint(ls_g.body, "id") == id1);
  CHECK(ls_g.body.find("\"pin\":true") != std::string::npos);
  CHECK(http(port, "GET", "/files/list?target=" + gtarget, H("outsider"), "")
            .status == 403); // 非成员不可列
  // 分页参数生效（两行的 gid2 取 limit=1 只出一行）
  const auto ls_g2 = http(
      port, "GET",
      "/files/list?target=group:" + std::to_string(gid2) + "&limit=1&offset=0",
      H("member1"), "");
  CHECK(ls_g2.status == 200);
  CHECK(ls_g2.body.find("\"files\":[{") != std::string::npos);
  CHECK(ls_g2.body.find("},{") == std::string::npos); // 只有一行

  // —— 删除：成员（含传者本人）403（设计：群文件管理归群主/管理员）；
  //    管理员删除成功 → 引用归零删字节、两级退费 ——
  CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(id1),
             H("member1"), "").status == 403);
  CHECK(fake->objects_.count(gkey) == 1);
  CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(id1),
             H("admin1"), "").status == 200);
  CHECK(fake->objects_.count(gkey) == 0); // 引用归零 → 字节清
  CHECK(gq_used(store, std::to_string(gid)) == 0);
  CHECK(uq_used(store, "member1") == blob_n + 12);
  // 个人文件：本人可删；他人不可
  CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(idp),
             H("owner1"), "").status == 403);
  CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(idp),
             H("member1"), "").status == 200);

  // —— 配额：群主/管理员可设；成员 403；负数 400；受检扣费 60 进 50 拒 ——
  CHECK(http(port, "POST", "/files/manage/quota?target=" + gtarget,
             H("member1"), "{\"limit_bytes\":100}").status == 403);
  CHECK(http(port, "POST", "/files/manage/quota?target=" + gtarget,
             H("owner1"), "{\"limit_bytes\":-1}").status == 400);
  CHECK(http(port, "POST", "/files/manage/quota?target=" + gtarget,
             H("admin1"), "{\"limit_bytes\":100}").status == 200);
  const auto qg =
      http(port, "GET", "/files/quota?target=" + gtarget, H("member1"), "");
  CHECK(qg.status == 200);
  CHECK(jint(qg.body, "limit_bytes") == 100);
  const std::string b60(60, 'x');
  const std::string b50(50, 'o');
  const auto upq1 = http(port, "POST", "/files/upload?target=" + gtarget,
                         {{"Authorization", "Bearer " + tok["member1"]},
                          {"X-File-Name", "sixty.bin"}},
                         b60);
  CHECK(upq1.status == 200);
  const auto upq2 = http(port, "POST", "/files/upload?target=" + gtarget,
                         {{"Authorization", "Bearer " + tok["member1"]},
                          {"X-File-Name", "fifty.bin"}},
                         b50);
  CHECK(upq2.status == 413);
  CHECK(jstr(upq2.body, "error").find("配额不足") != std::string::npos);
  // 删 60 退费 → 50 可进（换群主传，个人配额独立不受 member1 账影响）
  const auto idq1 = jint(upq1.body, "id");
  CHECK(http(port, "POST", "/files/manage/delete?id=" + std::to_string(idq1),
             H("owner1"), "").status == 200);
  const auto upq3 = http(port, "POST", "/files/upload?target=" + gtarget,
                         {{"Authorization", "Bearer " + tok["owner1"]},
                          {"X-File-Name", "fifty.bin"}},
                         b50);
  CHECK(upq3.status == 200);
  const auto idq3 = jint(upq3.body, "id");
  CHECK(gq_used(store, std::to_string(gid)) == 50);

  // —— 隔离（R23-5 杀毒钩子落点）：读 403（显式拒）、秒传不命中、重传
  //     409 且字节保留（键仍被隔离行引用，引用计数不归零不删）——
  CHECK(store.set_file_status(idq3, ServerStore::FileStatus::Quarantine));
  const auto dlq = http(port, "GET",
                        "/files/download?id=" + std::to_string(idq3),
                        H("owner1"), "");
  CHECK(dlq.status == 403);
  CHECK(jstr(dlq.body, "error").find("deny:file-not-normal") !=
        std::string::npos);
  CHECK(http(port, "POST", "/files/upload?target=" + gtarget,
             {{"Authorization", "Bearer " + tok["owner1"]},
              {"X-File-Name", "fifty.bin"}},
             b50).status == 409);
  CHECK(fake->objects_.count("groups/" + std::to_string(gid) + "/" +
                             memex::server::sha256_hex(b50)) == 1);
  // 扣费照回滚：群账回到 50、owner 个人回到 50
  CHECK(gq_used(store, std::to_string(gid)) == 50);
  CHECK(uq_used(store, "owner1") == 50);

  // —— 落字节失败：502 且配额全回滚 ——
  fake->fail_next_put_ = true;
  const auto upf = http(port, "POST", "/files/upload?target=" + gtarget,
                        {{"Authorization", "Bearer " + tok["member1"]},
                         {"X-File-Name", "boom.bin"}},
                        std::string(30, 'z'));
  CHECK(upf.status == 502);
  const auto qg2 =
      http(port, "GET", "/files/quota?target=" + gtarget, H("member1"), "");
  CHECK(jint(qg2.body, "used_bytes") == 50);

  // —— R23-3 备忘录：本人建/查/改/删全链路（真 TCP）；他人 default-deny ——
  CHECK(http(port, "POST", "/files/memo", {},
             "{\"content\":\"x\"}").status == 401);
  const auto mc1 =
      http(port, "POST", "/files/memo", H("member1"),
           "{\"content\":\"first note\"}");
  CHECK(mc1.status == 200);
  const auto mid1 = jint(mc1.body, "id");
  CHECK(mid1 > 0);
  CHECK(http(port, "POST", "/files/memo", H("member1"), "{\"content\":\"second note\"}")
            .status == 200);
  // 空文 400；非法 JSON 400
  CHECK(http(port, "POST", "/files/memo", H("member1"), "{\"content\":\"\"}")
            .status == 400);
  CHECK(http(port, "POST", "/files/memo", H("member1"), "not-json").status == 400);
  // 单条读：本人 200；他人 403（default-deny）；不存在 404
  const auto mg =
      http(port, "GET", "/files/memo?id=" + std::to_string(mid1), H("member1"),
           "");
  CHECK(mg.status == 200);
  CHECK(jstr(mg.body, "content") == "first note");
  CHECK(http(port, "GET", "/files/memo?id=" + std::to_string(mid1), H("owner1"),
             "").status == 403);
  CHECK(http(port, "GET", "/files/memo?id=99999", H("member1"), "").status == 404);
  // 列表：本人两条；他人列表不串别人条目
  const auto mls = http(port, "GET", "/files/memo", H("member1"), "");
  CHECK(mls.status == 200);
  CHECK(mls.body.find("first note") != std::string::npos &&
        mls.body.find("second note") != std::string::npos);
  const auto mls_o = http(port, "GET", "/files/memo", H("owner1"), "");
  CHECK(mls_o.status == 200);
  CHECK(mls_o.body.find("first note") == std::string::npos);
  // 编辑：带 id POST；改后内容回读一致；他人编辑 403
  CHECK(http(port, "POST", "/files/memo", H("member1"),
             "{\"id\":" + std::to_string(mid1) +
                 ",\"content\":\"first note v2\"}").status == 200);
  CHECK(jstr(http(port, "GET", "/files/memo?id=" + std::to_string(mid1),
                  H("member1"), "").body,
             "content") == "first note v2");
  CHECK(http(port, "POST", "/files/memo", H("owner1"),
             "{\"id\":" + std::to_string(mid1) +
                 ",\"content\":\"hijack\"}").status == 403);
  // 删除：他人 403；本人 200；删后查 404
  CHECK(http(port, "DELETE",
             "/files/memo?id=" + std::to_string(mid1 + 1), H("owner1"), "")
            .status == 403);
  CHECK(http(port, "DELETE", "/files/memo?id=" + std::to_string(mid1 + 1),
             H("member1"), "").status == 200);
  CHECK(http(port, "GET", "/files/memo?id=" + std::to_string(mid1 + 1),
             H("member1"), "").status == 404);

  // —— R23-3 收件箱（手机发自己=文件传输）：target=inbox 落 users/{uid}/、
  //     kind=inbox；同哈希个人空间是另一行；inbox 列表=文件+备忘录混排 ——
  const std::string inbox_blob = "from my phone\n"; // 14 字节
  const auto upi = http(port, "POST", "/files/upload?target=inbox",
                        {{"Authorization", "Bearer " + tok["member1"]},
                         {"X-File-Name", "phone.jpg"}},
                        inbox_blob);
  CHECK(upi.status == 200);
  const auto idi = jint(upi.body, "id");
  CHECK(idi > 0);
  // 对象键仍 users/{uid}/{hash}（收件箱共用个人前缀，kind 区分空间）
  CHECK(fake->objects_.count("users/member1/" +
                             memex::server::sha256_hex(inbox_blob)) == 1);
  // 只扣个人配额：此前 member1 个人账 12（群文件两份在先、private.txt
  // 删除退过费、idq1 删除退过费），加 14
  CHECK(uq_used(store, "member1") == blob_n + 14);
  // 同哈希进个人空间 = 新行（kind 入秒传键，跨空间不串用）
  const auto upi2 = http(port, "POST", "/files/upload?target=me",
                         {{"Authorization", "Bearer " + tok["member1"]},
                          {"X-File-Name", "same-bytes-personal.jpg"}},
                         inbox_blob);
  CHECK(upi2.status == 200);
  CHECK(jint(upi2.body, "id") != idi);
  CHECK(uq_used(store, "member1") == blob_n + 28);
  // 收件箱内重传 = 秒传命中（同 id、不再落字节）
  const auto upi3 = http(port, "POST", "/files/upload?target=inbox",
                         {{"Authorization", "Bearer " + tok["member1"]},
                          {"X-File-Name", "phone.jpg"}},
                         inbox_blob);
  CHECK(upi3.status == 200);
  CHECK(jint(upi3.body, "id") == idi);
  CHECK(upi3.body.find("\"second_transfer\":true") != std::string::npos);
  CHECK(fake->objects_.count("users/member1/" +
                             memex::server::sha256_hex(inbox_blob)) == 1);
  // 收件箱文件本人可读、字节一致
  const auto dli = http(port, "GET", "/files/download?id=" + std::to_string(idi),
                        H("member1"), "");
  CHECK(dli.status == 200);
  CHECK(dli.body == inbox_blob);

  // inbox 列表混排时间序：备忘录（updated_ms）与文件（upload_ts）统一倒序。
  // 用 sleep 拉开毫秒级时间差，顺序可断言：旧备忘录 < 收件箱文件 < 新备忘录
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto lmb =
      http(port, "POST", "/files/memo", H("member1"),
           "{\"content\":\"third note\"}");
  CHECK(lmb.status == 200);
  const auto lsi =
      http(port, "GET", "/files/list?target=inbox", H("member1"), "");
  CHECK(lsi.status == 200);
  const auto p_memo = lsi.body.find("\"type\":\"memo\"");
  const auto p_file = lsi.body.find("\"type\":\"file\"");
  CHECK(p_memo != std::string::npos && p_file != std::string::npos);
  CHECK(p_memo < p_file); // 新备忘录在前
  CHECK(jint(lsi.body, "id") == jint(lmb.body, "id")); // 首条=最新备忘录
  CHECK(lsi.body.find("phone.jpg") != std::string::npos);
  CHECK(lsi.body.find("first note v2") != std::string::npos);
  // 分页在混排后取窗：limit=1 只出首条（新备忘录）
  const auto lsi1 = http(port, "GET", "/files/list?target=inbox&limit=1",
                         H("member1"), "");
  CHECK(lsi1.status == 200);
  CHECK(lsi1.body.find("third note") != std::string::npos);
  CHECK(lsi1.body.find("phone.jpg") == std::string::npos);

  // me 列表只出个人空间文件：不含收件箱行、无混排类型字段
  const auto ls_me2 =
      http(port, "GET", "/files/list?target=me", H("member1"), "");
  CHECK(ls_me2.status == 200);
  CHECK(ls_me2.body.find("same-bytes-personal.jpg") != std::string::npos);
  CHECK(ls_me2.body.find("phone.jpg") == std::string::npos);
  CHECK(ls_me2.body.find("\"type\":") == std::string::npos);
  CHECK(ls_me2.body.find("first note") == std::string::npos);

  // 配额视角：target=inbox 走个人配额（与 me 同账）
  const auto qgi = http(port, "GET", "/files/quota?target=inbox", H("member1"),
                        "");
  CHECK(qgi.status == 200);
  CHECK(jint(qgi.body, "used_bytes") == uq_used(store, "member1"));

  // —— 无 Content-Length 头：GET/DELETE 放行（curl/浏览器/Qt QNAM 的
  //     无体请求口径，R23-3 走查实录锁回归）；POST 仍 400 ——
  CHECK(http(port, "GET", "/files/list?target=me", H("member1"), "", false)
            .status == 200);
  CHECK(http(port, "GET", "/files/memo", H("member1"), "", false).status == 200);
  CHECK(http(port, "DELETE", "/files/memo?id=99999", H("member1"), "", false)
            .status == 404);
  CHECK(http(port, "POST", "/files/memo", H("member1"), "{\"content\":\"x\"}",
             false).status == 400);

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

  // —— R25-1 群工具框架：白名单配置（memo:config=群主/管理员）、
  //     成员可读清单、动作代理调用（stub 回显＋谁/何时/动作/参数留痕）、
  //     审计管理面——入群即授权/退群即失（file:read 群继承） ——
  {
    const std::string gids = std::to_string(gid);
    // config：未登录 401；普通成员 403；缺字段/坏 actions 400；
    // 幽灵群不透存在性（owner1 也 403）
    CHECK(http(port, "POST", "/files/group-tools/config", {},
               "{\"gid\":" + gids + ",\"tool\":\"ci\",\"actions\":[]}")
              .status == 401);
    CHECK(http(port, "POST", "/files/group-tools/config", H("member1"),
               "{\"gid\":" + gids + ",\"tool\":\"ci\",\"actions\":[]}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids + ",\"tool\":\"ci\"}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":0,\"tool\":\"ci\",\"actions\":[]}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids + ",\"tool\":\"\",\"actions\":[]}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"ci\",\"actions\":[\"deploy\",42]}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":999999,\"tool\":\"ci\",\"actions\":[]}")
              .status == 403);
    // 管理员可配置；upsert 覆盖（再设即替换旧清单）
    CHECK(http(port, "POST", "/files/group-tools/config", H("admin1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"ci\",\"actions\":[\"deploy\",\"rollback\"]}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"ci\",\"actions\":[\"deploy\",\"rollback\","
                   "\"status\"]}")
              .status == 200);
    // list：未登录 401；非成员 403；坏 gid 400；成员见最新清单（覆盖生效）
    CHECK(http(port, "GET", "/files/group-tools/list?gid=" + gids, {}, "")
              .status == 401);
    CHECK(http(port, "GET", "/files/group-tools/list?gid=" + gids,
               H("outsider"), "").status == 403);
    CHECK(http(port, "GET", "/files/group-tools/list?gid=x", H("member1"), "")
              .status == 400);
    const auto lt = http(port, "GET",
                         "/files/group-tools/list?gid=" + gids, H("member1"),
                         "");
    CHECK(lt.status == 200);
    CHECK(lt.body.find("\"tool\":\"ci\"") != std::string::npos);
    CHECK(lt.body.find("\"deploy\"") != std::string::npos);
    CHECK(lt.body.find("\"rollback\"") != std::string::npos);
    CHECK(lt.body.find("\"status\"") != std::string::npos);
    // call：字段校验（params 须对象）400；非成员 403；幽灵群 403；
    // 未配置工具 404（gid2 有成员无配置）
    CHECK(http(port, "POST", "/files/group-tools/call", H("member1"),
               "{\"gid\":" + gids + ",\"tool\":\"ci\",\"action\":\"deploy\","
                                    "\"params\":[1]}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-tools/call", H("outsider"),
               "{\"gid\":" + gids + ",\"tool\":\"ci\",\"action\":\"deploy\","
                                    "\"params\":{}}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-tools/call", H("owner1"),
               "{\"gid\":999999,\"tool\":\"ci\",\"action\":\"deploy\","
               "\"params\":{}}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-tools/call", H("member1"),
               "{\"gid\":" + std::to_string(gid2) +
                   ",\"tool\":\"nope\",\"action\":\"deploy\",\"params\":{}}")
              .status == 404);
    // 白名单外 403；白名单内 200 且 stub 回显带参数
    CHECK(http(port, "POST", "/files/group-tools/call", H("member1"),
               "{\"gid\":" + gids + ",\"tool\":\"ci\",\"action\":\"hack\","
                                    "\"params\":{}}")
              .status == 403);
    const auto c1 = http(port, "POST", "/files/group-tools/call", H("member1"),
                         "{\"gid\":" + gids +
                             ",\"tool\":\"ci\",\"action\":\"deploy\","
                             "\"params\":{\"env\":\"prod\",\"rev\":\"a1b2\"}}");
    CHECK(c1.status == 200);
    CHECK(c1.body.find("\"stub\":true") != std::string::npos);
    CHECK(c1.body.find("\"env\":\"prod\"") != std::string::npos);
    // 再调一笔（供审计倒序断言：后调的在前）
    CHECK(http(port, "POST", "/files/group-tools/call", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"ci\",\"action\":\"status\",\"params\":{}}")
              .status == 200);
    // audit：成员 403（管理面）；群主见两行且倒序（status 在 deploy 前）、
    // 行带 actor/params/结果
    CHECK(http(port, "GET", "/files/group-tools/audit?gid=" + gids,
               H("member1"), "").status == 403);
    const auto ta = http(port, "GET",
                         "/files/group-tools/audit?gid=" + gids, H("owner1"),
                         "");
    CHECK(ta.status == 200);
    CHECK(ta.body.find("\"actor\":\"member1\"") != std::string::npos);
    CHECK(ta.body.find("\"rev\":\"a1b2\"") != std::string::npos);
    const auto pos_status = ta.body.find("\"action\":\"status\"");
    const auto pos_deploy = ta.body.find("\"action\":\"deploy\"");
    CHECK(pos_status != std::string::npos && pos_deploy != std::string::npos);
    CHECK(pos_status < pos_deploy); // id DESC：最新在前
  }

  // —— R25-2 CI/CD 工具：流水线定义（memo:config）、红绿灯列表（成员）、
  //     触发走 R25-1 白名单闸＋谁触发留痕＋结果卡片回群（注桩捕获） ——
  {
    const std::string gids = std::to_string(gid);
    // 结果卡片回群面：注桩捕获（main 接线 deliver_notice，测试注桩验卡片）
    std::vector<std::string> notices;
    files.set_notice([&notices](const std::string& t, const std::string& ti,
                                const std::string& c, int u) {
      notices.push_back(t + "|" + ti + "|" + c + "|" + std::to_string(u));
    });
    // 流水线定义：未登录 401；成员 403；缺字段/name 空 400；管理员可建；
    // 幽灵群 403（存在性不透）
    CHECK(http(port, "POST", "/files/group-ci/pipeline", {},
               "{\"gid\":" + gids + ",\"name\":\"dev\"}")
              .status == 401);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("member1"),
               "{\"gid\":" + gids + ",\"name\":\"dev\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("owner1"),
               "{\"gid\":" + gids + "}").status == 400);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("owner1"),
               "{\"gid\":" + gids + ",\"name\":\"\"}").status == 400);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("admin1"),
               "{\"gid\":" + gids + ",\"name\":\"dev\","
                             "\"description\":\"主构建\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("owner1"),
               "{\"gid\":999999,\"name\":\"dev\"}").status == 403);
    // 列表（红绿灯面）：非成员 403；成员见定义且未跑时无 last_status
    CHECK(http(port, "GET", "/files/group-ci/list?gid=" + gids,
               H("outsider"), "").status == 403);
    const auto l0 = http(port, "GET",
                         "/files/group-ci/list?gid=" + gids, H("member1"), "");
    CHECK(l0.status == 200);
    CHECK(l0.body.find("\"name\":\"dev\"") != std::string::npos);
    CHECK(l0.body.find("last_status") == std::string::npos);
    // 触发：白名单未开放 403（ci 工具尚未配置）；非成员 403；幽灵流水线
    // 404（先开白名单）；stub 默认成功；params fail=true 出红
    CHECK(http(port, "POST", "/files/group-ci/trigger", H("member1"),
               "{\"gid\":" + gids + ",\"pipeline\":\"dev\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"ci\",\"actions\":[\"trigger\"]}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-ci/trigger", H("outsider"),
               "{\"gid\":" + gids + ",\"pipeline\":\"dev\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-ci/trigger", H("member1"),
               "{\"gid\":" + gids + ",\"pipeline\":\"ghost\"}")
              .status == 404);
    const auto t1 = http(port, "POST", "/files/group-ci/trigger",
                         H("member1"),
                         "{\"gid\":" + gids + ",\"pipeline\":\"dev\"}");
    CHECK(t1.status == 200);
    CHECK(t1.body.find("\"status\":\"success\"") != std::string::npos);
    CHECK(t1.body.find("\"actor\":\"member1\"") != std::string::npos);
    CHECK(jint(t1.body, "run_id") > 0);
    const auto t2 = http(port, "POST", "/files/group-ci/trigger", H("owner1"),
                         "{\"gid\":" + gids + ",\"pipeline\":\"dev\","
                                              "\"params\":{\"fail\":true}}");
    CHECK(t2.status == 200);
    CHECK(t2.body.find("\"status\":\"failed\"") != std::string::npos);
    // 红绿灯翻转：最近一笔失败→last_status=failed（绿翻红）
    const auto l1 = http(port, "GET",
                         "/files/group-ci/list?gid=" + gids, H("member1"), "");
    CHECK(l1.body.find("\"last_status\":\"failed\"") != std::string::npos);
    CHECK(l1.body.find("\"last_actor\":\"owner1\"") != std::string::npos);
    // 结果卡片回群：两笔触发两卡片；后笔失败在标题（红绿灯语义）
    CHECK(notices.size() == 2);
    CHECK(notices[0].find("group:" + gids) != std::string::npos);
    CHECK(notices[0].find("构建成功：dev") != std::string::npos);
    CHECK(notices[1].find("构建失败：dev") != std::string::npos);
    // run 历史：成员可读、id DESC（后触发的 failed 在前）、谁触发可回溯
    const auto rs = http(port, "GET",
                         "/files/group-ci/runs?gid=" + gids, H("member1"), "");
    CHECK(rs.status == 200);
    const auto pos_fail = rs.body.find("\"status\":\"failed\"");
    const auto pos_ok = rs.body.find("\"status\":\"success\"");
    CHECK(pos_fail != std::string::npos && pos_ok != std::string::npos);
    CHECK(pos_fail < pos_ok);
    CHECK(rs.body.find("\"actor\":\"member1\"") != std::string::npos);
    // 过滤：?pipeline=ghost 空集；坏 gid 400
    CHECK(http(port, "GET",
               "/files/group-ci/runs?gid=" + gids + "&pipeline=ghost",
               H("member1"), "").body.find("\"runs\":[]") !=
            std::string::npos);
    CHECK(http(port, "GET", "/files/group-ci/runs?gid=x", H("member1"), "")
              .status == 400);
    // 触发留痕进 R25-1 工具审计（tool=ci action=trigger 两行）
    const auto ca = http(port, "GET",
                         "/files/group-tools/audit?gid=" + gids, H("owner1"),
                         "");
    CHECK(ca.body.find("\"tool\":\"ci\"") != std::string::npos);
    CHECK(ca.body.find("\"action\":\"trigger\"") != std::string::npos);
    // 流水线删除：成员 403；管理员删 200；再触发 404
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("member1"),
               "{\"gid\":" + gids + ",\"name\":\"dev\",\"op\":\"delete\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-ci/pipeline", H("admin1"),
               "{\"gid\":" + gids + ",\"name\":\"dev\",\"op\":\"delete\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-ci/trigger", H("member1"),
               "{\"gid\":" + gids + ",\"pipeline\":\"dev\"}")
              .status == 404);
  }

  // —— R25-3 打包工具＋配置导出（首批动作类示例）——
  {
    const std::string gids = std::to_string(gid);
    // 打包卡片回群面：换新注桩捕获
    std::vector<std::string> notices;
    files.set_notice([&notices](const std::string& t, const std::string& ti,
                                const std::string& c, int u) {
      notices.push_back(t + "|" + ti);
    });
    // 打包：未登录 401；缺字段 400；非成员 403；白名单未开 403（成员也拒）
    CHECK(http(port, "POST", "/files/group-pack/build", {},
               "{\"gid\":" + gids + ",\"name\":\"app\",\"version\":\"1.0\"}")
              .status == 401);
    CHECK(http(port, "POST", "/files/group-pack/build", H("member1"),
               "{\"gid\":" + gids + ",\"name\":\"app\"}")
              .status == 400);
    CHECK(http(port, "POST", "/files/group-pack/build", H("outsider"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-pack/build", H("member1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\"}")
              .status == 403);
    // 开白名单（pack/build）→成员可打包（入群即授权）；同款重复=覆盖台账
    CHECK(http(port, "POST", "/files/group-tools/config", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"tool\":\"pack\",\"actions\":[\"build\"]}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-pack/build", H("member1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\","
                   "\"note\":\"首包\"}")
              .status == 200);
    CHECK(http(port, "POST", "/files/group-pack/build", H("member1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\","
                   "\"note\":\"重打\"}")
              .status == 200);
    // 卡片回群：两笔两卡片，标题带产物名与版本
    CHECK(notices.size() == 2);
    CHECK(notices[1].find("打包完成：app 1.0") != std::string::npos);
    // 台账：非成员 403；成员见一条（同款覆盖）且 created_by=member1
    CHECK(http(port, "GET", "/files/group-pack/list?gid=" + gids,
               H("outsider"), "").status == 403);
    const auto pl = http(port, "GET",
                         "/files/group-pack/list?gid=" + gids, H("member1"),
                         "");
    CHECK(pl.status == 200);
    CHECK(pl.body.find("\"name\":\"app\"") != std::string::npos);
    CHECK(pl.body.find("\"note\":\"重打\"") != std::string::npos);
    CHECK(pl.body.find("\"created_by\":\"member1\"") != std::string::npos);
    // 删除恒归管理员：成员 403；幽灵产物 404；管理员删 200
    CHECK(http(port, "POST", "/files/group-pack/delete", H("member1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\"}")
              .status == 403);
    CHECK(http(port, "POST", "/files/group-pack/delete", H("owner1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"ghost\",\"version\":\"9.9\"}")
              .status == 404);
    CHECK(http(port, "POST", "/files/group-pack/delete", H("admin1"),
               "{\"gid\":" + gids +
                   ",\"name\":\"app\",\"version\":\"1.0\"}")
              .status == 200);
    // 配置导出：管理面——成员 403；坏 gid 400；群主 200 快照带成员角色/
    // 工具白名单/备忘录开关/密码箱存在性；密文面永不进导出
    CHECK(http(port, "GET", "/files/group-export?gid=" + gids, H("member1"),
               "").status == 403);
    CHECK(http(port, "GET", "/files/group-export?gid=x", H("owner1"), "")
              .status == 400);
    const auto ex = http(port, "GET",
                         "/files/group-export?gid=" + gids, H("owner1"), "");
    CHECK(ex.status == 200);
    CHECK(ex.body.find("\"owner\":\"owner1\"") != std::string::npos);
    CHECK(ex.body.find("\"account\":\"member1\"") != std::string::npos);
    CHECK(ex.body.find("\"role\":\"admin\"") != std::string::npos);
    CHECK(ex.body.find("\"tool\":\"ci\"") != std::string::npos);
    CHECK(ex.body.find("\"tool\":\"pack\"") != std::string::npos);
    CHECK(ex.body.find("\"exists\":true") != std::string::npos);
    // 密文不进导出：包裹块/条目密文键一律不在快照
    CHECK(ex.body.find("wrapped_dek") == std::string::npos);
    CHECK(ex.body.find("secret_ct") == std::string::npos);
    CHECK(ex.body.find("kdf_salt") == std::string::npos);
    // 导出=敏感动作：进工具留痕（tool=export）
    const auto ea = http(port, "GET",
                         "/files/group-tools/audit?gid=" + gids, H("owner1"),
                         "");
    CHECK(ea.body.find("\"tool\":\"export\"") != std::string::npos);
  }

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
  std::unique_ptr<memex::server::FileServer> no_cred;
  std::unique_ptr<memex::server::FileServer> bare;
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

  io.stop();
  th.join();

  if (g_failures == 0) {
    std::cout << "test_files_api: all checks passed\n";
    return 0;
  }
  std::cout << "test_files_api: " << g_failures << " check(s) FAILED\n";
  return 1;
}
