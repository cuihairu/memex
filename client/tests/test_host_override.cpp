// 部署批 424：域名→IP 解析覆盖规则解析（apply_host_rules 纯函数）——
// 命中回 IP／未命中・空规则・非法项・空 host 一律原样返回（默认不覆盖，
// 公网解析不受影响）；大小写不敏感精确匹配；多条规则分号分隔首中即回。
#include <QCoreApplication>
#include <QString>

#include <app/collab_defaults.hpp>

using memex::client::apply_host_rules;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);

  // 默认不覆盖：空规则串任何 host 原样返回
  CHECK(apply_host_rules(QStringLiteral("server.memex.cuihairu.site"),
                         QString()) ==
        QStringLiteral("server.memex.cuihairu.site"));
  CHECK(apply_host_rules(QStringLiteral("server.memex.cuihairu.site"),
                         QStringLiteral("  ")) ==
        QStringLiteral("server.memex.cuihairu.site"));

  // 未命中域名原样返回（IP 与其他域名不受影响）
  CHECK(apply_host_rules(QStringLiteral("192.168.5.188"),
                         QStringLiteral("server.memex.cuihairu.site=10.0.0.2")) ==
        QStringLiteral("192.168.5.188"));
  CHECK(apply_host_rules(QStringLiteral("other.example.com"),
                         QStringLiteral("server.memex.cuihairu.site=10.0.0.2")) ==
        QStringLiteral("other.example.com"));

  // 命中：精确匹配回覆盖 IP
  CHECK(apply_host_rules(QStringLiteral("server.memex.cuihairu.site"),
                         QStringLiteral("server.memex.cuihairu.site=192.168.5.5")) ==
        QStringLiteral("192.168.5.5"));

  // 大小写不敏感（域名语义）
  CHECK(apply_host_rules(QStringLiteral("Server.Memex.Cuihairu.Site"),
                         QStringLiteral("server.memex.cuihairu.site=192.168.5.5")) ==
        QStringLiteral("192.168.5.5"));

  // 多条规则：首中即回；非法项（缺 = / 空段 / 空 IP）跳过不断链
  CHECK(apply_host_rules(
            QStringLiteral("a.example.com"),
            QStringLiteral("bad; ;x=;a.example.com=10.1.1.1;b.example.com=10.1.1.2")) ==
        QStringLiteral("10.1.1.1"));
  CHECK(apply_host_rules(
            QStringLiteral("b.example.com"),
            QStringLiteral("bad; ;x=;a.example.com=10.1.1.1;b.example.com=10.1.1.2")) ==
        QStringLiteral("10.1.1.2"));

  // 键值两侧空白容忍
  CHECK(apply_host_rules(QStringLiteral("server.memex.cuihairu.site"),
                         QStringLiteral(" server.memex.cuihairu.site = 192.168.5.5 ")) ==
        QStringLiteral("192.168.5.5"));

  // host 本身为空：原样返回（连接点自会拦空值）
  CHECK(apply_host_rules(QString(),
                         QStringLiteral("server.memex.cuihairu.site=10.0.0.2"))
            .isEmpty());

  if (g_failures == 0) {
    qInfo("test_host_override: ALL PASS");
    return 0;
  }
  qCritical("test_host_override: %d FAILURES", g_failures);
  return 1;
}
