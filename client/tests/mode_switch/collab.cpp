// test_mode_switch 拆分模块（LAY-014）：协作登录·组织·好友视图·协作会话流——腿体自原文件逐字搬移；CHECK/工具/共享态见 common.hpp。
#include "common.hpp"

void leg_collab_views(MainWindow& window, DirectEngine& db) {
  // —— 登录协作态（不重启）——
  window.login_collab(QStringLiteral("127.0.0.1"), port,
                      QStringLiteral("alice"), QStringLiteral("pass-a"));
  CHECK(wait_until([&] { return window.collab_logged_in(); }, 8000));

  // —— T3.1：登录后可拉取组织架构（ORG_QUERY→ORG_DATA 下发生效）——
  window.request_org();
  CHECK(wait_until([&] { return window.org_json().contains("bob"); }, 8000));

  // —— main-empty 主区欢迎页（登录态、无选中会话）：先关掉截图腿留下的
  //    dev-B2 会话（再点当前项＝关回列表态）再验：登录后空选态渲染欢迎
  //    页；侧栏因直连双实例在场（dev-A2/B2 行）非零行——空态 hint 依口
  //    径不摆。验完重开 dev-B2 还原后续截图腿的窗口几何
  {
    auto* mlist =
        window.findChild<QListWidget*>(QStringLiteral("device_list"));
    CHECK(mlist != nullptr);
    if (mlist) {
      for (int i = 0; i < mlist->count(); ++i) {
        if (mlist->item(i)->data(Qt::UserRole).toString() ==
            QStringLiteral("dev-B2")) {
          emit mlist->itemClicked(mlist->item(i)); // 当前会话项＝关回列表态
          break;
        }
      }
    }
    CHECK(!window.chat_panel_visible());
    CHECK(window.chat_html().contains(QStringLiteral("欢迎使用 Memex")));
    CHECK(window.findChild<QLabel*>(QStringLiteral("empty_list_hint")) ==
          nullptr);
    window.open_direct_peer(QStringLiteral("dev-B2")); // 还原截图腿几何
    CHECK(window.chat_panel_visible());
  }

  // —— 好友视图（原型 desktop-friends.html）：rail「好友」切侧栏 IA——
  //    通讯录行（org 成员 bob）以 collab 会话行入场＋部门分组头；切回
  //    「消息」分组头退场（会话视图回场）
  {
    auto* rail = window.findChild<memex::client::RailNav*>();
    CHECK(rail != nullptr);
    auto* flist =
        window.findChild<QListWidget*>(QStringLiteral("device_list"));
    CHECK(rail && flist);
    if (rail && flist) {
      emit rail->item_clicked(QStringLiteral("friends"));
      CHECK(wait_until([&] {
        bool bob_row = false, dept_head = false;
        for (int i = 0; i < flist->count(); ++i) {
          auto* it = flist->item(i);
          if (it->data(Qt::UserRole).toString() ==
                  QStringLiteral("bob") &&
              it->data(Qt::UserRole + 1).toString() ==
                  QStringLiteral("collab"))
            bob_row = true;
          if (it->text().contains(QStringLiteral("· 1 人")))
            dept_head = true;
        }
        return bob_row && dept_head;
      }, 8000));
      emit rail->item_clicked(QStringLiteral("messages"));
      CHECK(wait_until([&] {
        for (int i = 0; i < flist->count(); ++i)
          if (flist->item(i)->text().contains(QStringLiteral("· 1 人")))
            return false;
        return true;
      }, 8000));
      // 好友视图实况进 docs 截图目录（浅＋深；随拍随回亮色基线）
      emit rail->item_clicked(QStringLiteral("friends"));
      CHECK(wait_until([&] {
        for (int i = 0; i < flist->count(); ++i)
          if (flist->item(i)->text().contains(QStringLiteral("· 1 人")))
            return true;
        return false;
      }, 8000));
      {
        auto& tm = memex::client::ThemeManager::instance();
        CHECK(window.grab().save(QStringLiteral(MEMEX_DOCS_SHOT_DIR) +
                                 QStringLiteral("/layout-friends-light.png")));
        tm.apply(qApp);
        tm.set_mode(QStringLiteral("dark"));
        CHECK(window.grab().save(QStringLiteral(MEMEX_DOCS_SHOT_DIR) +
                                 QStringLiteral("/layout-friends-dark.png")));
        tm.set_mode(QStringLiteral("light"));
      }
      emit rail->item_clicked(QStringLiteral("messages"));
    }
  }

  // —— 组织架构整屏视图（原型 desktop-org.html）：CLI 造部门树＋汇报链
  //    （alice＝研发部部长无上级，bob＝研发一组组长汇报 alice），rail
  //    「组织架构」切三栏视图——左部门树（含计数）＋中成员卡（bob 卡＋
  //    「我」高亮卡＋负责人胶囊）＋右档案（点卡展开，汇报链路 bob→alice）；
  //    实况进 docs 截图目录（浅＋深；随拍随回亮色基线），回「消息」退场 ——
  {
    auto org_cli = [&](const QStringList& tail) {
      QStringList args = tail;
      args << QStringLiteral("--db") << srv_db;
      CHECK(QProcess::execute(server_bin, args) == 0);
    };
    org_cli({QStringLiteral("org"), QStringLiteral("set"),
             QStringLiteral("alice"), QStringLiteral("--dept"),
             QStringLiteral("华信制造集团/产品研发部"), QStringLiteral("--title"),
             QStringLiteral("部长"), QStringLiteral("--manager"),
             QStringLiteral("none")});
    org_cli({QStringLiteral("org"), QStringLiteral("set"),
             QStringLiteral("bob"), QStringLiteral("--dept"),
             QStringLiteral("华信制造集团/产品研发部/研发一组"),
             QStringLiteral("--title"), QStringLiteral("组长"),
             QStringLiteral("--manager"), QStringLiteral("alice")});
    // org 重拉（拿新部门树；enter_org_view 首次进入自会拉，这里先拉好再切）
    window.request_org();
    CHECK(wait_until([&] {
      return window.org_json().contains(QStringLiteral("研发一组"));
    }, 8000));

    auto* rail = window.findChild<memex::client::RailNav*>();
    CHECK(rail != nullptr);
    if (rail) {
      emit rail->item_clicked(QStringLiteral("org"));
      CHECK(wait_until([&] { return window.org_view_active(); }, 8000));
      auto* tree = window.findChild<QTreeWidget*>(
          QStringLiteral("org_tree"));
      CHECK(tree != nullptr);
      bool grp_item = false;
      if (tree) {
        QTreeWidgetItemIterator it(tree);
        while (*it) {
          if ((*it)->text(0).contains(QStringLiteral("研发一组")) &&
              (*it)->text(1).contains(QStringLiteral("1")))
            grp_item = true;
          ++it;
        }
      }
      CHECK(grp_item); // 部门树含「研发一组 · 1」（bob）
      // 「全部成员」头：2 人；bob 卡与「我」卡同场；负责人胶囊如实
      auto* head_title = window.findChild<QLabel*>(
          QStringLiteral("org_head_title"));
      auto* bob_card =
          window.findChild<QWidget*>(QStringLiteral("org_card_bob"));
      auto* me_card =
          window.findChild<QWidget*>(QStringLiteral("org_me_card"));
      auto* head_pill = window.findChild<QLabel*>(
          QStringLiteral("org_head_pill"));
      CHECK(head_title && head_title->text().contains(
                              QStringLiteral("全部成员 · 2 人")));
      CHECK(bob_card && !bob_card->isHidden());
      CHECK(me_card && !me_card->isHidden());
      // 档案栏未选人时收起
      CHECK(window.findChild<QWidget*>(QStringLiteral("org_profile")) ==
            nullptr || window.findChild<QWidget*>(
                                        QStringLiteral("org_profile"))
                                            ->isHidden());
      // 点 bob 卡 → 右栏档案：汇报链路 bob→alice、直属上级行
      if (bob_card) {
        const QPointF c = QPointF(bob_card->width() / 2.0,
                                  bob_card->height() / 2.0);
        QMouseEvent press(QEvent::MouseButtonPress, c, c, c, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, c, c, c,
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bob_card, &press);
        QApplication::sendEvent(bob_card, &release);
      }
      auto* profile = window.findChild<QWidget*>(
          QStringLiteral("org_profile"));
      CHECK(profile && !profile->isHidden());
      bool chain_head = false, chain_alice = false, mgr_row = false;
      if (profile) {
        for (QLabel* l : profile->findChildren<QLabel*>()) {
          if (l->text() == QStringLiteral("汇报链路")) chain_head = true;
          if (l->text().contains(QStringLiteral("alice"))) chain_alice = true;
        }
        // kv 行＝行件下键/值两个 QLabel（无单标签同时含键与值）
        for (QWidget* roww : profile->findChildren<QWidget*>()) {
          const QList<QLabel*> ls = roww->findChildren<QLabel*>();
          if (ls.size() == 2 &&
              ls.at(0)->text() == QStringLiteral("直属上级") &&
              ls.at(1)->text().contains(QStringLiteral("alice")))
            mgr_row = true;
        }
      }
      CHECK(chain_head);
      CHECK(chain_alice); // 汇报链路含上级 alice（bob→alice）
      CHECK(mgr_row);     // kv 行：直属上级 alice
      CHECK(head_pill == nullptr || head_pill->isHidden() ||
            !head_pill->text().contains(QStringLiteral("："))); // 全员视图无负责人胶囊
      // 组织架构实况进 docs 截图目录（浅＋深；随拍随回亮色基线）
      {
        auto& tm = memex::client::ThemeManager::instance();
        CHECK(window.grab().save(QStringLiteral(MEMEX_DOCS_SHOT_DIR) +
                                 QStringLiteral("/live-org-light.png")));
        tm.apply(qApp);
        tm.set_mode(QStringLiteral("dark"));
        CHECK(window.grab().save(QStringLiteral(MEMEX_DOCS_SHOT_DIR) +
                                 QStringLiteral("/live-org-dark.png")));
        tm.set_mode(QStringLiteral("light"));
      }
      // 还原组织数据：后续文件腿的授权裁决依赖「双方未建档＝空部门路径
      // 相等」（见下方 win-dir 腿注释）；本腿造的不同部门会让 alice→bob
      // 被裁决拒绝、文件全灭。清空＝部门/职务/上级全空回基线。
      org_cli({QStringLiteral("org"), QStringLiteral("set"),
               QStringLiteral("alice"), QStringLiteral("--dept"),
               QStringLiteral(""), QStringLiteral("--title"),
               QStringLiteral(""), QStringLiteral("--manager"),
               QStringLiteral("none")});
      org_cli({QStringLiteral("org"), QStringLiteral("set"),
               QStringLiteral("bob"), QStringLiteral("--dept"),
               QStringLiteral(""), QStringLiteral("--title"),
               QStringLiteral(""), QStringLiteral("--manager"),
               QStringLiteral("none")});
      window.request_org();
      // 成员 department_path 清空即可（部门行本身不删——org set 建过的
      // 部门表记录留存，授权裁决只看成员部门路径）
      CHECK(wait_until([&] {
        return !window.org_json().contains(
            QStringLiteral("\"department_path\":\"华信"));
      }, 8000));
      // 回「消息」：org 视图退场（会话面回场）
      emit rail->item_clicked(QStringLiteral("messages"));
      CHECK(wait_until([&] { return !window.org_view_active(); }, 8000));
    }
  }
}

void leg_collab_flow(MainWindow& window) {
  // —— 需求批⑪ 个性签名：发送面（验收缝 apply_signature）→ 服务端受理
  //    回执 → 重拉 org → own_signature 生效（持久面断言；回执状态行是
  //    瞬态提示，不等它）——
  window.apply_signature(QStringLiteral("今天也要专注交付"));
  CHECK(wait_until([&] {
    return window.own_signature() == QStringLiteral("今天也要专注交付");
  }, 8000));

  // —— 需求批⑩ 在线时长：org 数据带出滚动窗并集秒数（在线中即累加），
  //    缓存格式化串齐三窗（持久面断言——org 重拉后缓存即有，不依赖瞬态）——
  CHECK(wait_until([&] {
    return window.own_online().contains(QStringLiteral("在线时长"));
  }, 8000));
  CHECK(window.own_online().contains(QStringLiteral("今日")));
  CHECK(window.own_online().contains(QStringLiteral("本周")));
  CHECK(window.own_online().contains(QStringLiteral("本月")));

  // —— 协作会话：提示条切换为归档口径 ——
  window.open_collab_peer(QStringLiteral("bob"));
  CHECK(window.banner_text().contains(QStringLiteral("全量归档")));
  CHECK(!window.banner_text().contains(QStringLiteral("消息不进归档")));

  // —— 协作会话发送：经服务端受理（回执）——
  // 只等持久面（delivery_text＝最近一条发出消息的状态，send 同步重置为
  // 「发送中…」后由服务端受理回执单调推进到「协作消息已送达」）。原断言
  // 等状态栏 contains("已送达")＝匹配到早前腿的残留文案：孤立跑采样快即
  // 绿、高载下 org/在线时长刷新回包覆盖后即红（假断言面，非实现回归）。
  // 气泡 ✓（DELIVER_NOTICE）本腿到不了——bob 离线，须对端 ACK，T4.3 腿
  //（后文 delivery_text 已送达/已读）才覆盖该面
  CHECK(window.send_in_current_chat(QStringLiteral("模式切换验收消息")));
  CHECK(wait_until([&] {
    return window.delivery_text().contains(QStringLiteral("协作消息已送达"));
  }, 8000));

  // —— T4.5 常用联系人（主窗验收）：发送后 FAV 数据应含 bob，星标可切换 ——
  CHECK(wait_until([&] { return window.fav_json().contains("bob"); }, 8000));

  // —— 协作会话列表态实况（布局令列表态·协作侧）：协作会话已建立（bob
  //    入「最近联系」分组）后，经生产 ✕ 路径关回列表态——「左菜单+列表」
  //    长方形面板（在线设备＋协作会话两分组），抓一张浅色进 docs 截图目录。
  //    2026-10-30 起改存 layout-messages-*（layout-friends-* 让位真·好友
  //    视图实况，见上方好友视图腿） ——
  {
    QPushButton* close_btn = nullptr;
    for (QPushButton* b : window.findChildren<QPushButton*>()) {
      if (b->toolTip().contains(QStringLiteral("关闭会话"))) {
        close_btn = b;
        break;
      }
    }
    CHECK(close_btn != nullptr);
    if (close_btn) {
      close_btn->click();
      CHECK(!window.chat_panel_visible());
      CHECK(window.grab().save(QStringLiteral(MEMEX_DOCS_SHOT_DIR) +
                               QStringLiteral("/layout-messages-light.png")));
      // 深色随需补摄（2026-10-10 浅深配平轮）：同一列表态切暗再抓一张，
      // 随后显式回亮色（同 layout-chat-dark 还原法——system 档判暗会回读
      // 已被染暗的调色板再落回深色）
      {
        auto& tm = memex::client::ThemeManager::instance();
        tm.apply(qApp);
        tm.set_mode(QStringLiteral("dark"));
        CHECK(window.grab().save(QStringLiteral(MEMEX_DOCS_SHOT_DIR) +
                                 QStringLiteral("/layout-messages-dark.png")));
        tm.set_mode(QStringLiteral("light"));
      }
    }
  }

  // —— T4.5 自定义表情导入（A18「表情包可导入」；发送半边＝文件通道，
  //    direct_file 已验；面板走 import_emoji 缝，QFileDialog 面不进断言）——
  {
    QTemporaryDir emoji_root;
    CHECK(emoji_root.isValid());
    const QString src_dir = emoji_root.filePath(QStringLiteral("import-src"));
    const QString dst_dir = emoji_root.filePath(QStringLiteral("emoji-store"));
    CHECK(QDir().mkpath(src_dir));
    CHECK(QDir().mkpath(dst_dir));
    qputenv("MEMEX_TEST_EMOJI_DIR", dst_dir.toUtf8());
    const QString src = src_dir + QStringLiteral("/验收表情.png");
    {
      QFile f(src);
      CHECK(f.open(QIODevice::WriteOnly));
      f.write("fake-png-bytes");
    }
    CHECK(window.import_emoji(src)); // 首次导入成功
    CHECK(window.status_text().contains(QStringLiteral("已导入表情")));
    CHECK(QFile::exists(dst_dir + QStringLiteral("/验收表情.png")));
    CHECK(window.import_emoji(src)); // 同名重复导入＝覆盖，幂等
    CHECK(!window.import_emoji(
        src_dir + QStringLiteral("/不存在.png"))); // 源缺失明确失败不半就
    qunsetenv("MEMEX_TEST_EMOJI_DIR");
  }

  // —— 登出／再登录：不重启切换，历史不丢 ——
  window.logout_collab();
  CHECK(wait_until([&] { return !window.collab_logged_in(); }, 5000));
  CHECK(window.banner_text().contains(QStringLiteral("消息不进归档")));
  // 需求批⑪：登出后签名设置被登录门拦（同步状态行断言＝同轮取值无竞态）
  window.apply_signature(QStringLiteral("不应生效"));
  CHECK(window.status_text().contains(QStringLiteral("设置签名需登录协作态")));
  window.login_collab(QStringLiteral("127.0.0.1"), port,
                      QStringLiteral("alice"), QStringLiteral("pass-a"));
  CHECK(wait_until([&] { return window.collab_logged_in(); }, 8000));

  // —— T4.3 消息状态与多端（主窗验收）——
  // 在线表：登录推送含自己（服务端在线表变化即广播，含自己）。
  CHECK(wait_until([&] {
    return window.online_accounts().contains(QStringLiteral("alice"));
  }, 8000));
  // 发送状态＋已读：对端 bob 真引擎在线——窗口发 bob，bob 端上报已读，
  // 窗口收 READ_NOTICE → 发送中…→已送达→对方已读（单调推进，不回退）。
}
