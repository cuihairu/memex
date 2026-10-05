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

  // —— 存储未配置：面在、字节面 503、元数据面照常 ——
  {
    memex::server::FileServer bare(io, store, nullptr, 0);
    const std::uint16_t bport = bare.port();
    bare.start_accept();
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
