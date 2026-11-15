// test_mode_switch 拆分共性面（LAY-014 自 test_mode_switch.cpp 逐字搬移）：
// include 块/别名/CHECK·等待·端口工具/跨腿共享态/腿声明；窗口等 QApplication 依赖对象仍由入口按原位持有。
#pragma once

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QShortcut>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUdpSocket>

#include <cstdlib>
#include <functional>

#include <app/main_window.hpp>
#include <app/rail_nav.hpp>
#include <app/net_guard.hpp>
#include <app/net_remote_settings.hpp>
#include <app/theme.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>
#include <engine/direct/direct_engine.hpp>

namespace away_lock = memex::client::away_lock; // 离开锁屏设置面（C++17 无 using-ns）
namespace net_blacklist = memex::client::net_blacklist;
namespace remote_control = memex::client::remote_control;
using memex::client::CollabEngine;
using memex::client::DirectEngine;
using memex::client::LocalStore;
using memex::client::MainWindow;
using memex::client::NetRemoteSettingsDialog;
using memex::client::StoredMessage;

// —— 跨腿共享态（LAY-014 自 main 局部量提升为 inline 全局；QApplication 依赖的 window/peer/da/db/server/run_cli 仍由入口按原位持有、以同名录参数传入，腿体逐字零改写）——

inline QTemporaryDir tmp;  // LAY-014 自 main:109 提升（构造先于 main：QTemporaryDir 走系统临时目录，与 XDG 重定向无关）
inline const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);  // 自 main:117 提升
inline const QString srv_db = tmp.filePath(QStringLiteral("srv.db"));  // 自 main:118 提升
inline quint16 port = 0;  // 入口起服时赋值（原 main:130 局部量改写）
inline int db_received = 0;  // 自 main:212 提升
inline bool da_delivered = false;  // 自 main:213 提升
inline bool peer_in = false;  // 自 main:708 提升
inline QString peer_msg;  // 自 main:709 提升

inline int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      qCritical("FAIL %s:%d %s", __FILE__, __LINE__, #cond);                 \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)

inline bool wait_until(const std::function<bool()>& cond, int timeout_ms) {
  QElapsedTimer timer;
  timer.start();
  while (!cond()) {
    if (timer.elapsed() > timeout_ms) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return true;
}

inline quint16 free_port() {
  QTcpServer probe;
  probe.listen(QHostAddress::LocalHost, 0);
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

inline bool port_listening(quint16 port) {
  QTcpServer probe;
  if (probe.listen(QHostAddress::LocalHost, port)) {
    probe.close();
    return false;
  }
  return true;
}

// 服务端 CLI 直读库（与在跑的 serve 进程并发：短暂锁冲突在轮询中自愈）
// LAY-014 自 main:140 run_cli lambda 提升为 inline 函数（读全局 server_bin），体逐字。
inline QString run_cli(const QStringList& args) {
    QProcess p;
    p.start(server_bin, args);
    if (!p.waitForFinished(5000)) return QString();
    return QString::fromUtf8(p.readAllStandardOutput());
}

// —— 腿声明（LAY-014）——
void leg_empty();
void leg_folder(DirectEngine& da);
void leg_layout_find(MainWindow& window);
void leg_collab_views(MainWindow& window, DirectEngine& db);
void leg_collab_flow(MainWindow& window);
void leg_msg_cross(MainWindow& window, CollabEngine& peer, DirectEngine& da, DirectEngine& db, const std::function<QString(const QStringList&)>& run_cli);
void leg_nudge_fallback(MainWindow& window, CollabEngine& peer, DirectEngine& da, DirectEngine& db, QProcess& server, const std::function<QString(const QStringList&)>& run_cli);
void leg_settings(MainWindow& window, DirectEngine& da, DirectEngine& db);
