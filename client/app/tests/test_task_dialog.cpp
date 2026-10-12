// R27-1 任务清单窗口冒烟：真服务端进程 × 离屏 QDialog。
// CollabEngine 建群（alice+bob 同群=分配资格夹具）→ TaskDialog 连接文件
// 面→自建→分配给 bob（权限模型「同群可派」）→ bob 侧清单见分派来源→
// bob 勾完成→bob 撤回→到期提醒（通知中心触发＋服务端回执只提醒一次）。
// 判权矩阵（无关系 default-deny/主人专属动作）走 test_files_api 协议腿，
// 不在此重复。
#include <QApplication>
#include <QCalendarWidget>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDir>
#include <QGroupBox>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTextCharFormat>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <functional>

#include <app/notify_center.hpp>
#include <app/task_dialog.hpp>
#include <app/task_provider_store.hpp>
#include <app/task_template_settings.hpp>
#include <app/theme.hpp>
#include <core/local_store.hpp>
#include <engine/collab/collab_engine.hpp>
#include <engine/task/task_http.hpp>

using memex::client::CollabEngine;
using memex::client::TaskDialog;
using memex::client::LocalStore;
using memex::client::NotificationCenter;
using memex::client::TaskProviderStore;
using memex::client::TaskTemplateSettingsDialog;

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
    QApplication::processEvents(QEventLoop::AllEvents, 30);
    QThread::msleep(5);
  }
  return cond();
}

quint16 free_port() {
  QTcpServer probe;
  probe.listen(QHostAddress::LocalHost, 0);
  const quint16 port = probe.serverPort();
  probe.close();
  return port;
}

// R27-3 拉取腿假传输：回放固定响应并记请求形态（同 provider 单测口径）
class FakeHttp : public memex::client::TaskHttp {
 public:
  QString method, err;
  QUrl url;
  int next_status = 200;
  QByteArray next_body = "{}";

  void request(const QString& m, const QUrl& u,
               const QList<QPair<QByteArray, QByteArray>>&,
               const QByteArray&, const memex::client::HttpFn& done) override {
    method = m;
    url = u;
    done(next_status, next_body, err);
  }
};

} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QTemporaryDir tmp;
  CHECK(tmp.isValid());
  const QString db = tmp.filePath(QStringLiteral("srv.db"));
  const QString server_bin = QStringLiteral(MEMEX_SERVER_BIN);

  for (const auto& row :
       {std::pair<QString, QString>{QStringLiteral("alice"),
                                    QStringLiteral("pass-1")},
        std::pair<QString, QString>{QStringLiteral("bob"),
                                    QStringLiteral("pass-2")}}) {
    CHECK(QProcess::execute(
              server_bin,
              {QStringLiteral("account"), QStringLiteral("add"), row.first,
               row.second, QStringLiteral("--db"), db}) == 0);
  }

  // 平台-12 建群需特权：alice 授 group_creator（建同群夹具=分配资格）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("role"),
             QStringLiteral("grant"), QStringLiteral("alice"),
             QStringLiteral("group_creator"), QStringLiteral("--by"),
             QStringLiteral("alice"), QStringLiteral("--db"),
             db}) == 0);

  // R27-7 领导视角夹具：bob ← alice（直属上级=看下属泳道的唯一判权）
  CHECK(QProcess::execute(
            server_bin,
            {QStringLiteral("org"), QStringLiteral("reporting"),
             QStringLiteral("set"), QStringLiteral("bob"),
             QStringLiteral("alice"), QStringLiteral("--db"),
             db}) == 0);

  // 双端口 free_port() + 重试整个 server 启动（最多 3 次，与
  // test_approval_dialog 同构）：吸收 CI 共享 runner 竞态——free_port
  // 探活与 serve bind 之间端口可被抢占，文件面 bind 失败即全腿级联
  quint16 collab_port = 0;
  quint16 files_port = 0;
  QProcess server;
  bool server_ok = false;
  for (int attempt = 0; attempt < 3 && !server_ok; ++attempt) {
    collab_port = free_port();
    files_port = free_port();
    if (collab_port == 0 || files_port == 0) {
      QThread::msleep(100);
      continue;
    }
    server.setProcessChannelMode(QProcess::ForwardedChannels);
    server.start(server_bin,
                 {QStringLiteral("serve"), QStringLiteral("--db"), db,
                  QStringLiteral("--port"), QString::number(collab_port),
                  QStringLiteral("--webhook-port"), QStringLiteral("0"),
                  QStringLiteral("--files-port"),
                  QString::number(files_port)});
    if (!server.waitForStarted(5000)) {
      qCritical("FAIL server 启动超时（尝试 %d/3）", attempt + 1);
      continue;
    }
    if (wait_until([&] {
          QTcpServer probe;
          return probe.listen(QHostAddress::LocalHost, files_port)
                     ? (probe.close(), false)
                     : true;
        }, 15000)) {
      server_ok = true;
      break;
    }
    qCritical("FAIL files_port %u 探活超时（尝试 %d/3），重试", files_port,
              attempt + 1);
    server.kill();
    server.waitForFinished(3000);
  }
  if (!server_ok) {
    qCritical("FAIL server 启动重试耗尽");
    return 1;
  }

  // 建群（alice 建，拉 bob）：同群=task-assign 规则的现查数据面
  LocalStore store_ce;
  CHECK(store_ce.open(tmp.filePath(QStringLiteral("ce.db"))));
  CollabEngine ce;
  ce.attach_store(&store_ce);
  bool ce_in = false;
  quint64 gid = 0;
  QObject::connect(&ce, &CollabEngine::logged_in, &ce,
                   [&](const QString&, const QString&) { ce_in = true; });
  QObject::connect(&ce, &CollabEngine::group_result, &ce,
                   [&](bool ok, const QString&, const QString& op, quint64 id) {
                     if (ok && op == QStringLiteral("create")) gid = id;
                   });
  ce.login(QStringLiteral("127.0.0.1"), collab_port, QStringLiteral("alice"),
           QStringLiteral("pass-1"));
  CHECK(wait_until([&] { return ce_in; }, 8000));
  ce.create_group(QStringLiteral("任务测试群"), {QStringLiteral("bob")});
  CHECK(wait_until([&] { return gid > 0; }, 8000));
  (void)gid;

  // 通知计数（提醒腿的观察点）
  int tray_notifies = 0;
  QObject::connect(&NotificationCenter::instance(),
                   &NotificationCenter::want_tray_notify, &app,
                   [&](const QString&, const QString&) { ++tray_notifies; });

  TaskDialog dlg;
  CHECK(!dlg.is_connected());

  // 错口令：状态栏明示连接失败
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("wrong"));
  CHECK(wait_until(
      [&] { return dlg.status_text().contains(QStringLiteral("连接失败")); },
      8000));
  CHECK(!dlg.is_connected());

  // 正口令：已连接并自动拉空列表
  dlg.connect_to(QStringLiteral("127.0.0.1"), files_port,
                 QStringLiteral("alice"), QStringLiteral("pass-1"));
  CHECK(wait_until(
      [&] {
        return dlg.is_connected() &&
               dlg.status_text().contains(QStringLiteral("已连接"));
      },
      8000));

  // 自建一条（不设提醒）
  CHECK(dlg.add_task(QStringLiteral("自建任务"), QString(), 0, QString()));
  CHECK(wait_until([&] { return dlg.list()->count() == 1; }, 8000));
  CHECK(dlg.list()->item(0)->text().contains(QStringLiteral("自建")));
  CHECK(dlg.list()->item(0)->text().contains(QStringLiteral("自建任务")));

  // 空标题本地拒（不发网）
  const int before = dlg.list()->count();
  CHECK(!dlg.add_task(QString(), QString(), 0, QString()));

  // 分配给 bob（alice/bob 同在任务测试群→task-assign 命中）
  CHECK(dlg.add_task(QStringLiteral("帮我看下机器"), QStringLiteral("10.0.0.9"),
                     0, QStringLiteral("bob")));
  CHECK(wait_until([&] { return dlg.list()->count() == 2; }, 8000));
  // 我的清单在前、我派出的行缀尾（[→] 派给 bob）
  CHECK(dlg.list()->item(0)->text().contains(QStringLiteral("自建任务")));
  CHECK(dlg.list()->item(1)->text().contains(QStringLiteral("派给 bob")));

  // bob 侧：清单出现 alice 派的任务（标来源）
  TaskDialog dlg_bob;
  dlg_bob.connect_to(QStringLiteral("127.0.0.1"), files_port,
                     QStringLiteral("bob"), QStringLiteral("pass-2"));
  CHECK(wait_until(
      [&] {
        return dlg_bob.is_connected() &&
               dlg_bob.status_text().contains(QStringLiteral("已连接"));
      },
      8000));
  CHECK(wait_until([&] { return dlg_bob.list()->count() == 1; }, 8000));
  CHECK(dlg_bob.list()->item(0)->text().contains(
      QStringLiteral("由 alice 分配")));
  CHECK(dlg_bob.list()->item(0)->text().contains(QStringLiteral("帮我看下机器")));

  // bob 勾完成（完成归清单主人）：列表翻 [x]
  dlg_bob.list()->setCurrentRow(0);
  CHECK(dlg_bob.toggle_selected_done());
  CHECK(wait_until([&] {
    return dlg_bob.list()->item(0) != nullptr &&
           dlg_bob.list()->item(0)->text().startsWith(QStringLiteral("[x]"));
  }, 8000));

  // bob 撤回（清单主人可删）：列表清空，alice 侧显式刷新后派出行消失
  //（跨端不推数据——各端靠自己的轮询/刷新对齐）
  dlg_bob.list()->setCurrentRow(0);
  CHECK(dlg_bob.delete_selected());
  CHECK(wait_until([&] { return dlg_bob.list()->count() == 0; }, 8000));
  dlg.refresh();
  CHECK(wait_until([&] { return dlg.list()->count() == 1; }, 8000));

  // 到期提醒：alice 建一条到期任务（due 在过去）→ check_due 触发通知＋
  // 服务端回执；再次 check_due 不重复提醒（reminded_ms 已落）
  const qint64 past = QDateTime::currentMSecsSinceEpoch() - 1000;
  CHECK(dlg.add_task(QStringLiteral("到期任务"), QString(), past, QString()));
  CHECK(wait_until([&] { return dlg.list()->count() == 2; }, 8000));
  dlg.check_due();
  CHECK(wait_until([&] { return tray_notifies >= 1; }, 8000));
  // 回执落库有竞态（提醒回执与列表刷新两个连接不保证序）——节流重拉
  qint64 last_pull = 0;
  CHECK(wait_until([&] {
    for (int i = 0; i < dlg.list()->count(); ++i) {
      if (dlg.list()->item(i)->text().contains(QStringLiteral("到期任务")) &&
          dlg.list()->item(i)->data(Qt::UserRole + 3).toLongLong() > 0) {
        return true;
      }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - last_pull > 500) {
      last_pull = now;
      dlg.refresh();
    }
    return false;
  }, 8000));
  const int notifies_after_first = tray_notifies;
  dlg.check_due();
  dlg.refresh(); // 回执落库后显式刷新对齐（轮询窗 30s 内手动对齐）
  QApplication::processEvents();
  CHECK(tray_notifies == notifies_after_first);

  // —— R27-2 外部任务：直通链接登记（粘贴链接即登记）→ 🌐 行＋详情
  //     解析；未知 provider 本地拒（不发网）；模板 provider 组合键；
  //     引用持久化（刷新回带复解析）；勾完成=memex 本地标记 ——
  const int before_ext = dlg.list()->count();
  CHECK(dlg.add_external_task(QStringLiteral("url"),
                              QStringLiteral("https://github.com/x/y/issues/9"),
                              QString()));
  CHECK(wait_until(
      [&] { return dlg.list()->count() == before_ext + 1; }, 8000));
  int ext_row = -1;
  for (int i = 0; i < dlg.list()->count(); ++i) {
    if (dlg.list()->item(i)->text().contains(QStringLiteral("🌐"))) {
      ext_row = i;
      break;
    }
  }
  CHECK(ext_row >= 0);
  dlg.list()->setCurrentRow(ext_row);
  // 标题空取键原文兜底；详情 URL 直出
  CHECK(dlg.list()->item(ext_row)->text().contains(
      QStringLiteral("https://github.com/x/y/issues/9")));
  CHECK(dlg.selected_detail_url() ==
        QStringLiteral("https://github.com/x/y/issues/9"));
  // 未知 provider：解析不出链接本地拒（不发网不落库）
  const int n_now = dlg.list()->count();
  CHECK(!dlg.add_external_task(QStringLiteral("ghost"),
                               QStringLiteral("org/repo#1"), QString()));
  CHECK(dlg.list()->count() == n_now);
  // 模板 provider 组合键（project#键）
  CHECK(dlg.add_external_task(QStringLiteral("github-issue"),
                              QStringLiteral("cuihairu/memex#12"), QString()));
  CHECK(wait_until([&] { return dlg.list()->count() == n_now + 1; }, 8000));
  int gh_row = -1;
  for (int i = 0; i < dlg.list()->count(); ++i) {
    if (dlg.list()->item(i)->text().contains(
            QStringLiteral("cuihairu/memex#12"))) {
      gh_row = i;
      break;
    }
  }
  CHECK(gh_row >= 0);
  dlg.list()->setCurrentRow(gh_row);
  CHECK(dlg.selected_detail_url() ==
        QStringLiteral("https://github.com/cuihairu/memex/issues/12"));
  // 持久化：显式刷新后引用回带、URL 复解析一致（跨端对齐同径）
  dlg.refresh();
  CHECK(wait_until([&] {
    for (int i = 0; i < dlg.list()->count(); ++i) {
      if (dlg.list()->item(i)->text().contains(
              QStringLiteral("cuihairu/memex#12"))) {
        dlg.list()->setCurrentRow(i);
        return dlg.selected_detail_url() ==
               QStringLiteral("https://github.com/cuihairu/memex/issues/12");
      }
    }
    return false;
  }, 8000));
  // 外部条目勾完成（memex 本地进度标记，回写外部是 L3 留 R27-3）
  dlg.list()->setCurrentRow(gh_row);
  CHECK(dlg.toggle_selected_done());
  CHECK(wait_until([&] {
    for (int i = 0; i < dlg.list()->count(); ++i) {
      if (dlg.list()->item(i)->text().contains(
              QStringLiteral("cuihairu/memex#12")) &&
          dlg.list()->item(i)->text().startsWith(QStringLiteral("[x]"))) {
        return true;
      }
    }
    return false;
  }, 8000));

  // —— R27-1 关窗后台提醒常驻化：关窗=hide 不销毁——连接与 30s 轮询
  //     持续（重开现窗不重建，后台提醒经常驻通知中心照发）——
  {
    auto* poll = dlg.findChild<QTimer*>(QStringLiteral("task_poll"));
    CHECK(poll != nullptr && poll->isActive());
    dlg.close();
    CHECK(!dlg.isVisible());
    CHECK(dlg.is_connected()); // 连接保活
    CHECK(poll->isActive());   // 轮询保活
    dlg.show();
    CHECK(dlg.isVisible()); // 重开现窗
  }

  // —— R27-3 拉取接线：独立 org 存储注入 → GitHub 凭据 → 拉取渲染 ⇣ 行
  //     （PR 滤除/详情解析/只读守卫/401 错误腿）；外部拉取不经文件面，
  //     断连态可用 ——
  const QString torg = QStringLiteral("memex-selftest-%1")
                           .arg(QCoreApplication::applicationPid());
  auto tcleanup = [&] {
    QSettings(torg, QStringLiteral("task-providers")).clear();
  };
  TaskProviderStore tstore(torg);
  CHECK(tstore.create(QStringLiteral("pass-1")));
  QJsonObject gh;
  gh.insert(QStringLiteral("repo"), QStringLiteral("cuihairu/memex"));
  gh.insert(QStringLiteral("token"), QStringLiteral("ghp-test"));
  tstore.save(QStringLiteral("github-issue"), gh);

  FakeHttp fake;
  TaskDialog dlg_pull(nullptr, &fake, &tstore);
  CHECK(dlg_pull.pull_provider_ids().contains(QStringLiteral("github-issue")));
  fake.next_status = 200;
  fake.next_body =
      "[{\"number\":7,\"title\":\"修部署重试\",\"state\":\"open\","
      "\"html_url\":\"https://github.com/cuihairu/memex/issues/7\"},"
      "{\"number\":8,\"title\":\"pr 项\",\"state\":\"open\","
      "\"html_url\":\"https://github.com/cuihairu/memex/pull/8\","
      "\"pull_request\":{\"url\":\"x\"}}]";
  dlg_pull.pull_external(QStringLiteral("github-issue"));
  CHECK(wait_until([&] {
    for (int i = 0; i < dlg_pull.list()->count(); ++i) {
      if (dlg_pull.list()->item(i)->text().contains(QStringLiteral("⇣"))) {
        return true;
      }
    }
    return false;
  }, 8000));
  int pull_row = -1;
  for (int i = 0; i < dlg_pull.list()->count(); ++i) {
    if (dlg_pull.list()->item(i)->text().contains(
            QStringLiteral("修部署重试"))) {
      pull_row = i;
      break;
    }
  }
  CHECK(pull_row >= 0);
  // PR 行已滤（同 provider 单测口径）
  CHECK(!dlg_pull.list()->item(pull_row)->text().contains(
      QStringLiteral("pr 项")));
  dlg_pull.list()->setCurrentRow(pull_row);
  CHECK(dlg_pull.selected_detail_url() ==
        QStringLiteral("https://github.com/cuihairu/memex/issues/7"));
  // 只读守卫：勾完成/撤回被拦（外部行无服务端 id，列表条数不变）
  const int pull_count = dlg_pull.list()->count();
  CHECK(!dlg_pull.toggle_selected_done());
  CHECK(dlg_pull.status_text().contains(QStringLiteral("只读")));
  CHECK(!dlg_pull.delete_selected());
  CHECK(dlg_pull.list()->count() == pull_count);
  // 401 错误腿：状态行明示拉取失败
  fake.next_status = 401;
  dlg_pull.pull_external(QStringLiteral("github-issue"));
  CHECK(wait_until(
      [&] {
        return dlg_pull.status_text().contains(QStringLiteral("拉取失败"));
      },
      8000));
  // 未配置凭据的 provider 不进拉取下拉
  CHECK(!dlg_pull.pull_provider_ids().contains(
      QStringLiteral("dingtalk-todo")));
  tcleanup();

  // R27-2 模板设置面接线：模板…按钮弹设置窗（非模态 WA_DeleteOnClose），
  // 关窗 finished→reload_custom＋登记下拉刷新（默认 org 只读不写配置）
  {
    const int combo_before = dlg_pull.ext_combo_count();
    dlg_pull.btn_templates()->click();
    auto* tdlg = dlg_pull.findChild<TaskTemplateSettingsDialog*>();
    CHECK(tdlg != nullptr);
    if (tdlg != nullptr) {
      tdlg->close(); // 触发 finished→reload_custom＋refresh_ext_combo
      CHECK(dlg_pull.ext_combo_count() == combo_before); // 无配置=不变
    }
  }
  qApp->processEvents(); // WA_DeleteOnClose 收尾防悬垂

  // —— R27-6 类型/开始时间/状态/看板/提醒开关 ——
  {
    auto find_row = [&dlg](const QString& title) -> int {
      for (int i = 0; i < dlg.task_count(); ++i) {
        if (dlg.list()->item(i)->text().contains(title)) return i;
      }
      return -1;
    };
    // 类型清单随登录拉取（服务端初始四类，缺省选中默认「任务」）
    CHECK(wait_until(
        [&] { return dlg.task_type_names().size() == 4; }, 8000));
    CHECK(dlg.task_type_names().contains(QStringLiteral("任务")) &&
          dlg.task_type_names().contains(QStringLiteral("需求")) &&
          dlg.task_type_names().contains(QStringLiteral("缺陷")) &&
          dlg.task_type_names().contains(QStringLiteral("其他")));
    const int def_idx =
        dlg.task_type_names().indexOf(QStringLiteral("任务"));
    CHECK(def_idx == 0); // is_default 排首（ORDER BY is_default DESC）

    // 带 type+start 创建：列表行带类型与开始时间（roles 回读）
    const qint64 start_past =
        QDateTime::currentMSecsSinceEpoch() - 2000;
    CHECK(dlg.add_task(QStringLiteral("类型甲"), QString(), 0, QString(),
                       QStringLiteral("需求"), start_past));
    CHECK(wait_until([&] { return find_row(QStringLiteral("类型甲")) >= 0; },
                     8000));
    int tz = find_row(QStringLiteral("类型甲"));
    CHECK(dlg.list()->item(tz)->text().contains(QStringLiteral("·需求")));
    CHECK(dlg.list()->item(tz)->text().contains(QStringLiteral("开始")));
    CHECK(dlg.list()->item(tz)->data(Qt::UserRole + 9).toString() ==
          QStringLiteral("需求"));
    CHECK(dlg.list()->item(tz)->data(Qt::UserRole + 7).toLongLong() ==
          start_past);
    CHECK(dlg.list()->item(tz)->data(Qt::UserRole + 10).toString() ==
          QStringLiteral("待办"));

    // 状态切换：待办→进行中→完成→待办（服务端双写 done，读取以 status
    // 为准）；状态按钮文案随选中态同步（进行中=暂停）
    dlg.list()->setCurrentRow(find_row(QStringLiteral("类型甲")));
    CHECK(dlg.set_selected_status(QStringLiteral("进行中")));
    CHECK(wait_until(
        [&] {
          const int r = find_row(QStringLiteral("类型甲"));
          return r >= 0 && dlg.list()->item(r)->text().contains(
                               QStringLiteral("（进行中）"));
        },
        8000));
    dlg.list()->setCurrentRow(find_row(QStringLiteral("类型甲")));
    bool saw_pause = false;
    for (auto* b : dlg.findChildren<QPushButton*>()) {
      if (b->text() == QStringLiteral("暂停")) saw_pause = true;
    }
    CHECK(saw_pause);
    dlg.list()->setCurrentRow(find_row(QStringLiteral("类型甲")));
    CHECK(dlg.set_selected_status(QStringLiteral("完成")));
    CHECK(wait_until(
        [&] {
          const int r = find_row(QStringLiteral("类型甲"));
          return r >= 0 &&
                 dlg.list()->item(r)->text().startsWith(
                     QStringLiteral("[x]"));
        },
        8000));
    CHECK(dlg.list()->item(find_row(QStringLiteral("类型甲")))
              ->data(Qt::UserRole + 1)
              .toBool()); // done 双写
    dlg.list()->setCurrentRow(find_row(QStringLiteral("类型甲")));
    CHECK(dlg.set_selected_status(QStringLiteral("待办")));
    CHECK(wait_until(
        [&] {
          const int r = find_row(QStringLiteral("类型甲"));
          return r >= 0 &&
                 !dlg.list()->item(r)->text().startsWith(
                     QStringLiteral("[x]"));
        },
        8000));
    CHECK(!dlg.list()->item(find_row(QStringLiteral("类型甲")))
               ->data(Qt::UserRole + 1)
               .toBool()); // done/done_ms 一并清

    // 开始到点提醒：start 已过（两开关默认开）→ check_due 触发＋
    // kind=start 回执（start_reminded_ms 与 deadline 回执相互独立）
    const int base_notifies = tray_notifies;
    dlg.check_due();
    CHECK(wait_until([&] { return tray_notifies >= base_notifies + 1; },
                     8000));
    last_pull = 0;
    CHECK(wait_until([&] {
      const int r = find_row(QStringLiteral("类型甲"));
      if (r >= 0 &&
          dlg.list()->item(r)->data(Qt::UserRole + 8).toLongLong() > 0 &&
          dlg.list()->item(r)->data(Qt::UserRole + 3).toLongLong() == 0) {
        return true;
      }
      const qint64 now = QDateTime::currentMSecsSinceEpoch();
      if (now - last_pull > 500) {
        last_pull = now;
        dlg.refresh();
      }
      return false;
    }, 8000));

    // 提醒开关：持久化（QSettings）＋闸门生效（关=到点不提醒）。收尾
    // remove 回缺省开（QSettings 走真配置，勿留测试值）
    auto* cb_start = dlg.findChild<QCheckBox*>(
        QStringLiteral("task_remind_start"));
    auto* cb_due =
        dlg.findChild<QCheckBox*>(QStringLiteral("task_remind_due"));
    CHECK(cb_start != nullptr && cb_due != nullptr);
    CHECK(cb_start->isChecked() && cb_due->isChecked()); // 默认开
    {
      QSettings s(QStringLiteral("memex"), QStringLiteral("collab"));
      cb_start->setChecked(false);
      CHECK(!s.value(QStringLiteral("task_remind_start"), true).toBool());
      cb_start->setChecked(true);
      CHECK(s.value(QStringLiteral("task_remind_start"), true).toBool());
    }
    // 开始提醒关闸：新建 start 已过的任务，check_due 不触发
    const qint64 start_past2 =
        QDateTime::currentMSecsSinceEpoch() - 2000;
    {
      QSettings s(QStringLiteral("memex"), QStringLiteral("collab"));
      s.setValue(QStringLiteral("task_remind_start"), false);
    }
    CHECK(dlg.add_task(QStringLiteral("类型乙"), QString(), 0, QString(),
                       QStringLiteral("缺陷"), start_past2));
    CHECK(wait_until([&] { return find_row(QStringLiteral("类型乙")) >= 0; },
                     8000));
    const int quiet1 = tray_notifies;
    dlg.check_due();
    QApplication::processEvents();
    CHECK(tray_notifies == quiet1);
    {
      QSettings s(QStringLiteral("memex"), QStringLiteral("collab"));
      s.setValue(QStringLiteral("task_remind_start"), true);
    }
    dlg.check_due();
    CHECK(wait_until([&] { return tray_notifies >= quiet1 + 1; }, 8000));
    // 类型乙回执落库后再动 deadline 闸（stale 列表会把同一行重燃，
    // 污染下一腿的静默计数——回执先对齐）
    last_pull = 0;
    CHECK(wait_until([&] {
      const int r = find_row(QStringLiteral("类型乙"));
      if (r >= 0 &&
          dlg.list()->item(r)->data(Qt::UserRole + 8).toLongLong() > 0) {
        return true;
      }
      const qint64 now = QDateTime::currentMSecsSinceEpoch();
      if (now - last_pull > 500) {
        last_pull = now;
        dlg.refresh();
      }
      return false;
    }, 8000));
    // deadline 提醒同闸：关=到期不提醒，开=触发
    const qint64 past_due =
        QDateTime::currentMSecsSinceEpoch() - 1000;
    {
      QSettings s(QStringLiteral("memex"), QStringLiteral("collab"));
      s.setValue(QStringLiteral("task_remind_due"), false);
    }
    CHECK(dlg.add_task(QStringLiteral("到期乙"), QString(), past_due,
                       QString()));
    CHECK(wait_until([&] { return find_row(QStringLiteral("到期乙")) >= 0; },
                     8000));
    const int quiet2 = tray_notifies;
    dlg.check_due();
    QApplication::processEvents();
    CHECK(tray_notifies == quiet2);
    {
      QSettings s(QStringLiteral("memex"), QStringLiteral("collab"));
      s.setValue(QStringLiteral("task_remind_due"), true);
    }
    dlg.check_due();
    CHECK(wait_until([&] { return tray_notifies >= quiet2 + 1; }, 8000));
    // 回执落库后不再重复提醒（节流重拉对齐，同到期提醒腿口径）
    last_pull = 0;
    CHECK(wait_until([&] {
      const int r = find_row(QStringLiteral("到期乙"));
      if (r >= 0 &&
          dlg.list()->item(r)->data(Qt::UserRole + 3).toLongLong() > 0) {
        return true;
      }
      const qint64 now = QDateTime::currentMSecsSinceEpoch();
      if (now - last_pull > 500) {
        last_pull = now;
        dlg.refresh();
      }
      return false;
    }, 8000));
    dlg.check_due();
    dlg.refresh();
    QApplication::processEvents();
    CHECK(tray_notifies == quiet2 + 1);
    {
      QSettings s(QStringLiteral("memex"), QStringLiteral("collab"));
      s.remove(QStringLiteral("task_remind_start"));
      s.remove(QStringLiteral("task_remind_due"));
    }

    // 看板：同一批任务数据的另一视图（三列分桶＋逾期红/完成灰；双击
    // 卡片推进状态）。看板只收本端数据（⇣/派出行不进看板）
    dlg.list()->setCurrentRow(find_row(QStringLiteral("类型甲")));
    CHECK(dlg.set_selected_status(QStringLiteral("进行中")));
    CHECK(wait_until(
        [&] {
          const int r = find_row(QStringLiteral("类型甲"));
          return r >= 0 && dlg.list()->item(r)->text().contains(
                               QStringLiteral("（进行中）"));
        },
        8000));
    dlg.toggle_kanban();
    CHECK(dlg.is_kanban());
    // mine 行：自建任务/到期任务/url外部/类型乙/到期乙=待办；类型甲=
    // 进行中；gh外部（已勾完成）=完成
    CHECK(dlg.kanban_count(0) == 5);
    CHECK(dlg.kanban_count(1) == 1);
    CHECK(dlg.kanban_count(2) == 1);
    // 逾期卡片红（到期任务 due 在过去且未完成）
    auto* todo_col =
        dlg.findChild<QListWidget*>(QStringLiteral("kanban_todo"));
    bool saw_red = false;
    for (int i = 0; i < todo_col->count(); ++i) {
      auto* card = todo_col->item(i);
      if (card->text().contains(QStringLiteral("到期任务")) &&
          card->foreground().color() == QColor(Qt::red)) {
        saw_red = true;
      }
    }
    CHECK(saw_red);
    // 双击推进：完成列卡片→待办；进行列卡片→完成
    auto* done_col =
        dlg.findChild<QListWidget*>(QStringLiteral("kanban_done"));
    CHECK(done_col->count() == 1);
    done_col->itemDoubleClicked(done_col->item(0)); // gh 外部→待办
    CHECK(wait_until([&] {
      return dlg.kanban_count(0) == 6 && dlg.kanban_count(2) == 0;
    }, 8000));
    auto* doing_col =
        dlg.findChild<QListWidget*>(QStringLiteral("kanban_doing"));
    CHECK(doing_col->count() == 1);
    doing_col->itemDoubleClicked(doing_col->item(0)); // 类型甲→完成
    CHECK(wait_until([&] {
      return dlg.kanban_count(1) == 0 && dlg.kanban_count(2) == 1;
    }, 8000));
    dlg.toggle_kanban(); // 回列表视图（后续日历腿与抓图按列表面走查）
    CHECK(!dlg.is_kanban());
  }

  // —— R27-7 领导视角：bob←alice 汇报线 → alice 端缺省开、每直属下属
  //     一条泳道（负荷三数随任务面走）、关=隐藏＋持久化、再开=重拉 ——
  {
    auto* cb_lead =
        dlg.findChild<QCheckBox*>(QStringLiteral("task_lead_view"));
    CHECK(cb_lead != nullptr && cb_lead->isChecked()); // 缺省开
    // bob 无任务时泳道也在（0 计数）：登录首拉已随连接发生
    CHECK(wait_until([&] { return dlg.lead_lane_count() == 1; }, 8000));
    // 播 bob 的任务面（alice 派：同群可派）：无期/逾期/临期
    const qint64 now7 = QDateTime::currentMSecsSinceEpoch();
    CHECK(dlg.add_task(QStringLiteral("泳道无期"), QString(), 0,
                       QStringLiteral("bob")));
    CHECK(dlg.add_task(QStringLiteral("泳道逾期"), QString(),
                       now7 - 86400000LL, QStringLiteral("bob")));
    CHECK(dlg.add_task(QStringLiteral("泳道临期"), QString(),
                       now7 + 2 * 86400000LL, QStringLiteral("bob")));
    // 关＝隐藏＋QSettings 落 false；再开＝重拉（不等 30s 轮询）。
    // 泳道数据走 QNAM 连接池（GET 与前置 POST 不保序）——节流重开关
    // 重拉对齐（同回执落库竞态的节流重拉口径）
    cb_lead->setChecked(false);
    CHECK(dlg.lead_lane_count() == 0);
    {
      QSettings s(QStringLiteral("memex"), QStringLiteral("collab"));
      CHECK(!s.value(QStringLiteral("task_lead_view"), true).toBool());
    }
    cb_lead->setChecked(true);
    const QString lane_title =
        QStringLiteral("bob（未完成 3 · 逾期 1 · 7 天内到期 1）");
    qint64 last_toggle = 0;
    CHECK(wait_until(
        [&] {
          for (auto* box : dlg.findChildren<QGroupBox*>()) {
            if (box->title() == lane_title) {
              auto* cards = box->findChild<QListWidget*>();
              return cards != nullptr && cards->count() == 3;
            }
          }
          const qint64 now = QDateTime::currentMSecsSinceEpoch();
          if (now - last_toggle > 700) {
            last_toggle = now;
            cb_lead->setChecked(false);
            cb_lead->setChecked(true); // 再拉一次（吞 GET/POST 连接池乱序）
          }
          return false;
        },
        8000));
    {
      QSettings s(QStringLiteral("memex"), QStringLiteral("collab"));
      s.remove(QStringLiteral("task_lead_view")); // 收尾回缺省开
    }
  }

  // —— R27-1 余量 日历视图：月历标到期日＋点日筛选该日到期 ——
  {
    // 甲落在 today+2：前置腿的「到期任务」due=now-1000 也是今天，避开
    const QDate today = QDate::currentDate();
    const QDate day_a = today.addDays(2);
    const QDate other = today.addDays(3);
    const int before = dlg.task_count();
    CHECK(dlg.add_task(QStringLiteral("日历甲"), QString(),
                       QDateTime(day_a, QTime(10, 0)).toMSecsSinceEpoch(),
                       QString()));
    CHECK(dlg.add_task(QStringLiteral("日历乙"), QString(),
                       QDateTime(other, QTime(12, 0)).toMSecsSinceEpoch(),
                       QString()));
    CHECK(wait_until([&] { return dlg.task_count() == before + 2; }, 8000));
    // 默认收起，「日历」按钮展开
    auto* cal = dlg.calendar();
    CHECK(cal != nullptr && !cal->isVisible());
    CHECK(dlg.btn_calendar() != nullptr);
    dlg.btn_calendar()->click();
    CHECK(cal->isVisible());
    // 到期日标品牌橙加粗；无任务日（today+1）无自定义标
    const QColor brand(QStringLiteral("#e16531"));
    const QTextCharFormat marked = cal->dateTextFormat(day_a);
    CHECK(marked.foreground().color() == brand);
    const QTextCharFormat unmarked =
        cal->dateTextFormat(today.addDays(1));
    CHECK(unmarked.foreground().color() != brand);
    // 点日筛该日到期：无到期数据行（自建未设提醒/🌐 外部行）一并隐藏
    dlg.toggle_day_filter(day_a);
    CHECK(dlg.day_filter() == day_a);
    CHECK(dlg.visible_task_count() == 1);
    bool saw_other = false;
    for (int i = 0; i < dlg.list()->count(); ++i) {
      auto* it = dlg.list()->item(i);
      if (!it->isHidden()) {
        CHECK(it->text().contains(QStringLiteral("日历甲")));
      } else if (it->text().contains(QStringLiteral("日历乙"))) {
        saw_other = true;
      }
    }
    CHECK(saw_other);
    // 同日再点＝清筛选回全量
    dlg.toggle_day_filter(day_a);
    CHECK(!dlg.day_filter().isValid());
    CHECK(dlg.visible_task_count() == before + 2);
    // 筛选态跨刷新保持（30s 轮询重建列表后重施）
    dlg.toggle_day_filter(other);
    CHECK(dlg.visible_task_count() == 1);
    dlg.refresh();
    CHECK(wait_until(
        [&] {
          return dlg.status_text().contains(QStringLiteral("清单已刷新"));
        },
        8000));
    CHECK(dlg.visible_task_count() == 1);
    bool saw_yi = false;
    for (int i = 0; i < dlg.list()->count(); ++i) {
      auto* it = dlg.list()->item(i);
      if (!it->isHidden()) {
        CHECK(it->text().contains(QStringLiteral("日历乙")));
      }
      if (it->text().contains(QStringLiteral("日历乙"))) saw_yi = true;
    }
    CHECK(saw_yi);
    dlg.toggle_day_filter(QDate()); // 清筛选
    CHECK(!dlg.day_filter().isValid());
    // 收尾前：文档站实况抓图（test_live_shots grab_pair 同式——亮暗各一、
    // 抓完显式回亮；清单真实数据面＋日历展开态，此点日历仍展开）
    {
      const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
      CHECK(QDir().mkpath(dir));
      auto& tm = memex::client::ThemeManager::instance();
      tm.apply(qApp);
      QThread::msleep(120);
      QApplication::processEvents();
      CHECK(dlg.grab().save(
          dir + QStringLiteral("/live-task-dialog-light.png")));
      tm.set_mode(QStringLiteral("dark"));
      QThread::msleep(120);
      QApplication::processEvents();
      CHECK(dlg.grab().save(
          dir + QStringLiteral("/live-task-dialog-dark.png")));
      tm.set_mode(QStringLiteral("light"));
      QThread::msleep(120);
      QApplication::processEvents();
    }
    // 收尾：撤回两条日历任务（主人程序化入口），列表还原
    int remaining = dlg.task_count();
    for (const QString& t :
         {QStringLiteral("日历甲"), QStringLiteral("日历乙")}) {
      for (int i = 0; i < dlg.list()->count(); ++i) {
        if (dlg.list()->item(i)->text().contains(t)) {
          dlg.list()->setCurrentRow(i);
          break;
        }
      }
      CHECK(dlg.delete_selected());
      --remaining;
      CHECK(wait_until([&] { return dlg.task_count() == remaining; }, 8000));
    }
    CHECK(dlg.task_count() == before);
  }

  server.kill();
  server.waitForFinished(3000);

  if (g_failures == 0) {
    std::cout << "test_task_dialog: all checks passed\n";
    return 0;
  }
  std::cout << "test_task_dialog: " << g_failures << " check(s) FAILED\n";
  return 1;
}
