// MemexServer 入口：serve（默认）与账号／登录记录管理子命令。
// 管理后台（T3.1）接管运维面之前，账号开通与记录查询走本 CLI。
#include <asio.hpp>

#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <string_view>

#include <memex/protocol/messages.hpp>

#include "server.hpp"
#include "store.hpp"

#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif

namespace {

// 服务端默认监听端口。评审报告只固定了直连态端口段（UDP 2425–2436／TCP 2426–2437），
// 服务端监听端口未钉死，此默认值待与网络管理侧确认后写入部署文档。
constexpr std::uint16_t kDefaultPort = 24360;
constexpr const char* kDefaultDb = "memex-server.db";

// db_path 由 main 统一解析（--db 剥离会就地改写 argv，不能再从 argv 复读）
int cmd_serve(int argc, char** argv, const std::string& db_path) {
  std::uint16_t port = kDefaultPort;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
      if (port == 0) {
        std::cerr << "无效端口\n";
        return 2;
      }
    }
  }

  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }

  try {
    asio::io_context io;
    memex::server::CollabServer server(io, store, port);
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](std::error_code, int sig) {
      std::cout << "[MEMEX] 收到信号 " << sig << "，退出" << std::endl;
      io.stop();
    });
    server.start_accept();
    io.run();
  } catch (const std::exception& e) {
    std::cerr << "服务端异常退出：" << e.what() << std::endl;
    return 1;
  }
  return 0;
}

int cmd_account(int argc, char** argv, const std::string& db_path) {
  // account add <账号> <口令> [--name 显示名]
  if (argc < 3 || std::string_view(argv[0]) != "add") {
    std::cerr << "用法：memex_server account add <账号> <口令> [--name 显示名] [--db <库>]\n";
    return 2;
  }
  const std::string account = argv[1];
  const std::string password = argv[2];
  std::string display_name = account;
  for (int i = 3; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) == "--name") display_name = argv[++i];
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  if (!store.create_account(account, password, display_name)) {
    std::cerr << "建号失败（账号已存在或写库失败）：" << account << "\n";
    return 1;
  }
  std::cout << "已建号：" << account << "（" << display_name << "）\n";
  return 0;
}

int cmd_logins(int argc, char** argv, const std::string& db_path) {
  // logins [账号]：全量可查（不带账号=全部，倒序）
  std::string account;
  if (argc >= 1) account = argv[0];
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  const auto rows = store.login_records(account);
  std::cout << "id\t账号\t结果\t设备类型\t设备名\t指纹前8\t来源\t版本\t时间(ms)\n";
  for (const auto& r : rows) {
    std::cout << r.id << '\t' << r.account << '\t' << r.result << '\t'
              << r.kind << '\t' << r.name << '\t'
              << r.fingerprint.substr(0, 8) << '\t' << r.source_ip << '\t'
              << r.version << '\t' << r.ts_ms << '\n';
  }
  return 0;
}

// messages [账号] [--limit N]：管理员检索归档（T2.5 验收面；T3.2 扩展导出）
// 撤回消息原文照常可见并标「已撤回」——留痕纪律：撤回仅置标记不清正文。
int cmd_messages(int argc, char** argv, const std::string& db_path) {
  std::string account;
  int limit = 200;
  for (int i = 0; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--limit" && i + 1 < argc) {
      limit = std::atoi(argv[++i]);
      if (limit <= 0) {
        std::cerr << "无效 limit\n";
        return 2;
      }
    } else {
      account = argv[i];
    }
  }
  memex::server::ServerStore store;
  if (!store.open(db_path)) {
    std::cerr << "本地库打开失败：" << db_path << "\n";
    return 1;
  }
  const auto rows = store.messages(account, limit);
  std::cout << "msg_id\t发送方\t接收方\t类型\t状态\t时间\t正文\n";
  for (const auto& m : rows) {
    std::time_t secs = static_cast<std::time_t>(m.ts_ms / 1000);
    std::tm tm{};
    localtime_r(&secs, &tm);
    char when[24];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    std::cout << m.msg_id << '\t' << m.from_account << '\t' << m.to_account
              << '\t' << memex::protocol::msg_type_name(
                             static_cast<memex::protocol::MsgType>(m.type))
              << '\t' << (m.recalled ? "已撤回" : "正常") << '\t' << when
              << '\t' << m.text << '\n';
  }
  std::cout << "共 " << rows.size() << " 条\n";
  return 0;
}

int self_test() {
  asio::io_context io;
  asio::ip::tcp::acceptor a(
      io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  std::cout << "self-test ok: listen 127.0.0.1:" << a.local_endpoint().port()
            << ", protocol " << memex::protocol::msg_type_name(
                                   memex::protocol::v1::HELLO)
            << std::endl;
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (argc > 1) {
    const std::string_view cmd = argv[1];
    if (cmd == "--version") {
      std::cout << "memex-server " << MEMEX_VERSION << std::endl;
      return 0;
    }
    if (cmd == "--self-test") return self_test();

    // 子命令统一支持 --db 覆盖库路径
    std::string db_path = kDefaultDb;
    int sub_argc = 0;
    char** sub_argv = argv + 2;
    char** sub_end = argv + argc;
    for (char** p = sub_argv; p < sub_end; ++p) {
      if (std::string_view(*p) == "--db" && p + 1 < sub_end) {
        db_path = *++p;
      } else {
        sub_argv[sub_argc++] = *p;
      }
    }

    if (cmd == "serve") return cmd_serve(sub_argc, sub_argv, db_path);
    if (cmd == "account") return cmd_account(sub_argc, sub_argv, db_path);
    if (cmd == "logins") return cmd_logins(sub_argc, sub_argv, db_path);
    if (cmd == "messages") return cmd_messages(sub_argc, sub_argv, db_path);
    std::cerr << "未知子命令：" << cmd << "\n"
              << "用法：memex_server [serve [--port N] [--db P]] | account add … | "
                 "logins [账号] | messages [账号] [--limit N] | --version | --self-test\n";
    return 2;
  }
  return cmd_serve(0, argv, kDefaultDb);
}
