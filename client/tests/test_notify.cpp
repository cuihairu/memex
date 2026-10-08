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
#include <QComboBox>
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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <utility>

#if defined(__GNUC__)
#include <unistd.h> // _exit
// 显式落覆盖率（退出段硬着陆用）：weak——无覆盖率编译（Release 本地跑）时
// 符号空置不调用，--coverage 构建（CI 门禁）下指向 libgcov 真实现
extern "C" void __gcov_dump(void) __attribute__((weak));
#endif

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
using memex::client::SoundEvent;
using memex::client::decide;
using memex::client::dnd_active;
using memex::client::level_from_urgency;
using memex::client::presence_joined;
using memex::client::sound_should_play;

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

// —— 卡死定位（CI 慢机上曾经整跑 240s 超时且日志尾部被块缓冲吞掉）：
// 相位标记＋看门狗。wait_until 每圈推进 g_progress；任一相位超过
// WATCHDOG_TICKS×2s 无进展即打印卡点相位并硬退（ctest 上界内自行报红点名，
// 不再留给超时杀手）。相位串全部为静态字面量，看门狗线程无锁直读。
std::atomic<const char*> g_phase{"boot"};
std::atomic<int> g_progress{0};
constexpr int WATCHDOG_TICKS = 60; // 60×2s＝120s 无进展即判卡死

#define PHASE(p)                                                             \
  do {                                                                       \
    g_phase.store(p, std::memory_order_relaxed);                             \
    g_progress.fetch_add(1, std::memory_order_relaxed);                      \
    qInfo("phase: %s", p);                                                   \
    std::fflush(nullptr);                                                    \
  } while (false)

bool wait_until(const std::function<bool()>& cond, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    if (timer.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
    g_progress.fetch_add(1, std::memory_order_relaxed);
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

// 有界 CLI 跑法：QProcess::execute 无超时上界（CLI 卡住＝整测挂死），统一
// 走 start＋waitForFinished，超时杀进程并报 false。断言仍由调用方 CHECK。
bool run_cli(const QString& bin, const QStringList& args,
             int timeout_ms = 15000) {
  QProcess p;
  p.start(bin, args);
  if (!p.waitForStarted(5000)) return false;
  if (!p.waitForFinished(timeout_ms)) {
    p.kill();
    p.waitForFinished(2000);
    return false;
  }
  return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
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
  // 事件通知开关组＋提示音三档（用户令 2026-10-08）
  p.popup_peer_online = false;
  p.popup_message = false;
  p.popup_file_arrive = false;
  p.popup_transfer_done = true;
  p.sound_mode = 2;
  p.sound_msg = false;
  p.sound_file = true;
  p.sound_online = false;
  p.save();

  const NotifyPrefs q = NotifyPrefs::load();
  CHECK(q.popup_normal);
  CHECK(!q.popup_important);
  CHECK(q.popup_urgent);
  CHECK(q.dnd);
  CHECK(q.dnd_start == QStringLiteral("21:30"));
  CHECK(q.dnd_end == QStringLiteral("07:45"));
  CHECK(q.fullscreen_allow);
  CHECK(!q.popup_peer_online);
  CHECK(!q.popup_message);
  CHECK(!q.popup_file_arrive);
  CHECK(q.popup_transfer_done);
  CHECK(q.sound_mode == 2);
  CHECK(!q.sound_msg);
  CHECK(q.sound_file);
  CHECK(!q.sound_online);

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
  CHECK(d.popup_peer_online);
  CHECK(d.popup_message);
  CHECK(d.popup_file_arrive);
  CHECK(!d.popup_transfer_done);
  CHECK(d.sound_mode == 0); // 全部关闭（默认档）
}

// —— ②b 提示音三档穷举＋presence 对比（纯函数）——
void test_sound_and_presence() {
  // mode 0＝全部关闭：任何事件任何位都不播
  for (const auto ev : {SoundEvent::Message, SoundEvent::File,
                        SoundEvent::Online}) {
    CHECK(!sound_should_play(0, true, true, true, ev));
  }
  // mode 1＝每条都播：任何事件任何位都播
  for (const auto ev : {SoundEvent::Message, SoundEvent::File,
                        SoundEvent::Online}) {
    CHECK(sound_should_play(1, false, false, false, ev));
  }
  // mode 2＝按事件类型：对应位裁决
  CHECK(sound_should_play(2, true, false, false, SoundEvent::Message));
  CHECK(!sound_should_play(2, true, false, false, SoundEvent::File));
  CHECK(sound_should_play(2, false, true, false, SoundEvent::File));
  CHECK(!sound_should_play(2, false, true, false, SoundEvent::Online));
  CHECK(sound_should_play(2, false, false, true, SoundEvent::Online));
  // 未知 mode 值按全关（坏值不放大成噪声）
  CHECK(!sound_should_play(7, true, true, true, SoundEvent::Message));

  // presence 前后对比：新增保序／重复去重／无新增空表
  const QStringList joined = presence_joined(
      {QStringLiteral("alice"), QStringLiteral("bob")},
      {QStringLiteral("bob"), QStringLiteral("carol"),
       QStringLiteral("alice"), QStringLiteral("dave"),
       QStringLiteral("carol")});
  CHECK(joined.size() == 2);
  CHECK(joined.at(0) == QStringLiteral("carol"));
  CHECK(joined.at(1) == QStringLiteral("dave"));
  CHECK(presence_joined({QStringLiteral("alice")},
                        {QStringLiteral("alice")})
            .isEmpty());
  CHECK(presence_joined({}, {QStringLiteral("alice")})
            .size() == 1); // 首次推送＝全员「新增」（自己由调用方排除）
}

// —— ③ 通知中心：普通不弹、重要托盘信号、紧急弹窗抓图＋确认 ——
void test_center_execution() {
  NotificationCenter& c = NotificationCenter::instance();
  int toast = 0;
  QString last_title;
  // CI 挂死根因（2026-10-04）：context 曾挂单例 &c（永生），lambda 捕获的
  // toast/last_title 是本函数栈——函数返回后连接仍活着，e2e 阶段第二条个人
  // 通知（important→want_tray_notify）把悬垂 lambda 点燃，读死栈位（ASAN：
  // stack-use-after-scope，经 dispatch→Toast 链；本地 Release 或崩或腐坏
  // e2e 活栈，CI Debug/gcov 演成 240s 挂死）。改挂本函数栈上的 context：
  // 返回即 auto 断连，断言与作用域内语义不变。
  QObject scope;
  QObject::connect(&c, &NotificationCenter::want_tray_notify, &scope,
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
    // 事件通知开关组＋提示音三档往返（用户令 2026-10-08）
    auto* transfer =
        dlg->findChild<QCheckBox*>(QStringLiteral("chk_popup_transfer"));
    CHECK(transfer != nullptr);
    if (transfer) transfer->setChecked(true);
    auto* sound_mode =
        dlg->findChild<QComboBox*>(QStringLiteral("cmb_sound_mode"));
    CHECK(sound_mode != nullptr);
    if (sound_mode) sound_mode->setCurrentIndex(2);
    auto* snd_msg = dlg->findChild<QCheckBox*>(QStringLiteral("chk_sound_msg"));
    CHECK(snd_msg != nullptr);
    CHECK(snd_msg && snd_msg->isEnabled()); // mode=2 联动启用
    if (snd_msg) snd_msg->setChecked(false);
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
  CHECK(q.popup_transfer_done);
  CHECK(q.sound_mode == 2);
  CHECK(!q.sound_msg); // mode=2 位裁决落盘
  CHECK(q.sound_file);

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
    PHASE("e2e:account-add");
    CHECK(run_cli(server_bin, {QStringLiteral("account"), QStringLiteral("add"),
                               row.first, row.second, QStringLiteral("--db"),
                               db}));
  }

  PHASE("e2e:server-start");
  const quint16 port = free_port();
  const quint16 wh_port = free_port();
  QProcess server;
  server.setProcessChannelMode(QProcess::ForwardedChannels);
  server.start(server_bin,
               {QStringLiteral("serve"), QStringLiteral("--db"), db,
                QStringLiteral("--port"), QString::number(port),
                QStringLiteral("--webhook-port"), QString::number(wh_port)});
  CHECK(server.waitForStarted(5000));
  CHECK(wait_until([&] { return port_listening(port); }, 15000));
  CHECK(wait_until([&] { return port_listening(wh_port); }, 15000));

  // 建 webhook（CLI 真跑，token 从 stdout 解析——与运维口径一致）
  const auto make_webhook = [&](const QString& target) {
    PHASE("e2e:webhook-create");
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
  PHASE("e2e:login");
  // 平台-12 建群需特权（权限模型「建群需授权」）：alice 授 group_creator
  CHECK(QProcess::execute(server_bin,
                          {QStringLiteral("org"), QStringLiteral("role"),
                           QStringLiteral("grant"), QStringLiteral("alice"),
                           QStringLiteral("group_creator"),
                           QStringLiteral("--by"), QStringLiteral("alice"),
                           QStringLiteral("--db"), db}) == 0);
  a.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
          QStringLiteral("pass-a"));
  b.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"),
          QStringLiteral("pass-b"));
  CHECK(wait_until([&] { return a_in && b_in; }, 15000));

  const QString tok_personal = make_webhook(QStringLiteral("alice"));
  CHECK(!tok_personal.isEmpty());

  // 个人·普通 → 站内消息渲染（chat 面）＋分级入口，但默认不弹
  PHASE("e2e:post-normal");
  QString resp;
  int status = http_post(
      wh_port, QStringLiteral("/hook/") + tok_personal,
      QStringLiteral(R"({"title":"站内提醒","content":"请查收周报",)"
                     R"("urgency":"normal"})"),
      &resp);
  CHECK(status == 200);
  CHECK(resp.contains(QStringLiteral("\"ok\":true")));
  CHECK(wait_until([&] { return notice_a == 1; }, 10000));
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
  PHASE("e2e:post-important");
  status = http_post(
      wh_port, QStringLiteral("/hook/") + tok_personal,
      QStringLiteral(R"({"title":"巡检通报","content":"磁盘水位 82%",)"
                     R"("urgency":"important","jump_url":"https://oa.local/d"})"),
      &resp);
  CHECK(status == 200);
  CHECK(wait_until([&] { return notice_a == 2; }, 10000));
  CHECK(toast == 1);

  // 建群（引擎真跑）→ 群 webhook → 双引擎扇出＋紧急弹窗
  PHASE("e2e:create-group");
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
  CHECK(wait_until([&] { return grp_ok; }, 15000));
  CHECK(gid > 0);

  const QString tok_group =
      make_webhook(QStringLiteral("group:") + QString::number(gid));
  CHECK(!tok_group.isEmpty());
  PHASE("e2e:post-urgent-group");
  status = http_post(
      wh_port, QStringLiteral("/hook/") + tok_group,
      QStringLiteral(R"({"title":"紧急通知","content":"机房割接",)"
                     R"("urgency":"urgent","jump_url":"https://oa.local/m"})"),
      &resp);
  CHECK(status == 200);
  CHECK(resp.contains(QStringLiteral("\"recipients\":2")));
  CHECK(wait_until([&] { return notice_a == 3 && notice_b == 1; }, 10000));
  CHECK(wait_until([&] { return group_a == 1; }, 6000));

  // 紧急群通知 → 置顶弹窗（真实端到端弹出）→ 抓图 → 确认收悉
  PHASE("e2e:urgent-dialog");
  QDialog* dlg = nullptr;
  CHECK(wait_until(
      [&] {
        dlg = qobject_cast<QDialog*>(
            find_top(QStringLiteral("notice_urgent_dialog")));
        return dlg != nullptr;
      },
      6000));
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
          6000));
    }
  }
  CHECK(toast == 1); // 群紧急走弹窗，不加托盘计数

  // 前后对账：本地库与服务端同源 compose（个人 2 条＋群 1 条／双方各见各的）
  PHASE("e2e:reconcile");
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

  PHASE("e2e:logout");
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

  // 看门狗线程：120s 无进展 → 打印卡点相位硬退（ctest 240s 上界内自行报红
  // 点名，日志尾部不再被块缓冲吞掉）。正常跑完经 g_done 收队。
  std::atomic<bool> g_done{false};
  std::thread watchdog([&g_done] {
    int stall = 0;
    int last = g_progress.load(std::memory_order_relaxed);
    while (!g_done.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      const int now = g_progress.load(std::memory_order_relaxed);
      stall = (now == last) ? stall + 1 : 0;
      last = now;
      if (stall >= WATCHDOG_TICKS) {
        std::fprintf(stderr,
                     "WATCHDOG: 相位「%s」已 %ds 无进展，判卡死硬退(70)\n",
                     g_phase.load(std::memory_order_relaxed),
                     stall * 2);
        std::fflush(nullptr);
        _exit(70); // 跳过退出链：此刻任何析构都可能正是卡点
      }
    }
  });

  PHASE("decide-matrix");
  test_decide_matrix();
  PHASE("prefs-roundtrip");
  test_prefs_roundtrip();
  PHASE("sound-and-presence");
  test_sound_and_presence();
  PHASE("center-execution");
  test_center_execution();
  PHASE("fullscreen-defer");
  test_fullscreen_defer();
  PHASE("settings-dialog");
  test_settings_dialog();
  PHASE("e2e");
  test_e2e(tmp.path());

  g_done.store(true);
  watchdog.join();

  if (g_failures == 0) {
    qInfo("notify tests: all passed");
  } else {
    qCritical("notify tests: %d failure(s)", g_failures);
  }

  // 退出段硬着陆：曾疑似卡在进程退出链（QApplication/静态析构/atexit 的
  // gcov 落盘在共享慢机上等待）。显式落覆盖率后 _exit，跳过全部退出析构；
  // 覆盖率数据不丢，退出挂死面归零。临时目录手动清（跳过其析构）。
  tmp.remove();
  env.remove();
  std::fflush(nullptr);
#if defined(__GNUC__)
  if (&__gcov_dump) __gcov_dump();
  _exit(g_failures == 0 ? 0 : 1);
#else
  return g_failures == 0 ? 0 : 1;
#endif
}
