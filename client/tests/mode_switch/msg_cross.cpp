// test_mode_switch 拆分模块（LAY-014）：消息状态多端·系统集成·跨态互通·截图——腿体自原文件逐字搬移；CHECK/工具/共享态见 common.hpp。
#include "common.hpp"

void leg_msg_cross(MainWindow& window, CollabEngine& peer, DirectEngine& da, DirectEngine& db, const std::function<QString(const QStringList&)>& run_cli) {
  peer.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"),
             QStringLiteral("pass-b"));
  CHECK(wait_until([&] { return peer_in; }, 8000));
  // bob 上线 → 窗口在线表含双方
  CHECK(wait_until([&] {
    return window.online_accounts().contains(QStringLiteral("bob"));
  }, 8000));
  window.open_collab_peer(QStringLiteral("bob"));
  // 先吃掉离线补投（前文发 bob 的消息，bob 离线时入队）：清空后再走本次验收
  CHECK(wait_until([&] { return !peer_msg.isEmpty(); }, 8000));
  peer_msg.clear();
  CHECK(window.send_in_current_chat(QStringLiteral("T43主窗发送状态验收")));
  CHECK(window.delivery_text().contains(QStringLiteral("发送中")));
  CHECK(wait_until([&] {
    return window.delivery_text().contains(QStringLiteral("已送达"));
  }, 8000));
  CHECK(wait_until([&] { return !peer_msg.isEmpty(); }, 8000));
  peer.mark_read(peer_msg);
  CHECK(wait_until([&] {
    return window.delivery_text().contains(QStringLiteral("已读"));
  }, 8000));

  // 单点在线提示：peer 转登 alice → 本窗被踢 → 互踢文案常驻可查、
  // 回落直连提示条；重登后恢复（peer 随块结束析构）。
  {
    peer.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
               QStringLiteral("pass-a"));
    CHECK(wait_until([&] { return !window.collab_logged_in(); }, 8000));
    CHECK(window.kick_text().contains(QStringLiteral("顶替下线")));
    CHECK(wait_until([&] {
      return window.banner_text().contains(QStringLiteral("消息不进归档"));
    }, 5000));
    window.login_collab(QStringLiteral("127.0.0.1"), port,
                        QStringLiteral("alice"), QStringLiteral("pass-a"));
    CHECK(wait_until([&] { return window.collab_logged_in(); }, 8000));
    CHECK(window.kick_text().isEmpty()); // 新会话清除旧互踢提示
  }

  // —— T4.7 系统集成（主窗验收）——
  // 开机启动登记往返（MEMEX_TEST_AUTOSTART_DIR 覆盖到临时目录，不污染家目录）
  {
    QTemporaryDir autostart_dir;
    CHECK(autostart_dir.isValid());
    qputenv("MEMEX_TEST_AUTOSTART_DIR", autostart_dir.path().toUtf8());
    CHECK(!window.autostart_enabled());
    window.set_autostart(true);
    CHECK(window.autostart_enabled());
    CHECK(QFile::exists(autostart_dir.filePath(
        QStringLiteral("memex-client.desktop"))));
    window.set_autostart(false);
    CHECK(!window.autostart_enabled());
    qunsetenv("MEMEX_TEST_AUTOSTART_DIR");
  }
  // 系统通知：窗口隐藏（未激活）时收协作消息 → 通知面记录发送方；
  // 互踢同样走通知（T4.3 kick_text_ 不变，通知面叠加不断言面）。
  // offscreen 无托盘：tray_available() 为假但不断言（有屏环境人工核托盘气泡）。
  {
    CollabEngine peer2;
    bool peer2_in = false;
    QObject::connect(&peer2, &CollabEngine::logged_in, &peer2,
                     [&](const QString&, const QString&) { peer2_in = true; });
    peer2.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("bob"),
                QStringLiteral("pass-b"));
    CHECK(wait_until([&] { return peer2_in; }, 8000));
    window.hide(); // 隐藏即未激活 → 通知路径必触发
    CHECK(peer2.send_text(QStringLiteral("alice"),
                          QStringLiteral("T47通知验收")) != 0);
    CHECK(wait_until([&] {
      return window.last_notify().contains(QStringLiteral("bob"));
    }, 8000));
    window.show();
    // 互踢通知：peer2 转登 alice → 本窗被踢 → 通知面含顶替下线
    peer2.login(QStringLiteral("127.0.0.1"), port, QStringLiteral("alice"),
                QStringLiteral("pass-a"));
    CHECK(wait_until([&] { return !window.collab_logged_in(); }, 8000));
    CHECK(window.last_notify().contains(QStringLiteral("顶替下线")));
    // 重登恢复（后文 T4.2 块要求登录态；重登把 peer2 顶掉，其析构无影响）
    window.login_collab(QStringLiteral("127.0.0.1"), port,
                        QStringLiteral("alice"), QStringLiteral("pass-a"));
    CHECK(wait_until([&] { return window.collab_logged_in(); }, 8000));
  }

  // —— T4.2 跨态互通：已登录端 × 未登录端的直连会话 ——
  // A7：跨态会话固定「未归档」标识（横幅＋正文，常驻不可关闭）；
  // 首触上报会话建立（时间/双方/时长，无内容），登出前服务端可查。
  CHECK(wait_until([&] { return window.has_direct_peer(QStringLiteral("dev-B2")); },
                   10000));
  window.open_direct_peer(QStringLiteral("dev-B2"));
  CHECK(window.banner_text().contains(QStringLiteral("跨态")));
  CHECK(window.banner_text().contains(QStringLiteral("未归档")));
  CHECK(window.chat_html().contains(QStringLiteral("未归档"))); // A7 正文标识
  CHECK(window.chat_html().contains(QStringLiteral("归档自")));  // A8 客户端面
  CHECK(window.send_in_current_chat(QStringLiteral("跨态验收消息")));
  CHECK(wait_until([&] {
    const QString out =
        run_cli({QStringLiteral("cross"), QStringLiteral("50"), QStringLiteral("--db"),
                 srv_db});
    return out.contains(QStringLiteral("alice")) &&
           out.contains(QStringLiteral("dev-B2")) &&
           out.contains(QStringLiteral("进行中"));
  }, 8000));

  // —— 需求批④ 截图发送＝图片消息（气泡内直接渲染图片，非系统行）——
  // 授权链修复（用户令 2026-10-09）：发送腿须落在「服务端可达＋已登录」
  // 区——此前排在停服务端降级区，文件授权 fail-closed 必拒、气泡乐观上屏
  // 掩盖未达（靠收方来图侥幸过断言）。可达态：截图/文件夹真送达。
  {
    window.open_direct_peer(QStringLiteral("dev-B2"));
    // 可达态 allow 全链：对端须有真实协作账号（授权四问第二问「收方须为
    // 真实账号」）——db 临时宣告 bob 账号（立即补一轮宣告），块尾还原。
    // 授权裁决四问：alice→bob 同部门（双方均未建档＝空路径相等）→允许。
    db.set_collab_account("bob");
    CHECK(wait_until([&] {
      return !window.banner_text().contains(QStringLiteral("跨态"));
    }, 5000)); // 对端宣告落地（跨态判定解除）＝peer 表已带出账号
    // 本端发送流：真 PNG 走 send_shot_to_current_chat（截图确认回调同一路径）
    const QString shot = tmp.filePath(QStringLiteral("shot-msg.png"));
    {
      QPixmap pm(96, 60);
      pm.fill(Qt::red);
      CHECK(pm.save(shot, "PNG"));
    }
    // 可达态授权回执：截图真送达对端（db 侧文件落地，非仅本端气泡）
    QString shot_recv;
    QObject::connect(&db, &DirectEngine::file_received, &db,
                     [&](const QString&, const QString&, const QString& p) {
                       if (p.endsWith(QStringLiteral("shot-msg.png")))
                         shot_recv = p;
                     });
    CHECK(window.send_shot_to_current_chat(shot));
    CHECK(wait_until([&] {
      return window.chat_html().contains(QStringLiteral("<img"));
    }, 8000));
    CHECK(wait_until([&] { return !shot_recv.isEmpty(); }, 15000));
    CHECK(window.last_file_error().isEmpty()); // 无失败终态
    CHECK(window.chat_html().contains(QStringLiteral("图片消息")));
    CHECK(!window.chat_html().contains(QStringLiteral("[截图] 开始发送")));
    // 收方渲染腿：db 发真 PNG 给窗口（窗口正开着 dev-B2 会话）→
    // file_received 图片且会话匹配＝气泡追加（非文件系统行）。
    // 文件名带随机段：同进程窗口与 db 共用 AppData 下载目录，基线腿
    // 落过同名文件会撞幂等重发直达终态（收不到 FILE_RESUME 数据面）
    const QString recv_png = tmp.filePath(QStringLiteral("recv-msg-%1.png")
                                              .arg(QRandomGenerator::global()
                                                       ->generate()));
    {
      QPixmap pm(80, 50);
      pm.fill(Qt::blue);
      CHECK(pm.save(recv_png, "PNG"));
    }
    const int imgs_before = window.chat_html().count(
        QStringLiteral("<img"));
    const QString win_id =
        QSettings().value(QStringLiteral("direct/device_id")).toString();
    // 对照基线：裸引擎对文件通道（da→db，test_screenshot 已验窗口发方向）
    QString db_recv;
    QObject::connect(&db, &DirectEngine::file_received, &db,
                     [&](const QString&, const QString&, const QString& path) {
                       db_recv = path;
                     });
    CHECK(!da.send_file("dev-B2", recv_png).empty());
    CHECK(wait_until([&] { return !db_recv.isEmpty(); }, 15000));
    // 探针：窗口 file_received 是否到达（验收面分离传输层 vs 渲染层）
    CHECK(!db.send_file(win_id.toStdString(), recv_png).empty());
    CHECK(wait_until([&] {
      return window.last_received_file().startsWith(
          QStringLiteral("dev-B2|"));
    }, 15000));
    CHECK(wait_until([&] {
      return window.chat_html().count(QStringLiteral("<img")) ==
             imgs_before + 1;
    }, 5000));
    // 验收截图：图片消息气泡（本端发送+收方接收两条）
    {
      const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
      CHECK(QDir().mkpath(dir));
      CHECK(window.grab().save(dir + QStringLiteral("/image-message.png")));
    }

    // —— 需求批⑤ 发文件夹（主窗验收缝）：会话开着 dev-B2 → 真发整目录，
    //    等系统行（持久面）明示完成；落盘结构一并断言 ——
    CHECK(window.has_direct_peer(QStringLiteral("dev-B2")));
    const QString win_src = tmp.filePath(QStringLiteral("win-dir"));
    CHECK(QDir().mkpath(win_src + QStringLiteral("/nested")));
    {
      QFile f(win_src + QStringLiteral("/t.txt"));
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("T");
    }
    const QString win_job = window.send_folder_to_current_chat(win_src);
    CHECK(!win_job.isEmpty());
    CHECK(wait_until([&] {
      return window.chat_html().contains(
          QStringLiteral("[文件夹] win-dir 发送完成"));
    }, 10000));
    CHECK(QFile::exists(
        tmp.filePath(QStringLiteral("b-files/t.txt")))); // rel 相对作业根
    db.set_collab_account(""); // 还原匿名宣告（后文降级态跨态判定不受扰）
  }
}
