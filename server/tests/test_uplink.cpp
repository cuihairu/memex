// R23-4 外网单向 uplink 验收（真实走查）：两套路由两套 scope、唯一落点=
// 文件助手收件箱、无任何下载/读取内网数据端点、上传全审计（uplink_logs
// 流水＋files.source=1）、删除只许自己的上传、默认关闭（实例级 uplink_mode
// 缺省 false；CLI --uplink-port 缺省 0 不创建监听）。对象层内存 Fake
//（铁律：字节全过本面；权限全在元数据层判）。
#include <asio.hpp>

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

// —— 内存对象存储 Fake（同 test_files_api 口径，注入无失败面）——
class FakeS3 final : public S3Storage {
 public:
  bool create_bucket() override { return true; }
  bool put_object(const std::string& key, const std::string& body,
                  std::string* etag_out) override {
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

  std::map<std::string, std::string> objects_;
};

// —— 阻塞 HTTP 客户端（真 TCP；Connection: close 读到对端关）——
struct HttpReply {
  int status{0};
  std::map<std::string, std::string> headers; // 小写键
  std::string body;
};

HttpReply http(std::uint16_t port, const std::string& method,
               const std::string& path,
               const std::map<std::string, std::string>& headers,
               const std::string& body) {
  HttpReply rep;
  asio::io_context io;
  asio::ip::tcp::socket s(io);
  s.connect({asio::ip::make_address("127.0.0.1"), port});
  std::string req = method + " " + path + " HTTP/1.1\r\n";
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
  std::getline(in, line);
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
bool jhas(const std::string& j, const char* needle) {
  return j.find(needle) != std::string::npos;
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
  for (const char* a : {"alice", "bob"}) {
    CHECK(store.create_account(a, "pw-" + std::string(a), a));
  }

  auto fake = std::make_shared<FakeS3>();
  // 内网面（默认 uplink_mode=false＝默认关闭外网路由）＋ uplink 面
  //（独立监听口；物理面分离）；会话库两面共享一份（生产双实例同款），
  // scope 闸才能认出跨面令牌；port=0 即临时端口
  const auto sessions = memex::server::make_file_sessions();
  memex::server::FileServer files(io, store, fake, 0, /*uplink_mode=*/false,
                                  sessions);
  memex::server::FileServer uplink(io, store, fake, 0, /*uplink_mode=*/true,
                                   sessions);
  const std::uint16_t fport = files.port();
  const std::uint16_t uport = uplink.port();
  files.start_accept();
  uplink.start_accept();
  std::thread th([&io] { io.run(); });

  // —— 健康探针两面均通（探活口径一致；health 无任何数据面语义）——
  CHECK(http(uport, "GET", "/uplink/health", {}, "").status == 200);
  CHECK(http(fport, "GET", "/files/health", {}, "").status == 200);

  // —— 两套路由：内网面无 /uplink/*；uplink 面无 /files/* ——
  CHECK(http(fport, "GET", "/uplink/mine", {}, "").status == 404);
  CHECK(http(uport, "GET", "/files/list?target=inbox", {}, "").status == 404);
  CHECK(http(uport, "GET", "/files/download?id=1", {}, "").status == 404);
  CHECK(http(uport, "GET", "/uplink/download?id=1", {}, "").status == 404);

  // —— 换会话：同源账号库；uplink 面 token scope=uplink ——
  CHECK(http(uport, "POST", "/uplink/session", {},
             "{\"account\":\"alice\",\"password\":\"wrong\"}").status == 401);
  const auto us_r = http(uport, "POST", "/uplink/session", {},
                         "{\"account\":\"alice\",\"password\":\"pw-alice\"}");
  CHECK(us_r.status == 200);
  CHECK(jhas(us_r.body, "\"scope\":\"uplink\""));
  const std::string ua = jstr(us_r.body, "token");
  const std::string ub = jstr(
      http(uport, "POST", "/uplink/session", {},
           "{\"account\":\"bob\",\"password\":\"pw-bob\"}").body,
      "token");
  CHECK(!ua.empty() && !ub.empty());
  // 内网面令牌（scope=internal）
  const auto fs_r = http(fport, "POST", "/files/session", {},
                         "{\"account\":\"alice\",\"password\":\"pw-alice\"}");
  CHECK(fs_r.status == 200);
  CHECK(jhas(fs_r.body, "\"scope\":\"internal\""));
  const std::string fi = jstr(fs_r.body, "token");
  CHECK(!fi.empty());

  // —— scope 两道闸：内网令牌打 uplink 面 403；uplink 令牌打内网面读 403 ——
  CHECK(http(uport, "POST", "/uplink/upload",
             {{"Authorization", "Bearer " + fi}}, "x").status == 403);
  CHECK(http(fport, "GET", "/files/list?target=inbox",
             {{"Authorization", "Bearer " + ua}}, "").status == 403);
  CHECK(http(fport, "POST", "/files/upload?target=inbox",
             {{"Authorization", "Bearer " + ua}}, "x").status == 403);
  // 无令牌 401
  CHECK(http(uport, "GET", "/uplink/mine", {}, "").status == 401);

  // —— 上传：唯一落点=文件助手收件箱（target 参数不收）＋审计流水＋配额 ——
  const std::string blob = "uplink bytes\n";
  const auto blob_n = static_cast<std::int64_t>(blob.size());
  const auto up = http(uport, "POST", "/uplink/upload?target=group:99",
                       {{"Authorization", "Bearer " + ua},
                        {"X-File-Name", "外网件.txt"}},
                       blob);
  CHECK(up.status == 200);
  const auto uid1 = jint(up.body, "id");
  CHECK(uid1 > 0);
  const auto meta1 = store.file_by_id(uid1);
  CHECK(meta1.has_value());
  if (meta1.has_value()) {
    CHECK(meta1->source == ServerStore::FileSource::Uplink); // 来源=外网
    CHECK(meta1->kind == ServerStore::FileKind::Inbox);      // 收件箱落点
    CHECK(meta1->owner == "alice");
    CHECK(meta1->belong_uid == "alice");
  }
  const auto logs = store.list_uplink_logs("alice", 100);
  CHECK(logs.size() == 1);
  if (!logs.empty()) {
    CHECK(logs[0].file_name == "外网件.txt");
    CHECK(logs[0].file_size == blob_n);
    CHECK(!logs[0].object_key.empty());
  }
  CHECK(uq_used(store, "alice") == blob_n); // 收件箱同款个人配额扣费

  // 秒传（同属主同哈希同类目复用既有行——不建新行、不重复扣费），审计
  // 流水照记（流水按受理计，文件行只有一条）
  const auto up2 = http(uport, "POST", "/uplink/upload",
                        {{"Authorization", "Bearer " + ua},
                         {"X-File-Name", "外网件-再来.txt"}},
                        blob);
  CHECK(up2.status == 200);
  CHECK(jhas(up2.body, "\"second_transfer\":true"));
  CHECK(jint(up2.body, "id") == uid1); // 复用同一文件行
  CHECK(uq_used(store, "alice") == blob_n); // 未二次扣费
  CHECK(store.list_uplink_logs("alice", 100).size() == 2);

  // —— /uplink/mine：只列自己的 uplink 上传（内网收件箱文件不可见）——
  // 先在内网面传一份 alice 的 inbox 文件（source=internal）
  const std::string inner = "inner inbox\n";
  const auto in_up = http(fport, "POST", "/files/upload?target=inbox",
                          {{"Authorization", "Bearer " + fi},
                           {"X-File-Name", "内网件.txt"}},
                          inner);
  CHECK(in_up.status == 200);
  const auto in_id = jint(in_up.body, "id");
  CHECK(in_id > 0);

  const auto mine = http(uport, "GET", "/uplink/mine",
                         {{"Authorization", "Bearer " + ua}}, "");
  CHECK(mine.status == 200);
  CHECK(jhas(mine.body, "外网件.txt"));
  CHECK(!jhas(mine.body, "外网件-再来.txt")); // 秒传不建新行：记录仍是原行
  CHECK(!jhas(mine.body, "内网件.txt")); // 内网文件不入外网记录

  // bob 的记录：mine 里只有 bob 自己的（他还没有上传＝空）
  const auto mine_b = http(uport, "GET", "/uplink/mine",
                           {{"Authorization", "Bearer " + ub}}, "");
  CHECK(mine_b.status == 200);
  CHECK(!jhas(mine_b.body, "外网件.txt"));

  // —— 删除：只许删自己的 uplink 上传 ——
  // 内网文件 id：403（不借道内网判权）
  CHECK(http(uport, "POST", "/uplink/delete?id=" + std::to_string(in_id),
             {{"Authorization", "Bearer " + ua}}, "").status == 403);
  // 他人记录：403（bob 尝试删 alice 的）
  CHECK(http(uport, "POST", "/uplink/delete?id=" + std::to_string(uid1),
             {{"Authorization", "Bearer " + ub}}, "").status == 403);
  // 自己的：成功（配额退还、字节按引用计数清理）
  const auto del = http(uport, "POST", "/uplink/delete?id=" + std::to_string(uid1),
                        {{"Authorization", "Bearer " + ua}}, "");
  CHECK(del.status == 200);
  CHECK(!store.file_by_id(uid1).has_value());
  CHECK(uq_used(store, "alice") ==
        static_cast<std::int64_t>(inner.size())); // uplink 行退费；内网
  // 收件箱文件行（in_id）配额照旧
  // 字节面：键无引用（秒传只是流水，不建第二行）→ uplink 对象已删；
  // 内网收件箱文件的对象（另一键）不受牵连
  CHECK(fake->objects_.size() == 1);
  CHECK(fake->objects_.count("users/alice/" + memex::server::sha256_hex(blob)) ==
        0);
  // 不存在 id：404；非数字 id：400
  CHECK(http(uport, "POST", "/uplink/delete?id=999999",
             {{"Authorization", "Bearer " + ua}}, "").status == 404);
  CHECK(http(uport, "POST", "/uplink/delete?id=abc",
             {{"Authorization", "Bearer " + ua}}, "").status == 400);

  io.stop();
  th.join();

  if (g_failures == 0) {
    std::cout << "R23-4 uplink 验收全部通过\n";
    return 0;
  }
  std::cerr << g_failures << " 处断言失败\n";
  return 1;
}
