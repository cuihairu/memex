// test_mode_switch 拆分模块（LAY-014）：振屏·停服回落·授权链·锁屏与设置页——腿体自原文件逐字搬移；CHECK/工具/共享态见 common.hpp。
#include "common.hpp"

void leg_nudge_fallback(MainWindow& window, CollabEngine& peer, DirectEngine& da, DirectEngine& db, QProcess& server, const std::function<QString(const QStringList&)>& run_cli) {
  // —— 需求批⑥ 振屏（直连域）：主窗缝发送＋对端引擎收＋防刷限频＋抖窗 ——
  {
    const QString win_id =
        QSettings().value(QStringLiteral("direct/device_id")).toString();
    int db_nudge = 0;
    QString db_nudge_from;
    QObject::connect(&db, &DirectEngine::nudge_received, &db,
                     [&](const QString& from, quint64, qint64) {
                       ++db_nudge;
                       db_nudge_from = from;
                     });
    CHECK(window.send_nudge_to_current_chat());
    CHECK(wait_until([&] { return db_nudge == 1; }, 6000));
    CHECK(db_nudge_from == win_id);
    // 对端本地标记行（历史重载渲染源；seq＝对端发送序号）
    const auto b_hist = db.history(win_id, 5);
    CHECK(!b_hist.isEmpty() &&
          b_hist.crbegin()->text == QStringLiteral("[振屏]"));
    // 本端系统行（受控标记渲染，非气泡）
    CHECK(window.chat_html().contains(QStringLiteral("你发送了窗口抖动")));
    // 防刷限频：同一会话 10s 内第二次拒（同轮状态断言无竞态）
    CHECK(!window.send_nudge_to_current_chat());
    CHECK(window.status_text().contains(QStringLiteral("太频繁")));
    // 反向：db 振窗口（裸引擎发送面）→ 窗口抖窗＋系统行（当前会话匹配）
    CHECK(db.send_nudge(win_id.toStdString()) != 0);
    CHECK(wait_until([&] { return window.shake_count() >= 1; }, 6000));
    CHECK(window.chat_html().contains(QStringLiteral("给你发来窗口抖动")));
  }

  // 同库注入一条直连历史（peer 同为 bob）：合并展示按来源标注
  {
    const QString win_db = tmp.filePath(
        QStringLiteral("xdg/memex-test/mode-switch-test/memex-local.db"));
    LocalStore inject;
    CHECK(inject.open(win_db));
    StoredMessage direct_row;
    direct_row.peer = "bob";
    direct_row.from = "bob";
    direct_row.to = "local";
    direct_row.seq = 987654;
    direct_row.ts_ms = QDateTime::currentMSecsSinceEpoch() - 60000;
    direct_row.text = "直连旧消息（仅本机）";
    direct_row.source = "direct";
    CHECK(inject.append(direct_row));
    inject.close();
  }

  window.open_collab_peer(QStringLiteral("bob"));
  CHECK(wait_until([&] {
    return window.chat_html().contains(QStringLiteral("模式切换验收消息"));
  }, 5000));
  CHECK(window.chat_html().contains(QStringLiteral("直连·仅本机")));
  CHECK(window.chat_html().contains(QStringLiteral("协作·已归档")));
  CHECK(window.chat_html().contains(QStringLiteral("直连旧消息（仅本机）")));

  // —— 需求批⑥ 振屏（协作域）：NUDGE 经服务端（归档＋受理回执＋限频
  //    语义见服务端 test_nudge）；本端系统行＋同会话 10s 限频 ——
  CHECK(window.send_nudge_to_current_chat());
  CHECK(window.chat_html().contains(QStringLiteral("你发送了窗口抖动")));
  CHECK(!window.send_nudge_to_current_chat());
  CHECK(window.status_text().contains(QStringLiteral("太频繁")));

  // —— 停服务端 → 登录回落直连态，明确提示「消息不进归档」 ——
  window.logout_collab();
  CHECK(wait_until([&] { return !window.collab_logged_in(); }, 5000));
  // T4.2：登出即闭环跨态会话（end 帧先于 LOGOUT）——服务端日志含
  // 时间/双方/时长；归档起点显示为实际登录时间（A8 服务端面）。
  CHECK(wait_until([&] {
    const QString out =
        run_cli({QStringLiteral("cross"), QStringLiteral("50"), QStringLiteral("--db"),
                 srv_db});
    return out.contains(QStringLiteral("dev-B2")) &&
           out.contains(QStringLiteral("已结束"));
  }, 8000));
  CHECK(wait_until([&] {
    const QString out =
        run_cli({QStringLiteral("messages"), QStringLiteral("alice"),
                 QStringLiteral("--db"), srv_db});
    return out.contains(QStringLiteral("归档自"));
  }, 8000));
  server.kill();
  CHECK(server.waitForFinished(5000));

  window.login_collab(QStringLiteral("127.0.0.1"), port,
                      QStringLiteral("alice"), QStringLiteral("pass-a"));
  CHECK(wait_until([&] {
    return window.status_text().contains(QStringLiteral("服务端不可达"));
  }, 8000));
  CHECK(wait_until([&] {
    return window.status_text().contains(QStringLiteral("消息不进归档"));
  }, 3000));
  CHECK(!window.collab_logged_in());
  CHECK(window.banner_text().contains(QStringLiteral("消息不进归档")));
  // 平台-7 降级显式化：状态栏（已有断言）之外——横幅明示「已降级」、
  // 输入框提示随态（未归档通信）、直连会话标记「已降级 · 未归档」前缀
  CHECK(window.banner_text().contains(QStringLiteral("已降级")));
  CHECK(window.input_hint().contains(QStringLiteral("未归档")));
  CHECK(window.input_hint().contains(QStringLiteral("已降级")));
  window.open_direct_peer(QStringLiteral("dev-B2"));
  CHECK(window.chat_meta().contains(QStringLiteral("已降级 · 未归档")));

  // —— 服务端已死：直连态仍可收发（A11 直连半边）——
  db_received = 0;
  da_delivered = false;
  CHECK(da.send_text("dev-B2", "服务端已停，直连仍可用") != 0);
  CHECK(wait_until([&] { return db_received == 1 && da_delivered; }, 6000));

  // —— 授权链回归（用户令 2026-10-09）：服务端不可达态＝未登录，
  //    fail-closed 本地拒（权限面不可绕开服务器，设计口径）——
  //    截图/文件夹缝同步返回失败、不乐观上屏，回执 deny:server-unreachable
  {
    window.open_direct_peer(QStringLiteral("dev-B2"));
    const QString shot_deny = tmp.filePath(QStringLiteral("shot-deny.png"));
    {
      QPixmap pm(60, 40);
      pm.fill(Qt::green);
      CHECK(pm.save(shot_deny, "PNG"));
    }
    const int imgs_before_deny =
        window.chat_html().count(QStringLiteral("<img"));
    CHECK(!window.send_shot_to_current_chat(shot_deny)); // 拒＝false
    CHECK(window.chat_html().count(QStringLiteral("<img")) ==
          imgs_before_deny); // 无假「已发送」气泡
    CHECK(window.last_file_error().contains(
        QStringLiteral("deny:server-unreachable")));
    const QString deny_dir = tmp.filePath(QStringLiteral("deny-dir"));
    CHECK(QDir().mkpath(deny_dir));
    {
      QFile f(deny_dir + QStringLiteral("/d.txt"));
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("D");
    }
    CHECK(window.send_folder_to_current_chat(deny_dir).isEmpty()); // 拒＝空
    CHECK(window.last_file_error().contains(
        QStringLiteral("deny:server-unreachable")));
  }
}

void leg_settings(MainWindow& window, DirectEngine& da, DirectEngine& db) {
  // —— 离开锁屏（用户令 2026-10-08 ④）：无操作超时→锁屏只显未读数不显
  //     内容→错密码拒→对密码解锁（db 仍活着：锁屏期间真来一条直连消息）——
  {
    CHECK(away_lock::set_password(QStringLiteral("lock-pwd")));
    away_lock::set_timeout_seconds(1); // 1 秒超时（测试内无输入事件＝必到期）
    away_lock::set_enabled(true);
    window.apply_away_lock_settings(); // 设置对话框关闭即此路径＝即时生效
    window.hide(); // 无输入事件，超时即锁
    CHECK(wait_until([&] { return window.lock_screen_visible(); }, 8000));
    CHECK(window.lock_screen_text().contains(QStringLiteral("暂无新消息")));
    // 锁屏期间真来一条直连消息：只累计条数；弹窗与提示音全静默
    // （消息文本不出锁屏面——last_notify 不动＝通知路径被抑制）
    const QString prev_notify = window.last_notify();
    const QString win_id =
        QSettings().value(QStringLiteral("direct/device_id")).toString();
    CHECK(!win_id.isEmpty());
    CHECK(db.send_text(win_id.toStdString(),
                       std::string("锁屏期间不该上屏的内容")) != 0);
    CHECK(wait_until([&] { return window.lock_unread_count() == 1; }, 6000));
    CHECK(window.last_notify() == prev_notify);
    CHECK(window.lock_screen_text().contains(
        QStringLiteral("锁屏期间新消息 1 条")));
    CHECK(!window.lock_screen_text().contains(QStringLiteral("不该上屏")));
    // 截图验收：锁屏遮罩面只含数量（docs/src/public/screenshots/lock-screen.png）
    {
      QWidget* lock_w = QApplication::activeWindow(); // 全屏遮罩＝当前激活窗
      CHECK(lock_w && lock_w->isVisible());
      const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
      CHECK(QDir().mkpath(dir));
      if (lock_w)
        CHECK(lock_w->grab().save(dir + QStringLiteral("/lock-screen.png")));
    }
    // 错密码拒（仍在屏）；对密码解锁
    window.lock_try_unlock(QStringLiteral("wrong-pwd"));
    CHECK(window.lock_screen_visible());
    window.lock_try_unlock(QStringLiteral("lock-pwd"));
    CHECK(wait_until([&] { return !window.lock_screen_visible(); }, 3000));
    CHECK(window.lock_unread_count() == 0); // 解锁即清（随遮罩销毁）
    // 还原：关开关（QSettings 面 XDG_CONFIG_HOME 已隔离；也保重复执行可重跑）
    away_lock::set_enabled(false);
    window.apply_away_lock_settings();
    window.show();
  }

  // —— 网络与远程设置页（用户令 2026-10-08 ⑤）：坏段本地门、开关与
  //     配对密码落盘、保存即时生效（apply_net_settings 同源调用）——
  {
    NetRemoteSettingsDialog dlg(&window);
    // 坏段本地拒不入表（段输入过 validate_cidr 门）
    CHECK(!dlg.add_entry(QStringLiteral("10.0.0/8")).isEmpty());
    CHECK(dlg.add_entry(QStringLiteral("172.16.0.0/12")).isEmpty());
    CHECK(dlg.add_entry(QStringLiteral("172.16.0.0/12")).isEmpty() ==
          false); // 重复不双录
    // 远程控制开且未设密码＝保存拒（先设配对密码门）
    auto* chk_net = dlg.findChild<QCheckBox*>("chk_blacklist");
    auto* chk_remote = dlg.findChild<QCheckBox*>("chk_remote");
    auto* edit_pwd = dlg.findChild<QLineEdit*>("edit_pwd");
    auto* edit_confirm = dlg.findChild<QLineEdit*>("edit_confirm");
    auto* list_entries = dlg.findChild<QListWidget*>("list_entries");
    CHECK(chk_net && chk_remote && edit_pwd && edit_confirm && list_entries);
    chk_net->setChecked(true);
    chk_remote->setChecked(true);
    CHECK(dlg.save_settings().contains(QStringLiteral("配对密码")));
    // 密码两次不一致拒
    edit_pwd->setText(QStringLiteral("pair-1"));
    edit_confirm->setText(QStringLiteral("pair-2"));
    CHECK(dlg.save_settings().contains(QStringLiteral("不一致")));
    // 合法保存：段表+两开关+密码落盘
    edit_confirm->setText(QStringLiteral("pair-1"));
    CHECK(dlg.save_settings().isEmpty());
    CHECK(net_blacklist::enabled());
    CHECK(net_blacklist::entries() ==
          QStringList{QStringLiteral("172.16.0.0/12")});
    CHECK(remote_control::enabled());
    CHECK(remote_control::verify_password(QStringLiteral("pair-1")));
    CHECK(!remote_control::verify_password(QStringLiteral("pair-2")));
    // 删段：选中入表段删除→保存=段表清空
    list_entries->setCurrentRow(0);
    dlg.del_entry();
    CHECK(dlg.save_settings().isEmpty());
    CHECK(net_blacklist::entries().isEmpty());
    // 还原（保重复执行可重跑；XDG_CONFIG_HOME 面已隔离）
    net_blacklist::set_enabled(false);
    remote_control::set_enabled(false);
    window.apply_net_settings();
  }

  da.stop();
  db.stop();

  // —— 截图快捷键改键＋冲突检测（用户令 2026-10-08 ③；设置页
  // ShortcutSettingsDialog 走同一路径 apply_screenshot_shortcut）——
  {
    // 造一个占用键的既有快捷键＝冲突检测的比对对象
    auto* other = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+K")),
                                &window);
    other->setObjectName(QStringLiteral("other_sc"));
    // 冲突：不改现状、返回冲突文案
    const QString conflict = window.apply_screenshot_shortcut(
        QKeySequence(QStringLiteral("Ctrl+Shift+K")));
    CHECK(conflict.contains(QStringLiteral("冲突")));
    CHECK(MainWindow::screenshot_shortcut() ==
          QStringLiteral("Ctrl+Alt+A")); // 落盘未动
    // 合法键：重绑＋落盘＋回读
    CHECK(window.apply_screenshot_shortcut(
              QKeySequence(QStringLiteral("Ctrl+Alt+S")))
              .isEmpty());
    CHECK(MainWindow::screenshot_shortcut() ==
          QStringLiteral("Ctrl+Alt+S"));
    // 还原默认（不污染后续执行体；QSettings 面 XDG_CONFIG_HOME 已隔离）
    CHECK(window.apply_screenshot_shortcut(
              QKeySequence(QStringLiteral("Ctrl+Alt+A")))
              .isEmpty());
    delete other;
  }
}
