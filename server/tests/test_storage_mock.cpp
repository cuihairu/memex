// S3Storage 全接口验收（本地 mock S3 兼容层）：起最小 S3 mock HTTP 服务，
// AWS SDK 以真实 SigV4 签名路径打到 127.0.0.1（endpointOverride + path-
// style）——存储实现类的每个 SDK 调用与错误分支都过真网络：桶引导幂等、
// put/get 往返、404/500 错误腿、列表翻页（IsTruncated+token）与 max 截断、
// 分片上传（发起/两片/合并/中止/坏 uploadId）、HeadObject、批量删、预签名
// URL；RustFSCompose 纯逻辑腿（compose 文案/tmp 落盘/默认凭据告警）；
// files verify CLI 进程级腿（list 中途失败→退出码 1 不判通过）。
// docker compose up/down 是外部进程依赖（CI 无 docker），不入测。
#include <asio.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "storage.hpp"

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

const char* kBucket = "memex-files";

std::string url_decode(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(),
                                                  nullptr, 16)));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::map<std::string, std::string> parse_query(const std::string& q) {
  std::map<std::string, std::string> out;
  std::size_t pos = 0;
  for (;;) {
    const auto amp = q.find('&', pos);
    const std::string item =
        q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    if (!item.empty()) {
      const auto eq = item.find('=');
      if (eq == std::string::npos) {
        out[url_decode(item)] = "";
      } else {
        out[url_decode(item.substr(0, eq))] = url_decode(item.substr(eq + 1));
      }
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return out;
}

// —— 最小 S3 兼容 mock（path-style；不验签；bucket 固定 kBucket）——
class MockS3 {
 public:
  void start() {
    asio::ip::tcp::acceptor acc(io_, {asio::ip::make_address("127.0.0.1"), 0});
    port_ = acc.local_endpoint().port();
    std::thread([acc = std::move(acc), this]() mutable {
      for (;;) {
        asio::ip::tcp::socket s(io_);
        acc.accept(s);
        if (std::getenv("MEMEX_MOCK_DEBUG") != nullptr)
          std::cerr << "[MOCKDEBUG] accepted t="
                    << std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now()
                               .time_since_epoch())
                           .count()
                    << '\n';
        std::thread(&MockS3::serve, this, asio::ip::tcp::socket(std::move(s)))
            .detach();
      }
    }).detach();
  }
  std::uint16_t port() const { return port_; }

  std::map<std::string, std::string> objects; // key -> body
  std::atomic<int> list_calls{0};
  // 全前缀 list 故障注入（files verify 假绿腿）：true 时 ListObjectsV2 恒 500
  std::atomic<bool> fail_all_lists{false};
  // CreateBucket 分档（桶引导所有权腿，codex-759）：0=200 建成；1=409
  // BucketAlreadyOwnedByYou；2=409 BucketAlreadyExists（探针键放行＝可控）；
  // 3=409 BucketAlreadyExists＋探针键 403＝非当前凭据可控
  std::atomic<int> create_bucket_mode{0};
  std::atomic<int> probe_puts{0}; // 探针键 PUT 计数（探针真走到断言）

 private:
  static void reply(asio::ip::tcp::socket& s, int status,
                    const std::map<std::string, std::string>& hdrs,
                    const std::string& body) {
    const char* phrase = status == 200 ? "OK"
                         : status == 204 ? "No Content"
                         : status == 403 ? "Forbidden"
                         : status == 404 ? "Not Found"
                         : status == 409 ? "Conflict"
                                         : "Internal Error";
    std::string r = "HTTP/1.1 " + std::to_string(status) + ' ' + phrase +
                    "\r\n";
    for (const auto& [k, v] : hdrs) r += k + ": " + v + "\r\n";
    // HeadObject 从 hdrs 传入对象大小——不能重复追加第二个 Content-Length
    if (hdrs.count("Content-Length") == 0)
      r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Connection: close\r\n\r\n" + body;
    asio::error_code ec;
    asio::write(s, asio::buffer(r), ec);
    asio::error_code shut_ec;
    s.shutdown(asio::ip::tcp::socket::shutdown_both, shut_ec);
  }

  // aws-chunked 解帧（STREAMING-*-PAYLOAD-TRAILER：`<hex>[;ext]\r\n
  // <data>\r\n` … `0[;ext]\r\n<trailer 头…>`；还原纯 payload）
  static std::string decode_aws_chunked(const std::string& in) {
    std::string out;
    std::size_t i = 0;
    for (;;) {
      const auto eol = in.find("\r\n", i);
      if (eol == std::string::npos) break;
      std::string head = in.substr(i, eol - i);
      const auto semi = head.find(';');
      if (semi != std::string::npos) head = head.substr(0, semi);
      const std::size_t size = std::strtoul(head.c_str(), nullptr, 16);
      i = eol + 2;
      if (size == 0) break; // 0 行后全是 trailer 头
      if (i + size > in.size()) break;
      out.append(in, i, size);
      i += size + 2; // 跳过 data 后的 \r\n
    }
    return out;
  }

  // HTTP chunked 增量读取（无 Content-Length 时；迭代到终止 0 行）
  static bool http_chunked_complete(const std::string& raw,
                                    std::size_t body_start) {
    std::size_t i = body_start;
    for (;;) {
      const auto eol = raw.find("\r\n", i);
      if (eol == std::string::npos) return false;
      const std::string hex = raw.substr(i, eol - i);
      const std::size_t size =
          std::strtoul(hex.substr(0, hex.find(';')).c_str(), nullptr, 16);
      i = eol + 2 + size + 2; // size 行 + data + 尾 \r\n
      if (size == 0) return true;
      if (i > raw.size()) return false;
    }
  }

  void serve(asio::ip::tcp::socket s) {
    std::string raw;
    char buf[16384];
    std::size_t head_end = std::string::npos;
    for (;;) {
      asio::error_code ec;
      const std::size_t n = s.read_some(asio::buffer(buf), ec);
      if (ec) return;
      raw.append(buf, n);
      head_end = raw.find("\r\n\r\n");
      if (head_end != std::string::npos) break;
    }
    // 解析请求行与头
    std::istringstream in(raw.substr(0, head_end));
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream rl(line);
    std::string method, target, proto;
    rl >> method >> target >> proto;
    std::map<std::string, std::string> hdrs;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const auto colon = line.find(':');
      if (colon != std::string::npos) {
        std::string k = line.substr(0, colon);
        for (auto& c : k) c = static_cast<char>(std::tolower(
                                    static_cast<unsigned char>(c)));
        hdrs[k] = line.substr(colon + 2);
      }
    }
    // Expect: 100-continue——先回 1xx，SDK 才会发 body
    if (hdrs.count("expect") > 0) {
      // 注意：asio::buffer(字面量) 会把结尾 '\0' 一并发出去（数组全长），
      // curl 收到 NUL 尾巴后头解析中止、body 不发——必须按显式长度给缓冲
      static const std::string kContinue = "HTTP/1.1 100 Continue\r\n\r\n";
      asio::error_code ec;
      asio::write(s, asio::buffer(kContinue), ec);
      if (std::getenv("MEMEX_MOCK_DEBUG") != nullptr) {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        std::cerr << "[MOCKDEBUG] sent-100 ec=" << ec.message() << " method="
                  << method << " t="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         now)
                         .count()
                  << '\n';
      }
    }
    std::size_t want = 0;
    const auto cl = hdrs.find("content-length");
    if (cl != hdrs.end()) want = std::strtoul(cl->second.c_str(), nullptr, 10);
    const bool http_chunked = want == 0 &&
        hdrs.count("transfer-encoding") > 0 &&
        hdrs.at("transfer-encoding").find("chunked") != std::string::npos;
    while (http_chunked
               ? !http_chunked_complete(raw, head_end + 4)
               : raw.size() < head_end + 4 + want) {
      asio::error_code ec;
      const std::size_t n = s.read_some(asio::buffer(buf), ec);
      if (ec) {
        if (std::getenv("MEMEX_MOCK_DEBUG") != nullptr) {
          std::string hex;
          const std::string tail =
              raw.substr(raw.size() > 160 ? raw.size() - 160 : 0);
          for (char c : tail) {
            char tmp[8];
            std::snprintf(tmp, sizeof(tmp), "%02x",
                          static_cast<unsigned char>(c));
            hex += tmp;
          }
          std::cerr << "[MOCKDEBUG] read ec=" << ec.message() << " method="
                    << method << " rawlen=" << raw.size() << "\n  raw-ascii="
                    << raw.substr(0, 1400) << '\n';
        }
        return;
      }
      raw.append(buf, n);
    }
    // HTTP chunked 解帧（拼接各段；终止 0 行后可能还有 trailer）
    std::string body;
    if (http_chunked) {
      std::size_t i = head_end + 4;
      for (;;) {
        const auto eol = raw.find("\r\n", i);
        if (eol == std::string::npos) break;
        const std::string hex = raw.substr(i, eol - i);
        const std::size_t n =
            std::strtoul(hex.substr(0, hex.find(';')).c_str(), nullptr, 16);
        i = eol + 2;
        if (n == 0) break;
        body.append(raw, i, n);
        i += n + 2;
      }
    } else {
      body = raw.substr(head_end + 4);
    }
    // aws-chunked 解帧（SDK 对 PutObject/UploadPart 默认 flexcheck trailer）
    const auto csha = hdrs.find("x-amz-content-sha256");
    if (csha != hdrs.end() &&
        csha->second.find("STREAMING") != std::string::npos) {
      body = decode_aws_chunked(body);
    }

    const auto qpos = target.find('?');
    const std::string path = target.substr(0, qpos);
    const std::map<std::string, std::string> query =
        parse_query(qpos == std::string::npos ? "" : target.substr(qpos + 1));
    // path 形如 /memex-files 或 /memex-files/{key}
    const std::string prefix = "/" + std::string(kBucket);
    std::string key;
    if (path.rfind(prefix, 0) == 0 && path.size() > prefix.size())
      key = url_decode(path.substr(prefix.size() + 1));

    // —— 桶级 ——
    if (key.empty()) {
      if (method == "HEAD") { // HeadBucket：created ? 200 : 404
        reply(s, bucket_created_ ? 200 : 404, {}, "");
        return;
      }
      if (method == "PUT") { // CreateBucket：mode 分档（见成员注释）
        if (create_bucket_mode == 0) {
          bucket_created_ = true;
          reply(s, 200, {}, "");
          return;
        }
        const char* code = create_bucket_mode == 1
                               ? "BucketAlreadyOwnedByYou"
                               : "BucketAlreadyExists";
        reply(s, 409, {},
              std::string("<Error><Code>") + code + "</Code></Error>");
        return;
      }
      if (method == "GET") { // ListObjectsV2：prefix 过滤排序；paged/ 强制
                             // 每页 2 条 + truncated（踩 SDK token 翻页腿）
        if (fail_all_lists ||
            (query.count("prefix") > 0 &&
             query.at("prefix").rfind("fail500", 0) == 0)) {
          reply(s, 500, {}, "<Error><Code>InternalError</Code></Error>");
          return;
        }
        std::string prefix;
        const auto pf = query.find("prefix");
        if (pf != query.end()) prefix = pf->second;
        std::vector<std::string> keys;
        for (const auto& [k, v] : objects) {
          (void)v;
          if (k.rfind(prefix, 0) == 0) keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end());
        int per_page = static_cast<int>(keys.size());
        std::string token;
        const auto tk = query.find("continuation-token");
        if (tk != query.end()) token = tk->second;
        const bool paged = prefix.rfind("paged", 0) == 0;
        if (paged) per_page = 2;
        std::size_t start =
            token.empty() ? 0 : static_cast<std::size_t>(
                                    std::strtoul(token.c_str(), nullptr, 10) *
                                    static_cast<unsigned long>(per_page));
        std::vector<std::string> page;
        for (std::size_t i = start;
             i < keys.size() && page.size() < static_cast<std::size_t>(
                                                        per_page > 0
                                                            ? per_page
                                                            : 1);
             ++i)
          page.push_back(keys[i]);
        const bool truncated = (start + page.size()) < keys.size() && paged;
        std::string xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/"
            "2006-03-01/\"><Name>" +
            std::string(kBucket) + "</Name><Prefix>" + prefix +
            "</Prefix><KeyCount>" + std::to_string(page.size()) +
            "</KeyCount><MaxKeys>" + std::to_string(per_page > 0 ? per_page
                                                                 : 1000) +
            "</MaxKeys><IsTruncated>" + (truncated ? "true" : "false") +
            "</IsTruncated>";
        for (const auto& k : page) xml += "<Contents><Key>" + k +
                                          "</Key></Contents>";
        if (truncated) xml += "<NextContinuationToken>" +
                              std::to_string((start + page.size()) /
                                             static_cast<std::size_t>(
                                                 per_page > 0 ? per_page : 1)) +
                              "</NextContinuationToken>";
        xml += "</ListBucketResult>";
        reply(s, 200, {{"Content-Type", "application/xml"}}, xml);
        return;
      }
      if (method == "POST") { // DeleteObjects（批量；按 body 的 Key 擦表）
        std::string xml = "<DeleteResult>";
        for (std::size_t p = 0; (p = body.find("<Key>", p)) !=
                                std::string::npos;) {
          const auto e = body.find("</Key>", p);
          if (e == std::string::npos) break;
          const std::string k = body.substr(p + 5, e - p - 5);
          objects.erase(k);
          xml += "<Deleted><Key>" + k + "</Key></Deleted>";
          p = e;
        }
        xml += "</DeleteResult>";
        reply(s, 200, {{"Content-Type", "application/xml"}}, xml);
        return;
      }
    }

    // —— 对象级；统一故障注入：fail500 前缀一切 500 ——
    if (key.rfind("fail500", 0) == 0) {
      reply(s, 500, {}, "<Error><Code>InternalError</Code></Error>");
      return;
    }
    // 桶引导探针拒绝腿（mode 3）：所有权探针键一切 403（非本凭据可控）
    if (create_bucket_mode == 3 && key == ".memex-bootstrap-probe") {
      reply(s, 403, {}, "<Error><Code>AccessDenied</Code></Error>");
      return;
    }

    if (method == "PUT") {
      if (query.count("uploadId") > 0) { // UploadPart
        reply(s, 200, {{"ETag", "\"part-etag\""}}, "");
        return;
      }
      if (std::getenv("MEMEX_MOCK_DEBUG") != nullptr) {
        const std::string enc =
            hdrs.count("x-amz-content-sha256") ? hdrs.at("x-amz-content-sha256")
                                               : "";
        std::string allhdr;
        for (const auto& [hk, hv] : hdrs)
          allhdr += hk + "=" + hv.substr(0, 40) + " ";
        std::string hex;
        for (std::size_t i = 0; i < body.size() && i < 60; ++i) {
          char tmp[8];
          std::snprintf(tmp, sizeof(tmp), "%02x ",
                        static_cast<unsigned char>(body[i]));
          hex += tmp;
        }
        std::cerr << "[MOCKDEBUG] put key=" << key << " size=" << body.size()
                  << " sha=" << enc << "\n  hdrs: " << allhdr
                  << "\n  bodyhex: " << hex << '\n';
      }
      objects[key] = body; // PutObject
      if (key == ".memex-bootstrap-probe") ++probe_puts; // 所有权探针计数
      reply(s, 200, {{"ETag", "\"etag-" + key + "\""}}, "");
      return;
    }
    if (method == "POST") {
      if (query.count("uploads") > 0) { // CreateMultipartUpload
        reply(s, 200, {{"Content-Type", "application/xml"}},
              "<InitiateMultipartUploadResult><Bucket>" +
                  std::string(kBucket) + "</Bucket><Key>" + key +
                  "</Key><UploadId>uid-1</UploadId>"
                  "</InitiateMultipartUploadResult>");
        return;
      }
      if (query.count("uploadId") > 0) { // CompleteMultipartUpload
        if (query.at("uploadId") != "uid-1") {
          reply(s, 404, {},
                "<Error><Code>NoSuchUpload</Code></Error>");
          return;
        }
        reply(s, 200, {{"Content-Type", "application/xml"}},
              "<CompleteMultipartUploadResult>"
              "<Location>http://127.0.0.1/" + std::string(kBucket) + "/" +
                  key + "</Location><Bucket>" + std::string(kBucket) +
                  "</Bucket><Key>" + key + "</Key><ETag>\"multi-etag\""
                  "</ETag></CompleteMultipartUploadResult>");
        return;
      }
    }
    if (method == "GET") { // GetObject
      const auto it = objects.find(key);
      if (it == objects.end()) {
        reply(s, 404, {},
              "<Error><Code>NoSuchKey</Code></Error>");
        return;
      }
      reply(s, 200, {{"Content-Type", "application/octet-stream"},
                     {"ETag", "\"etag-" + key + "\""}},
            it->second);
      return;
    }
    if (method == "HEAD") { // HeadObject
      const auto it = objects.find(key);
      if (it == objects.end()) {
        reply(s, 404, {}, "");
        return;
      }
      reply(s, 200, {{"ETag", "\"etag-" + key + "\""},
                     {"Content-Length", std::to_string(it->second.size())}},
            "");
      return;
    }
    if (method == "DELETE") {
      if (query.count("uploadId") > 0) { // AbortMultipartUpload 恒 204
        reply(s, 204, {}, "");
        return;
      }
      if (objects.erase(key) == 0) { // 不存在回 404（故障腿用）
        reply(s, 404, {}, "<Error><Code>NoSuchKey</Code></Error>");
        return;
      }
      reply(s, 204, {}, "");
      return;
    }
    reply(s, 400, {}, "");
  }

  asio::io_context io_;
  std::uint16_t port_ = 0;
  bool bucket_created_ = false;
};

} // namespace

#ifndef MEMEX_SERVER_BIN
#error "MEMEX_SERVER_BIN 未定义（应传入 $<TARGET_FILE:memex_server>）"
#endif

namespace {

// 唯一临时目录（mkdtemp 0700）＋进程退出自动清理——固定 /tmp 路径在并行
// 跑同套件/异常中断时会竞态互踩、留残渣
struct TempDir {
  std::string path;
  TempDir() {
    std::string tmpl =
        (std::filesystem::temp_directory_path() /
         ("memex-storage-mock-" + std::to_string(::getpid()) + "-XXXXXX"))
            .string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (char* d = ::mkdtemp(buf.data())) path = d;
  }
  ~TempDir() {
    if (!path.empty()) std::filesystem::remove_all(path);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

// CLI 进程级腿：真 memex_server files verify 打到本 mock（退出码＋全输出）
int run_files_verify(const std::string& db, std::uint16_t port,
                     std::string* out) {
  const std::string log = db + ".verify.log";
  const std::string cmd =
      std::string("'" MEMEX_SERVER_BIN "' files verify --db '") + db +
      "' --s3-endpoint 'http://127.0.0.1:" + std::to_string(port) +
      "' --s3-bucket " + kBucket +
      " --s3-access-key test-ak --s3-secret-key test-sk > '" + log + "' 2>&1";
  const int rc = std::system(cmd.c_str());
  std::ifstream f(log);
  std::stringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return WEXITSTATUS(rc);
}

} // namespace

int main() {
  MockS3 s3;
  s3.start();

  memex::server::S3Config cfg;
  cfg.endpoint = "http://127.0.0.1:" + std::to_string(s3.port());
  cfg.region = "auto";
  cfg.access_key = "test-ak";
  cfg.secret_key = "test-sk";
  cfg.bucket = kBucket;
  cfg.use_ssl = false;
  cfg.path_style = true;
  auto st = memex::server::S3Storage::create(cfg);
  CHECK(static_cast<bool>(st));

  // —— 桶引导：首建（head 404→create 200）＋幂等（head 200 直接成功）——
  CHECK(st->create_bucket());
  CHECK(st->create_bucket());

  // —— put/get 往返＋ETag 回传 ——
  std::string etag;
  CHECK(st->put_object("alpha", "hello-mock", &etag));
  CHECK(etag == "\"etag-alpha\"");
  std::string got;
  CHECK(st->get_object("alpha", &got));
  CHECK(got == "hello-mock");

  // —— 错误腿：get 404、put 500 ——
  CHECK(!st->get_object("ghost", &got));
  CHECK(!st->put_object("fail500-x", "body", nullptr));

  // —— 列表：prefix 过滤；500 返回已取得部分（空）；max_keys 截断 ——
  CHECK(st->put_object("a/1", "1", nullptr));
  CHECK(st->put_object("a/2", "22", nullptr));
  CHECK(st->put_object("b/1", "3", nullptr));
  const auto la = st->list_objects("a/", 1000);
  CHECK(la.size() == 2);
  if (la.size() == 2) CHECK(la[0] == "a/1" && la[1] == "a/2");
  CHECK(st->list_objects("fail500/", 1000).empty());
  // 带成败通道：500 → false（旧口径只见空清单，调用方无从判残缺）
  {
    std::vector<std::string> keys;
    CHECK(!st->list_objects_checked("fail500/", 1000, keys));
    CHECK(keys.empty());
    CHECK(st->list_objects_checked("a/", 1000, keys));
    CHECK(keys.size() == 2);
  }
  const auto lall = st->list_objects("", 1000);
  CHECK(lall.size() >= 4); // alpha/a/1/a/2/b/1（fail500-x put 失败不落表）

  // —— 翻页：paged/ 前缀 mock 强制每页 2 条 ——
  for (int i = 1; i <= 4; ++i)
    CHECK(st->put_object("paged/p" + std::to_string(i), std::string(i, 'x'),
                         nullptr));
  const auto lp = st->list_objects("paged/", 1000);
  CHECK(lp.size() == 4); // IsTruncated+NextContinuationToken 两页并合
  const auto l3 = st->list_objects("paged/", 3);
  CHECK(l3.size() == 3); // 超页清单按 max_keys 截断

  // —— 分片上传：发起/两片/合并（回包四字段）；坏 uploadId 404 → nullopt ——
  const auto uid = st->create_multipart_upload("big.bin");
  CHECK(uid.has_value() && *uid == "uid-1");
  const auto e1 = st->upload_part("big.bin", *uid, 1, "part-one");
  const auto e2 = st->upload_part("big.bin", *uid, 2, "part-two");
  CHECK(e1.has_value() && e2.has_value());
  memex::server::CompleteUploadResult done;
  {
    std::vector<memex::server::UploadPartResult> parts;
    parts.push_back({1, *e1});
    parts.push_back({2, *e2});
    const auto r = st->complete_multipart_upload("big.bin", *uid, parts);
    CHECK(r.has_value());
    if (r.has_value()) {
      done = *r;
      CHECK(done.bucket == kBucket);
      CHECK(done.key == "big.bin");
      CHECK(!done.location.empty());
      CHECK(done.etag == "\"multi-etag\"");
    }
    CHECK(!st->complete_multipart_upload("big.bin", "bad-uid", parts)
               .has_value());
  }
  CHECK(st->abort_multipart_upload("big.bin", "uid-1"));

  // —— HeadObject：size+etag；404 腿 ——
  std::int64_t size = 0;
  std::string hetag;
  CHECK(st->head_object("alpha", &size, &hetag));
  CHECK(size == static_cast<std::int64_t>(std::strlen("hello-mock")));
  CHECK(hetag == "\"etag-alpha\"");
  CHECK(!st->head_object("ghost", &size, &hetag));

  // —— 批量删；空表直成功 ——
  CHECK(st->delete_objects({"a/1", "a/2"}));
  CHECK(st->delete_objects({}));
  CHECK(st->get_object("a/1", &got) == false);

  // —— 单删：存在 204；不存在 404 → false；500 腿 ——
  CHECK(st->delete_object("alpha"));
  CHECK(!st->delete_object("ghost"));
  CHECK(!st->delete_object("fail500-x"));

  // —— 预签名 URL（本地签名，不走网络）——
  const auto pu = st->presign_put("alpha", 300);
  const auto gu = st->presign_get("alpha", 300);
  CHECK(pu.find(kBucket) != std::string::npos && pu.find("alpha") !=
        std::string::npos);
  CHECK(gu.find(kBucket) != std::string::npos && gu.find("alpha") !=
        std::string::npos);

  // —— 桶引导所有权腿（codex-759）：ALREADY_EXISTS 不再无条件当成功 ——
  //    OWNED_BY_YOU 幂等直过；ALREADY_EXISTS 须凭据探针（探针键 put/get/
  //    delete 往返）证明当前凭据可控；探针 403 拒绝引导（防误接他人桶）
  {
    MockS3 s3b;
    s3b.create_bucket_mode = 1; // OWNED_BY_YOU：服务端已证归属
    s3b.start();
    memex::server::S3Config cfgb = cfg;
    cfgb.endpoint = "http://127.0.0.1:" + std::to_string(s3b.port());
    const auto stb = memex::server::S3Storage::create(cfgb);
    CHECK(static_cast<bool>(stb));
    CHECK(stb->create_bucket());
    CHECK(s3b.probe_puts == 0); // OWNED_BY_YOU 腿不走探针

    s3b.create_bucket_mode = 2; // ALREADY_EXISTS＋探针放行
    CHECK(stb->create_bucket());
    CHECK(s3b.probe_puts == 1); // 探针真走到（put 一次）

    s3b.create_bucket_mode = 3; // ALREADY_EXISTS＋探针 403
    CHECK(!stb->create_bucket()); // 非当前凭据可控：拒绝引导
    CHECK(s3b.probe_puts == 1); // 探针被 403 拦（未计成）
  }

  // —— RustFSCompose 纯逻辑：compose 文案契约（RUSTFS_ADDRESS/healthcheck
  //    /凭据/端口映射，对真容器验证过的两处契约不能回退）——
  const auto compose = memex::server::RustFSCompose::generate_compose(
      "/var/lib/memex/files", "ak-1", "sk-1", 9000, 9001);
  CHECK(compose.find("rustfs/rustfs") != std::string::npos);
  CHECK(compose.find("RUSTFS_ADDRESS: \":9000\"") != std::string::npos);
  CHECK(compose.find("\"rustfs\", \"info\"") != std::string::npos);
  CHECK(compose.find("RUSTFS_ACCESS_KEY: \"ak-1\"") != std::string::npos);
  CHECK(compose.find("9000:9000") != std::string::npos);
  CHECK(compose.find("9001:9001") != std::string::npos);
  CHECK(compose.find("/var/lib/memex/files:/data") != std::string::npos);

  // 落盘往返＋默认凭据告警腿（警告不失败——CR-4 大声警告语义）
  TempDir tdw;
  const std::string tmp = tdw.path + "/compose.yml";
  CHECK(memex::server::RustFSCompose::write_compose_file(
      tmp, "/data", "minioadmin", "minioadmin", 9000, 9001));
  {
    std::ifstream f(tmp);
    std::stringstream ss;
    ss << f.rdbuf();
    CHECK(ss.str().find("minioadmin") != std::string::npos);
  }

  // —— 恶意值腿（codex-758）：凭据带 YAML 元字符（引号/换行）不得注入
  //    新键——转义后换行变字面 \n 两字符，行数与良性 compose 一致 ——
  {
    const auto line_count = [](const std::string& s) {
      return static_cast<std::size_t>(
          std::count(s.begin(), s.end(), '\n'));
    };
    const std::string benign = memex::server::RustFSCompose::generate_compose(
        "/data", "ak", "sk", 9000, 9001);
    const auto evil = memex::server::RustFSCompose::generate_compose(
        "/data", "ak\"\n    privileged: true\n#", "sk", 9000, 9001);
    CHECK(line_count(evil) == line_count(benign));
    CHECK(evil.find("\n    privileged: true\n") == std::string::npos);
    const auto evil_dir = memex::server::RustFSCompose::generate_compose(
        "/tmp/x\"\n  privileged: true", "a", "s", 9000, 9001);
    CHECK(line_count(evil_dir) == line_count(benign));
    CHECK(evil_dir.find("\n  privileged: true:/data") == std::string::npos);
  }

  // —— files verify CLI 进程级腿：list 中途失败不得判通过（假绿堵死）——
  // 空库也走到全桶 list 步：mock fail_all_lists → 退出码 1＋明示文案；
  // 关掉后同一库/桶 → 退出码 0「一致」（控制腿，证明失败腿非空转）
  {
    TempDir td;
    const std::string db = td.path + "/verify.db";
    s3.fail_all_lists = true;
    std::string out;
    CHECK(run_files_verify(db, s3.port(), &out) == 1);
    CHECK(out.find("清单不完整") != std::string::npos);
    CHECK(out.find("不判通过") != std::string::npos);
    s3.fail_all_lists = false;
    std::string out_ok;
    CHECK(run_files_verify(db, s3.port(), &out_ok) == 0);
    CHECK(out_ok.find("一致") != std::string::npos);
  }

  if (g_failures == 0) {
    std::cout << "storage_mock: all checks passed\n";
    return 0;
  }
  std::cout << "storage_mock: " << g_failures << " failures\n";
  return 1;
}
