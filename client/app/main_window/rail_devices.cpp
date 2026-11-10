// 导航与设备列表（LAY-002 拆分）：rail 图标导航、侧栏头、局域网
// 设备列表刷新、空态、会话开关与窗口几何持久化。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QGraphicsOpacityEffect>
#include <QIcon>
#include <QJsonObject>
namespace memex::client {

// —— 原型 rail 面（docs/design/prototypes 桌面屏 .rail，用户令 2026-10-10
//    原型走查 P0＝主导航缺失）：图标导航两套项严格按原型——
//    直连：消息／局域网设备／文件传输／搜索 ┊ 设置／头像
//    协作：消息／组织架构／文件／共享空间（只读）┊ 个人／头像(在线点)
//    「共享空间」全工程（客户端/服务端）均无功能体，导航位照原型落位但
//    置灰，不冒充可用入口；文件传输（desktop-files）与组织架构
//    （desktop-org）均已切整屏视图；好友视图占协作 rail 第二位
//    （desktop-friends）。消息工作区常亮＝本版不换屏（残差如实报）。——
void MainWindow::rebuild_rail() {
  if (!rail_) return;
  const bool collab = collab_engine_.is_logged_in();
  QVector<RailNav::Item> items;
  RailNav::Item messages;
  messages.id = QStringLiteral("messages");
  messages.tip = QStringLiteral("消息");
  messages.icon = RailNav::Icon::Chat;
  items.append(messages);
  if (collab) {
    // 好友视图入口（原型 desktop-friends.html：侧栏好友分组 IA）
    RailNav::Item friends;
    friends.id = QStringLiteral("friends");
    friends.tip = QStringLiteral("好友");
    friends.icon = RailNav::Icon::Person;
    items.append(friends);
    RailNav::Item org;
    org.id = QStringLiteral("org");
    org.tip = QStringLiteral("组织架构");
    org.icon = RailNav::Icon::Org;
    items.append(org);
    RailNav::Item files;
    files.id = QStringLiteral("files");
    files.tip = QStringLiteral("文件");
    files.icon = RailNav::Icon::Folder;
    items.append(files);
    RailNav::Item share;
    share.id = QStringLiteral("share");
    share.tip = QStringLiteral("共享空间（只读）");
    share.icon = RailNav::Icon::Share;
    share.disabled = true;
    items.append(share);
    // 邮箱入口（原型 desktop-mailbox.html：会话/通讯录/文件/搜索/邮箱）；
    // 未读徽章随服务端计数（set_badge；共享空间后＝协作组末位）
    RailNav::Item mailbox;
    mailbox.id = QStringLiteral("mailbox");
    mailbox.tip = QStringLiteral("邮箱");
    mailbox.icon = RailNav::Icon::Mail;
    items.append(mailbox);
  } else {
    RailNav::Item devices;
    devices.id = QStringLiteral("devices");
    devices.tip = QStringLiteral("局域网设备");
    devices.icon = RailNav::Icon::Devices;
    items.append(devices);
    RailNav::Item files;
    files.id = QStringLiteral("files");
    files.tip = QStringLiteral("文件传输");
    files.icon = RailNav::Icon::Folder;
    items.append(files);
    RailNav::Item search;
    search.id = QStringLiteral("search");
    search.tip = QStringLiteral("搜索");
    search.icon = RailNav::Icon::Search;
    items.append(search);
  }
  RailNav::Item spacer;
  spacer.spacer = true; // 原型 .rail .spacer：上下两组分隔
  items.append(spacer);
  if (collab) {
    RailNav::Item profile;
    profile.id = QStringLiteral("profile");
    profile.tip = QStringLiteral("个人");
    profile.icon = RailNav::Icon::Person;
    items.append(profile);
  } else {
    RailNav::Item settings;
    settings.id = QStringLiteral("settings");
    settings.tip = QStringLiteral("设置");
    settings.icon = RailNav::Icon::Gear;
    items.append(settings);
  }
  RailNav::Item avatar;
  avatar.id = QStringLiteral("avatar");
  avatar.tip = QStringLiteral("个人资料");
  avatar.avatar = true;
  items.append(avatar);
  rail_->set_items(items);
  rail_->set_active(QStringLiteral("messages"));
  rail_->apply_tokens(tk());
}

void MainWindow::rail_clicked(const QString& id) {
  // 本版消息工作区＝设备/会话同一列表（原型直连分「消息/局域网设备」两屏，
  // 合并口径残差如实报）；两入口都落焦点到列表。
  if (id == QStringLiteral("messages") || id == QStringLiteral("devices")) {
    leave_transfers_view(); // 传输面 → 会话/列表态（几何随态收口）
    leave_org_view();       // 组织架构面 → 会话/列表态
    leave_mailbox_view();   // 邮箱面 → 会话/列表态
    if (sidebar_mode_ != QStringLiteral("sessions")) {
      sidebar_mode_ = QStringLiteral("sessions");
      update_side_head();
      refresh_devices();
    }
    rail_->set_active(id);
    device_list_->setFocus();
    return;
  }
  if (id == QStringLiteral("friends")) {
    leave_transfers_view();
    leave_org_view();
    leave_mailbox_view();
    if (sidebar_mode_ != QStringLiteral("friends")) {
      sidebar_mode_ = QStringLiteral("friends");
      update_side_head();
      refresh_devices();
    }
    rail_->set_active(id);
    device_list_->setFocus();
    return;
  }
  if (id == QStringLiteral("files")) {
    // 文件传输整屏（原型 desktop-files.html）；文件助手对话框仍在
    // 「协作 → 文件助手…」菜单
    enter_transfers_view();
    return;
  }
  if (id == QStringLiteral("org")) {
    // 组织架构整屏三栏（原型 desktop-org.html）
    enter_org_view();
    return;
  }
  if (id == QStringLiteral("mailbox")) {
    // 邮箱整屏收件箱（原型 desktop-mailbox.html）
    enter_mailbox_view();
    return;
  }
  if (id == QStringLiteral("search")) {
    search_box_->setFocus();
    search_box_->selectAll();
    return;
  }
  if (id == QStringLiteral("settings")) {
    // 二期·统一设置窗口（原型 settings-general.html 统一设置面）：
    // 侧栏 220px 导航（通用/账号/消息/隐私/高级）＋按页堆叠内容。
    show_settings_window();
    return;
  }
  if (id == QStringLiteral("profile") || id == QStringLiteral("avatar")) {
    show_profile_dialog();
    return;
  }
}

// 侧栏头随形态切换（原型：直连「局域网设备 · N 台在线」／协作「消息」／
// 好友视图「好友」）；搜索占位同换（协作「搜索消息 / 联系人 / 群聊」、好友
// 「搜索好友 / 工号」）；推广卡仅直连可见。
// rail 两套项只在模式翻转时重建（refresh_devices 高频调用）。
void MainWindow::update_side_head() {
  const bool collab = collab_engine_.is_logged_in();
  const bool friends = collab && sidebar_mode_ == QStringLiteral("friends");
  if (side_title_) {
    side_title_->setText(friends ? QStringLiteral("好友")
                                 : (collab ? QStringLiteral("消息")
                                           : QStringLiteral("局域网设备")));
  }
  if (search_box_) {
    search_box_->setPlaceholderText(
        friends ? QStringLiteral("搜索好友 / 工号")
                : (collab ? QStringLiteral("搜索消息 / 联系人 / 群聊")
                          : QStringLiteral("搜索设备 / IP")));
  }
  if (disc_hint_) {
    // 注脚随视图：直连＝发现口径；好友＝通讯录来源（应用无加友审批流，
    // 不照抄原型「须经对方同意」）
    disc_hint_->setText(friends ? QStringLiteral("来自组织架构通讯录")
                                : disc_direct_text_);
    disc_hint_->setVisible(!collab || friends);
  }
  if (device_count_) device_count_->setVisible(!collab || friends);
  if (collab_promo_) collab_promo_->setVisible(!collab);
  if (new_chat_btn_) new_chat_btn_->setVisible(collab);
  const QString mode =
      collab ? QStringLiteral("collab") : QStringLiteral("direct");
  if (!collab && sidebar_mode_ != QStringLiteral("sessions") &&
      sidebar_mode_ != QStringLiteral("transfers")) {
    sidebar_mode_ = QStringLiteral("sessions"); // 登出即回会话视图（传输面
                                                // 两形态都有，保留）
  }
  if (rail_ && rail_mode_ != mode) {
    rail_mode_ = mode;
    rebuild_rail();
    rail_->set_avatar_text(collab ? collab_engine_.account()
                                  : CollabEngine::device_name());
  }
  if (rail_) {
    rail_->set_presence(collab, collab && online_accounts_.contains(
                                                   collab_engine_.account()));
  }
  // 视图互斥收口（传输面时盖回上方对会话侧栏控件的显隐写）
  apply_view_mode();
}

void MainWindow::refresh_devices() {
  update_side_head();
  const QString selected = current_peer_;
  device_list_->blockSignals(true);
  device_list_->clear();
  // 好友视图（原型 desktop-friends.html）：侧栏只摆组织架构通讯录
  //（星标好友＋部门分组）；设备行/会话行/临时群都在消息视图
  const bool friends_view =
      collab_engine_.is_logged_in() &&
      sidebar_mode_ == QStringLiteral("friends");
  if (friends_view) {
    struct FEntry {
      QString account, display, dept;
    };
    QList<FEntry> roster;
    const QJsonDocument od =
        QJsonDocument::fromJson(last_org_json_.toUtf8());
    if (od.isObject()) {
      for (const auto& m :
           od.object().value(QStringLiteral("members")).toArray()) {
        const QJsonObject mo = m.toObject();
        FEntry e;
        e.account = mo.value(QStringLiteral("account")).toString();
        e.display = mo.value(QStringLiteral("display_name")).toString();
        e.dept = mo.value(QStringLiteral("department_path")).toString();
        if (!e.account.isEmpty() && e.account != collab_engine_.account())
          roster.push_back(e);
      }
    }
    int online = 0;
    for (const FEntry& e : roster)
      if (online_accounts_.contains(e.account)) ++online;
    device_count_->setText(
        roster.isEmpty()
            ? QStringLiteral("组织架构未下发")
            : QStringLiteral("<span style=\"color:%1;\">●</span> %2 / %3 在线")
                  .arg(tk().success.name())
                  .arg(online)
                  .arg(roster.size()));
    auto add_header = [this](const QString& title) {
      auto* header = new QListWidgetItem(title);
      header->setFlags(Qt::NoItemFlags);
      device_list_->addItem(header);
    };
    auto add_friend_row = [this](const FEntry& e) {
      const QString shown =
          e.display.isEmpty() ? e.account : e.display;
      // 原型行名「人名 · 部门」（部门取路径末级；未分配不拼）
      QString leaf;
      if (!e.dept.isEmpty()) leaf = e.dept.section(QChar('/'), -1);
      const QString rowname =
          leaf.isEmpty()
              ? shown
              : QStringLiteral("%1 · %2").arg(shown, leaf);
      // 末条预览：与协作会话行同口径（撤回如实标；无历史「暂无消息」）
      QString preview;
      const auto hist = direct_engine_.history(e.account, 1);
      if (!hist.isEmpty()) {
        const QString text =
            hist.last().recalled
                ? QStringLiteral("撤回了一条消息")
                : QString::fromStdString(hist.last().text);
        preview = hist.last().from == collab_engine_.account().toStdString()
                      ? QStringLiteral("我：%1").arg(text)
                      : text;
      } else {
        preview = QStringLiteral("暂无消息");
      }
      auto* item = new QListWidgetItem(device_list_);
      QWidget* row = make_session_row(
          tk(), shown, online_accounts_.contains(e.account) ? 1 : 0,
          rowname, fmt_last_contact(fav_last_ms_(e.account)), preview,
          unread_counts_.value(e.account));
      item->setSizeHint(row->sizeHint());
      device_list_->setItemWidget(item, row);
      item->setData(Qt::UserRole, e.account);
      item->setData(Qt::UserRole + 1, QStringLiteral("collab"));
      item->setData(Qt::UserRole + 6,
                    QStringList{e.account, e.display, e.dept}.join(
                        QLatin1Char('\n')));
      item->setToolTip(QStringLiteral("账号：%1\n部门：%2")
                           .arg(e.account,
                                e.dept.isEmpty()
                                    ? QStringLiteral("（未分配）")
                                    : e.dept));
    };
    if (roster.isEmpty()) {
      auto* hint = new QListWidgetItem(
          QStringLiteral("组织架构未下发（登录后自动拉取）"));
      hint->setFlags(Qt::NoItemFlags);
      device_list_->addItem(hint);
    } else {
      QList<FEntry> starred, plain;
      for (const FEntry& e : roster)
        (fav_is_starred_(e.account) ? starred : plain).push_back(e);
      if (!starred.isEmpty()) {
        add_header(QStringLiteral("星标好友"));
        for (const FEntry& e : starred) add_friend_row(e);
      }
      // 部门分组：全路径为键（末级同名不撞），组内按显示名
      std::sort(plain.begin(), plain.end(),
                [](const FEntry& a, const FEntry& b) {
                  return a.display < b.display;
                });
      QMap<QString, QList<FEntry>> by_dept;
      for (const FEntry& e : plain)
        by_dept[e.dept.isEmpty() ? QStringLiteral("（未分配部门）")
                                 : e.dept].push_back(e);
      for (auto it = by_dept.constBegin(); it != by_dept.constEnd(); ++it) {
        add_header(QStringLiteral("%1 · %2 人").arg(it.key()).arg(it->size()));
        for (const FEntry& e : it.value()) add_friend_row(e);
      }
    }
    device_list_->blockSignals(false);
    if (!selected.isEmpty()) {
      for (int i = 0; i < device_list_->count(); ++i) {
        if (device_list_->item(i)->data(Qt::UserRole).toString() ==
            selected) {
          device_list_->setCurrentRow(i, QItemSelectionModel::NoUpdate);
          break;
        }
      }
    }
    return;
  }
  const auto peers = direct_engine_.peers();
  // 原型直连列表（desktop-direct.html）：「在线设备」一组会话行（头像/在
  // 线点/姓名·设备/时间/末条预览/未读胶囊）。离线对端由发现服务超时即摘，
  // 无离线留存表——「离线设备」分区不渲染（台账近似项）。
  if (!peers.isEmpty()) {
    auto* header = new QListWidgetItem(QStringLiteral("在线设备"));
    header->setFlags(Qt::NoItemFlags);
    device_list_->addItem(header);
  }
  QList<QPair<qint64, const Peer*>> order;
  for (const Peer& p : peers) order.push_back({fav_last_ms_(QString::fromStdString(p.device_id)), &p});
  std::sort(order.begin(), order.end(),
            [](const QPair<qint64, const Peer*>& a,
               const QPair<qint64, const Peer*>& b) {
              if (a.first != b.first) return a.first > b.first;
              return a.second->name < b.second->name;
            });
  for (const auto& kv : order) {
    const Peer& p = *kv.second;
    auto* item = new QListWidgetItem(device_list_);
    const QString id = QString::fromStdString(p.device_id);
    const QString name =
        p.name.empty() ? id.left(8) : QString::fromStdString(p.name);
    // 飞秋对端（"ipmsg:" 前缀）无 TCP 直连口：元信息行走 UDP 标注
    const bool is_ipmsg = id.startsWith(QStringLiteral("ipmsg:"));
    const QString meta =
        is_ipmsg
            ? QStringLiteral("飞秋 · UDP %1").arg(p.address.toString())
            : QStringLiteral("%1 · TCP %2")
                  .arg(p.address.toString(), QString::number(p.tcp_port));
    // T4.2：对端协作账号（仅作显示；空=未登录）——原型行「人名 · 设备名」，
    // 应用侧账号 id 即人名位
    const QString account = QString::fromStdString(p.account);
    const QString display =
        account.isEmpty() ? name
                          : QStringLiteral("%1 · %2").arg(account, name);
    // 末条预览（原型 .r-sub）：本地库最近一条（撤回如实标）；文件传输
    // 不落库，无历史走元信息行兜底（IP · 口）
    QString preview;
    const auto hist = direct_engine_.history(id, 1);
    if (!hist.isEmpty()) {
      preview = hist.last().recalled
                    ? QStringLiteral("撤回了一条消息")
                    : QString::fromStdString(hist.last().text);
    } else {
      preview = meta;
    }
    // 「盯」已读提醒高亮（原文本行 ◉ 前缀位；打开该会话即清）
    const QString shown =
        (watch_reminded_peers_.contains(id) ? QStringLiteral("◉ ")
                                            : QString()) +
        display;
    // 列表内对端都在发现超时窗内＝在线（presence=1）；头像首字取人名位
    QWidget* row = make_session_row(
        tk(), account.isEmpty() ? name : account, 1, shown,
        fmt_last_contact(fav_last_ms_(id)), preview,
        unread_counts_.value(id));
    item->setSizeHint(row->sizeHint());
    device_list_->setItemWidget(item, row);
    // 元信息（IP/端口/协作账号）退悬浮——行面让位原型会话行
    item->setToolTip(QStringLiteral("%1\n%2%3").arg(
        display, meta,
        account.isEmpty()
            ? QString()
            : QStringLiteral("\n协作账号 %1").arg(account)));
    item->setData(Qt::UserRole, id);
    item->setData(Qt::UserRole + 1, QStringLiteral("direct"));
    // 需求批⑨查找：可检索字段＝昵称/设备 id/IP/协作账号（\n 分行，整行比对
    // ＝精确、子串＝模糊，两态都能命中）
    item->setData(Qt::UserRole + 6,
                  QStringList{name, id, p.address.toString(), account}
                      .join(QLatin1Char('\n')));
  }

  // 协作会话（本地历史 + 本会话窗口期的对端）与服务端群合并成原型两组
  //（desktop-collab.html 侧栏）：置顶（星标）/最近联系（其余）——群行与
  // 人行同列混排（原型产品研发部/MES 群均在两组内），行控件化（头像/在
  // 离线点/末条预览/未读胶囊）替原两行纯文本。
  if (collab_engine_.is_logged_in() &&
      (!collab_peers_.isEmpty() || !groups_.isEmpty())) {
    struct Entry {
      QString key, kind, name;
      quint64 gid{0};
      int members{0};
    };
    QList<Entry> entries;
    for (const QString& a : collab_peers_)
      entries.push_back({a, QStringLiteral("collab"), a, 0, 0});
    for (auto it = groups_.constBegin(); it != groups_.constEnd(); ++it) {
      entries.push_back({QStringLiteral("group:%1").arg(it.key()),
                         QStringLiteral("group"), it->name, it.key(),
                         int(it->members.size())});
    }
    // T4.5：星标置顶（最近优先）、其余按最近联系、再按显示名
    std::sort(entries.begin(), entries.end(),
              [this](const Entry& a, const Entry& b) {
                const bool sa = fav_is_starred_(a.key),
                           sb = fav_is_starred_(b.key);
                if (sa != sb) return sa;
                const qint64 ta = fav_last_ms_(a.key), tb = fav_last_ms_(b.key);
                if (ta != tb) return ta > tb;
                return a.name < b.name;
              });
    QList<Entry> starred, recent;
    for (const Entry& e : entries)
      (fav_is_starred_(e.key) ? starred : recent).push_back(e);
    auto add_header = [this](const QString& title) {
      auto* header = new QListWidgetItem(title);
      header->setFlags(Qt::NoItemFlags); // 分组标题：不可选（空 id 不进会话）
      device_list_->addItem(header);
    };
    auto add_session_row = [this](const Entry& e) {
      const bool group = e.kind == QStringLiteral("group");
      auto* item = new QListWidgetItem(device_list_);
      // 末条预览（原型 .r-sub）：本地库最近一条（撤回行如实标）；文件
      // 不落消息库（会话内存态），预览只反映文本消息。无历史走归档口径
      // 兜底行（群行保留人数/群号信息位）。
      QString preview;
      const auto hist = direct_engine_.history(e.key, 1);
      if (!hist.isEmpty()) {
        const auto& m = hist.last();
        const QString text =
            m.recalled ? QStringLiteral("撤回了一条消息")
                       : QString::fromStdString(m.text);
        if (m.from == collab_engine_.account().toStdString()) {
          preview = QStringLiteral("我：%1").arg(text);
        } else if (group) {
          preview = QStringLiteral("%1：%2")
                        .arg(QString::fromStdString(m.from), text);
        } else {
          preview = text;
        }
      } else if (group) {
        // 原型 .r-sub「N 人 · 群公告已更新」：有公告走公告口径，无公告保
        // 留群号＋归档信息位
        const GroupEntry& ge = groups_.value(e.gid);
        preview = !ge.announcement.isEmpty()
                      ? QStringLiteral("%1 人 · 群公告已更新")
                            .arg(QString::number(e.members))
                      : QStringLiteral("%1 人 · 群 %2 · 已归档")
                            .arg(QString::number(e.members),
                                 QString::number(e.gid));
      } else {
        preview = QStringLiteral("协作态 · 已归档");
      }
      // 「盯」已读提醒高亮（原文本行 ◉ 前缀位；打开该会话即清）
      const QString name =
          (watch_reminded_peers_.contains(e.key) ? QStringLiteral("◉ ")
                                                 : QString()) +
          e.name;
      // T4.3：在离线点随服务端推送刷新（在线表含自己；群行无点）
      QWidget* row =
          make_session_row(tk(), e.name,
                           group ? -1
                                 : (online_accounts_.contains(e.key) ? 1 : 0),
                           name, fmt_last_contact(fav_last_ms_(e.key)),
                           preview, unread_counts_.value(e.key));
      item->setSizeHint(row->sizeHint());
      device_list_->addItem(item);
      device_list_->setItemWidget(item, row);
      item->setData(Qt::UserRole, e.key);
      item->setData(Qt::UserRole + 1, e.kind);
      if (group) {
        // ⑨：群名/群号/成员账号（按成员找群＝找联系人的群面）
        item->setData(Qt::UserRole + 6,
                      e.name + QLatin1Char('\n') + QString::number(e.gid) +
                          QLatin1Char('\n') +
                          groups_.value(e.gid).members.join(
                              QLatin1Char('\n')));
      } else {
        item->setData(Qt::UserRole + 6, e.key); // ⑨：账号精确/模糊皆命中
        // 需求批⑪：签名悬浮可见（空=未设不显）
        QStringList tips;
        const QString sig = member_signatures_.value(e.key);
        if (!sig.isEmpty()) {
          tips << QStringLiteral("个性签名：%1").arg(sig);
        }
        // 需求批⑩：在线时长悬浮可见（滚动 24h/7d/30d 并集时长）
        const QString dur = member_online_.value(e.key);
        if (!dur.isEmpty()) {
          tips << dur;
        }
        // 需求批⑫：头像悬浮可见（富文本首行；已设走缓存文件，未设走默认组）
        QString tip_html =
            QStringLiteral("<img src='%1' width='32' height='32'>")
                .arg(avatar_tooltip_src(
                    e.key, member_avatar_ver_.value(e.key), 32));
        if (!tips.isEmpty()) {
          tip_html += QStringLiteral("<br>") + tips.join(QLatin1String("<br>"));
        }
        item->setToolTip(tip_html);
      }
    };
    if (!starred.isEmpty()) {
      add_header(QStringLiteral("置顶"));
      for (const Entry& e : starred) add_session_row(e);
    }
    if (!recent.isEmpty()) {
      add_header(QStringLiteral("最近联系"));
      for (const Entry& e : recent) add_session_row(e);
    }
  }

  // 免服务端临时群（直连态，T4.1）：点对点扇出，不进归档——行控件化对齐
  // 原型（desktop-group.html 列表临群行：opacity .7 弱化＋「免服务端 · 仅
  // 本网段 · 不归档」副行＋.noarch「未归档」徽标）
  if (!dgroup_members_.isEmpty()) {
    auto* header = new QListWidgetItem(QStringLiteral("临时群 · 不进归档"));
    header->setFlags(Qt::NoItemFlags);
    device_list_->addItem(header);
    for (auto it = dgroup_members_.constBegin(); it != dgroup_members_.constEnd();
         ++it) {
      auto* item = new QListWidgetItem(device_list_);
      const QString title = dgroup_title(it.key());
      QWidget* row = make_session_row(tk(), title, -1, title, QString(),
                                      QStringLiteral("免服务端 · 仅本网段 · 不归档"),
                                      unread_counts_.value(it.key()),
                                      QStringLiteral("未归档"));
      auto* dim = new QGraphicsOpacityEffect(row);
      dim->setOpacity(0.7); // 原型临时群行 opacity:.7（不归档群视觉弱化）
      row->setGraphicsEffect(dim);
      item->setSizeHint(row->sizeHint());
      device_list_->addItem(item);
      device_list_->setItemWidget(item, row);
      item->setData(Qt::UserRole, it.key());
      item->setData(Qt::UserRole + 1, QStringLiteral("dgroup"));
      item->setToolTip(QStringLiteral("%1 台设备 · 点对点扇出")
                           .arg(it->size()));
      // ⑨：临群名/成员设备 id
      item->setData(Qt::UserRole + 6,
                    QStringList{title}.join(QLatin1Char('\n')) +
                        QLatin1Char('\n') + it->join(QLatin1Char('\n')));
    }
  }
  device_list_->blockSignals(false);
  // 空会话空态（原型 main-empty.html .empty-list）：协作态已登录而列表
  // 零行——居中两行引导（muted 12px）。直连零行不摆（发现面的在线设备
  // hint/「未发现设备」计数已覆盖，desktop-direct 口径）
  if (collab_engine_.is_logged_in() && device_list_->count() == 0) {
    auto* item = new QListWidgetItem(device_list_);
    auto* empty = new QLabel(
        QStringLiteral("还没有会话\n从通讯录选择联系人，开始聊天"),
        device_list_);
    empty->setObjectName(QStringLiteral("empty_list_hint"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;")
            .arg(tk().text_muted.name()));
    item->setFlags(Qt::NoItemFlags);
    // QSS「item { padding:8px }」会把行内控件矩形上下各吃 8px：不补
    // 则两行 label 被压成 15px 只剩半行（实拍验证）。宽度由列宽接管
    item->setSizeHint(QSize(empty->sizeHint().width(),
                            empty->sizeHint().height() + 16));
    device_list_->setItemWidget(item, empty);
  }
  // 原型 device-count：success 绿点＋muted 计数；空表如实「未发现设备」
  device_count_->setText(
      peers.isEmpty()
          ? QStringLiteral("未发现设备")
          : QStringLiteral("<span style=\"color:%1;\">●</span> %2 台在线")
                .arg(tk().success.name())
                .arg(peers.size()));

  // 当前会话设备离线 → 保留视图但提示；列表选中态恢复
  if (!selected.isEmpty()) {
    for (int i = 0; i < device_list_->count(); ++i) {
      if (device_list_->item(i)->data(Qt::UserRole).toString() == selected) {
        device_list_->setCurrentRow(i, QItemSelectionModel::NoUpdate);
        break;
      }
    }
  }

  // 未选中会话时的空态：协作态已登录走 main-empty 欢迎页（主区文案就位；
  // 列表态窗口按布局令坍缩 320px，面板隐藏呈潜伏态）；直连态无设备显示
  // 发现引导；有设备提示先选择
  if (selected.isEmpty()) render_empty_state();
}

// 未选中会话时的主区空态（refresh_devices 与 close_chat 共用同口径）：
// 协作态已登录＝main-empty 欢迎页；直连零设备＝发现引导；有设备＝
// 「先选择」提示行（幂等：已在提示态不重复追加）
void MainWindow::render_empty_state() {
  if (collab_engine_.is_logged_in()) {
    show_welcome_guide();
    return;
  }
  if (direct_engine_.peers().isEmpty()) {
    show_guidance();
    return;
  }
  if (!chat_showing_guidance_ && !chat_rows_.isEmpty()) return; // 已在提示态
  chat_showing_guidance_ = false;
  chat_view_->clear();
  chat_rows_.clear();
  append_system_line(QStringLiteral("从左侧选择设备，开始点对点会话"));
}

void MainWindow::open_peer(const QString& device_id) {
  open_chat(QStringLiteral("direct"), device_id);
}

// 打开会话（kind=direct 设备 / collab 账号）：历史取自共享本地库——
// 直连与协作消息同库合并，按 source 字段标注来源（T2.4 合并展示）。
// 关会话回列表态（布局令）：清当前会话（历史在库里，重开即重渲），
// 面板收起、窗口缩回「左菜单+好友列表」长方形。启动也走此路＝无会话
// 必然坍缩到列表态尺寸（2026-10-15 缺陷修复：此前启动保持 1280 横窗）
void MainWindow::close_chat() {
  current_kind_.clear();
  current_peer_.clear();
  chat_rows_.clear();
  chat_view_->clear();
  chat_panel_->hide();
  chat_open_ = false; // 布局态收回列表态（存档键/最小尺寸判据）
  // 群会话附属面随会话收起（右栏/头部群钮/@提示——下次开非群会话不带出）
  if (group_panel_) group_panel_->hide();
  if (head_announce_btn_) head_announce_btn_->hide();
  if (head_gfiles_btn_) head_gfiles_btn_->hide();
  if (head_members_btn_) head_members_btn_->hide();
  if (at_hint_) at_hint_->hide();
  if (at_menu_) at_menu_->hide();
  chat_title_->setToolTip(QString());
  // 列表态：列表列占满 rail 之外的全部窗宽（微信/QQ 启动即列表；无论
  // 启动还是关会话，右缘都不再留空白——2026-10-14 用户报）
  side_->setMaximumWidth(QWIDGETSIZE_MAX);
  side_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  apply_state_minimum();
  if (!isMaximized() && width() > 400) resize(320, height());
  // 关会话即空选态：主区空态先行就位（面板已隐藏，潜伏态；下一次
  // refresh_devices 同口径重渲）
  render_empty_state();
  // 列表态收紧窗口右缘上限＝左侧列内容宽（rail 60＋会话列 260）：拖窗口右缘
  // 不再把列表与右框之间撑出死空间（2026-10-11 用户报——列表态拉宽窗口，
  // 对话面板已收起，右侧空档与列表视觉分离）。拉长只应发生在会话态（右窗
  // 可见时由 chat 面吸收）。最大化不受本上限约束（受屏幕几何支配）。
  if (!isMaximized()) setMaximumWidth(rail_->minimumWidth() + side_->minimumWidth());
}

// 最小尺寸随布局态（Telegram TGSplitView 每态各有一档最小宽度）：列表态
// 下限＝左侧两列内容宽（rail 60＋列表列 260）；会话态＝三列列宽之和
//（Telegram columnMinimalWidthLeft/Main 口径），与内容 sizeHint 解耦。
void MainWindow::apply_state_minimum() {
  if (chat_open_) {
    // 会话态＝rail＋列表列＋会话列三列全显：60＋260＋380（Telegram
    // columnMinimalWidthLeft/Main 口径＝列最小宽之和，与内容 sizeHint
    // 解耦）。不能用布局 minimumSizeHint：聊天输入行一串文字按钮把自然
    // 下限顶到 892——那是内容的下限不是窗口的下限，钳住反而把用户此前
    // 可自由缩窄的会话窗锁死；380 只保证会话列最小可读，工具条挤窄是
    // 既有表现（修复前窗口可缩到 320，内容同样挤压）
    setMinimumSize(rail_->minimumWidth() + side_->minimumWidth() + 380, 420);
  } else {
    setMinimumSize(320, 420);
  }
}

// —— 窗口几何持久化（同类 IM 通行做法：Telegram savePosition／Slack
// mainWindowSettings.bounds／Discord WINDOW_BOUNDS 均记住上次尺寸与位置）——
// 存档按布局态分键（Telegram TGWindowArchiver 按视图态归档同构）：列表态
// 与会话态各自存档，恢复时只取「当前态」的存档——会话态的宽窗不盖回无会话
// 态（2026-10-15 缺陷：启动无会话却按上次会话宽显示）。默认几何只在无存档
// 用。QSettings 与本文件其他键同口径（组织/应用名）。
void MainWindow::save_window_geometry() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  if (isMaximized()) return; // 最大化态不存（恢复时按普通态处理）
  settings.setValue(chat_open_
                        ? QStringLiteral("window_geometry_chat")
                        : QStringLiteral("window_geometry_list"),
                    saveGeometry());
}

// 启动恢复：启动必然是列表态（布局令＝构造即 close_chat），只取列表态存档；
// 无存档＝保持默认列表态尺寸（close_chat 已钳 320 宽）。
void MainWindow::restore_window_geometry() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  const QByteArray geom =
      settings.value(QStringLiteral("window_geometry_list")).toByteArray();
  // 首次启动＝无存档：保持默认几何（close_chat 已钳 320 宽）
  if (!geom.isEmpty() && restoreGeometry(geom) && !isMaximized() &&
      width() > 400)
    // 列表态必为 320 宽：旧版（单键时代）留下的宽窗存档兜底
    resize(320, height());
  // 列表态右缘上限无条件复归：空档新装（close_chat 已设）与有存档路径同
  // 一出口——只在 restoreGeometry 成功分支里设上限＝新装环境上限恒放开
  // （布局腿实录假红）
  if (!isMaximized())
    setMaximumWidth(rail_->minimumWidth() + side_->minimumWidth());
}

// 会话态恢复（open_chat 展开用）：优先会话态存档，无则退旧单键存档（旧版
// 只存过 ≥480 宽的窗——列表窄条从不存档，宽窗必然属会话态），再失败返回
// false 由调用方落会话态默认尺寸。返回值＝是否已恢复。
bool MainWindow::restore_chat_geometry() {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  QByteArray geom =
      settings.value(QStringLiteral("window_geometry_chat")).toByteArray();
  if (geom.isEmpty())
    geom = settings.value(QStringLiteral("window_geometry")).toByteArray();
  if (geom.isEmpty()) return false;
  if (!restoreGeometry(geom)) return false;
  if (isMaximized()) return true;
  if (width() < 520) resize(1280, height()); // 窄/损坏存档兜底：会话态默认宽
  return true;
}

} // namespace memex::client
