// R27-3 任务设置面冒烟：口令首建→解锁→GitHub/钉钉/飞书凭据保存（加密
// 落盘）→重载回填→必填校验→清除→换口令→旧口令失效。org 独立命名隔离
// （不碰真配置）、不起服务端。
#include <QApplication>
#include <QSettings>

#include <iostream>

#include <app/task_provider_settings.hpp>
#include <app/task_provider_store.hpp>

using memex::client::TaskProviderSettingsDialog;
using memex::client::TaskProviderStore;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);              \
      ++g_failures;                                                       \
    }                                                                     \
  } while (false)

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  const QString org = QStringLiteral("memex-selftest-%1")
                          .arg(QCoreApplication::applicationPid());
  auto cleanup = [&] {
    QSettings(org, QStringLiteral("task-providers")).clear();
  };

  TaskProviderStore store(org);
  TaskProviderSettingsDialog dlg(&store);
  CHECK(dlg.current_provider_id() == QStringLiteral("github-issue"));

  // 未解锁：空配置读（字段框为空串，不带货值）
  CHECK(!store.contains(QStringLiteral("github-issue")));
  CHECK(dlg.current_fields()
            .value(QStringLiteral("repo"))
            .toString()
            .isEmpty());
  CHECK(dlg.current_fields()
            .value(QStringLiteral("token"))
            .toString()
            .isEmpty());

  // 首建口令（无落盘包分支）
  CHECK(dlg.unlock_with(QStringLiteral("pass-1")));
  CHECK(dlg.status_text().contains(QStringLiteral("已解锁")));
  CHECK(store.is_unlocked());
  CHECK(dlg.unlock_with(QStringLiteral("other"))); // 已解锁幂等

  // 保存 GitHub 凭据（repo+token 必填）
  QJsonObject gh;
  gh.insert(QStringLiteral("repo"), QStringLiteral("cuihairu/memex"));
  gh.insert(QStringLiteral("token"), QStringLiteral("ghp-secret"));
  CHECK(dlg.save_current_provider(gh));
  CHECK(dlg.status_text().contains(QStringLiteral("已保存")));
  CHECK(store.contains(QStringLiteral("github-issue")));
  CHECK(store.config(QStringLiteral("github-issue"))
            .value(QStringLiteral("repo"))
            .toString() == QStringLiteral("cuihairu/memex"));
  // 回填
  CHECK(dlg.current_fields().value(QStringLiteral("repo")).toString() ==
        QStringLiteral("cuihairu/memex"));
  CHECK(dlg.current_fields().value(QStringLiteral("token")).toString() ==
        QStringLiteral("ghp-secret"));

  // 必填校验：token 空拒存
  QJsonObject bad;
  bad.insert(QStringLiteral("repo"), QStringLiteral("a/b"));
  CHECK(!dlg.save_current_provider(bad));
  CHECK(dlg.status_text().contains(QStringLiteral("必填")));

  // 钉钉：union_id 可空（获取流程留后续）
  dlg.show_provider(QStringLiteral("dingtalk-todo"));
  QJsonObject dt;
  dt.insert(QStringLiteral("app_key"), QStringLiteral("ding-key"));
  dt.insert(QStringLiteral("app_secret"), QStringLiteral("ding-sec"));
  CHECK(dlg.save_current_provider(dt));
  CHECK(store.contains(QStringLiteral("dingtalk-todo")));
  CHECK(store.config(QStringLiteral("dingtalk-todo"))
            .value(QStringLiteral("app_secret"))
            .toString() == QStringLiteral("ding-sec"));

  // 飞书：缺 app_id 拒存
  dlg.show_provider(QStringLiteral("feishu-task"));
  QJsonObject fs_missing;
  fs_missing.insert(QStringLiteral("app_secret"), QStringLiteral("s"));
  CHECK(!dlg.save_current_provider(fs_missing));
  dlg.show_provider(QStringLiteral("github-issue")); // 切走不误伤 GitHub

  // 清除钉钉（清除按钮同径=store.remove 后 changed 广播回填空）
  dlg.show_provider(QStringLiteral("dingtalk-todo"));
  store.remove(QStringLiteral("dingtalk-todo"));
  CHECK(!store.contains(QStringLiteral("dingtalk-todo")));

  // 换口令：整包重加密；上锁后旧口令失效、新口令可解
  CHECK(dlg.change_passphrase(QStringLiteral("pass-2")));
  store.lock();
  TaskProviderSettingsDialog dlg2(&store);
  CHECK(!dlg2.unlock_with(QStringLiteral("pass-1"))); // 旧口令拒
  CHECK(dlg2.unlock_with(QStringLiteral("pass-2")));
  CHECK(store.config(QStringLiteral("github-issue"))
            .value(QStringLiteral("token"))
            .toString() == QStringLiteral("ghp-secret"));

  if (g_failures == 0) {
    std::cout << "test_task_provider_settings: all checks passed\n";
    cleanup();
    return 0;
  }
  std::cout << "test_task_provider_settings: " << g_failures
            << " check(s) FAILED\n";
  return 1;
}
