// R27-2 自定义模板设置面冒烟：增/删/校验（必填、{key} 槽、撞内置预设、
// 重名）、列表观察、注册表集成（重挂/拒挂/删后回收/URL 解析）。org 独立
// 命名隔离（不碰真配置）、不起服务端。
#include <QApplication>
#include <QSettings>

#include <iostream>

#include <app/task_template_settings.hpp>
#include <engine/task/task_provider.hpp>

using memex::client::CustomTemplate;
using memex::client::TaskProviderRegistry;
using memex::client::TaskTemplateSettingsDialog;
using memex::client::custom_templates;
using memex::client::save_custom_templates;

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
  const QString kApp = QStringLiteral("task-providers");
  auto cleanup = [&] { QSettings(org, kApp).clear(); };

  // 初始：无落盘模板
  CHECK(custom_templates(org, kApp).isEmpty());

  TaskTemplateSettingsDialog dlg(nullptr, org, kApp);
  CHECK(dlg.template_count() == 0);

  // —— 校验腿（拒存＋状态提示）——
  CHECK(!dlg.add_template(QString(), QString(), QString()));
  CHECK(dlg.status_text().contains(QStringLiteral("必填")));
  CHECK(!dlg.add_template(QString(), QString(),
                          QStringLiteral("https://x/{key}")));
  CHECK(dlg.status_text().contains(QStringLiteral("必填")));
  CHECK(!dlg.add_template(QString(), QStringLiteral("zentao"),
                          QStringLiteral("https://x/tasks")));
  CHECK(dlg.status_text().contains(QStringLiteral("{key}")));
  CHECK(!dlg.add_template(QString(), QStringLiteral("github-issue"),
                          QStringLiteral("https://x/{key}")));
  CHECK(dlg.status_text().contains(QStringLiteral("撞内置")));

  // —— 合法增：名称空回退 id ——
  CHECK(dlg.add_template(QString(), QStringLiteral("zentao"),
                         QStringLiteral("https://zt.example.com/tasks/{key}")));
  CHECK(dlg.status_text().contains(QStringLiteral("已保存")));
  CHECK(dlg.template_count() == 1);
  auto saved = custom_templates(org, kApp);
  CHECK(saved.size() == 1);
  CHECK(saved[0].id == QStringLiteral("zentao"));
  CHECK(saved[0].name == QStringLiteral("zentao")); // 空名回退 id
  CHECK(saved[0].url_template ==
        QStringLiteral("https://zt.example.com/tasks/{key}"));

  // 带名增：名称保留
  CHECK(dlg.add_template(QStringLiteral("禅道项目"), QStringLiteral("zentao2"),
                         QStringLiteral("https://zt.example.com/p/{project}/t/{key}")));
  CHECK(dlg.template_count() == 2);
  saved = custom_templates(org, kApp);
  CHECK(saved.size() == 2);
  CHECK(saved[1].name == QStringLiteral("禅道项目"));

  // 重名拒（先删旧的再改）
  CHECK(!dlg.add_template(QString(), QStringLiteral("zentao"),
                          QStringLiteral("https://other/{key}")));
  CHECK(dlg.status_text().contains(QStringLiteral("已存在")));
  CHECK(custom_templates(org, kApp).size() == 2);

  // —— 注册表集成：重挂＋URL 解析 ——
  {
    TaskProviderRegistry reg;
    reg.reload_custom(org, kApp);
    const auto* p = reg.provider(QStringLiteral("zentao"));
    CHECK(p != nullptr);
    CHECK(p->name() == QStringLiteral("zentao"));
    CHECK(p->can_jump());
    CHECK(reg.detail_url(QStringLiteral("zentao"), QStringLiteral("42")) ==
          QStringLiteral("https://zt.example.com/tasks/42"));
    // project 槽解析＋拒半截（缺 project 不造坏链）
    CHECK(reg.detail_url(QStringLiteral("zentao2"), QStringLiteral("9"),
                          QStringLiteral("cuihairu")) ==
          QStringLiteral("https://zt.example.com/p/cuihairu/t/9"));
    CHECK(reg.detail_url(QStringLiteral("zentao2"), QStringLiteral("9"))
              .isEmpty());
    // 拒挂腿：撞内置预设的模板不挂（provider 仍命中内置件）
    QVector<CustomTemplate> bad = {{QStringLiteral("github-issue"),
                                    QStringLiteral("假 GitHub"),
                                    QStringLiteral("https://evil/{key}")}};
    save_custom_templates(bad, org, kApp);
    reg.reload_custom(org, kApp);
    const auto* builtin = reg.provider(QStringLiteral("github-issue"));
    CHECK(builtin != nullptr);
    CHECK(builtin->name() == QStringLiteral("GitHub Issue")); // 仍是内置
    CHECK(reg.detail_url(QStringLiteral("github-issue"), QStringLiteral("1"),
                          QStringLiteral("cuihairu/memex"))
              .startsWith(QStringLiteral("https://github.com/")));
    // 拒挂腿：缺 {key} 槽的模板不挂
    bad = {{QStringLiteral("nokey"), QStringLiteral("无槽"),
            QStringLiteral("https://x/tasks")}};
    save_custom_templates(bad, org, kApp);
    reg.reload_custom(org, kApp);
    CHECK(reg.provider(QStringLiteral("nokey")) == nullptr);
    // 恢复合法集
    save_custom_templates(saved, org, kApp);
    reg.reload_custom(org, kApp);
  }

  // —— 删：注册表回收＋列表同步 ——
  CHECK(dlg.remove_template(QStringLiteral("zentao2")));
  CHECK(dlg.status_text().contains(QStringLiteral("已删除")));
  CHECK(dlg.template_count() == 1);
  CHECK(!dlg.remove_template(QStringLiteral("zentao2"))); // 再删=未找到
  CHECK(custom_templates(org, kApp).size() == 1);
  {
    TaskProviderRegistry reg;
    reg.reload_custom(org, kApp);
    CHECK(reg.provider(QStringLiteral("zentao2")) == nullptr); // 删后回收
    CHECK(reg.provider(QStringLiteral("zentao")) != nullptr);
  }

  if (g_failures == 0) {
    std::cout << "test_task_template_settings: all checks passed\n";
    cleanup();
    return 0;
  }
  std::cout << "test_task_template_settings: " << g_failures
            << " check(s) FAILED\n";
  return 1;
}
