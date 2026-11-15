// task_dialog 拆分模块（LAY-012）：R27-6 看板＋R27-7 领导视角＋refresh——方法体自原文件逐字搬移；公共面见 common.hpp，声明在 task_dialog.hpp。
#include "common.hpp"

namespace memex::client {
// —— R27-6 看板（与列表同源同批数据，切换即重渲染）——

void TaskDialog::toggle_kanban() {
  const bool show = !kanban_->isVisible();
  kanban_->setVisible(show);
  list_->setVisible(!show);
  if (list_head_ != nullptr) list_head_->setVisible(!show);
  if (kanban_head_ != nullptr) kanban_head_->setVisible(show);
  btn_kanban_->setText(show ? QStringLiteral("列表（切看板）")
                            : QStringLiteral("看板（切列表）"));
  if (show) render_kanban();
}

void TaskDialog::render_kanban() {
  // 卡片=标题 ·类型＋开始/截止；逾期红、临近黄（阈值 QSettings
  // task_due_warn_days 缺省 3 天），完成灰
  const QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  const qint64 warn =
      settings.value(QStringLiteral("task_due_warn_days"), 3).toInt() *
      86400000LL;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  kanban_todo_->clear();
  kanban_doing_->clear();
  kanban_done_->clear();
  for (const auto& v : mine_cache_) {
    const auto t = v.toObject();
    const bool done = t.value(QStringLiteral("done")).toBool();
    const QString status = t.value(QStringLiteral("status")).toString();
    const qint64 start = static_cast<qint64>(
        t.value(QStringLiteral("start_ms")).toDouble());
    const qint64 due =
        static_cast<qint64>(t.value(QStringLiteral("due_ms")).toDouble());
    QString extra;
    if (start > 0) {
      extra += QStringLiteral("  开始 %1")
                   .arg(QDateTime::fromMSecsSinceEpoch(start)
                            .toString(QStringLiteral("MM-dd HH:mm")));
    }
    if (due > 0) {
      extra += QStringLiteral("  截止 %1")
                   .arg(QDateTime::fromMSecsSinceEpoch(due)
                            .toString(QStringLiteral("MM-dd HH:mm")));
    }
    auto* card = new QListWidgetItem(
        QStringLiteral("%1 ·%2%3")
            .arg(t.value(QStringLiteral("title")).toString(),
                 t.value(QStringLiteral("type")).toString(), extra));
    card->setData(Qt::UserRole,
                  t.value(QStringLiteral("id")).toDouble());
    QListWidget* col = kanban_todo_;
    if (done || status == QStringLiteral("完成")) {
      col = kanban_done_;
      card->setForeground(Qt::gray);
    } else if (status == QStringLiteral("进行中")) {
      col = kanban_doing_;
    }
    if (!done && due > 0 && due <= now) {
      card->setForeground(Qt::red); // 逾期
    } else if (!done && due > 0 && due - now <= warn) {
      card->setForeground(QColor(QStringLiteral("#b8860b"))); // 临近暗金黄
    }
    col->addItem(card);
  }
}

int TaskDialog::kanban_count(int col) const {
  const QListWidget* c = col == 0 ? kanban_todo_
                         : col == 1 ? kanban_doing_
                                    : kanban_done_;
  return c != nullptr ? c->count() : -1;
}

QStringList TaskDialog::task_type_names() const {
  QStringList out;
  if (type_ != nullptr) {
    for (int i = 0; i < type_->count(); ++i) out << type_->itemText(i);
  }
  return out;
}

// —— R27-7 领导视角（只读泳道；动作仍归下属本人清单）——

int TaskDialog::lead_lane_count() const {
  return lead_scroll_->isVisible() ? team_cache_.size() : 0;
}

void TaskDialog::render_lead() {
  // 泳道=每直属下属一条（服务端已按直属关系与 az 裁决过滤）；标头带
  // 负荷统计三数，卡片带类型与开始/截止，逾期红/临近黄/完成灰
  const bool show = cb_lead_->isChecked() && !team_cache_.isEmpty();
  lead_scroll_->setVisible(show);
  if (lead_head_ != nullptr) lead_head_->setVisible(show);
  if (!show) return;
  const QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  const qint64 warn =
      settings.value(QStringLiteral("task_due_warn_days"), 3).toInt() *
      86400000LL;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  auto* lanes = new QWidget;
  auto* lay = new QHBoxLayout(lanes);
  lay->setContentsMargins(0, 0, 0, 0);
  for (const auto& v : team_cache_) {
    const auto lane = v.toObject();
    const auto loads = lane.value(QStringLiteral("loads")).toObject();
    const int open = loads.value(QStringLiteral("open")).toInt();
    const int overdue = loads.value(QStringLiteral("overdue")).toInt();
    const int due_soon = loads.value(QStringLiteral("due_soon")).toInt();
    auto* box = new QGroupBox(
        QStringLiteral("%1（未完成 %2 · 逾期 %3 · 7 天内到期 %4）")
            .arg(lane.value(QStringLiteral("account")).toString(),
                 QString::number(open), QString::number(overdue),
                 QString::number(due_soon)),
        lanes);
    auto* vlay = new QVBoxLayout(box);
    auto* cards = new QListWidget(box);
    for (const auto& tv : lane.value(QStringLiteral("tasks")).toArray()) {
      const auto t = tv.toObject();
      const bool done = t.value(QStringLiteral("done")).toBool() ||
                        t.value(QStringLiteral("status")).toString() ==
                            QStringLiteral("完成");
      const qint64 start = static_cast<qint64>(
          t.value(QStringLiteral("start_ms")).toDouble());
      const qint64 due =
          static_cast<qint64>(t.value(QStringLiteral("due_ms")).toDouble());
      QString extra;
      if (start > 0) {
        extra += QStringLiteral("  开始 %1")
                     .arg(QDateTime::fromMSecsSinceEpoch(start)
                              .toString(QStringLiteral("MM-dd HH:mm")));
      }
      if (due > 0) {
        extra += QStringLiteral("  截止 %1")
                     .arg(QDateTime::fromMSecsSinceEpoch(due)
                              .toString(QStringLiteral("MM-dd HH:mm")));
      }
      auto* card = new QListWidgetItem(
          QStringLiteral("%1 ·%2%3%4")
              .arg(t.value(QStringLiteral("title")).toString(),
                   t.value(QStringLiteral("type")).toString(),
                   t.value(QStringLiteral("status")).toString() ==
                           QStringLiteral("进行中")
                       ? QStringLiteral("（进行中）")
                       : QString(),
                   extra));
      if (done) {
        card->setForeground(Qt::gray);
      } else if (due > 0 && due <= now) {
        card->setForeground(Qt::red); // 逾期
      } else if (due > 0 && due - now <= warn) {
        card->setForeground(QColor(QStringLiteral("#b8860b"))); // 临近
      }
      cards->addItem(card);
    }
    vlay->addWidget(cards);
    lay->addWidget(box);
  }
  lay->addStretch(1);
  lead_scroll_->setWidget(lanes); // 旧泳道容器由 QScrollArea 回收
}

bool TaskDialog::delete_selected() {
  if (selected_row_pulled()) {
    set_status(QStringLiteral("外部拉取行只读——外部清单不由 memex 撤回"),
               true);
    return false;
  }
  const qint64 id = selected_id();
  if (id < 0) {
    set_status(QStringLiteral("先选中一条任务"), true);
    return false;
  }
  client_->delete_task(id); // 确认框归按钮路径（程序化入口不弹框供测试）
  return true;
}

void TaskDialog::refresh() {
  if (is_connected()) client_->list_tasks();
}

} // namespace memex::client
