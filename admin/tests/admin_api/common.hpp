// LAY-009 共性面：原匿名空间 helper inline 化、main 种子局部量提升为 inline 变量保跨 TU 单例；套件定义在各域模块，入口按原序调用。
#pragma once

#include <admin_app.hpp>

#include <drogon/drogon.h>
#include <json/json.h>
#include <trantor/net/EventLoopThread.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <cred.hpp>
#include <store.hpp>

inline int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << g_phase << " " << __FILE__ << ":" << __LINE__ \
                << " " << #cond << "\n";                                                     \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

// —— CI 排障脚手架（CI 假 abort 诊断用；本机全绿保持不变）——
// abort 无输出无法定位：stderr 相位标记（无缓冲，崩前已出）＋ terminate
// 钩子回放当前异常与调用栈地址，下一轮 CI 红跑即可读出抛点。
inline const char* g_phase = "start";
#define PHASE(p)                                                             \
  do {                                                                       \
    g_phase = (p);                                                           \
    std::cerr << "[phase] " << (p) << "\n";                                   \
  } while (false)


#if defined(__GLIBC__)
#include <execinfo.h>
inline void diag_backtrace() {
  void* frames[64];
  const int n = backtrace(frames, 64);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
}
#else
inline void diag_backtrace() {}
#endif

  struct DiagTerminateHook {
  DiagTerminateHook() {
  std::set_terminate([] {
  std::cerr << "[diag] terminate; phase=" << g_phase << '\n';
  try {
  if (auto e = std::current_exception()) std::rethrow_exception(e);
  } catch (const std::exception& ex) {
  std::cerr << "[diag] exception: " << ex.what() << '\n';
  } catch (...) {
  std::cerr << "[diag] exception: unknown\n";
  }
  diag_backtrace();
  std::abort();
  });
  }
};

inline const DiagTerminateHook g_diag_hook;

inline const std::string kBase = "http://127.0.0.1:18089";

// 同步 HTTP（future 口）：method/path/body/cookie → {code, json|text, set-cookie}
struct Resp {
  int code = 0;
  std::string text;
  std::string set_cookie;
};


inline Resp http(const std::string& method, const std::string& path,
          const std::string& body = {}, const std::string& cookie = {},
          const std::string& ctype = "application/json") {
  static trantor::EventLoopThread loop;
  static auto client = [&] {
    loop.run(); // getLoop() 只回指针不起线程——不起 IO 线程同步请求永挂
    return drogon::HttpClient::newHttpClient(kBase, loop.getLoop());
  }();
  auto req = drogon::HttpRequest::newHttpRequest();
  req->setMethod(method == "GET" ? drogon::Get : drogon::Post);
  req->setPath(path);
  if (!body.empty()) {
    req->setContentTypeString(ctype); // 品牌素材上传走 image/png 原始字节体
    req->setBody(body);
  }
  if (!cookie.empty()) req->addHeader("Cookie", cookie);
  req->addHeader("X-Memex-Admin", "1");
  const auto [result, resp] = client->sendRequest(req, 5.0);
  if (result != drogon::ReqResult::Ok || !resp) return {};
  Resp r;
  r.code = static_cast<int>(resp->getStatusCode());
  r.text = std::string(resp->getBody());
  // Drogon 把应答 Cookie 解析进 cookie 表而非 headers——会话键逐字取回
  for (const auto& [name, ck] : resp->cookies()) {
    r.set_cookie = name + "=" + ck.value();
    break;
  }
  return r;
}

inline Json::Value jbody(const std::string& s) {
  Json::Value j;
  Json::Reader reader;
  reader.parse(s, j);
  return j;
}

inline bool has_role(const Json::Value& j, const char* role) {
  for (const auto& r : j["roles"]) {
    if (r.asString() == role) return true;
  }
  return false;
}

// 品牌/头像测试素材：最小合法 PNG（magic 8 字节＋IHDR 尺寸位，CRC 不验）。
// w/h 按大端写进固定偏移 16/20（png_asset_check 只读这两处）。
inline std::string test_png(std::uint32_t w, std::uint32_t h, std::size_t pad = 0) {
  static const unsigned char kHead[24] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // PNG magic
      0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, // IHDR chunk
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // w/h 占位
  };
  std::string b(reinterpret_cast<const char*>(kHead), sizeof(kHead));
  auto put32 = [&](std::size_t off, std::uint32_t v) {
    b[off] = static_cast<char>((v >> 24) & 0xFF);
    b[off + 1] = static_cast<char>((v >> 16) & 0xFF);
    b[off + 2] = static_cast<char>((v >> 8) & 0xFF);
    b[off + 3] = static_cast<char>(v & 0xFF);
  };
  put32(16, w);
  put32(20, h);
  b.append(pad, '\0');
  return b;
}


// test_png 返回临时串——同一表达式里调两次得到两个不同临时对象，
// (begin, end) 跨临时取迭代器对＝越界指针差（未定义：临时布局凑巧挨着
// 则侥幸可用，凑巧相距则 std::length_error 直接 abort——CI 假 abort 根因）。
// 先落命名局部再取区间。
inline std::vector<unsigned char> png_bytes(std::uint32_t w, std::uint32_t h,
                                     std::size_t pad = 0) {
  const std::string s = test_png(w, h, pad);
  return std::vector<unsigned char>(s.begin(), s.end());
}


inline std::filesystem::path tmp;
inline std::string db;
inline std::uint64_t gid = 0; // 通知与集成腿：webhook 群级目标用
inline std::uint64_t gid2 = 0; // 群组管理面腿：密码箱未建立的对照群
inline std::int64_t file_a = 0, file_b = 0; // 文件与配额腿：群文件 A／个人文件 B
inline std::int64_t memo_id = 0, vault_entry = 0; // 群组管理面腿：备忘录/密码箱条目
inline std::int64_t poll_plain = 0, poll_anon = 0; // 明票/匿名票
inline std::uint64_t srv_id = 0; // 群服务器
inline std::int64_t ap1 = 0, ap2 = 0; // 业务流程面腿：待决/已决申请
inline std::int64_t seat_a = 0, seat_b = 0; // 业务流程面腿：3F 工位
inline std::int64_t task_a = 0; // 业务流程面腿：m1 清单任务
inline std::string ra_id; // 业务流程面腿：远程协助会话

inline std::string root_cookie; // leg-4 登录后持有，各腿复用
inline std::string m1ck;        // member 会话（业务流程面起持有）

int seed_db();   // 返回 2＝造库失败（入口原样退出码）
void leg_auth();
void leg_policy();
void leg_archive();
void leg_surface();
void leg_notify();
void leg_files();
void leg_groups();
void leg_bizflow();
void leg_brand();
void leg_tasks_pull();
