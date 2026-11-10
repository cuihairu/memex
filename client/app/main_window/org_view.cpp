// 组织架构面（LAY-002 拆分）：部门树、成员网格、档案栏；成员卡
// 点击事件过滤 OrgCardClick 收编为本模块局部类。

#include "main_window.hpp"
#include "main_window/common.hpp"


#include <QHeaderView>
#include <QTreeWidget>
#include <functional>
#include <map>
#include <utility>

#include "theme.hpp"
namespace memex::client {

namespace {

// 组织架构成员卡点击选中（事件过滤：QWidget 无 clicked 信号，鼠标释放
// 即触发；随卡片父子链销毁）
class OrgCardClick : public QObject {
 public:
  explicit OrgCardClick(std::function<void()> fn, QObject* parent)
      : QObject(parent), fn_(std::move(fn)) {}
  bool eventFilter(QObject*, QEvent* e) override {
    if (e->type() == QEvent::MouseButtonRelease) {
      fn_();
      return true;
    }
    return false;
  }

 private:
  std::function<void()> fn_;
};

} // namespace

// 部门子树成员计数（path 空=全量；子树=本路径或以「路径/」开头）。
int MainWindow::org_dept_member_count(const QString& path) const {
  int n = 0;
  for (const OrgMember& m : org_members_) {
    if (path.isEmpty() || m.dept == path ||
        m.dept.startsWith(path + QStringLiteral("/")))
      ++n;
  }
  return n;
}

// 部门负责人推导（无独立字段，不造假）：部门子树内、直属上级不在本部门
// 子树内的成员唯一时给出（组织树意义上传出部门的那个人）；否则省略胶囊。
QString MainWindow::org_dept_head(const QString& path) const {
  QString head;
  for (const OrgMember& m : org_members_) {
    const bool in_dept =
        m.dept == path || m.dept.startsWith(path + QStringLiteral("/"));
    if (!in_dept || m.manager.isEmpty()) continue;
    bool manager_outside = true;
    for (const OrgMember& x : org_members_) {
      if (x.account != m.manager) continue;
      if (x.dept == path || x.dept.startsWith(path + QStringLiteral("/")))
        manager_outside = false;
      break;
    }
    if (!manager_outside) continue;
    if (!head.isEmpty()) return {}; // 多候选＝推导不唯一，不冒充
    head = m.name.isEmpty() ? m.account : m.name;
  }
  return head;
}

// 全量重建（org 数据到达／进入视图／主题切换）：缓存解析→左栏树→中栏
// 网格→右栏档案。清空重建循三招模板：先 hide 再 deleteLater、新件显式
// show、尾部 activate 同步结算（嵌套事件泵抓图实录）。
void MainWindow::rebuild_org_view() {
  if (!org_side_ || !org_page_) return;
  const ThemeTokens& t = tk();
  // —— 数据缓存（树/网格/档案共用）——
  org_members_.clear();
  org_dept_paths_.clear();
  {
    nlohmann::json j = nlohmann::json::parse(last_org_json_.toStdString(),
                                             nullptr, false);
    if (!j.is_discarded()) {
      if (j.contains("departments")) {
        for (const auto& d : j["departments"]) {
          const QString p =
              QString::fromStdString(d.value("path", std::string{}));
          if (!p.isEmpty()) org_dept_paths_ << p;
        }
      }
      if (j.contains("members")) {
        for (const auto& m : j["members"]) {
          OrgMember om;
          om.account =
              QString::fromStdString(m.value("account", std::string{}));
          om.name =
              QString::fromStdString(m.value("display_name", std::string{}));
          om.title =
              QString::fromStdString(m.value("title", std::string{}));
          om.dept = QString::fromStdString(
              m.value("department_path", std::string{}));
          om.manager =
              QString::fromStdString(m.value("manager", std::string{}));
          om.role = QString::fromStdString(m.value("role", std::string{}));
          if (!om.account.isEmpty()) org_members_.append(om);
        }
      }
    }
  }
  // —— 左栏：树＋可见性说明卡（org_tree_lay_ 里树销毁重建，说明卡常驻）——
  // 先断开旧过滤 connect 再清词（org_search_ 常驻，重建累积 connect 会泄漏；
  // 陈旧过滤词不带入新树）
  disconnect(org_search_, &QLineEdit::textChanged, this, nullptr);
  org_search_->clear();
  while (org_tree_lay_->count() > 0) {
    QLayoutItem* it = org_tree_lay_->takeAt(0);
    if (QWidget* w = it ? it->widget() : nullptr) {
      if (w != org_note_ && w != org_search_) {
        w->hide();
        w->deleteLater();
      }
    }
    delete it;
  }
  // 常驻件回挂（清空循环把它们也摘了下来）
  org_search_->show();
  org_tree_lay_->addWidget(org_search_);
  org_tree_ = new QTreeWidget(org_side_);
  org_tree_->setObjectName(QStringLiteral("org_tree"));
  org_tree_->setHeaderHidden(true);
  org_tree_->setRootIsDecorated(false);
  org_tree_->setIndentation(14);
  org_tree_->setFrameShape(QFrame::NoFrame);
  org_tree_->setColumnCount(2); // 0=部门名（拉伸） 1=子树计数（固定宽右对齐）
  org_tree_->header()->setStretchLastSection(false);
  org_tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  org_tree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
  org_tree_->header()->resizeSection(1, 38);
  org_tree_->setStyleSheet(
      QStringLiteral("QTreeWidget { background:transparent; border:none; "
                     "font-size:12.5px; color:%1; }"
                     "QTreeWidget::item { padding:5px 6px; border-radius:8px; }"
                     "QTreeWidget::item:selected { background:%2; color:%3; }")
          .arg(t.text.name(), t.brand_tint.name(), t.text.name()));
  // 全路径逐级挂树（同旧弹窗算法），节点带子树成员计数
  std::map<QString, QTreeWidgetItem*> nodes;
  {
    // 根节点「全部成员」（路径空=全员；唯一回全量视图的入口）
    auto* root = new QTreeWidgetItem(org_tree_->invisibleRootItem());
    root->setText(0, QStringLiteral("全部成员"));
    root->setData(0, Qt::UserRole, QString());
    root->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    nodes.emplace(QString(), root);
  }
  for (const QString& path : org_dept_paths_) {
    const QStringList segs = path.split(QChar('/'));
    QTreeWidgetItem* parent = nullptr;
    QString prefix;
    for (const QString& seg : segs) {
      prefix = prefix.isEmpty() ? seg : prefix + QChar('/') + seg;
      auto it = nodes.find(prefix);
      if (it == nodes.end()) {
        auto* item = new QTreeWidgetItem(
            parent ? parent : org_tree_->invisibleRootItem());
        item->setText(0, seg);
        item->setData(0, Qt::UserRole, prefix);
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        it = nodes.emplace(prefix, item).first;
      }
      parent = it->second;
    }
  }
  for (auto& [path, item] : nodes) {
    item->setText(1, QString::number(org_dept_member_count(path)));
    item->setForeground(
        1, QBrush(t.text_muted)); // 计数弱化（正文令牌色由树样式统管 0 列）
  }
  org_tree_->expandAll();
  connect(org_tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
    auto* sel = org_tree_ ? org_tree_->currentItem() : nullptr;
    org_selected_dept_ = sel ? sel->data(0, Qt::UserRole).toString()
                             : QString();
    refresh_org_members();
  });
  org_tree_->show();
  org_tree_lay_->addWidget(org_tree_, 1);
  // 可见性策略说明卡（服务端真实裁剪口径：隐藏部门/成员数据不出服务端）
  org_note_->setText(
      QStringLiteral("通讯录可见性按部门策略下发：被隐藏的部门与成员不出现在"
                     "此处、不可被搜索；受限部门只能看到本部门。"));
  org_note_->setStyleSheet(
      QStringLiteral("border:1px dashed %1; border-radius:10px; padding:10px "
                     "12px; font-size:11px; color:%2;")
          .arg(t.border.name(), t.text_muted.name()));
  org_note_->show();
  org_tree_lay_->addWidget(org_note_);
  org_tree_lay_->activate();
  // 搜索：部门名/成员名/账号任一命中即保留该部门子树（含祖先链）
  connect(org_search_, &QLineEdit::textChanged, this, [this](const QString&) {
    if (!org_tree_) return;
    const QString needle = org_search_->text().trimmed();
    QTreeWidgetItemIterator it(org_tree_);
    while (*it) {
      QTreeWidgetItem* item = *it;
      ++it;
      const QString path = item->data(0, Qt::UserRole).toString();
      bool hit = needle.isEmpty();
      if (!hit) hit = item->text(0).contains(needle, Qt::CaseInsensitive);
      if (!hit) {
        for (const OrgMember& m : org_members_) {
          const bool in_subtree =
              m.dept == path || m.dept.startsWith(path + QStringLiteral("/"));
          if (in_subtree &&
              (m.name.contains(needle, Qt::CaseInsensitive) ||
               m.account.contains(needle, Qt::CaseInsensitive))) {
            hit = true;
            break;
          }
        }
      }
      item->setHidden(!hit);
    }
    // 祖先链回显：命中节点的父链不藏
    QTreeWidgetItemIterator it2(org_tree_);
    while (*it2) {
      QTreeWidgetItem* item = *it2;
      ++it2;
      if (item->isHidden()) continue;
      for (QTreeWidgetItem* p = item->parent(); p; p = p->parent())
        p->setHidden(false);
    }
  });
  // —— 中栏＋右栏 ——
  refresh_org_members();
  show_org_profile(org_selected_member_);
}

// 中栏：部门头（部门 · N 人＋负责人胶囊）＋成员卡网格（两列流）。
void MainWindow::refresh_org_members() {
  if (!org_grid_lay_ || !org_grid_empty_ || !org_head_title_) return;
  const ThemeTokens& t = tk();
  const QString me = collab_engine_.account();
  // 部门成员集（子树；空路径=全部可见成员）
  QList<const OrgMember*> dept_members;
  for (const OrgMember& m : org_members_) {
    const bool in = org_selected_dept_.isEmpty() ||
                    m.dept == org_selected_dept_ ||
                    m.dept.startsWith(org_selected_dept_ +
                                      QStringLiteral("/"));
    if (in) dept_members.append(&m);
  }
  const QString dept_label =
      org_selected_dept_.isEmpty()
          ? QStringLiteral("全部成员")
          : org_selected_dept_.section(QChar('/'), -1);
  org_head_title_->setText(QStringLiteral("%1 · %2 人")
                               .arg(dept_label)
                               .arg(dept_members.size()));
  org_head_title_->setStyleSheet(
      QStringLiteral("font-size:15px; font-weight:700; color:%1;")
          .arg(t.text.name()));
  const QString head = org_dept_head(org_selected_dept_);
  if (head.isEmpty()) {
    org_head_pill_->clear();
    org_head_pill_->hide();
  } else {
    org_head_pill_->setText(QStringLiteral("部门负责人：%1").arg(head));
    org_head_pill_->setStyleSheet(
        QStringLiteral("color:%1; border:1px solid %2; border-radius:999px;"
                       " padding:2px 10px; font-size:11.5px;")
            .arg(t.text_muted.name(), t.border.name()));
    org_head_pill_->show();
  }
  // 清空重建（org_grid_empty_ 常驻复用；三招模板）
  while (org_grid_lay_->count() > 0) {
    QLayoutItem* it = org_grid_lay_->takeAt(0);
    if (QWidget* w = it ? it->widget() : nullptr) {
      if (w != org_grid_empty_) {
        w->hide();
        w->deleteLater();
      }
    }
    delete it;
  }
  if (dept_members.isEmpty()) {
    org_grid_empty_->setText(
        org_members_.isEmpty()
            ? QStringLiteral("组织架构数据为空（服务端未下发成员）")
            : QStringLiteral("该部门暂无成员"));
    org_grid_empty_->setStyleSheet(
        QStringLiteral("color:%1; font-size:11.5px;")
            .arg(t.text_muted.name()));
    org_grid_empty_->show();
    org_grid_lay_->addWidget(org_grid_empty_);
    org_grid_lay_->addStretch();
    org_grid_lay_->activate();
    return;
  }
  // 排序：显示名，本人在最后压轴（原型「张伟（我）」卡）
  std::sort(dept_members.begin(), dept_members.end(),
            [me](const OrgMember* a, const OrgMember* b) {
              const bool am = a->account == me, bm = b->account == me;
              if (am != bm) return bm; // 「我」排最后
              return a->name < b->name;
            });
  const auto online_set = online_accounts();
  auto* row = new QHBoxLayout();
  row->setSpacing(10);
  int col = 0;
  for (const OrgMember* pm : dept_members) {
    const OrgMember& m = *pm;
    const bool is_me = m.account == me;
    auto* card = new QWidget(org_grid_empty_->parentWidget());
    // objectName 先于样式表落定（#org_me_card 选择器不随后改名重求值）
    card->setObjectName(is_me ? QStringLiteral("org_me_card")
                              : QStringLiteral("org_card_") + m.account);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    card->setStyleSheet(
        QStringLiteral(
            "QWidget { background:%1; border:1px solid %2; border-radius:10px; }"
            "QWidget#org_me_card { background:%3; border:1px solid %4; }")
            .arg(t.surface_raised.name(), t.border.name(),
                 t.brand_wash.name(), t.brand.name()));
    auto* cl = new QHBoxLayout(card);
    cl->setContentsMargins(10, 9, 10, 9);
    cl->setSpacing(10);
    auto* av = new QLabel(card);
    av->setFixedSize(44, 44);
    av->setAlignment(Qt::AlignCenter);
    av->setStyleSheet(
        QStringLiteral("border-radius:12px; font-size:16px; font-weight:700; "
                       "background:%1; color:%2;")
            .arg(t.brand_tint.name(), t.brand_text.name()));
    const qint64 ver = member_avatar_ver_.value(m.account);
    const QPixmap px = avatar_for(m.account, ver, 44);
    if (!px.isNull()) av->setPixmap(px);
    else av->setText(m.name.isEmpty() ? m.account.left(1) : m.name.left(1));
    cl->addWidget(av);
    auto* tl = new QVBoxLayout();
    tl->setSpacing(1);
    auto* name_l =
        new QLabel(is_me ? QStringLiteral("%1（我）")
                           .arg(m.name.isEmpty() ? m.account : m.name)
                         : (m.name.isEmpty() ? m.account : m.name),
                   card);
    name_l->setStyleSheet(QStringLiteral("font-size:13px; font-weight:700; "
                                         "color:%1;")
                              .arg(t.text.name()));
    tl->addWidget(name_l);
    // 副行：部门末级 · 职务（缺项如实省略）
    QString sub;
    if (!m.dept.isEmpty()) sub += m.dept.section(QChar('/'), -1);
    if (!m.title.isEmpty())
      sub += sub.isEmpty() ? m.title
                           : QStringLiteral(" · ") + m.title;
    if (sub.isEmpty()) sub = QStringLiteral("（未建档）");
    auto* sub_l = new QLabel(sub, card);
    sub_l->setStyleSheet(
        QStringLiteral("font-size:11px; color:%1;").arg(t.text_muted.name()));
    tl->addWidget(sub_l);
    cl->addLayout(tl, 1);
    cl->addStretch();
    // 在线点（协作在线表；离线＝灰空心）
    auto* dot = new QLabel(card);
    dot->setFixedSize(9, 9);
    dot->setStyleSheet(QStringLiteral("background:%1; border-radius:4px;")
                           .arg(online_set.contains(m.account)
                                    ? t.success.name()
                                    : t.border.name()));
    dot->setToolTip(online_set.contains(m.account) ? QStringLiteral("在线")
                                                   : QStringLiteral("离线"));
    cl->addWidget(dot, 0, Qt::AlignTop);
    card->setCursor(Qt::PointingHandCursor);
    // 点卡选档案（事件过滤：鼠标释放即选中，卡片本体无 clicked 信号）
    const QString account = m.account;
    card->installEventFilter(
        new OrgCardClick([this, account] { show_org_profile(account); },
                         card));
    // 已可见父下新建子件显式 show；两列换行
    card->show();
    row->addWidget(card, 1);
    if (++col == 2) {
      auto* roww = new QWidget(org_grid_empty_->parentWidget());
      auto* rl = new QHBoxLayout(roww);
      rl->setContentsMargins(0, 0, 0, 0);
      rl->setSpacing(10);
      // 将 row 内件挪入 roww
      while (row->count() > 0) {
        QLayoutItem* it = row->takeAt(0);
        if (QWidget* w = it->widget()) {
          w->setParent(roww);
          w->show();
          rl->addWidget(w, 1);
        }
        delete it;
      }
      roww->show();
      org_grid_lay_->addWidget(roww);
      row = new QHBoxLayout();
      row->setSpacing(10);
      col = 0;
    }
  }
  if (row->count() > 0) {
    auto* roww = new QWidget(org_grid_empty_->parentWidget());
    auto* rl = new QHBoxLayout(roww);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(10);
    while (row->count() > 0) {
      QLayoutItem* it = row->takeAt(0);
      if (QWidget* w = it->widget()) {
        w->setParent(roww);
        w->show();
        rl->addWidget(w, 1);
      }
      delete it;
    }
    roww->show();
    org_grid_lay_->addWidget(roww);
  }
  delete row;
  org_grid_lay_->addStretch();
  org_grid_lay_->activate();
}

// 右栏成员档案：头像/职责行/状态徽标/汇报链路（沿 manager 指针上溯，
// 环防御）/档案 kv/动作钮（发消息、常用联系人星标）/数据口径注脚。
void MainWindow::show_org_profile(const QString& account) {
  org_selected_member_ = account;
  if (!org_profile_ || !org_profile_lay_) return;
  const ThemeTokens& t = tk();
  // 清空重建（三招模板）
  while (org_profile_lay_->count() > 0) {
    QLayoutItem* it = org_profile_lay_->takeAt(0);
    if (QWidget* w = it ? it->widget() : nullptr) {
      w->hide();
      w->deleteLater();
    }
    delete it;
  }
  const OrgMember* m = nullptr;
  for (const OrgMember& x : org_members_)
    if (x.account == account) m = &x;
  if (!m) { // 无选中（或成员不在可见集）：档案栏收起。只藏本栏——不回调
    // apply_view_mode（其 org 分支会再 rebuild_org_view → 再进本函数，
    // 空选中＝无限递归压栈，实录 SEGFAULT 在 QLabel 布局测量段）
    if (org_profile_) org_profile_->hide();
    org_profile_lay_->activate();
    return;
  }
  const bool online = online_accounts().contains(account);
  const bool starred = fav_is_starred_(account);
  const QString disp = m->name.isEmpty() ? m->account : m->name;
  auto add_label = [&](const QString& text, const QString& css) {
    auto* l = new QLabel(text, org_profile_);
    l->setStyleSheet(css);
    l->show();
    org_profile_lay_->addWidget(l);
    return l;
  };
  // 头像（64px，radius 18）＋名＋职责行
  auto* av = new QLabel(org_profile_);
  av->setFixedSize(64, 64);
  av->setAlignment(Qt::AlignCenter);
  av->setStyleSheet(
      QStringLiteral("background:%1; color:%2; border-radius:18px; "
                     "font-size:24px; font-weight:800;")
          .arg(t.brand_tint.name(), t.brand_text.name()));
  const QPixmap px =
      avatar_for(m->account, member_avatar_ver_.value(m->account), 64);
  if (!px.isNull()) av->setPixmap(px);
  else av->setText(avatar_initial(disp));
  av->show();
  org_profile_lay_->addWidget(av, 0, Qt::AlignLeft);
  add_label(disp, QStringLiteral("font-size:16px; font-weight:800; color:%1;")
                       .arg(t.text.name()));
  {
    QString duties = m->dept;
    if (!m->title.isEmpty())
      duties += duties.isEmpty() ? m->title
                                 : QStringLiteral(" · ") + m->title;
    if (duties.isEmpty()) duties = QStringLiteral("（未建档）");
    add_label(duties, QStringLiteral("font-size:11.5px; color:%1;")
                          .arg(t.text_muted.name()));
  }
  // 状态徽标行：在线/离线＋星标（真实状态）
  {
    auto* bl = new QHBoxLayout();
    bl->setSpacing(6);
    auto* badge = new QLabel(
        online ? QStringLiteral("● 在线") : QStringLiteral("● 离线"),
        org_profile_);
    badge->setStyleSheet(
        QStringLiteral("color:%1; font-size:11px; font-weight:700;")
            .arg(online ? t.success.name() : t.text_muted.name()));
    badge->show();
    bl->addWidget(badge);
    if (starred) {
      auto* sp = new QLabel(QStringLiteral("常用联系人"), org_profile_);
      sp->setStyleSheet(
          QStringLiteral("color:%1; border:1px solid %2; border-radius:999px;"
                         " padding:1px 8px; font-size:10.5px;")
              .arg(t.brand_text.name(), t.brand_tint.name()));
      sp->show();
      bl->addWidget(sp);
    }
    bl->addStretch();
    auto* bw = new QWidget(org_profile_);
    auto* bwl = new QHBoxLayout(bw);
    bwl->setContentsMargins(0, 0, 0, 0);
    while (bl->count() > 0) {
      QLayoutItem* it = bl->takeAt(0);
      if (QWidget* w = it->widget()) {
        w->setParent(bw);
        w->show();
        bwl->addWidget(w);
      }
      delete it;
    }
    delete bl;
    bw->show();
    org_profile_lay_->addWidget(bw);
  }
  // 汇报链路（沿 manager 上溯；环防御；缺失如实止步）
  {
    auto* head = add_label(
        QStringLiteral("汇报链路"),
        QStringLiteral("font-size:12px; font-weight:700; color:%1;")
            .arg(t.text_muted.name()));
    head->setContentsMargins(0, 8, 0, 0);
    QStringList chain; // account 逐级（本人在前，上级依次在后）
    QSet<QString> seen;
    QString cur = m->account;
    for (int hop = 0; hop < 6 && !cur.isEmpty() && !seen.contains(cur);
         ++hop) {
      seen.insert(cur);
      chain << cur;
      const OrgMember* x = nullptr;
      for (const OrgMember& y : org_members_)
        if (y.account == cur) x = &y;
      cur = x ? x->manager : QString();
    }
    for (int i = 0; i < chain.size(); ++i) {
      const OrgMember* x = nullptr;
      for (const OrgMember& y : org_members_)
        if (y.account == chain[i]) x = &y;
      const QString cname = x ? (x->name.isEmpty() ? x->account : x->name)
                              : chain[i];
      auto* ci = new QWidget(org_profile_);
      auto* cl = new QHBoxLayout(ci);
      cl->setContentsMargins(0, 0, 0, 0);
      cl->setSpacing(8);
      auto* cav = new QLabel(avatar_initial(cname), ci);
      cav->setFixedSize(26, 26);
      cav->setAlignment(Qt::AlignCenter);
      // 链路头像用令牌色（avatar_hue 散色进样式表会撞「样式全令牌」门禁）
      cav->setStyleSheet(
          QStringLiteral("background:%1; color:%2; border-radius:8px; "
                         "font-size:12px; font-weight:700;")
              .arg(t.brand_tint.name(), t.brand_text.name()));
      cav->show();
      cl->addWidget(cav);
      auto* cinfo = new QVBoxLayout();
      cinfo->setSpacing(0);
      auto* cn = new QLabel(cname, ci);
      cn->setStyleSheet(QStringLiteral("font-size:12.5px; font-weight:700; "
                                       "color:%1;")
                            .arg(t.text.name()));
      cn->show();
      cinfo->addWidget(cn);
      QString crole;
      if (x) {
        crole = x->dept.section(QChar('/'), -1);
        if (!x->title.isEmpty())
          crole += crole.isEmpty() ? x->title
                                   : QStringLiteral(" · ") + x->title;
      }
      if (i > 0 && crole.isEmpty()) crole = QStringLiteral("直属上级");
      if (!crole.isEmpty()) {
        auto* cr = new QLabel(crole, ci);
        cr->setStyleSheet(
            QStringLiteral("font-size:10.5px; color:%1;")
                .arg(t.text_muted.name()));
        cr->show();
        cinfo->addWidget(cr);
      }
      cl->addLayout(cinfo, 1);
      ci->show();
      org_profile_lay_->addWidget(ci);
      if (i + 1 < chain.size()) {
        auto* ln = new QLabel(QStringLiteral("│"), org_profile_);
        ln->setStyleSheet(QStringLiteral("color:%1; font-size:11px;")
                              .arg(t.border.name()));
        ln->setContentsMargins(0, 0, 0, 0);
        ln->show();
        org_profile_lay_->addWidget(ln, 0, Qt::AlignLeft);
      }
    }
  }
  // 档案 kv（真实字段：直属上级/部门/职务/在线时长；办公位置/手机协议
  // 未开——如实不摆，不造假行）
  {
    const auto kv = [&](const QString& k, QString v, bool brand = false) {
      auto* row = new QWidget(org_profile_);
      auto* rl = new QHBoxLayout(row);
      rl->setContentsMargins(0, 0, 0, 0);
      auto* kl = new QLabel(k, row);
      kl->setStyleSheet(QStringLiteral("font-size:11.5px; color:%1;")
                            .arg(t.text_muted.name()));
      kl->show();
      rl->addWidget(kl);
      rl->addStretch();
      // 缓存串自带「在线时长：」前缀（own_online 同源）——键名已给出，
      // 值里剥掉前缀免得键值重复；长值换行不截断
      if (v.startsWith(QStringLiteral("在线时长：")))
        v = v.mid(QStringLiteral("在线时长：").size());
      auto* vl = new QLabel(v, row);
      vl->setWordWrap(true);
      vl->setStyleSheet(QStringLiteral("font-size:11.5px; font-weight:600; "
                                       "color:%1;")
                            .arg(brand ? t.brand_text.name() : t.text.name()));
      vl->show();
      rl->addWidget(vl);
      row->show();
      org_profile_lay_->addWidget(row);
    };
    const auto mgr_of = [&](const QString& a) -> const OrgMember* {
      for (const OrgMember& y : org_members_)
        if (y.account == a) return &y;
      return nullptr;
    };
    if (!m->manager.isEmpty()) {
      const OrgMember* mgr = mgr_of(m->manager);
      kv(QStringLiteral("直属上级"),
         mgr ? (mgr->name.isEmpty() ? mgr->account : mgr->name) : m->manager,
         true);
    }
    if (!m->dept.isEmpty()) kv(QStringLiteral("部门"), m->dept);
    if (!m->title.isEmpty()) kv(QStringLiteral("职务"), m->title);
    const QString ol = member_online_.value(m->account);
    if (!ol.isEmpty()) kv(QStringLiteral("在线时长"), ol);
  }
  // 动作钮：发消息（brand）＋常用联系人星标（描边）
  {
    auto* acts = new QHBoxLayout();
    acts->setSpacing(8);
    auto* msg = new QPushButton(QStringLiteral("发消息"), org_profile_);
    msg->setCursor(Qt::PointingHandCursor);
    msg->setStyleSheet(
        QStringLiteral("QPushButton { background:%1; color:%2; border:none; "
                       "border-radius:8px; padding:7px 16px; "
                       "font-size:12.5px; font-weight:700; }"
                       "QPushButton:hover { background:%3; }")
            .arg(t.brand.name(), t.on_brand.name(), t.brand_text.name()));
    const QString acct = m->account;
    connect(msg, &QPushButton::clicked, this, [this, acct] {
      leave_org_view();
      open_collab_peer(acct);
    });
    msg->show();
    acts->addWidget(msg, 1);
    auto* fav = new QPushButton(starred ? QStringLiteral("取消常用联系人")
                                        : QStringLiteral("设为常用联系人"),
                                org_profile_);
    fav->setCursor(Qt::PointingHandCursor);
    fav->setStyleSheet(
        QStringLiteral("QPushButton { background:transparent; color:%1; "
                       "border:1px solid %2; border-radius:8px; "
                       "padding:7px 12px; font-size:12px; }"
                       "QPushButton:hover { background:%3; }")
            .arg(t.brand_text.name(), t.brand_tint.name(),
                 t.brand_wash.name()));
    connect(fav, &QPushButton::clicked, this, [this, acct, starred] {
      collab_engine_.fav_cmd(starred ? QStringLiteral("unstar")
                                     : QStringLiteral("star"),
                             acct);
    });
    fav->show();
    acts->addWidget(fav, 1);
    auto* aw = new QWidget(org_profile_);
    auto* awl = new QHBoxLayout(aw);
    awl->setContentsMargins(0, 4, 0, 0);
    while (acts->count() > 0) {
      QLayoutItem* it = acts->takeAt(0);
      if (QWidget* w = it->widget()) {
        w->setParent(aw);
        w->show();
        awl->addWidget(w, 1);
      }
      delete it;
    }
    delete acts;
    aw->show();
    org_profile_lay_->addWidget(aw);
  }
  // 数据口径注脚（直属上级为权威表字段——服务端真实模型）。注脚是长句，
  // 面板定宽下必须换行（QLabel 默认不换行＝右侧截断，实录截尾）
  if (auto* fn = add_label(
          QStringLiteral("直属上级为独立数据字段，由管理后台维护"
                         "（可批量导入），不从部门归属推导。"),
          QStringLiteral("font-size:10.5px; color:%1; line-height:1.6;")
              .arg(t.text_muted.name())))
    fn->setWordWrap(true);
  org_profile_lay_->addStretch();
  org_profile_lay_->activate();
  // 档案栏显隐就地收口（不回调 apply_view_mode——其 org 分支会再
  // rebuild_org_view → 再进本函数，选中有效成员同样成环）
  if (sidebar_mode_ == QStringLiteral("org")) org_profile_->show();
}

} // namespace memex::client
