// test_files_api 入口（LAY-005 自 main() 拆出）：夹具装配＋套件原序调用＋收尾。
// 装配语句与原 main() 等价（原栈布局改经 Fx 成员），场景体零改动。
#include "common.hpp"

namespace memex::server::tests_api {

int run_all() {
  test_safe_int_param();  // CR-2 单元腿：先于 HTTP 面，独立可红
  Fx f;
  CHECK(f.store.open(":memory:"));
  for (const char* a : {"owner1", "admin1", "member1", "outsider"}) {
    CHECK(f.store.create_account(a, "pw-" + std::string(a), a));
  }
  // 群 g1：owner1 群主，admin1 授管理员，member1 普通（角色走 store 层）
  f.gid = f.store.create_group("dev", "owner1",
                              {"owner1", "admin1", "member1"});
  CHECK(f.gid > 0);
  CHECK(f.store.group_set_role(f.gid, "admin1", "admin"));
  f.gtarget = "group:" + std::to_string(f.gid);

  f.fake = std::make_shared<FakeS3>();
  f.files = std::make_unique<memex::server::FileServer>(
      f.io, f.store, f.fake, 0);
  f.port = f.files->port();
  f.files->start_accept();
  std::thread th([&io = f.io] { io.run(); });

  files_suite(f);
  group_docs_suite(f);
  tools_suite(f);
  cred_servers_suite(f);
  platform_suite(f);
  tasks_suite(f);
  audit_brand_suite(f);
  assist_suite(f);

  f.io.stop();
  th.join();

  if (g_failures == 0) {
    std::cout << "test_files_api: all checks passed\n";
    return 0;
  }
  std::cout << "test_files_api: " << g_failures << " check(s) FAILED\n";
  return 1;
}

} // namespace memex::server::tests_api

int main() { return memex::server::tests_api::run_all(); }
