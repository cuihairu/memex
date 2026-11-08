// 服务端管理 CLI 进程级验收：本用例补齐此前只走库级协议的运维入口——
// 身份绑定、常用联系人、跨态会话、存储编排、根用法与坏参数。临时库写入
// 系统临时目录并在测试退出清理；只断言可观察输出与退出码，不起网络服务。
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>

#include <sqlite3.h>

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

#ifndef MEMEX_SERVER_BIN
#error "MEMEX_SERVER_BIN 未定义（应传入 $<TARGET_FILE:memex_server>）"
#endif

std::string run_cli(const std::string& args) {
  const std::string out_path =
      (std::filesystem::temp_directory_path() /
       "memex-server-cli-test-output.txt")
          .string();
  const std::string command = std::string("\"" MEMEX_SERVER_BIN "\" ") + args +
                              " > '" + out_path + "' 2>&1";
  const int status = std::system(command.c_str());
  std::ifstream output(out_path);
  std::stringstream text;
  text << output.rdbuf();
  return std::to_string(status) + "\n" + text.str();
}

std::string temporary_db() {
  std::random_device random;
  return (std::filesystem::temp_directory_path() /
          ("memex-server-cli-" + std::to_string(random()) + ".db"))
      .string();
}

bool sqlite_exec(const std::string& path, const std::string& sql) {
  sqlite3* database = nullptr;
  if (sqlite3_open(path.c_str(), &database) != SQLITE_OK) {
    sqlite3_close(database);
    return false;
  }
  char* error = nullptr;
  const bool ok =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error) == SQLITE_OK;
  sqlite3_free(error);
  sqlite3_close(database);
  return ok;
}

} // namespace

int main() {
  const std::string db = temporary_db();
  {
    memex::server::ServerStore store;
    CHECK(store.open(db));
    CHECK(store.create_account("alice", "password-alice", "Alice", "member"));
    CHECK(store.create_account("bob", "password-bob", "Bob", "admin"));
    CHECK(store.store_message("cli-msg", "alice", "bob", 10, "cli evidence",
                              1700000000000));
    CHECK(store.cross_log_start("alice", "alice-device", "Alice",
                                1700000000000));
    CHECK(store.cross_log_end("alice", "alice-device", 1700000000000,
                              1700000600000));
    store.close();
  }

  CHECK(run_cli("--version").find("memex-server") != std::string::npos);
  CHECK(run_cli("--help").find("用法：memex_server") != std::string::npos);

  const std::string bound =
      run_cli("identity bind corp alice@example.com alice --db '" + db + "'");
  CHECK(bound.find("已绑定") != std::string::npos);
  CHECK(run_cli("identity bind corp alice@example.com alice --db '" + db + "'")
            .find("绑定失败") != std::string::npos);
  const std::string bindings =
      run_cli("identity list alice --db '" + db + "'");
  CHECK(bindings.find("corp/alice@example.com") != std::string::npos);
  CHECK(run_cli("identity unbind 1 --db '" + db + "'").find("已解绑") !=
        std::string::npos);
  CHECK(run_cli("identity unbind 999 --db '" + db + "'").find("解绑失败") !=
        std::string::npos);
  CHECK(run_cli("identity --db '" + db + "'").find("用法") !=
        std::string::npos);

  CHECK(run_cli("favs alice --db '" + db + "'").find("对端") !=
        std::string::npos);
  CHECK(run_cli("cross --db '" + db + "'").find("已结束") !=
        std::string::npos);
  CHECK(run_cli("cross 0 --db '" + db + "'").find("无效条数") !=
        std::string::npos);

  CHECK(run_cli("account add cli-user password-cli Cli --role member --db '" +
                db + "'")
            .find("已建号") != std::string::npos);
  CHECK(run_cli("account list --db '" + db + "'").find("cli-user") !=
        std::string::npos);
  CHECK(run_cli("account --db '" + db + "'").find("用法") !=
        std::string::npos);

  CHECK(run_cli("messages alice --limit 0 --db '" + db + "'")
            .find("无效 limit") != std::string::npos);
  CHECK(run_cli("messages alice --since not-a-date --db '" + db + "'")
            .find("无效 --since") != std::string::npos);
  CHECK(run_cli("messages alice --until not-a-date --db '" + db + "'")
            .find("无效 --until") != std::string::npos);
  CHECK(run_cli("messages --export /proc/memex-not-writable --db '" + db + "'")
            .find("导出文件无法写入") != std::string::npos);

  CHECK(sqlite_exec(db, "UPDATE messages SET text = 'cli evidence';"));
  CHECK(run_cli("messages alice --db '" + db + "'").find("cli evidence") !=
        std::string::npos);

  const std::string compose = (std::filesystem::temp_directory_path() /
                               "memex-server-cli-compose.yml")
                                  .string();
  const std::string compose_result =
      run_cli("storage compose --out '" + compose +
              "' --data-dir /tmp/memex-cli-rustfs --api-port 19000 "
              "--console-port 19001 --access-key real-key --secret-key "
              "real-secret --db '" +
              db + "'");
  CHECK(compose_result.find("已生成") != std::string::npos);
  CHECK(std::filesystem::exists(compose));
  CHECK(run_cli("storage --db '" + db + "'").find("用法") !=
        std::string::npos);
  CHECK(run_cli("storage up --db '" + db + "'").find("用法") !=
        std::string::npos);
  CHECK(run_cli("storage health --db '" + db + "'").find("用法") !=
        std::string::npos);

  std::error_code cleanup_error;
  std::filesystem::remove(db, cleanup_error);
  std::filesystem::remove(compose, cleanup_error);
  std::filesystem::remove(
      std::filesystem::temp_directory_path() /
          "memex-server-cli-test-output.txt",
      cleanup_error);

  if (g_failures != 0) {
    std::cerr << "FAILURES: " << g_failures << '\n';
    return 1;
  }
  std::cout << "server CLI tests passed\n";
  return 0;
}
