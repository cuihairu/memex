// R27-3 provider 凭据存储单测（加密落盘）：首建→解锁→读写→换口令→上锁
// →跨实例重开（落盘持久）→错口令/残缺包拒→未解锁拒写。org 独立命名
// 隔离（QSettings 显式构造不吃 setPath 重定向——首轮实测踩坑），跑完
// 清理，不碰真配置。
#include <QCoreApplication>
#include <QFile>
#include <QSettings>

#include <iostream>

#include <app/task_provider_store.hpp>

using memex::client::TaskProviderStore;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " #cond    \
                << "\n";                                                  \
      ++g_failures;                                                       \
    }                                                                     \
  } while (false)

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  // org 独立命名（带 pid）：隔离真配置；跑完清文件
  const QString org = QStringLiteral("memex-selftest-%1")
                          .arg(QCoreApplication::applicationPid());
  auto cleanup = [&] {
    QSettings(org, QStringLiteral("task-providers")).clear();
  };

  TaskProviderStore store(org);
  CHECK(!store.has_store());
  CHECK(!store.is_unlocked());
  CHECK(store.providers().isEmpty());
  CHECK(!store.contains(QStringLiteral("github-issue")));

  // 未解锁拒写（不落盘）
  QJsonObject fields;
  fields.insert(QStringLiteral("repo"), QStringLiteral("cuihairu/memex"));
  fields.insert(QStringLiteral("token"), QStringLiteral("ghp-secret"));
  store.save(QStringLiteral("github-issue"), fields);
  CHECK(!store.has_store());

  // 首建（口令空拒）
  CHECK(!store.create(QString()));
  CHECK(store.create(QStringLiteral("pass-1")));
  CHECK(store.has_store());
  CHECK(store.is_unlocked());
  CHECK(!store.create(QStringLiteral("pass-2"))); // 已存在拒覆盖

  // 解锁态读写
  store.save(QStringLiteral("github-issue"), fields);
  CHECK(store.contains(QStringLiteral("github-issue")));
  CHECK(store.config(QStringLiteral("github-issue"))
            .value(QStringLiteral("repo"))
            .toString() == QStringLiteral("cuihairu/memex"));
  CHECK(store.config(QStringLiteral("github-issue"))
            .value(QStringLiteral("token"))
            .toString() == QStringLiteral("ghp-secret"));
  CHECK(store.providers().size() == 1);
  CHECK(store.config(QStringLiteral("dingtalk-todo")).isEmpty()); // 未配=空

  // 上锁：会话内存清空，落盘仍在
  store.lock();
  CHECK(store.has_store());
  CHECK(!store.is_unlocked());
  CHECK(!store.contains(QStringLiteral("github-issue")));
  CHECK(store.config(QStringLiteral("github-issue")).isEmpty());

  // 错口令拒（GCM tag 校验即口令校验）
  CHECK(!store.unlock(QStringLiteral("wrong")));
  CHECK(!store.is_unlocked());
  // 正口令解锁（跨实例：新对象读同一落盘包）
  TaskProviderStore store2(org);
  CHECK(store2.unlock(QStringLiteral("pass-1")));
  CHECK(store2.is_unlocked());
  CHECK(store2.config(QStringLiteral("github-issue"))
            .value(QStringLiteral("token"))
            .toString() == QStringLiteral("ghp-secret"));

  // 解锁态换口令：新盐重包裹；旧口令失效、新口令可解
  CHECK(store2.change_passphrase(QStringLiteral("pass-2")));
  CHECK(store2.unlock(QStringLiteral("pass-2"))); // 已解锁幂等
  store2.lock();
  CHECK(!store2.unlock(QStringLiteral("pass-1"))); // 旧口令失效
  CHECK(store2.unlock(QStringLiteral("pass-2")));
  CHECK(store2.config(QStringLiteral("github-issue"))
            .value(QStringLiteral("repo"))
            .toString() == QStringLiteral("cuihairu/memex"));

  // 删除配置：整包重加密后仍可读其余
  QJsonObject fs;
  fs.insert(QStringLiteral("app_id"), QStringLiteral("cli-x"));
  store2.save(QStringLiteral("feishu-task"), fs);
  CHECK(store2.providers().size() == 2);
  store2.remove(QStringLiteral("github-issue"));
  CHECK(!store2.contains(QStringLiteral("github-issue")));
  CHECK(store2.contains(QStringLiteral("feishu-task")));
  store2.lock();
  TaskProviderStore store3(org);
  CHECK(store3.unlock(QStringLiteral("pass-2")));
  CHECK(!store3.contains(QStringLiteral("github-issue")));
  CHECK(store3.config(QStringLiteral("feishu-task"))
            .value(QStringLiteral("app_id"))
            .toString() == QStringLiteral("cli-x"));

  // 落盘只见密文四键，无明文口令/令牌
  QSettings s(org, QStringLiteral("task-providers"));
  s.beginGroup(QStringLiteral("task-providers"));
  const QStringList keys = s.allKeys();
  CHECK(keys.contains(QStringLiteral("salt")));
  CHECK(keys.contains(QStringLiteral("iters")));
  CHECK(keys.contains(QStringLiteral("nonce")));
  CHECK(keys.contains(QStringLiteral("ct")));
  for (const QString& k : keys) {
    CHECK(!s.value(k).toString().contains(QStringLiteral("ghp-secret")));
    CHECK(!s.value(k).toString().contains(QStringLiteral("pass-2")));
  }

  if (g_failures == 0) {
    std::cout << "test_task_provider_store: all checks passed\n";
    cleanup();
    return 0;
  }
  std::cout << "test_task_provider_store: " << g_failures
            << " check(s) FAILED\n";
  return 1;
}
