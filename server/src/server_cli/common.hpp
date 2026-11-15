// memex_server CLI 共性面（LAY-006 自 main.cpp 匿名空间逐字搬移）。
// cmd_* 声明供 main 分发与模块定义共用；共享小工具 inline 化。
#pragma once

#include <asio.hpp>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <optional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <memex/protocol/messages.hpp>
#include "cred.hpp"
#include "files_server.hpp"
#include "group_assistant.hpp"
#include "ldap.hpp"
#include "model_gateway.hpp"
#include "serve_args.hpp"
#include "task_pull.hpp"
#include "server.hpp"
#include "storage.hpp"
#include "store.hpp"
#include "webhook.hpp"
#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif


// 可移植 strptime（T3.2 时间窗解析用）：MSVC 无此 POSIX 函数（win 腿
// nightly 首编即 C3861）。std::get_time 按格式前缀匹配、不校验尾部残留——
// 与 strptime 口径一致，最长格式在前逐个尝试（"12:34:56" 先中 HH:MM:SS）。
inline bool parse_time_prefix(const std::string& text, const char* fmt, std::tm* tm) {
  std::istringstream ss(text);
  ss >> std::get_time(tm, fmt);
  return !ss.fail();
}

// localtime_r 的可移植替身：Windows 走 localtime_s（参数序与 POSIX 相反）
inline void local_time(std::time_t secs, std::tm* out) {
#ifdef _WIN32
  localtime_s(out, &secs);
#else
  localtime_r(&secs, out);
#endif
}

// 时间参数解析（T3.2）："YYYY-MM-DD" 或 "YYYY-MM-DD HH:MM[:SS]"（本地时区）。
// date_only 端补零点／当日末秒，使日期粒度的开闭区间语义正确。失败返回 -1。
inline std::int64_t parse_time_arg(const std::string& text, bool end_of_day) {
  std::tm tm{};
  const char* fmts[] = {"%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"};
  bool ok = false;
  for (const char* f : fmts) {
    if (parse_time_prefix(text, f, &tm)) {
      ok = true;
      break;
    }
  }
  if (!ok) return -1;
  if (end_of_day) {
    if (tm.tm_hour == 0 && tm.tm_min == 0 && tm.tm_sec == 0) {
      tm.tm_hour = 23;
      tm.tm_min = 59;
      tm.tm_sec = 59;
    }
  }
  tm.tm_isdst = -1;
  return static_cast<std::int64_t>(mktime(&tm)) * 1000;
}

// 查阅操作者：管理 CLI 由运维在本机执行，取系统用户名留痕；拿不到记 "cli"。
inline std::string cli_operator() {
  const char* user = std::getenv("USER");
  if (!user || !*user) user = std::getenv("LOGNAME");
  return (user && *user) ? std::string(user) : std::string("cli");
}

int cmd_serve(int argc, char** argv, const std::string& db_path);
int cmd_identity(int argc, char** argv, const std::string& db_path);
int cmd_account(int argc, char** argv, const std::string& db_path);
int cmd_logins(int argc, char** argv, const std::string& db_path);
int cmd_device(int argc, char** argv, const std::string& db_path);
int cmd_messages(int argc, char** argv, const std::string& db_path);
int cmd_cross(int argc, char** argv, const std::string& db_path);
int cmd_favs(int argc, char** argv, const std::string& db_path);
int cmd_audit(int argc, char** argv, const std::string& db_path);
int cmd_branding(int argc, char** argv, const std::string& db_path);
int cmd_org(int argc, char** argv, const std::string& db_path);
int cmd_retention(int argc, char** argv, const std::string& db_path);
int cmd_policy(int argc, char** argv, const std::string& db_path);
int cmd_assist(int argc, char** argv, const std::string& db_path);
int cmd_bot(int argc, char** argv, const std::string& db_path);
int cmd_model(int argc, char** argv, const std::string& db_path);
int cmd_webhook(int argc, char** argv, const std::string& db_path);
int cmd_group(int argc, char** argv, const std::string& db_path);
int cmd_files(int argc, char** argv, const std::string& db_path);
int cmd_storage(int argc, char** argv, const std::string& db_path);
int cmd_archive(int argc, char** argv, const std::string& db_path);
