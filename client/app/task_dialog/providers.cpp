// task_dialog 拆分模块（LAY-012）：R27-3 拉取接线：store→live provider→外部列表＋到期待办——方法体自原文件逐字搬移；公共面见 common.hpp，声明在 task_dialog.hpp。
#include "common.hpp"

namespace memex::client {
// —— R27-3 拉取接线：凭据 store → live provider → 直拉外部列表 ——
void TaskDialog::rebuild_live_providers() {
  qDeleteAll(live_);
  live_.clear();
  if (store_ != nullptr && store_->is_unlocked()) {
    if (store_->contains(QStringLiteral("github-issue"))) {
      const QJsonObject c = store_->config(QStringLiteral("github-issue"));
      live_.insert(QStringLiteral("github-issue"),
                   new GitHubIssuesProvider(
                       c.value(QStringLiteral("repo")).toString(),
                       c.value(QStringLiteral("token")).toString(), http_));
    }
    if (store_->contains(QStringLiteral("dingtalk-todo"))) {
      const QJsonObject c = store_->config(QStringLiteral("dingtalk-todo"));
      live_.insert(QStringLiteral("dingtalk-todo"),
                   new DingtalkTodoProvider(
                       c.value(QStringLiteral("app_key")).toString(),
                       c.value(QStringLiteral("app_secret")).toString(),
                       // unionId 获取流程属设置页后续件；空按未取到用户走
                       c.value(QStringLiteral("union_id")).toString(),
                       http_));
    }
    if (store_->contains(QStringLiteral("feishu-task"))) {
      const QJsonObject c = store_->config(QStringLiteral("feishu-task"));
      live_.insert(QStringLiteral("feishu-task"),
                   new FeishuTaskProvider(
                       c.value(QStringLiteral("app_id")).toString(),
                       c.value(QStringLiteral("app_secret")).toString(),
                       http_));
    }
  }
  // 拉取下拉只收已配置且声明 L2 的（重新装配保持选择尽量不跳）
  const QString prev = pull_provider_->currentData().toString();
  pull_provider_->clear();
  QStringList ids;
  for (auto it = live_.constBegin(); it != live_.constEnd(); ++it) {
    if (it.value()->can_read()) ids << it.key();
  }
  ids.sort();
  for (const QString& id : ids) {
    pull_provider_->addItem(live_.value(id)->name(), id);
  }
  const int idx = pull_provider_->findData(prev);
  if (idx >= 0) pull_provider_->setCurrentIndex(idx);
  btn_pull_->setEnabled(!ids.isEmpty());
}

QStringList TaskDialog::pull_provider_ids() const {
  QStringList out;
  for (int i = 0; i < pull_provider_->count(); ++i) {
    out << pull_provider_->itemData(i).toString();
  }
  return out;
}

void TaskDialog::pull_external(const QString& provider_id) {
  auto* p = live_.value(provider_id, nullptr);
  if (p == nullptr || !p->can_read()) {
    set_status(QStringLiteral("该 provider 未配置或未声明只读能力"
                              "（任务设置里配置凭据）"),
               true);
    return;
  }
  set_status(QStringLiteral("拉取外部任务中（%1）…").arg(p->name()));
  p->list([this, provider_id](bool ok, const QVector<ExternalTask>& items,
                              const QString& err) {
    if (ok) {
      ext_cache_.insert(provider_id, items);
      render_ext_rows();
      set_status(QStringLiteral("外部任务已拉取（%1 %2 项）")
                     .arg(live_.value(provider_id) != nullptr
                              ? live_.value(provider_id)->name()
                              : provider_id,
                          QString::number(items.size())));
    } else {
      set_status(
          QStringLiteral("拉取失败（%1：%2）").arg(provider_id, err), true);
    }
  });
}

void TaskDialog::render_ext_rows() {
  // 旧 ⇣ 行移除（UserRole+6 拉取行标记）后整批重挂
  for (int i = list_->count() - 1; i >= 0; --i) {
    if (list_->item(i)->data(Qt::UserRole + 6).toBool()) {
      delete list_->takeItem(i);
    }
  }
  for (auto it = ext_cache_.constBegin(); it != ext_cache_.constEnd(); ++it) {
    const auto* lp = live_.value(it.key());
    const auto* rp = providers_.provider(it.key());
    const QString pname = lp != nullptr ? lp->name()
                          : rp != nullptr ? rp->name()
                                          : it.key();
    for (const ExternalTask& t : it.value()) {
      auto* item = new QListWidgetItem(QString(), list_);
      item->setData(Qt::UserRole, -1); // 无服务端 id（动作被只读守卫拦）
      item->setData(Qt::UserRole + 1, t.done);
      item->setData(Qt::UserRole + 5, t.detail_url); // 双击跳外部详情
      item->setData(Qt::UserRole + 6, true);
      item->setText(QStringLiteral("%1⇣ %2·%3  %4")
                        .arg(t.done ? QStringLiteral("[x] ")
                                    : QStringLiteral("[ ] "),
                             pname, t.key, t.title));
      if (t.done) item->setForeground(Qt::gray);
    }
  }
}

bool TaskDialog::selected_row_pulled() const {
  const auto* item = list_->currentItem();
  return item != nullptr && item->data(Qt::UserRole + 6).toBool();
}

void TaskDialog::check_due() {
  if (!is_connected()) return;
  // R27-6 两种提醒各给开关（默认开，QSettings 持久），同一处管理
  const QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  const bool start_on =
      settings.value(QStringLiteral("task_remind_start"), true).toBool();
  const bool due_on =
      settings.value(QStringLiteral("task_remind_due"), true).toBool();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  for (int i = 0; i < list_->count(); ++i) {
    const auto* it = list_->item(i);
    if (it->data(Qt::UserRole + 6).toBool()) continue; // ⇣ 只读镜像不提醒
    if (it->data(Qt::UserRole + 1).toBool()) continue; // 已完成不提醒
    const qint64 id = it->data(Qt::UserRole).toLongLong();
    // deadline 到期提醒（回执只提醒一次）
    const qint64 due = it->data(Qt::UserRole + 2).toLongLong();
    const qint64 reminded = it->data(Qt::UserRole + 3).toLongLong();
    if (due_on && due > 0 && due <= now && reminded == 0) {
      NotificationCenter::instance().on_notice(
          QStringLiteral("任务提醒"), it->data(Qt::UserRole + 4).toString(),
          QStringLiteral("任务已到期待办"), /*IMPORTANT*/ 2, QString(), now,
          QStringLiteral("task-%1-%2").arg(id).arg(now));
      client_->mark_task_reminded(id);
    }
    // 开始到点提醒（R27-6；start_reminded_ms 回执独立，只提醒一次）
    const qint64 start = it->data(Qt::UserRole + 7).toLongLong();
    const qint64 srem = it->data(Qt::UserRole + 8).toLongLong();
    if (start_on && start > 0 && start <= now && srem == 0) {
      NotificationCenter::instance().on_notice(
          QStringLiteral("任务提醒"), it->data(Qt::UserRole + 4).toString(),
          QStringLiteral("任务已到开始时间"), /*IMPORTANT*/ 2, QString(), now,
          QStringLiteral("task-start-%1-%2").arg(id).arg(now));
      client_->mark_task_reminded(id, QStringLiteral("start"));
    }
  }
}

qint64 TaskDialog::selected_id() const {
  const auto* item = list_->currentItem();
  return item ? item->data(Qt::UserRole).toLongLong() : -1;
}
} // namespace memex::client
