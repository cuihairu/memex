// task_dialog 拆分模块（LAY-012）：R27-1 日历视图与录入接线——方法体自原文件逐字搬移；公共面见 common.hpp，声明在 task_dialog.hpp。
#include "common.hpp"

namespace memex::client {
// —— R27-1 余量 日历视图 ——

void TaskDialog::update_day_marks() {
  // 无参重载清全表再标——刷新重建后重入不叠色
  calendar_->setDateTextFormat(QDate(), QTextCharFormat());
  QTextCharFormat fmt;
  fmt.setFontWeight(QFont::Bold);
  // 品牌橙（与主窗 accent 兜底同源；本窗未接主题面，同 Qt::gray 行内惯例）
  fmt.setForeground(QColor(QStringLiteral("#e16531")));
  for (int i = 0; i < list_->count(); ++i) {
    const auto* it = list_->item(i);
    const qint64 due = it->data(Qt::UserRole + 2).toLongLong();
    if (due <= 0) continue; // 派出行/⇣ 行/未设提醒项无到期数据
    calendar_->setDateTextFormat(QDateTime::fromMSecsSinceEpoch(due).date(),
                                 fmt);
  }
}

int TaskDialog::apply_day_filter() {
  int visible = 0;
  const bool only_open = cb_only_open_ != nullptr && cb_only_open_->isChecked();
  for (int i = 0; i < list_->count(); ++i) {
    auto* it = list_->item(i);
    const qint64 due = it->data(Qt::UserRole + 2).toLongLong();
    const bool hit =
        day_filter_.isValid() && due > 0 &&
        QDateTime::fromMSecsSinceEpoch(due).date() == day_filter_;
    // 筛选态只留命中日；无到期数据行（派出/⇣/未设提醒）一并隐藏。
    // 只看未完成：done 行（UserRole+1 或 status=完成）隐藏，与日历叠加
    const bool done = it->data(Qt::UserRole + 1).toBool() ||
                      it->data(Qt::UserRole + 10).toString() ==
                          QStringLiteral("完成");
    it->setHidden((day_filter_.isValid() && !hit) || (only_open && done));
    if (!it->isHidden()) ++visible;
  }
  return visible;
}

void TaskDialog::toggle_day_filter(const QDate& d) {
  day_filter_ = d.isValid() && d == day_filter_ ? QDate() : d;
  const int visible = apply_day_filter();
  if (day_filter_.isValid()) {
    set_status(QStringLiteral("日历筛选：%1（可见 %2 项，再点同日回全量）")
                   .arg(day_filter_.toString(QStringLiteral("yyyy-MM-dd")),
                        QString::number(visible)));
  } else {
    set_status(QStringLiteral("日历筛选已清（全量 %1 项）")
                   .arg(visible));
  }
}

int TaskDialog::visible_task_count() const {
  int n = 0;
  for (int i = 0; i < list_->count(); ++i) {
    if (!list_->item(i)->isHidden()) ++n;
  }
  return n;
}

void TaskDialog::refresh_ext_combo() {
  const QString prev = ext_provider_->currentData().toString();
  ext_provider_->clear();
  for (const auto* p : providers_.providers()) {
    ext_provider_->addItem(p->name(), p->id());
  }
  const int url_idx = ext_provider_->findData(QStringLiteral("url"));
  if (url_idx >= 0) ext_provider_->setCurrentIndex(url_idx); // 粘贴链接即登记
  const int prev_idx = ext_provider_->findData(prev);
  if (prev_idx >= 0) ext_provider_->setCurrentIndex(prev_idx);
}

void TaskDialog::connect_to(const QString& host, quint16 files_port,
                            const QString& acc, const QString& pass) {
  if (host.isEmpty() || acc.isEmpty()) {
    set_status(QStringLiteral("服务器地址与账号不能为空"), true);
    return;
  }
  if (files_port == 0) {
    set_status(QStringLiteral("文件面端口非法"), true);
    return;
  }
  host_->setText(host);
  port_->setText(QString::number(files_port));
  account_box_->setText(acc);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  settings.setValue(QStringLiteral("files_port"), QString::number(files_port));
  btn_connect_->setEnabled(false);
  account_ = acc;
  client_->login(host, files_port, acc, pass);
}

bool TaskDialog::is_connected() const { return client_->is_logged_in(); }

void TaskDialog::set_assignees(const QStringList& accounts) {
  while (assignee_->count() > 1) assignee_->removeItem(assignee_->count() - 1);
  for (const QString& a : accounts) {
    if (!a.isEmpty() && a != account_) assignee_->addItem(a);
  }
}

bool TaskDialog::add_task(const QString& title, const QString& note,
                          qint64 due_ms, const QString& assignee,
                          const QString& type, qint64 start_ms) {
  if (title.isEmpty()) {
    set_status(QStringLiteral("标题不能为空"), true);
    return false;
  }
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->create_task(title, note, due_ms, assignee, QString(), QString(),
                       type, start_ms);
  return true;
}

void TaskDialog::split_ext_key(const QString& raw, QString& project,
                               QString& key) {
  const qint64 at = raw.lastIndexOf(QLatin1Char('#'));
  if (at > 0) {
    project = raw.left(at);
    key = raw.mid(at + 1);
  } else {
    key = raw; // 无 # = 整串为键（直通链接等）
  }
}

bool TaskDialog::add_external_task(const QString& provider_id,
                                   const QString& key_input,
                                   const QString& title) {
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  const QString raw = key_input.trimmed();
  if (raw.isEmpty()) {
    set_status(QStringLiteral("外部键不能为空"), true);
    return false;
  }
  // detailUrl 必带：解析不出 URL 本地拒不发网不落库（宁缺毋滥造坏链）
  QString project, key;
  split_ext_key(raw, project, key);
  if (providers_.detail_url(provider_id, key, project).isEmpty()) {
    set_status(QStringLiteral("解析不出详情链接（provider 未登记或键不合法）"),
               true);
    return false;
  }
  client_->create_task(title.trimmed().isEmpty() ? raw : title.trimmed(),
                       QString(), 0, QString(), provider_id, raw);
  return true;
}

QString TaskDialog::selected_detail_url() const {
  const auto* item = list_->currentItem();
  return item ? item->data(Qt::UserRole + 5).toString() : QString();
}

int TaskDialog::ext_combo_count() const { return ext_provider_->count(); }

bool TaskDialog::toggle_selected_done() {
  if (selected_row_pulled()) {
    // ⇣ 行是外部现态只读镜像：回写是 L3（complete），清单标记须先登记
    set_status(QStringLiteral("外部拉取行只读——登记后才能在清单标记完成"),
               true);
    return false;
  }
  const qint64 id = selected_id();
  if (id < 0) {
    set_status(QStringLiteral("先选中一条任务"), true);
    return false;
  }
  // 现值取反：列表行带 done 态（数据角色）
  const auto* item = list_->currentItem();
  const bool cur = item->data(Qt::UserRole + 1).toBool();
  client_->set_task_done(id, !cur);
  return true;
}

// R27-6 状态切换：待办/进行中/完成（服务端判权 owner-only 并双写 done，
// 读取以 status 为准——客户端只提交与展示）
bool TaskDialog::set_selected_status(const QString& status) {
  if (selected_row_pulled()) {
    set_status(QStringLiteral("外部拉取行只读"), true);
    return false;
  }
  const qint64 id = selected_id();
  if (id < 0) {
    set_status(QStringLiteral("先选中一条任务"), true);
    return false;
  }
  client_->set_task_status(id, status);
  return true;
}
} // namespace memex::client
