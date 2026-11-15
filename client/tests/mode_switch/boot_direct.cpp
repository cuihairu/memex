// test_mode_switch 拆分模块（LAY-014）：空态引导＋引擎级发文件夹＋布局腿与查找——腿体自原文件逐字搬移；CHECK/工具/共享态见 common.hpp。
#include "common.hpp"

void leg_empty() {
  // —— 空态引导（原型 main-empty.html）：登录后零行侧栏摆「还没有会话」
  //    两行 hint、主区欢迎页文案就位（潜伏态：布局令列表态窗口坍缩
  //    320px、面板隐藏，文案先行）；有会话行即撤、登出即撤。须在直连
  //    双实例起场前验收——dev-A2/B2 一旦宣告列表非零行，hint 依口径不摆
  //    （有会话不冒充空态）。独立 app 名隔离 QSettings/本地库，不染主窗腿
  {
    QCoreApplication::setApplicationName(QStringLiteral("w0-empty-test"));
    MainWindow w0;
    w0.show();
    w0.login_collab(QStringLiteral("127.0.0.1"), port,
                    QStringLiteral("alice"), QStringLiteral("pass-a"));
    CHECK(wait_until([&] { return w0.collab_logged_in(); }, 8000));
    CHECK(w0.findChild<QListWidget*>(QStringLiteral("device_list")) != nullptr);
    // 两行高度守卫：QSS 行 padding 会把行内控件矩形压扁（修复前 15px
    // 只剩半行）。字体度量随平台/字体栈浮动（绝对值与 fm 口径都曾被
    // 打穿），故在布局沉降后只守「不低于自身 sizeHint−2」的相对下限；
    // 真压扁时高度恒远低于 sizeHint，仍会被抓住
    if (auto* hint = w0.findChild<QLabel*>(QStringLiteral("empty_list_hint"))) {
      wait_until([&] { return hint->height() >= hint->sizeHint().height() - 2; }, 3000);
      if (hint->height() < hint->sizeHint().height() - 2)
        qWarning("hint squashed: h=%d sizeHintH=%d fmH=%d", hint->height(),
                 hint->sizeHint().height(), hint->fontMetrics().height());
      CHECK(hint->height() >= hint->sizeHint().height() - 2);
    }
    CHECK(w0.chat_html().contains(QStringLiteral("欢迎使用 Memex")));
    CHECK(w0.chat_html().contains(
              QStringLiteral("先从通讯录找到同事"))); // 副题同落主区
    CHECK(!w0.chat_html().contains(
              QStringLiteral("还没有会话"))); // hint 在侧栏，不串主区
    // 空态实况进 docs 截图目录（浅＋深；随拍随回亮色基线——还原显式回
    // light：system 档判暗兜底会回读已染暗的调色板再落回深色）
    {
      const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
      CHECK(QDir().mkpath(dir));
      CHECK(w0.grab().save(dir + QStringLiteral("/layout-empty-light.png")));
      auto& tm = memex::client::ThemeManager::instance();
      tm.apply(qApp);
      tm.set_mode(QStringLiteral("dark"));
      CHECK(w0.grab().save(dir + QStringLiteral("/layout-empty-dark.png")));
      tm.set_mode(QStringLiteral("light"));
    }
    // 有会话行即撤：开一个协作会话 → hint 退场（collab_peers_ 内存驻留，
    // 关回列表态会话行仍在，持续不摆——有会话不冒充空态）。列表重建走
    // deleteLater，须排干 DeferredDelete 再查树（否则死指针假在场）
    w0.open_collab_peer(QStringLiteral("bob"));
    CHECK(wait_until([&] {
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      return w0.findChild<QLabel*>(QStringLiteral("empty_list_hint")) ==
             nullptr;
    }, 3000));
    w0.logout_collab();
    CHECK(wait_until([&] { return !w0.collab_logged_in(); }, 5000));
    // 登出即撤：hint 只属协作态零行（登出不清 current_peer_，聊天面留着
    // 已开会话，但列表已重建为直连口径——零行也不再摆协作空态 hint）
    CHECK(wait_until([&] {
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      return w0.findChild<QLabel*>(QStringLiteral("empty_list_hint")) ==
             nullptr;
    }, 3000));
    QCoreApplication::setApplicationName(QStringLiteral("mode-switch-test"));
  }
}

void leg_folder(DirectEngine& da) {
  // —— 需求批⑤ 发文件夹（引擎级）：递归遍历＋相对路径重建＋聚合进度 ——
  {
    const QString src_root = tmp.filePath(QStringLiteral("dir-src"));
    CHECK(QDir().mkpath(src_root + QStringLiteral("/sub")));
    {
      QFile f(src_root + QStringLiteral("/a.txt"));
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("A");
    }
    {
      QFile f(src_root + QStringLiteral("/sub/b.txt"));
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("B");
    }
    bool dir_done = false, dir_ok = false;
    quint64 prog_done = 0, prog_total = 0;
    QObject::connect(&da, &DirectEngine::directory_finished, &da,
                     [&](const QString&, bool ok) {
                       dir_done = true;
                       dir_ok = ok;
                     });
    QObject::connect(&da, &DirectEngine::directory_progress, &da,
                     [&](const QString&, quint64 d, quint64 t) {
                       prog_done = qMax(prog_done, d);
                       prog_total = qMax(prog_total, t);
                     });
    const QString job = da.send_directory("dev-B2", src_root);
    CHECK(!job.isEmpty());
    CHECK(wait_until([&] { return dir_done; }, 10000));
    CHECK(dir_ok);
    CHECK(prog_total == 2); // 聚合面：作业总文件数＝2
    CHECK(prog_done + 1 == prog_total); // 末次回报＝第 2/2 个文件起传
    const QString dst_root = tmp.filePath(QStringLiteral("b-files"));
    CHECK(QFile::exists(dst_root + QStringLiteral("/a.txt")));
    CHECK(QFile::exists(dst_root + QStringLiteral("/sub/b.txt"))); // 结构重建
    {
      QFile f(dst_root + QStringLiteral("/sub/b.txt"));
      CHECK(f.open(QIODevice::ReadOnly));
      CHECK(f.readAll() == QByteArray("B")); // 内容一致
    }
    // 空目录不起作业（目录须存在但无文件＝空集拒绝）
    const QString empty_dir = tmp.filePath(QStringLiteral("dir-empty"));
    CHECK(QDir().mkpath(empty_dir));
    CHECK(da.send_directory("dev-B2", empty_dir).isEmpty());
  }
}

void leg_layout_find(MainWindow& window) {
  // —— 布局腿（布局令）：启动＝列表态（「左菜单+好友列表」长方形面板，
  // 不摆聊天面板）；点好友→对话面板展开；再点当前好友→关回列表态；
  // 重开＝会话照常展开（历史在库里重载）
  CHECK(!window.chat_panel_visible());
  window.open_direct_peer(QStringLiteral("dev-B2"));
  CHECK(window.chat_panel_visible());
  {
    auto* list =
        window.findChild<QListWidget*>(QStringLiteral("device_list"));
    CHECK(list);
    // 列表里 dev-B2 项可能还没到（发现广播在途），到不了就只验开/关口径
    QListWidgetItem* target = nullptr;
    wait_until([&] {
      for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(Qt::UserRole).toString() ==
            QStringLiteral("dev-B2")) {
          target = list->item(i);
          return true;
        }
      }
      return false;
    }, 8000);
    if (target) {
      emit list->itemClicked(target); // 再点当前会话项＝关回列表态
      CHECK(!window.chat_panel_visible());
      // 验收截图①：启动无对话＝「左菜单+好友列表」长方形面板（无聊天面板）
      {
        const QString dir = QStringLiteral(MEMEX_DOCS_SHOT_DIR);
        CHECK(QDir().mkpath(dir));
        CHECK(window.grab().save(dir + QStringLiteral("/layout-list.png")));
      }
      window.open_direct_peer(QStringLiteral("dev-B2"));
      CHECK(window.chat_panel_visible());
      // 验收截图②：点好友后＝对话面板展开
      CHECK(window.grab().save(QStringLiteral(MEMEX_DOCS_SHOT_DIR) +
                               QStringLiteral("/layout-chat.png")));
      // 验收截图②b：会话展开态·深色（实况补摄）——切暗抓一张后还原原模式。
      // apply(qApp) 先行：set_mode 只在 applied_ 后才 push_to_app（测试环境
      // 无人调 apply，缺这步令牌换了界面不刷，抓图仍是浅色）
      {
        auto& tm = memex::client::ThemeManager::instance();
        tm.apply(qApp);
        tm.set_mode(QStringLiteral("dark"));
        CHECK(window.grab().save(QStringLiteral(MEMEX_DOCS_SHOT_DIR) +
                                 QStringLiteral("/layout-chat-dark.png")));
        // 还原须显式回 light：system 档的判暗兜底读的是「当前」调色板，
        // 深色推过后已染暗，set_mode(system) 会再解析回深色（2026-10-10
        // 浅深配平轮实测：好友列表浅图因此带暗入库）。测试基线＝浅色，
        // 显式回亮让后续腿与后续抓图回到基线
        tm.set_mode(QStringLiteral("light"));
      }

      // —— 需求批⑨ 查找：昵称/账号(设备 id)/IP 多字段，模糊+精确 ——
      // 模糊＝子串（dev- 前缀全命中）；精确＝整 id（dev-A2 只剩一台）；
      // 不存在串全隐；清空恢复（列表里 dev-A2/dev-B2 来自双引擎宣告）
      // （占位文案 2026-10-30 对齐原型 desktop-direct.html 改「搜索设备 / IP」，
      //  检索字段不变）
      QLineEdit* search = nullptr;
      for (QLineEdit* e : window.findChildren<QLineEdit*>()) {
        if (e->placeholderText().contains(QStringLiteral("搜索设备"))) {
          search = e;
          break;
        }
      }
      CHECK(search != nullptr);
      if (search) {
        const auto visible_count = [&]() {
          int n = 0;
          for (int i = 0; i < list->count(); ++i) {
            if (!list->item(i)->isHidden() &&
                !list->item(i)->data(Qt::UserRole).toString().isEmpty()) ++n;
          }
          return n;
        };
        // 两台都到齐再数：宣告 3s 周期漂入列表，上面 target 等待只保证
        // dev-B2 到场——无等待即计数＝竞态（负载下 dev-A2 项未到即闪失）
        CHECK(wait_until([&] { return visible_count() >= 2; }, 8000));
        const int total = visible_count();
        CHECK(total >= 2); // dev-A2 + dev-B2
        search->setText(QStringLiteral("dev-"));
        CHECK(visible_count() == total); // 模糊：前缀两台都在
        search->setText(QStringLiteral("dev-A2"));
        CHECK(visible_count() == 1); // 精确：整 id 只剩一台
        search->setText(QStringLiteral("zzz-不存在"));
        CHECK(visible_count() == 0); // 无命中全隐
        search->setText(QString());
        CHECK(visible_count() == total); // 清空恢复
      }
    }
  }
}
