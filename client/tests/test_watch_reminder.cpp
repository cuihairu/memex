// 「盯」提醒验收（todo「盯」单·实现单修订 2026-10-14）：盯创建／已读拒绝／
// 解除三通道（已读＝静默自动解·手动解·超时静默过期）／未读超阈值提醒只
// 一次（默认 10 分钟，QSettings 可配）／盯列表快照与取消／重启持久。
// 纯 UI 缝驱动（watch_current_row/unwatch_key/watch_read_fired/sweep_watches/
// watch_list_snapshot 与生产信号路径同口径）；超时腿经本机库回拨 created_ms。
#include <QApplication>
#include <QDateTime>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <app/main_window.hpp>

using memex::client::MainWindow;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

QString db_path() {
  return QStandardPaths::writableLocation(
             QStandardPaths::AppDataLocation) +
         QStringLiteral("/memex-local.db");
}

// 回拨某盯行的创建时刻（超时腿：未读超阈值提醒／超时过期）。独立连接
// 直写库（与窗口的连接并存；本机库 SQLite 支持多连接）
void backdate_watch(const QString& key, qint64 created_ms) {
  const QString conn = QStringLiteral("watch-backdate");
  {
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(db_path());
    CHECK(db.open());
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE watched_messages SET created_ms = ? WHERE key = ?"));
    q.addBindValue(created_ms);
    q.addBindValue(key);
    CHECK(q.exec());
    db.close();
  }
  QSqlDatabase::removeDatabase(conn);
}

qint64 now_ms() { return QDateTime::currentMSecsSinceEpoch(); }

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  // 配置与数据隔离：QSettings 与本地库全落临时 HOME（测后无污染）
  QCoreApplication::setOrganizationName(QStringLiteral("memex-test"));
  QCoreApplication::setApplicationName(QStringLiteral("watch-test"));
  QTemporaryDir home;
  CHECK(home.isValid());
  qputenv("HOME", home.path().toUtf8());

  const QString peer = QStringLiteral("dev-C8");
  const QString key = QStringLiteral("d:dev-C8:777");

  {
    MainWindow window;
    window.show();
    window.open_direct_peer(peer);

    // —— 盯创建：我发出的直连行可盯（锚＋灰 ◉ 落 meta 行）——
    window.inject_message(peer, QStringLiteral("盯住这条消息验收"), true,
                           QString(), QString(), 0, 777);
    CHECK(window.chat_html().contains(QStringLiteral("memexwatch:")));
    CHECK(!window.chat_html().contains(QStringLiteral("◉")));
    CHECK(window.watch_current_row(key));
    CHECK(window.watched_count() == 1);
    CHECK(window.is_watched(key));
    CHECK(window.chat_html().contains(QStringLiteral("◉")));
    CHECK(window.status_text().contains(QStringLiteral("已盯住本消息")));
    // 重复盯＝刷新（UPSERT）不报错不叠加
    CHECK(window.watch_current_row(key));
    CHECK(window.watched_count() == 1);

    // —— 盯列表：快照可见「我在盯哪些」，按下标取消＝手动解同路径 ——
    const QStringList snapshot = window.watch_list_snapshot();
    CHECK(snapshot.size() == 1);
    CHECK(snapshot.first().contains(QStringLiteral("直连")));
    CHECK(snapshot.first().contains(QStringLiteral("dev-C8")));
    CHECK(snapshot.first().contains(QStringLiteral("盯住这条消息验收")));
    CHECK(window.cancel_watch_by_index(0));
    CHECK(window.watched_count() == 0);
    CHECK(!window.chat_html().contains(QStringLiteral("◉")));
    CHECK(window.watch_list_snapshot().isEmpty());
    CHECK(!window.cancel_watch_by_index(3)); // 越界安全

    // —— 重建盯（供后续腿复用）；已读拒绝：receipt=read 拒绝创建 ——
    CHECK(window.watch_current_row(key));
    window.inject_message(peer, QStringLiteral("这条已被读"), true,
                           QString(), QStringLiteral("read"), 0, 778);
    CHECK(!window.watch_current_row(QStringLiteral("d:dev-C8:778")));
    CHECK(window.watched_count() == 1);
    CHECK(window.status_text().contains(QStringLiteral("无需盯")));

    // —— 已读＝静默自动解除（不提醒——已读通知不是盯）：摘 ◉、无状态、
    //    无列表高亮、无托盘文案 ——
    window.watch_read_fired(key, peer);
    CHECK(window.watch_released_count() == 1);
    CHECK(window.watched_count() == 0);
    CHECK(!window.chat_html().contains(QStringLiteral("◉")));
    CHECK(window.last_watch_notify().isEmpty()); // 静默：无提醒文案
    CHECK(window.watch_reminded_peers().isEmpty()); // 无列表高亮
    window.watch_read_fired(key, peer); // 幂等：无盯不重复触发
    CHECK(window.watch_released_count() == 1);

    // —— 协作域键（c:<msg_id>）：同口径创建与静默解除 ——
    window.open_collab_peer(QStringLiteral("alice"));
    window.inject_message(QStringLiteral("alice"),
                          QStringLiteral("协作态盯住验收"), true,
                          QStringLiteral("mid-abc"), QString(), 0, 0);
    CHECK(window.watch_current_row(QStringLiteral("c:mid-abc")));
    CHECK(window.watched_count() == 1);
    window.watch_read_fired(QStringLiteral("c:mid-abc"), QStringLiteral("alice"));
    CHECK(window.watch_released_count() == 2);
    CHECK(window.watched_count() == 0);
    window.open_direct_peer(peer);

    // —— 手动解除（菜单「取消盯」同路径）：摘 ◉ 且幂等 ——
    window.inject_message(peer, QStringLiteral("手动解腿消息"), true,
                           QString(), QString(), 0, 880);
    CHECK(window.watch_current_row(QStringLiteral("d:dev-C8:880")));
    window.unwatch_key(QStringLiteral("d:dev-C8:880"));
    CHECK(window.watched_count() == 0);
    CHECK(!window.chat_html().contains(QStringLiteral("◉")));
    window.unwatch_key(QStringLiteral("d:dev-C8:880")); // 未盯的键静默幂等
    CHECK(window.watched_count() == 0);

    // —— 未读超阈值提醒（默认 10 分钟）：阈值内不提醒，过阈值提醒一次 ——
    window.inject_message(peer, QStringLiteral("超时腿消息"), true,
                           QString(), QString(), 0, 900);
    const QString key900 = QStringLiteral("d:dev-C8:900");
    CHECK(window.watch_current_row(key900));
    backdate_watch(key900, now_ms() - 9 * 60 * 1000);
    window.sweep_watches(); // 阈值内：不提醒
    CHECK(window.watch_timeout_count() == 0);
    CHECK(window.watched_count() == 1);
    backdate_watch(key900, now_ms() - 11 * 60 * 1000);
    window.sweep_watches(); // 过阈值：提醒一次＋列表高亮
    CHECK(window.watch_timeout_count() == 1);
    CHECK(window.watched_count() == 1); // 提醒不解除（等已读／超时解）
    CHECK(window.last_watch_notify().contains(QStringLiteral("仍未读")));
    CHECK(window.last_watch_notify().contains(QStringLiteral("超时腿消息")));
    CHECK(window.watch_reminded_peers().contains(peer));
    window.sweep_watches(); // 只一次：不循环轰炸
    CHECK(window.watch_timeout_count() == 1);

    // —— 阈值可配：watch/remind_minutes=1 → 2 分钟未读即提醒 ——
    {
      QSettings settings(QCoreApplication::organizationName(),
                         QCoreApplication::applicationName());
      settings.setValue(QStringLiteral("watch/remind_minutes"), 1);
    }
    window.inject_message(peer, QStringLiteral("阈值可配腿消息"), true,
                           QString(), QString(), 0, 901);
    const QString key901 = QStringLiteral("d:dev-C8:901");
    CHECK(window.watch_current_row(key901));
    backdate_watch(key901, now_ms() - 2 * 60 * 1000);
    window.sweep_watches();
    CHECK(window.watch_timeout_count() == 2);
    CHECK(window.watched_count() == 2);
    {
      QSettings settings(QCoreApplication::organizationName(),
                         QCoreApplication::applicationName());
      settings.remove(QStringLiteral("watch/remind_minutes")); // 还原默认
    }

    // —— 超时静默过期（默认 24h）：解除不通知 ——
    backdate_watch(key900, now_ms() - 25 * 60 * 60 * 1000);
    window.sweep_watches();
    CHECK(window.watch_expired_count() == 1);
    CHECK(window.watched_count() == 1); // 只清 900，901 仍在
    CHECK(!window.chat_html().contains(QStringLiteral("d:dev-C8:900")));
    CHECK(window.last_watch_notify().contains(QStringLiteral("仍未读"))); // 过期不通知

    // —— 重启持久：盯行落库，新窗口装载后仍有效并重挂 ◉ ——
    CHECK(window.is_watched(key901));
  }

  {
    MainWindow window; // 同 HOME 新窗口＝重启语义
    window.show();
    CHECK(window.watched_count() == 1); // 盯行从库装载
    window.open_direct_peer(peer);
    window.inject_message(peer, QStringLiteral("阈值可配腿消息"), true,
                           QString(), QString(), 0, 901);
    CHECK(window.chat_html().contains(QStringLiteral("◉"))); // 重挂眼睛
    window.sweep_watches(); // 新窗口创建时刻＝现在，不误报超时
    CHECK(window.watch_timeout_count() == 0);
    CHECK(window.watched_count() == 1);
  }

  if (g_failures == 0) {
    qInfo("watch reminder tests: all passed");
    return 0;
  }
  qCritical("watch reminder tests: %d failure(s)", g_failures);
  return 1;
}
