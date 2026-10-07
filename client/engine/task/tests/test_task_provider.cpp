// R27-2 外部任务 Provider SPI 单测（QtCore，无网络无 UI）：能力声明位、
// URL 模板槽替换与缺槽拒、直通链接 http(s) 门、注册表预设/未知 id、
// L1-only 调 L2/L3 默认不支持。「跳转链接是基本件」= 解析不出必空串。
#include <QCoreApplication>

#include <iostream>

#include <engine/task/task_provider.hpp>

using memex::client::ExternalTask;
using memex::client::kCapL1Jump;
using memex::client::kCapL2Read;
using memex::client::kCapL3Write;
using memex::client::TaskListFn;
using memex::client::TaskProvider;
using memex::client::TaskProviderRegistry;
using memex::client::TaskWriteFn;
using memex::client::UrlTemplateProvider;

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

// 双能力样例（L1+L2）：验声明位与便利判定；异步 list 同步回放
class ReadJumpProvider : public UrlTemplateProvider {
 public:
  ReadJumpProvider()
      : UrlTemplateProvider(QStringLiteral("readjump"),
                            QStringLiteral("读加跳"),
                            QStringLiteral("https://ex.example/{key}"),
                            kCapL1Jump | kCapL2Read) {}
  // L2 形态：拉一条外部条目（detailUrl 必带）
  void list(const TaskListFn& done) const override {
    ExternalTask t;
    t.provider_id = QStringLiteral("readjump");
    t.key = QStringLiteral("7");
    t.title = QStringLiteral("外部条目");
    t.detail_url = detail_url(QStringLiteral("7"));
    done(true, {t}, QString());
  }
};

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);

  // —— 能力声明位与便利判定 ——
  ReadJumpProvider rj;
  CHECK(rj.can_jump());
  CHECK(rj.can_read());
  CHECK(!rj.can_write());
  CHECK(rj.auth_kind() == memex::client::TaskProviderAuth::kNone); // 默认无凭据

  // —— URL 模板槽替换 ——
  const UrlTemplateProvider gh(QStringLiteral("t1"), QStringLiteral("模板"),
                               QStringLiteral(
                                   "https://github.com/{project}/issues/{key}"));
  CHECK(gh.detail_url(QStringLiteral("12"), QStringLiteral("org/repo")) ==
        QStringLiteral("https://github.com/org/repo/issues/12"));
  // 模板要 project 而未给 → 拒（不造半截链接）
  CHECK(gh.detail_url(QStringLiteral("12")).isEmpty());
  CHECK(gh.detail_url(QString()).isEmpty()); // 空键拒
  CHECK(gh.detail_url(QStringLiteral("  "), QStringLiteral("org")).isEmpty());
  // 无 project 槽模板：project 多给不碍事
  const UrlTemplateProvider lin(
      QStringLiteral("t2"), QStringLiteral("L"),
      QStringLiteral("https://linear.app/{project}/issue/{key}"));
  CHECK(lin.detail_url(QStringLiteral("ENG-9"), QStringLiteral("team")) ==
        QStringLiteral("https://linear.app/team/issue/ENG-9"));
  // 未知槽残留（模板写错）→ 拒
  const UrlTemplateProvider bad(QStringLiteral("t3"), QStringLiteral("坏"),
                                QStringLiteral("https://x/{site}/{key}"));
  CHECK(bad.detail_url(QStringLiteral("1"), QStringLiteral("p")).isEmpty());
  // 模板缺 {key} 槽=配置错 → 一律无跳转（不给整串原样假链接）
  const UrlTemplateProvider nokey(QStringLiteral("t4"), QStringLiteral("无键"),
                                  QStringLiteral("https://x/list"));
  CHECK(nokey.detail_url(QStringLiteral("1")).isEmpty());

  // —— 直通链接：只放行 http(s) ——
  memex::client::UrlPassthroughProvider pass;
  CHECK(pass.detail_url(QStringLiteral("https://a.example/t/9")) ==
        QStringLiteral("https://a.example/t/9"));
  CHECK(pass.detail_url(QStringLiteral("  http://a.example/x  ")) ==
        QStringLiteral("http://a.example/x")); // 首尾空白先修边
  CHECK(pass.detail_url(QStringLiteral("ftp://a/x")).isEmpty());
  CHECK(pass.detail_url(QStringLiteral("org/repo#12")).isEmpty());
  CHECK(pass.detail_url(QString()).isEmpty());

  // —— L2/L3 默认不支持（能力未开的级别调用即同步回「不可用」，不碰网）——
  bool l2_ok = true;
  pass.list([&](bool ok, const QVector<ExternalTask>&, const QString&) {
    l2_ok = ok;
  });
  CHECK(!l2_ok);
  bool w_ok = true;
  pass.create(QStringLiteral("标题"), QStringLiteral(""),
              [&](bool ok, const QString&, const QString&) { w_ok = ok; });
  CHECK(!w_ok);
  pass.complete(QStringLiteral("1"),
                [&](bool ok, const QString&, const QString&) { w_ok = ok; });
  CHECK(!w_ok);

  // —— L2 形态：拉到的条目 detailUrl 必带 ——
  QVector<ExternalTask> pulled;
  rj.list([&](bool ok, const QVector<ExternalTask>& items, const QString&) {
    if (ok) pulled = items;
  });
  CHECK(pulled.size() == 1);
  CHECK(!pulled.first().detail_url.isEmpty());

  // —— 注册表：内置预设、未知 id、自定义挂载 ——
  TaskProviderRegistry reg;
  CHECK(reg.providers().size() >= 5); // 内置：GitHub Issue/PR、GitLab、
                                      // Jira、Linear、直通链接
  CHECK(reg.detail_url(QStringLiteral("github-issue"),
                       QStringLiteral("12"),
                       QStringLiteral("cuihairu/memex")) ==
        QStringLiteral("https://github.com/cuihairu/memex/issues/12"));
  CHECK(reg.detail_url(QStringLiteral("github-pr"), QStringLiteral("3"),
                       QStringLiteral("cuihairu/memex")) ==
        QStringLiteral("https://github.com/cuihairu/memex/pull/3"));
  CHECK(reg.detail_url(QStringLiteral("gitlab-issue"), QStringLiteral("5"),
                       QStringLiteral("g/x")) ==
        QStringLiteral("https://gitlab.com/g/x/-/issues/5"));
  CHECK(reg.detail_url(QStringLiteral("jira"), QStringLiteral("PROJ-7"),
                       QStringLiteral("ex.atlassian.net")) ==
        QStringLiteral("https://ex.atlassian.net/browse/PROJ-7"));
  CHECK(reg.detail_url(QStringLiteral("linear"), QStringLiteral("ENG-1"),
                       QStringLiteral("team")) ==
        QStringLiteral("https://linear.app/team/issue/ENG-1"));
  CHECK(reg.detail_url(QStringLiteral("url"),
                       QStringLiteral("https://a.example/z/1"))
            == QStringLiteral("https://a.example/z/1"));
  CHECK(reg.detail_url(QStringLiteral("ghost"), QStringLiteral("1")).isEmpty());
  CHECK(reg.provider(QStringLiteral("ghost")) == nullptr);
  reg.add(new UrlTemplateProvider(QStringLiteral("zentao"),
                                  QStringLiteral("禅道"),
                                  QStringLiteral(
                                      "https://{project}/task-view-{key}.html")));
  CHECK(reg.detail_url(QStringLiteral("zentao"), QStringLiteral("1024"),
                       QStringLiteral("zentao.example")) ==
        QStringLiteral("https://zentao.example/task-view-1024.html"));
  CHECK(reg.provider(QStringLiteral("zentao")) != nullptr); // 接管所有权验证

  if (g_failures == 0) {
    std::cout << "test_task_provider: all checks passed\n";
    return 0;
  }
  std::cout << "test_task_provider: " << g_failures << " check(s) FAILED\n";
  return 1;
}
