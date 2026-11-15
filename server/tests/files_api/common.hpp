// test_files_api 共性面（LAY-005 自 test_files_api.cpp 匿名空间逐字搬移）。
// 自由函数加 inline 供多 TU 包含；g_failures 改 inline 变量；其余逐字。
#pragma once

#include <asio.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sqlite3.h>
#include <unistd.h>

#include "cred.hpp"
#include "files_server.hpp"
#include "storage.hpp"
#include "store.hpp"

namespace memex::server::tests_api {

inline int g_failures = 0;

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

// CR-2：网络侧整数查询参数溢出防护——≥20 位数字串 atoi 溢出回绕（glibc
// strtol 饱和 LONG_MAX→int 回绕 -1），绝不能漏成 SQLite LIMIT -1（无限制
// 整表）或负数进存储层；safe_int_param 必须拒/夹，且不误伤合法值。
inline void test_safe_int_param() {
  using memex::server::safe_int_param;
  CHECK(safe_int_param("", 200, 1000) == 200);             // 空→默认
  CHECK(safe_int_param("abc", 200, 1000) == 200);          // 非数字→默认
  CHECK(safe_int_param("0", 200, 1000) == 0);              // 合法 0 保留
  CHECK(safe_int_param("3", 200, 1000) == 3);              // 合法小值保留
  CHECK(safe_int_param("1500", 200, 1000) == 1000);        // 超上限→夹到上限
  CHECK(safe_int_param("999999999", 200, 1000) == 1000);   // 9 位仍夹
  CHECK(safe_int_param("1000000000", 200, 1000) == 200);   // 10 位→回退默认
  CHECK(safe_int_param("99999999999999999999", 200, 1000) == 200);  // CR-2 触发串
  CHECK(safe_int_param("999999999999999999999999", 0, 1000) == 0);   // 恒非负
  CHECK(safe_int_param("12345678901234567890", 100, 500) == 100);    // 恒非负
}

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

inline HttpReply http(std::uint16_t port, const std::string& method,
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
inline std::string jstr(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto pos = j.find(k);
  if (pos == std::string::npos) return "";
  std::size_t i = pos + k.size();
  while (i < j.size() && (j[i] == ' ' || j[i] == '"')) ++i;
  std::string out;
  while (i < j.size() && j[i] != '"') out += j[i++];
  return out;
}
inline std::int64_t jint(const std::string& j, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  const auto pos = j.find(k);
  if (pos == std::string::npos) return 0;
  return std::atoll(j.c_str() + pos + k.size());
}

// 配额账本直查（走查断言用；空行视为失败防 optional 解引用 UB）
inline std::int64_t gq_used(ServerStore& s, const std::string& gid) {
  const auto q = s.get_group_quota(gid);
  CHECK(q.has_value());
  return q.has_value() ? q->used_bytes : -1;
}
inline std::int64_t uq_used(ServerStore& s, const std::string& uid) {
  const auto q = s.get_user_quota(uid);
  CHECK(q.has_value());
  return q.has_value() ? q->used_bytes : -1;
}

// —— LAY-005 夹具：原 main() 栈上的跨模块共享状态。files 经 unique_ptr
//    持有（成员函数集与原栈对象一致）；H 为原 lambda 改成员函数（语义逐字）。
//    no_cred/bare 声明在结构体末尾：须活到 io 停摆之后（裸 this 链式
//    accept，作用域内析构＝悬空完成回调，见原文件 cred_servers 段注释）。
struct Fx {
  ServerStore store;
  asio::io_context io;
  std::shared_ptr<FakeS3> fake;
  std::unique_ptr<memex::server::FileServer> files;
  std::unique_ptr<memex::server::FileServer> no_cred;
  std::unique_ptr<memex::server::FileServer> bare;
  std::uint16_t port{0};
  std::map<std::string, std::string> tok;
  std::uint64_t gid{0};
  std::string gtarget;
  std::uint64_t gid2{0};
  std::int64_t idp{0};
  std::map<std::string, std::string> H(const char* a) const {
    return {{"Authorization", "Bearer " + tok.at(a)}};
  }
};

void files_suite(Fx& f);
void group_docs_suite(Fx& f);
void tools_suite(Fx& f);
void cred_servers_suite(Fx& f);
void platform_suite(Fx& f);
void tasks_suite(Fx& f);
void audit_brand_suite(Fx& f);
void assist_suite(Fx& f);

} // namespace memex::server::tests_api
