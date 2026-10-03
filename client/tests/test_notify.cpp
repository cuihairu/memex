// T4.10 通知子系统客户端验收：
// ① 分级裁决矩阵（decide／dnd_active 纯函数）与紧急程度映射；
// ② 个人通知偏好 QSettings 落盘往返（配置目录隔离，不碰用户真配置）；
// ③ 通知中心执行体：普通默认不弹、重要＝托盘强提醒信号、紧急＝置顶弹窗
//    （真抓 PNG ＋ 点「确认收悉」关闭）、全屏期间递延、退出全屏补弹；
// ④ 设置页交互（勾普通级→OK→落盘回读）；
// ⑤ 全链路（进程级，真实服务端）：serve --webhook-port → CLI 建 webhook
//    → HTTP POST 三级紧急程度 → 引擎 NOTICE（chat 渲染＋分级入口）→
//    建群＋群 webhook 双引擎扇出，本地库与服务端同源 compose 对账。
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <utility>

#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>
#include <memex/protocol/messages.hpp>

#include "notify_center.hpp"
#include "notify_prefs.hpp"

using memex::client::CollabEngine;
using memex::client::LocalStore;
using memex::client::NoticeLevel;
using memex::client::NotificationCenter;
using memex::client::NotifyPrefs;
using memex::client::PopupAction;
using memex::client::decide;
using memex::client::dnd_active;
using memex::client::level_from_urgency;

#ifndef MEMEX_SERVER_BIN
#error "MEMEX_SERVER_BIN 未定义（应传入 $<TARGET_FILE:memex_server>）"
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

bool wait_until(const std::function<bool()>& cond, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    if (timer.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return true;
}

quint16 free_port() {
  QTcpServer probe;
  probe.listen(QHostAddress::LocalHost, 0);
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

bool port_listening(quint16 port) {
  QTcpServer probe;
  if (probe.listen(QHostAddress::LocalHost, port)) {
    probe.close();
    return false;
  }
  return true;
}

QWidget* find_top(const QString& name) {
  const auto tops = QApplication::topLevelWidgets();
  for (auto* w : tops) {
    if (w->objectName() == name) return w;
  }
  return nullptr;
}

// 阻塞式 POST /hook/<path>：读到服务端 Connection: close 落 EOF。
// 返回 HTTP 状态码（连接失败 -1），resp 取整个响应报文。
int http_post(quint16 port, const QString& path, const QString& body,
              QString* resp = nullptr) {
  QTcpSocket sock;
  sock.connectToHost(QStringLiteral("127.0.0.1"), port);
  if (!sock.waitForConnected(3000)) return -1;
  const QByteArray b = body.toUtf8();
  QByteArray req = "POST " + path.toUtf8() +
                   " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                   "Content-Type: application/json\r\nContent-Length: " +
                   QByteArray::number(b.size()) +
                   "\r\nConnection: close\r\n\r\n";
  req += b;
  sock.write(req);
  sock.waitForBytesWritten(3000);
  QByteArray all;
  while (sock.waitForReadyRead(3000)) all += sock.readAll();
  all += sock.readAll();
  if (resp) *resp = QString::fromUtf8(all);
  if (!all.startsWith("HTTP/1.1 ")) return -1;
  return all.mid(9, 3).toInt();
}

// —— ① 分级裁决矩阵与免打扰（纯函数） ——
void test_decide_matrix() {
  NotifyPrefs p; // 默认：普通关／重要开／紧急开／免打扰关／全屏递延

  // 默认偏好（非免打扰、非全屏）
  CHECK(decide(NoticeLevel::Normal, p, false, false) == PopupAction::ChatOnly);
  CHECK(decide(NoticeLevel::Important, p, false, false) == PopupAction::Toast);
  CHECK(decide(NoticeLevel::Urgent, p, false, false) == PopupAction::Modal);

  // 三级弹窗开关（开＝弹；关＝仅站内消息）
  NotifyPrefs on = p;
  on.popup_normal = true;
  CHECK(decide(NoticeLevel::Normal, on, false, false) == PopupAction::Toast);
  NotifyPrefs off = p;
  off.popup_important = false;
  off.popup_urgent = false;
  CHECK(decide(NoticeLevel::Important, off, false, false) ==
        PopupAction::ChatOnly);
  CHECK(decide(NoticeLevel::Urgent, off, false, false) ==
        PopupAction::ChatOnly);

  // 免打扰：普通／重要静默；紧急不静默（必须确认收悉，不因免打扰丢）
  CHECK(decide(NoticeLevel::Normal, on, true, false) == PopupAction::ChatOnly);
  CHECK(decide(NoticeLevel::Important, p, true, false) ==
        PopupAction::ChatOnly);
  CHECK(decide(NoticeLevel::Urgent, p, true, false) == PopupAction::Modal);

  // 全屏／演示模式＋递延策略：三级全部进队列（含紧急，退出全屏补弹）
  CHECK(decide(NoticeLevel::Normal, on, false, true) == PopupAction::Defer);
  CHECK(decide(NoticeLevel::Important, p, false, true) ==
        PopupAction::Defer);
  CHECK(decide(NoticeLevel::Urgent, p, false, true) == PopupAction::Defer);
  // 全屏＋紧急＋免打扰：免打扰不拦紧急，但全屏递延仍生效
  CHECK(decide(NoticeLevel::Urgent, p, true, true) == PopupAction::Defer);
  // 策略为「照常弹窗」：全屏不递延
  NotifyPrefs allow = p;
  allow.fullscreen_allow = true;
  CHECK(decide(NoticeLevel::Urgent, allow, false, true) == PopupAction::Modal);
  CHECK(decide(NoticeLevel::Important, allow, false, true) ==
        PopupAction::Toast);

  // 紧急程度映射（0 未指定与未知值按普通）
  CHECK(level_from_urgency(0) == NoticeLevel::Normal);
  CHECK(level_from_urgency(1) == NoticeLevel::Normal);
  CHECK(level_from_urgency(2) == NoticeLevel::Important);
  CHECK(level_from_urgency(3) == NoticeLevel::Urgent);
  CHECK(level_from_urgency(99) == NoticeLevel::Normal);

  // 免打扰时段（跨零点与同日两态；配置非法＝未启用）
  NotifyPrefs d = p;
  d.dnd = true;
  d.dnd_start = QStringLiteral("22:00");
  d.dnd_end = QStringLiteral("08:00");
  CHECK(dnd_active(d, QTime(23, 30)));
  CHECK(dnd_active(d, QTime(3, 0)));
  CHECK(dnd_active(d, QTime(7, 59)));
  CHECK(!dnd_active(d, QTime(8, 0)));
  CHECK(!dnd_active(d, QTime(21, 59)));
  d.dnd_start = QStringLiteral("09:00");
  d.dnd_end = QStringLiteral("17:00");
  CHECK(dnd_active(d, QTime(12, 0)));
  CHECK(!dnd_active(d, QTime(18, 0)));
  CHECK(!dnd_active(d, QTime(8, 59)));
  d.dnd_start = QStringLiteral("10:00");
  d.dnd_end = QStringLiteral("10:00"); // 同刻区间＝未启用
  CHECK(!dnd_active(d, QTime(10, 0)));
  d.dnd_end = QStringLiteral("bad"); // 配置非法不放大成持续静默
  CHECK(!dnd_active(d, QTime(10, 0)));
  d.dnd = false;
  d.dnd_start = QStringLiteral("22:00");
  d.dnd_end = QStringLiteral("08:00");
  CHECK(!dnd_active(d, QTime(23, 0)));
}

// —— ② 偏好落盘往返 ——
void test_prefs_roundtrip() {
  NotifyPrefs p;
  p.popup_normal = true;
  p.popup_important = false;
  p.popup_urgent = true;
  p.dnd = true;
  p.dnd_start = QStringLiteral("21:30");
  p.dnd_end = QStringLiteral("07:45");
  p.fullscreen_allow = true;
  p.save();

  const NotifyPrefs q = NotifyPrefs::load();
  CHECK(q.popup_normal);
  CHECK(!q.popup_important);
  CHECK(q.popup_urgent);
  CHECK(q.dnd);
  CHECK(q.dnd_start == QStringLiteral("21:30"));
  CHECK(q.dnd_end == QStringLiteral("07:45"));
  CHECK(q.fullscreen_allow);

  // 还原默认（后续执行体断言都按默认偏好走）
  QSettings(QCoreApplication::organizationName(),
            QCoreApplication::applicationName())
      .clear();
  const NotifyPrefs d = NotifyPrefs::load();
  CHECK(!d.popup_normal);
  CHECK(d.popup_important);
  CHECK(d.popup_urgent);
  CHECK(!d.dnd);
  CHECK(!d.fullscreen_allow);
}

// —— ③ 通知中心：普通不弹、重要托盘信号、紧急弹窗抓图＋确认 ——
void test_center_execution() {
  NotificationCenter& c = NotificationCenter::instance();
  int toast = 0;
  QString last_title;
  QObject::connect(&c, &NotificationCenter::want_tray_notify, &c,
                   [&](const QString& title, const QString&) {
                     ++toast;
                     last_title = title;
                   });

  // 普通（默认关）＝仅站内消息：无托盘信号、无弹窗
  c.on_notice(QStringLiteral("通知"), QStringLiteral("普通标题"),
              QStringLiteral("普通内容"), 1, QString(), 0,
              QStringLiteral("id-normal"));
  CHECK(toast == 0);
  CHECK(find_top(QStringLiteral("notice_urgent_dialog")) == nullptr);

  // 重要＝桌面通知强提醒（托盘信号；窗口激活态不压制——通知中心直发）
  c.on_notice(QStringLiteral("通知"), QStringLiteral("重要标题"),
              QStringLiteral("重要内容"), 2,
              QStringLiteral("https://oa.local/i"), 0,
              QStringLiteral("id-important"));
  CHECK(toast == 1);
  CHECK(last_title == QStringLiteral("重要标题"));

  // 紧急＝置顶弹窗需确认收悉（不走托盘信号）
  c.on_notice(QStringLiteral("通知"), QStringLiteral("紧急标题"),
              QStringLiteral("紧急内容"), 3,
              QStringLiteral("https://oa.local/u"), 0,
              QStringLiteral("id-urgent"));
  QDialog* dlg = nullptr;
  CHECK(wait_until(
      [&] {
        dlg = qobject_cast<QDialog*>(
            find_top(QStringLiteral("notice_urgent_dialog")));
        return dlg != nullptr;
      },
      3000));
  if (dlg) {
    CHECK(dlg->testAttribute(Qt::WA_ShowModal) || dlg->isVisible());
    // 抓图（验收截图：真弹窗真渲染）
    const QString png =
        QDir::current().absoluteFilePath(QStringLiteral("notify_urgent_dialog.png"));
    CHECK(dlg->grab().save(png));
    CHECK(QFileInfo::exists(png));
    qInfo() << "紧急弹窗截图：" << png;
    auto* ack = dlg->findChild<QPushButton*>(QStringLiteral("btn_ack"));
    CHECK(ack != nullptr);
    if (ack) {
      ack->click(); // 确认收悉 → 关闭
      CHECK(wait_until(
          [&] {
            return find_top(QStringLiteral("notice_urgent_dialog")) ==
                   nullptr;
          },
          3000));
    }
  }
  CHECK(toast == 1); // 紧急弹窗不重复走托盘
}

// —— ③b 全屏递延与退出补弹 ——
void test_fullscreen_defer() {
  NotificationCenter& c = NotificationCenter::instance();
  if (c.deferred_count() != 0) {
    c.flush_deferred(); // 兜底清场
  }
  QWidget fs;
  fs.showFullScreen();
  if (!wait_until([&] { return fs.isFullScreen(); }, 2000)) {
    // offscreen 平台不给全屏态：该路径无法端到端（裁决面已在矩阵覆盖）
    qWarning("offscreen 平台无全屏状态，递延执行面跳过（decide 矩阵已覆盖）");
    fs.close();
    return;
  }

  c.on_notice(QStringLiteral("通知"), QStringLiteral("递延标题"),
              QStringLiteral("递延内容"), 3, QString(), 0,
              QStringLiteral("id-defer"));
  CHECK(c.deferred_count() == 1); // 全屏期间进队列
  CHECK(find_top(QStringLiteral("notice_urgent_dialog")) == nullptr);

  fs.showNormal(); // 退出全屏 → 事件过滤器补弹
  QDialog* dlg = nullptr;
  CHECK(wait_until(
      [&] {
        dlg = qobject_cast<QDialog*>(
            find_top(QStringLiteral("notice_urgent_dialog")));
        return dlg != nullptr;
      },
      3000));
  CHECK(c.deferred_count() == 0);
  if (dlg) {
    if (auto* ack = dlg->findChild<QPushButton*>(QStringLiteral("btn_ack"))) {
      ack->click();
      CHECK(wait_until(
          [&] {
            return find_top(QStringLiteral("notice_urgent_dialog")) ==
                   nullptr;
          },
          3000));
    }
  }
  fs.close();
}

// —— ④ 设置页交互：勾「普通通知弹窗」→ OK → 落盘回读 ——
void test_settings_dialog() {
  // 安全网：6 秒仍未交互就取消，避免 exec 挂死（超时即红，不假绿）
  QTimer::singleShot(6000, [] {
    if (auto* dlg = find_top(QStringLiteral("notify_settings_dialog"))) {
      if (auto* box =
              dlg->findChild<QDialogButtonBox*>(QStringLiteral("btn_settings"))) {
        if (auto* cancel = box->button(QDialogButtonBox::Cancel)) cancel->click();
      }
    }
  });
  QTimer::singleShot(0, [] {
    auto* dlg = find_top(QStringLiteral("notify_settings_dialog"));
    if (!dlg) return;
    auto* chk = dlg->findChild<QCheckBox*>(QStringLiteral("chk_normal"));
    CHECK(chk != nullptr);
    if (chk) chk->setChecked(true);
    auto* box =
        dlg->findChild<QDialogButtonBox*>(QStringLiteral("btn_settings"));
    CHECK(box != nullptr);
    if (box) {
      if (auto* ok = box->button(QDialogButtonBox::Ok)) ok->click();
    }
  });

  NotificationCenter::instance().show_settings(); // 阻塞到 accept/reject

  const NotifyPrefs q = NotifyPrefs::load();
  CHECK(q.popup_normal); // 交互结果落盘
  CHECK(q.popup_important);
  CHECK(q.popup_urgent);

  QSettings(QCoreApplication::organizationName(),
            QCoreApplication::applicationName())
      .clear(); // 还原默认
}

// —— ⑤ 全链路：真实服务端 webhook → 引擎 NOTICE → 分级执行面 ——
void test_e2e(const QString& tmp_path) {
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);
  const QString db = tmp_path + QStringLiteral("/srv.db");

  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("alice"),
                                    QStringLiteral("pass-a")},
        std::pair<QString, QString>{QStringLiteral("bob"),
                                    QStringLiteral("pass-b")}}) {
    CHECK(QProcess::execute(server_bin,
                            {QStringLiteral("account"), QStringLiteral("add"),
                             row.first, row.second, QStringLiteral("--db"),
                             db}) == 0);
  }

  const quint16 port = free_port();
  const quint16 wh_port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin,
               {QStringLiteral("serve"), QStringLiteral("--db"), db,
                QStringLiteral("--port"), QString::number(port),
                QStringLiteral("--webhook-port"), QString::number(wh_port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 8000));
  CHECK(wait_until([&] { return port_listening(wh_port); }, 8000));

  // 建 webhook（CLI 真跑，token 从 stdout 解析——与运维口径一致）
  const auto make_webhook = [&](const QString& target) {
    QProcess p;
    p.start(server_bin, {QStringLiteral("webhook"), QStringLiteral("create"),
                         QStringLiteral("--target"), target,
                         QStringLiteral("--name"), QStringLiteral("验收"),
                         QStringLiteral("--db"), db});
    p.waitForFinished(5000);
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    const QRegularExpression rx(QStringLiteral("token：(whk_[0-9a-fA-F]+)"));
    const auto m = rx.match(out);
    CHECK(m.hasMatch());
    return m.captured(1);
  };

  LocalStore store_a, store_b;
  CHECK(store_a.open(tmp_path + QStringLiteral("/a.db")));
  CHECK(store_b.open(tmp_path + QStringLiteral("/b.db")));
  CollabEngine a, b;
  a.attach_store(&store_a);
  b.attach_store(&store_b);

  int notice_a = 0, notice_b = 0, group_a = 0, toast = 0;
  QObject::connect(&a, &CollabEngine::notice_received, &a,
                   [&](const QString&, const QString&, const QString&, int,
                       const QString&, qint64, const QString&) {
                     ++notice_a;
                   });
  QObject::connect(&b, &CollabEngine::notice_received, &b,
                   [&](const QString&, const QString&, const QString&, int,
                       const QString&, qint64, const QString&) {
                     ++notice_b;
                   });
  QObject::connect(&a, &CollabEngine::group_message_received, &a,
                   [&](const QString&, const QString&, const QString&, qint64,
                       const QString&) { ++group_a; });
  QObject::connect(&NotificationCenter::instance(),
                   &NotificationCenter::want_tray_notify, &a,
                   [&](const QString&, const QString&) { ++toast; });
  // 与主窗同款接线：引擎信号 → 通知中心裁决（生产一进程一引擎；本测试
  // 双引擎同进程，只接 a——否则同一条群通知两个引擎各推一次＝弹两次）
  QObject::connect(&a, &CollabEngine::notice_received,
                   &NotificationCenter::instance(),
                   &NotificationCenter::on_notice);

  bool a_in = false, b_in = false;
  QObject::connect(&a, &CollabEngine::logged_in, &a,
                   [&](const QString&, const QString&) { a_in = true; });
  QObject::connect(&b, &CollabEngine::logged_in, &b,
                   [&](const QString&, const QString&) { b_in = true; });
  a.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
          QStringLiteral("pass-a"));
  b.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"),
          QStringLiteral("pass-b"));
  CHECK(wait_until([&] { return a_in && b_in; }, 8000));

  const QString tok_personal = make_webhook(QStringLiteral("alice"));
  CHECK(!tok_personal.isEmpty());

  // 个人·普通 → 站内消息渲染（chat 面）＋分级入口，但默认不弹
  QString resp;
  int status = http_post(
      wh_port, QStringLiteral("/hook/") + tok_personal,
      QStringLiteral(R"({"title":"站内提醒","content":"请查收周报",)"
                     R"("urgency":"normal"})"),
      &resp);
  CHECK(status == 200);
  CHECK(resp.contains(QStringLiteral("\"ok\":true")));
  CHECK(wait_until([&] { return notice_a == 1; }, 5000));
  CHECK(toast == 0); // 普通默认不弹
  {
    const auto hist = store_a.history(QStringLiteral("通知"));
    CHECK(hist.size() == 1);
    if (!hist.isEmpty()) {
      CHECK(hist[0].from == QStringLiteral("通知"));
      CHECK(hist[0].text ==
            QString::fromStdString(memex::protocol::compose_notice_text(
                "站内提醒", "请查收周报", "")));
      CHECK(!hist[0].msg_id.empty());
    }
  }

  // 个人·重要 → 桌面通知强提醒（托盘信号）＋分级入口
  status = http_post(
      wh_port, QStringLiteral("/hook/") + tok_personal,
      QStringLiteral(R"({"title":"巡检通报","content":"磁盘水位 82%",)"
                     R"("urgency":"important","jump_url":"https://oa.local/d"})"),
      &resp);
  CHECK(status == 200);
  CHECK(wait_until([&] { return notice_a == 2; }, 5000));
  CHECK(toast == 1);

  // 建群（引擎真跑）→ 群 webhook → 双引擎扇出＋紧急弹窗
  bool grp_ok = false;
  quint64 gid = 0;
  QObject::connect(&a, &CollabEngine::group_result, &a,
                   [&](bool ok, const QString&, const QString&, quint64 id) {
                     if (ok && !grp_ok) {
                       grp_ok = true;
                       gid = id;
                     }
                   });
  a.create_group(QStringLiteral("验收群"),
                 {QStringLiteral("alice"), QStringLiteral("bob")});
  CHECK(wait_until([&] { return grp_ok; }, 8000));
  CHECK(gid > 0);

  const QString tok_group =
      make_webhook(QStringLiteral("group:") + QString::number(gid));
  CHECK(!tok_group.isEmpty());
  status = http_post(
      wh_port, QStringLiteral("/hook/") + tok_group,
      QStringLiteral(R"({"title":"紧急通知","content":"机房割接",)"
                     R"("urgency":"urgent","jump_url":"https://oa.local/m"})"),
      &resp);
  CHECK(status == 200);
  CHECK(resp.contains(QStringLiteral("\"recipients\":2")));
  CHECK(wait_until([&] { return notice_a == 3 && notice_b == 1; }, 5000));
  CHECK(wait_until([&] { return group_a == 1; }, 3000));

  // 紧急群通知 → 置顶弹窗（真实端到端弹出）→ 抓图 → 确认收悉
  QDialog* dlg = nullptr;
  CHECK(wait_until(
      [&] {
        dlg = qobject_cast<QDialog*>(
            find_top(QStringLiteral("notice_urgent_dialog")));
        return dlg != nullptr;
      },
      3000));
  if (dlg) {
    const QString png = QDir::current().absoluteFilePath(
        QStringLiteral("notify_urgent_dialog.png"));
    CHECK(dlg->grab().save(png));
    qInfo() << "全链路紧急弹窗截图：" << png;
    if (auto* ack = dlg->findChild<QPushButton*>(QStringLiteral("btn_ack"))) {
      ack->click();
      CHECK(wait_until(
          [&] {
            return find_top(QStringLiteral("notice_urgent_dialog")) ==
                   nullptr;
          },
          3000));
    }
  }
  CHECK(toast == 1); // 群紧急走弹窗，不加托盘计数

  // 前后对账：本地库与服务端同源 compose（个人 2 条＋群 1 条／双方各见各的）
  CHECK(wait_until(
      [&] {
        return store_a
                   .history(QStringLiteral("group:") + QString::number(gid))
                   .size() == 1 &&
               store_b
                   .history(QStringLiteral("group:") + QString::number(gid))
                   .size() == 1;
      },
      5000));
  CHECK(store_a.history(QStringLiteral("通知")).size() == 2);
  {
    const auto gh = store_a.history(QStringLiteral("group:") +
                                    QString::number(gid));
    if (!gh.isEmpty()) {
      CHECK(gh[0].text ==
            QString::fromStdString(memex::protocol::compose_notice_text(
                "紧急通知", "机房割接", "https://oa.local/m")));
      CHECK(gh[0].from == QStringLiteral("通知"));
    }
  }

  a.logout();
  b.logout();
  server.terminate();
  server.waitForFinished(3000);
}

} // namespace

int main(int argc, char** argv) {
  // 配置隔离先行：QSettings 走临时目录，不碰用户真实通知偏好
  QTemporaryDir env;
  CHECK(env.isValid());
  qputenv("XDG_CONFIG_HOME", env.filePath(QStringLiteral("cfg")).toUtf8());

  QApplication app(argc, argv);
  QCoreApplication::setApplicationName(QStringLiteral("Memex"));
  QCoreApplication::setOrganizationName(QStringLiteral("memex"));

  QTemporaryDir tmp;
  CHECK(tmp.isValid());

  test_decide_matrix();
  test_prefs_roundtrip();
  test_center_execution();
  test_fullscreen_defer();
  test_settings_dialog();
  test_e2e(tmp.path());

  if (g_failures == 0) {
    qInfo("notify tests: all passed");
    return 0;
  }
  qCritical("notify tests: %d failure(s)", g_failures);
  return 1;
}
