// 用户头像（需求批⑫，服务端面）：member_avatars 四档（32/64/128/256）BLOB
// 落库——库级（upsert 覆盖/取字节/清除/版本戳）＋HTTP 全链：上传
// （PNG/JPEG/GIF magic、1MiB 上限 413、非图 415、缺体 400、坏档位 400、
// 他人 403）、下载（登录成员可读＝组织内互见、字节往返＋Content-Type）、
// 删除（本人，无头像 404）、ORG_QUERY 带出 avatar_ver。全程真 TCP；未登录 401。
#include <asio.hpp>

#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

#include "files_server.hpp"
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

} // namespace

int main() {
  // —— 库级：档位表／upsert 覆盖／取字节／版本戳／清除 ——
  {
    ServerStore s;
    CHECK(s.open(":memory:"));
    CHECK(s.create_account("alice", "pw", "Alice"));
    CHECK(ServerStore::avatar_size_valid(32) && ServerStore::avatar_size_valid(64) &&
          ServerStore::avatar_size_valid(128) &&
          ServerStore::avatar_size_valid(256));
    CHECK(!ServerStore::avatar_size_valid(0) && !ServerStore::avatar_size_valid(48) &&
          !ServerStore::avatar_size_valid(512));
    CHECK(s.avatar_ver("alice") == 0); // 无行=0（默认头像）
    const std::vector<unsigned char> a1 = {1, 2, 3};
    const std::vector<unsigned char> a2 = {9, 8, 7, 6};
    CHECK(s.avatar_put("alice", 64, "image/png", a1, 1000));
    CHECK(!s.avatar_put("alice", 48, "image/png", a1, 1000)); // 坏档位
    CHECK(!s.avatar_put("alice", 64, "image/png", {}, 1000));  // 空体
    CHECK(!s.avatar_put("", 64, "image/png", a1, 1000));       // 空账号
    CHECK(s.avatar_ver("alice") == 1000);
    CHECK(s.avatar_put("alice", 32, "image/jpeg", a2, 2000)); // 另一档
    CHECK(s.avatar_ver("alice") == 2000); // 版本戳=各档 MAX
    CHECK(s.avatar_put("alice", 64, "image/png", a2, 3000));  // upsert 覆盖
    std::vector<unsigned char> out;
    std::string mime;
    CHECK(s.avatar_bytes("alice", 64, out, mime) && out == a2 &&
          mime == "image/png");
    CHECK(s.avatar_bytes("alice", 32, out, mime) && out == a2 &&
          mime == "image/jpeg");
    CHECK(!s.avatar_bytes("alice", 128, out, mime)); // 未传档位
    CHECK(!s.avatar_bytes("bob", 64, out, mime));    // 他人无行
    CHECK(s.avatar_ver("alice") == 3000);
    CHECK(s.avatar_clear("alice"));
    CHECK(s.avatar_ver("alice") == 0); // 清除即回落默认头像
    CHECK(!s.avatar_clear("alice"));   // 无行可删 false
    s.close();
  }

  // —— HTTP 全链 ——
  asio::io_context io;
  ServerStore store;
  CHECK(store.open(":memory:"));
  CHECK(store.create_account("alice", "pw-a", "Alice"));
  CHECK(store.create_account("bob", "pw-b", "Bob"));
  memex::server::FileServer files(io, store, nullptr, 0);
  const std::uint16_t port = files.port();
  files.start_accept();
  std::thread th([&io] { io.run(); });

  // 未登录 401（三面同口径）
  CHECK(http(port, "POST", "/files/avatar/upload?size=64", {}, "x").status ==
        401);
  CHECK(http(port, "GET", "/files/avatar/download?size=64", {}, "").status ==
        401);
  CHECK(http(port, "POST", "/files/avatar/delete", {}, "").status == 401);

  const auto login = [&](const char* a, const char* pw) {
    const auto r = http(port, "POST", "/files/session", {},
                        std::string("{\"account\":\"") + a +
                            "\",\"password\":\"" + pw + "\"}");
    CHECK(r.status == 200);
    return std::map<std::string, std::string>{
        {"Authorization", "Bearer " + jstr(r.body, "token")}};
  };
  const auto ha = login("alice", "pw-a");
  const auto hb = login("bob", "pw-b");

  const std::string png = std::string("\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8) +
                          std::string("\x00\x00\x00\x0DIHDR", 8) +
                          std::string("\x00\x00\x00\x40\x00\x00\x00\x40", 8) +
                          std::string("\x08\x06\x00\x00\x00", 5);
  const std::string jpg = std::string("\xFF\xD8\xFF\xE0", 4) +
                          std::string("\x00\x10JFIF", 6) + std::string(22, 'j');
  const std::string gif = "GIF89a" + std::string(26, '\0');

  // 上传形态校验
  CHECK(http(port, "POST", "/files/avatar/upload", ha, png).status == 400);
  CHECK(jstr(http(port, "POST", "/files/avatar/upload", ha, png).body, "error")
            .find("size") != std::string::npos); // 缺档位
  CHECK(http(port, "POST", "/files/avatar/upload?size=48", ha, png).status ==
        400); // 坏档位
  CHECK(http(port, "POST", "/files/avatar/upload?size=64", ha, "").status ==
        400); // 空体
  const auto not_img =
      http(port, "POST", "/files/avatar/upload?size=64", ha,
           "plain text not an image at all");
  CHECK(not_img.status == 415);
  CHECK(jstr(not_img.body, "error").find("PNG/JPEG/GIF") != std::string::npos);
  CHECK(http(port, "POST", "/files/avatar/upload?size=64", ha, "\x89PNG")
            .status == 415); // 过短
  {
    std::string over = png.substr(0, 8);
    over += std::string(1024 * 1024 + 1 - 8, 'x'); // 恰 1MiB+1
    const auto r = http(port, "POST", "/files/avatar/upload?size=256", ha,
                        over);
    CHECK(r.status == 413);
    CHECK(jstr(r.body, "error").find("1MiB") != std::string::npos);
  }
  // 他人属主裁决：bob 带 alice 为目标 → 403；删除同
  CHECK(http(port, "POST", "/files/avatar/upload?size=64&account=alice", hb,
             png).status == 403);
  CHECK(http(port, "POST", "/files/avatar/delete", hb,
             "{\"account\":\"alice\"}").status == 403);
  CHECK(store.avatar_ver("alice") == 0); // 被拒后零写入

  // 上传四档（PNG/JPEG/GIF 皆过）；回包带 ver
  const auto u32 = http(port, "POST", "/files/avatar/upload?size=32", ha, jpg);
  CHECK(u32.status == 200);
  CHECK(jint(u32.body, "size") == 32 && jint(u32.body, "ver") > 0);
  CHECK(http(port, "POST", "/files/avatar/upload?size=64", ha, png).status ==
        200);
  CHECK(http(port, "POST", "/files/avatar/upload?size=128&account=alice", ha,
             gif).status == 200); // 显式指定自己=本人，放行
  CHECK(http(port, "POST", "/files/avatar/upload?size=256", ha, png).status ==
        200);
  CHECK(store.avatar_ver("alice") > 0);

  // 下载：登录成员可读（bob 读 alice＝组织内互见）；字节往返＋Content-Type
  {
    const auto d64 = http(port, "GET",
                          "/files/avatar/download?account=alice&size=64", hb,
                          "");
    CHECK(d64.status == 200);
    CHECK(d64.body == png);
    CHECK(d64.headers.at("content-type") == "image/png");
    const auto d32 = http(port, "GET",
                          "/files/avatar/download?account=alice&size=32", hb,
                          "");
    CHECK(d32.status == 200 && d32.body == jpg);
    CHECK(d32.headers.at("content-type") == "image/jpeg");
    const auto d128 = http(port, "GET",
                           "/files/avatar/download?account=alice&size=128", ha,
                           "");
    CHECK(d128.status == 200 && d128.body == gif);
    CHECK(d128.headers.at("content-type") == "image/gif");
    // 缺省 account=自己
    CHECK(http(port, "GET", "/files/avatar/download?size=256", ha, "").body ==
          png);
    // 坏档位 400；未设置头像者 404（客户端据此回落默认头像）
    CHECK(http(port, "GET", "/files/avatar/download?account=alice&size=7", hb,
               "").status == 400);
    CHECK(http(port, "GET", "/files/avatar/download?account=bob&size=64", ha,
               "").status == 404);
  }

  // 覆盖：同档再传 → 字节换新、版本戳前进
  {
    const std::int64_t v0 = store.avatar_ver("alice");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(http(port, "POST", "/files/avatar/upload?size=64", ha, jpg).status ==
          200);
    CHECK(store.avatar_ver("alice") > v0);
    CHECK(http(port, "GET", "/files/avatar/download?account=alice&size=64", hb,
               "").body == jpg);
  }

  // 删除：本人；清空全部档位、版本归零；再删 404
  {
    const auto del = http(port, "POST", "/files/avatar/delete", ha, "");
    CHECK(del.status == 200);
    CHECK(store.avatar_ver("alice") == 0);
    CHECK(http(port, "GET", "/files/avatar/download?account=alice&size=32", hb,
               "").status == 404);
    CHECK(http(port, "POST", "/files/avatar/delete", ha, "").status == 404);
  }

  io.stop();
  th.join();
  store.close();

  if (g_failures == 0) {
    std::cout << "avatar tests: all passed\n";
    return 0;
  }
  std::cerr << "avatar tests: " << g_failures << " failure(s)\n";
  return 1;
}
