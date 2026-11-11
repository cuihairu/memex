// files_server 拆分模块的内部共享小工具（LAY-003）：原匿名命名空间
// 的工具函数/常量（多 TU 共享，inline 化）+ 文件会话实体。对外 API
// 仍只在 files_server.hpp（make_file_sessions 定义在 server.cpp）。
#pragma once

#include "files_server.hpp"


#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <memex/protocol/messages.hpp>

#include "cred.hpp"

namespace memex::server {


using json = nlohmann::json;
using tcp = asio::ip::tcp;

constexpr std::size_t kMaxHead = 8 * 1024;      // 请求头上限（防呆）
constexpr std::size_t kMaxJsonBody = 64 * 1024; // session/manage JSON 上限
constexpr std::size_t kMaxUpload = 512u * 1024 * 1024; // 单文件上限 512MiB
constexpr std::int64_t kSessionTtlMs = 12 * 3600 * 1000; // 令牌 12h
// R26-1 agent 在线判定窗：默认 30s 心跳的 3 倍宽限（错过两拍仍算在线）
constexpr std::int64_t kAgentOnlineMs = 90 * 1000;
// R26-3 一次性短票有效期：签发后 60s 内须兑现（过期=401 重签）
constexpr std::int64_t kSessionTicketTtlMs = 60 * 1000;

inline std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 日报归属日校验（YYYY-MM-DD 形态；不做历法深查——客户端日期控件给出，
// 形态门只挡明显坏值）
inline bool is_date_ymd(const std::string& s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i == 4 || i == 7) continue;
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

inline const char* reason_phrase(int status) {
  switch (status) {
  case 200: return "OK";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 413: return "Payload Too Large";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  default: return "Internal Server Error";
  }
}

struct FileSession {
  std::string account;
  std::int64_t expires_ms{0};
  // 令牌 scope（R23-4 两套路由两套 scope）：false=内网文件面全量；
  // true=外网单向 uplink（只许 /uplink/* 写入，内网面一律 403）
  bool uplink{false};
};

// "group:3" / "me" / "inbox" → is_group/gid/uid/is_inbox；非法返回 false
// inbox=文件助手收件箱：个人空间归属（uid=本人），类目由调用方落 kind
inline bool parse_target(const std::string& t, const std::string& me, bool& is_group,
                  std::uint64_t& gid, std::string& uid, bool& is_inbox) {
  is_inbox = false;
  if (t == "me" || t == "inbox") {
    is_group = false;
    gid = 0;
    uid = me;
    is_inbox = t == "inbox";
    return !me.empty();
  }
  if (t.rfind("group:", 0) == 0) {
    const std::string n = t.substr(6);
    if (n.empty() || n.find_first_not_of("0123456789") != std::string::npos) {
      return false;
    }
    is_group = true;
    gid = std::strtoull(n.c_str(), nullptr, 10);
    uid.clear();
    return true;
  }
  return false;
}

// query 串取参数（不考虑 urldecode：参数值只用 [A-Za-z0-9:_]）
inline std::string query_param(const std::string& query, const char* key) {
  const std::string prefix = std::string(key) + "=";
  std::size_t pos = 0;
  while ((pos = query.find(prefix, pos)) != std::string::npos) {
    if (pos == 0 || query[pos - 1] == '&') {
      std::size_t end = query.find('&', pos);
      if (end == std::string::npos) end = query.size();
      // percent-decode（R24-2 搜索词等中文参数走 %XX 编码；数字/ASCII
      // 参数无 % 不受影响）。只解 %XX——"+" 转空格属 form 惯例，此处不动。
      std::string raw =
          query.substr(pos + prefix.size(), end - pos - prefix.size());
      std::string out;
      out.reserve(raw.size());
      for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '%' && i + 2 < raw.size() &&
            std::isxdigit(static_cast<unsigned char>(raw[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(raw[i + 2]))) {
          const auto hex = [](char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
          };
          out += static_cast<char>(
              (hex(raw[i + 1]) << 4) | hex(raw[i + 2]));
          i += 2;
        } else {
          out += raw[i];
        }
      }
      return out;
    }
    ++pos;
  }
  return "";
}

// R23-5 外网面扩展名判定：白名单模式（allowlist 非空）严格比对——后缀
// 不在名单内一律拒、无后缀也拒；黑名单模式无后缀不命中（放过常规无后缀
// 文件名）。后缀取最后一段小写化比对。
inline bool ext_allowed(const UplinkPolicy& p, const std::string& file_name) {
  const auto dot = file_name.rfind('.');
  const bool has_ext =
      dot != std::string::npos && dot + 1 < file_name.size() &&
      file_name.find_first_of("/\\", dot) == std::string::npos;
  std::string ext;
  if (has_ext) {
    ext = file_name.substr(dot + 1);
    for (auto& c : ext) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  if (!p.ext_allowlist.empty()) {
    return has_ext && std::find(p.ext_allowlist.begin(), p.ext_allowlist.end(),
                                ext) != p.ext_allowlist.end();
  }
  if (!has_ext) return true;
  return std::find(p.ext_denylist.begin(), p.ext_denylist.end(), ext) ==
         p.ext_denylist.end();
}

// 远程协助权限名 ↔ 权限位（与 CLI assist --perms 同名：view/keyboard/
// mouse/clipboard/file；非法名=0 由 assist_mask_valid 拒）
inline int assist_perm_mask(const std::string& name) {
  if (name == "view") return ServerStore::kAssistView;
  if (name == "keyboard") return ServerStore::kAssistKeyboard;
  if (name == "mouse") return ServerStore::kAssistMouse;
  if (name == "clipboard") return ServerStore::kAssistClipboard;
  if (name == "file") return ServerStore::kAssistFileTransfer;
  return 0;
}

inline bool assist_mask_valid(int m) {
  const int all = ServerStore::kAssistView | ServerStore::kAssistKeyboard |
                  ServerStore::kAssistMouse | ServerStore::kAssistClipboard |
                  ServerStore::kAssistFileTransfer;
  return m != 0 && (m & ~all) == 0;
}


// 会话库实体（hpp 前置声明的 memex::server::FileSessions）：双实例部署时
// 内外两面共享一份——scope 闸才有实体：uplink 令牌打内网面被识别后 403
// 「外网会话只许写入」（而非当作未知令牌 401）、内网令牌打 uplink 面 403
// 「须外网 uplink 会话」同理；单实例缺省自建句柄，行为不变。
struct FileSessions {
  std::unordered_map<std::string, FileSession> map;
};


} // namespace memex::server
