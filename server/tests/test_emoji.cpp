// 表情包素材（需求批②，服务端面）：个人素材 BLOB 落库——上传
// （PNG/JPEG/GIF magic 校验、1MiB 上限、缺名/空体 400）、清单（仅本人）、
// 下载（字节往返＋Content-Type 嗅探）、删除（属主裁决，他人 404）。
// 全程 HTTP 真 TCP；未登录 401；素材面不依赖对象存储（BLOB 直存）。
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

// —— 阻塞 HTTP 客户端（真 TCP；服务端 Connection: close，读到对端关）——
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

// JSON 取字段（子串检索取首个）
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
  asio::io_context io;
  ServerStore store;
  CHECK(store.open(":memory:"));
  CHECK(store.create_account("alice", "pw-a", "Alice"));
  CHECK(store.create_account("bob", "pw-b", "Bob"));

  // 素材面不落对象存储（BLOB 直存库内）——storage 传空
  memex::server::FileServer files(io, store, nullptr, 0);
  const std::uint16_t port = files.port();
  files.start_accept();
  std::thread th([&io] { io.run(); });

  // —— 未登录 401（四面同口径）——
  CHECK(http(port, "GET", "/files/emoji/list", {}, "").status == 401);
  CHECK(http(port, "POST", "/files/emoji/upload?name=x", {}, "x").status ==
        401);
  CHECK(http(port, "GET", "/files/emoji/download?id=1", {}, "").status == 401);
  CHECK(http(port, "POST", "/files/emoji/delete", {}, "{\"id\":1}").status ==
        401);

  // —— 登录两账号 ——
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

  // 素材字节：PNG（magic+IHDR 头）、GIF89a、JPEG
  std::string png = std::string("\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8) +
                    std::string("\x00\x00\x00\x0DIHDR", 8) +
                    std::string("\x00\x00\x00\x20\x00\x00\x00\x20", 8) +
                    std::string("\x08\x06\x00\x00\x00", 5);
  const std::string gif = "GIF89a" + std::string(26, '\0');
  const std::string jpg = std::string("\xFF\xD8\xFF\xE0", 4) +
                          std::string("\x00\x10JFIF", 6) + std::string(22, 'j');

  // —— 上传形态校验 ——
  CHECK(http(port, "POST", "/files/emoji/upload", ha, png).status == 400);
  CHECK(jstr(http(port, "POST", "/files/emoji/upload", ha, png).body, "error")
            .find("name") != std::string::npos);
  CHECK(http(port, "POST", "/files/emoji/upload?name=empty.png", ha, "")
            .status == 400);
  CHECK(http(port, "POST", "/files/emoji/upload?name=t.txt", ha,
            "plain text not an image").status == 415);
  CHECK(http(port, "POST", "/files/emoji/upload?name=short.bin", ha, "\x89PNG")
            .status == 415); // <8 字节
  // 素材名 percent-encode 往返（中文）：服务端 query_param 已解 %XX
  const auto up1 = http(port, "POST",
                        "/files/emoji/upload?name=%E6%89%93%E6%B0%94%E7%8C%AB.png",
                        ha, png);
  CHECK(up1.status == 200);
  const std::int64_t id1 = jint(up1.body, "id");
  CHECK(id1 > 0);
  const auto up2 =
      http(port, "POST", "/files/emoji/upload?name=laugh.gif", ha, gif);
  CHECK(up2.status == 200);
  const std::int64_t id2 = jint(up2.body, "id");
  CHECK(id2 > 0 && id2 != id1);
  const auto up3 =
      http(port, "POST", "/files/emoji/upload?name=photo.jpg", ha, jpg);
  CHECK(up3.status == 200);
  const std::int64_t id3 = jint(up3.body, "id");
  CHECK(id3 > 0);

  // —— 大小上限：素材校验层 1MiB+1 → 413（语义化文案；再往上由通用
  //     body_cap 层兜底，与品牌物料/上传同口径不在此重复）——
  {
    std::string over = png.substr(0, 8);
    over += std::string(1024 * 1024 + 1 - 8, 'x'); // 恰 1MiB+1
    const auto r1 = http(port, "POST", "/files/emoji/upload?name=big.png", ha,
                         over);
    CHECK(r1.status == 413);
    CHECK(jstr(r1.body, "error").find("1MiB") != std::string::npos);
  }

  // —— 清单：本人三条（名字解码回中文）、他人不串 ——
  {
    const auto ls = http(port, "GET", "/files/emoji/list", ha, "");
    CHECK(ls.status == 200);
    CHECK(jint(ls.body, "id") == id3); // ts DESC, id DESC：最后传的在前
    CHECK(ls.body.find("\xE6\x89\x93\xE6\xB0\x94\xE7\x8C\xAB.png") !=
          std::string::npos); // 「打气猫.png」
    CHECK(ls.body.find("laugh.gif") != std::string::npos);
    CHECK(ls.body.find("photo.jpg") != std::string::npos);
    CHECK(ls.body.find("\"size\":" + std::to_string(png.size())) !=
          std::string::npos);
    CHECK(ls.body.find("\"size\":" + std::to_string(gif.size())) !=
          std::string::npos);
    const auto ls_b = http(port, "GET", "/files/emoji/list", hb, "");
    CHECK(ls_b.status == 200);
    CHECK(ls_b.body.find("laugh.gif") == std::string::npos);
    CHECK(ls_b.body.find("\"assets\":[]") != std::string::npos);
  }

  // —— 下载：字节往返＋Content-Type 嗅探；属主裁决 ——
  {
    const auto d1 = http(port, "GET",
                         "/files/emoji/download?id=" + std::to_string(id1), ha,
                         "");
    CHECK(d1.status == 200);
    CHECK(d1.body == png);
    CHECK(d1.headers.at("content-type") == "image/png");
    const auto d2 = http(port, "GET",
                         "/files/emoji/download?id=" + std::to_string(id2), ha,
                         "");
    CHECK(d2.status == 200);
    CHECK(d2.body == gif);
    CHECK(d2.headers.at("content-type") == "image/gif");
    const auto d3 = http(port, "GET",
                         "/files/emoji/download?id=" + std::to_string(id3), ha,
                         "");
    CHECK(d3.status == 200);
    CHECK(d3.body == jpg);
    CHECK(d3.headers.at("content-type") == "image/jpeg");
    // 他人取非本人素材 404（存在性不透）
    CHECK(http(port, "GET", "/files/emoji/download?id=" + std::to_string(id1),
               hb, "").status == 404);
    CHECK(http(port, "GET", "/files/emoji/download?id=0", ha, "").status ==
          404);
    CHECK(http(port, "GET", "/files/emoji/download?id=99999", ha, "").status ==
          404);
    CHECK(http(port, "GET", "/files/emoji/download?id=x", ha, "").status ==
          404);
  }

  // —— 删除：缺字段 400；他人 404；本人 200；删后再取 404、清单少一 ——
  {
    CHECK(http(port, "POST", "/files/emoji/delete", ha, "{}").status == 400);
    CHECK(http(port, "POST", "/files/emoji/delete", ha, "not-json").status ==
          400);
    CHECK(http(port, "POST", "/files/emoji/delete", hb,
               "{\"id\":" + std::to_string(id2) + "}")
              .status == 404);
    CHECK(http(port, "POST", "/files/emoji/delete", ha,
               "{\"id\":" + std::to_string(id2) + "}")
              .status == 200);
    CHECK(http(port, "POST", "/files/emoji/delete", ha,
               "{\"id\":" + std::to_string(id2) + "}")
              .status == 404); // 再删
    CHECK(http(port, "GET", "/files/emoji/download?id=" + std::to_string(id2),
               ha, "").status == 404);
    const auto ls = http(port, "GET", "/files/emoji/list", ha, "");
    CHECK(ls.status == 200);
    CHECK(ls.body.find("laugh.gif") == std::string::npos);
    CHECK(ls.body.find("photo.jpg") != std::string::npos); // 其余不动
  }

  io.stop();
  th.join();

  if (g_failures == 0) {
    std::cout << "test_emoji: all checks passed\n";
    return 0;
  }
  std::cout << "test_emoji: " << g_failures << " check(s) FAILED\n";
  return 1;
}
