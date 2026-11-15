// MemexServer 入口（LAY-006 自 main.cpp 拆出后保留）：
// 版本/自检脚手架＋子命令分发；各域命令实现在 server_cli/ 模块。
#include <asio.hpp>

#include <iostream>
#include <string>
#include <string_view>

#include <memex/protocol/messages.hpp>

#include "server_cli/common.hpp"

#ifndef MEMEX_VERSION
#define MEMEX_VERSION "dev"
#endif

namespace {

constexpr const char* kDefaultDb = "memex-server.db";


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
    if (cmd == "identity") return cmd_identity(sub_argc, sub_argv, db_path);
    if (cmd == "logins") return cmd_logins(sub_argc, sub_argv, db_path);
    if (cmd == "device") return cmd_device(sub_argc, sub_argv, db_path);
    if (cmd == "messages") return cmd_messages(sub_argc, sub_argv, db_path);
    if (cmd == "retention") return cmd_retention(sub_argc, sub_argv, db_path);
    if (cmd == "archive") return cmd_archive(sub_argc, sub_argv, db_path);
    if (cmd == "audit") return cmd_audit(sub_argc, sub_argv, db_path);
    if (cmd == "cross") return cmd_cross(sub_argc, sub_argv, db_path);
    if (cmd == "favs") return cmd_favs(sub_argc, sub_argv, db_path);
    if (cmd == "org") return cmd_org(sub_argc, sub_argv, db_path);
    if (cmd == "group") return cmd_group(sub_argc, sub_argv, db_path);
    if (cmd == "policy") return cmd_policy(sub_argc, sub_argv, db_path);
    if (cmd == "assist") return cmd_assist(sub_argc, sub_argv, db_path);
    if (cmd == "bot") return cmd_bot(sub_argc, sub_argv, db_path);
    if (cmd == "model") return cmd_model(sub_argc, sub_argv, db_path);
    if (cmd == "webhook") return cmd_webhook(sub_argc, sub_argv, db_path);
    if (cmd == "storage") return cmd_storage(sub_argc, sub_argv, db_path);
    if (cmd == "files") return cmd_files(sub_argc, sub_argv, db_path);
    if (cmd == "branding") return cmd_branding(sub_argc, sub_argv, db_path);
    std::cerr << "未知子命令：" << cmd << "\n"
              << "用法：memex_server [serve [--port N] [--webhook-port N] "
                 "[--model-port N] [--assistant <bot名> "
                 "[--assistant-token T]] "
                 "[--db P]] | account add … | "
                 "logins [账号] [--device 指纹前缀] | device … | "
                 "messages [账号] [--keyword K] [--since T] "
                 "[--until T] [--limit N] [--export 文件] | audit [N] | "
                 "cross [N] | favs <账号> | org … | group list|set-role | "
                 "archive status|seal|verify | "
                 "webhook create|list|revoke | "
                 "bot add|list|remove|disable|enable|join|leave | "
                 "model endpoint add|list|remove|enable|disable|local | calls | "
                 "assist policy|request|approve|deny|start|end|show|list|audit"
                 " | "
                 "storage compose|up|down|health | files verify | "
                 "--version | --self-test\n";
    return 2;
  }
  return cmd_serve(0, argv, kDefaultDb);
}

