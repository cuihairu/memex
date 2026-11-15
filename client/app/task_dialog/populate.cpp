// task_dialog 拆分模块（LAY-012）：populate/状态行/计数——方法体自原文件逐字搬移；公共面见 common.hpp，声明在 task_dialog.hpp。
#include "common.hpp"

namespace memex::client {
void TaskDialog::populate(const QJsonArray& mine, const QJsonArray& assigned) {
  mine_cache_ = mine;   // 看板同源数据（toggle_kanban 重渲染用）
  assigned_cache_ = assigned;
  list_->clear();
  for (const auto& v : mine) {
    const auto t = v.toObject();
    const QString prov = t.value(QStringLiteral("provider")).toString();
    const bool done = t.value(QStringLiteral("done")).toBool();
    auto* item = new QListWidgetItem(QString(), list_);
    item->setData(Qt::UserRole, t.value(QStringLiteral("id")).toDouble());
    item->setData(Qt::UserRole + 1, done);
    item->setData(Qt::UserRole + 2,
                  static_cast<qint64>(
                      t.value(QStringLiteral("due_ms")).toDouble()));
    item->setData(Qt::UserRole + 3,
                  static_cast<qint64>(
                      t.value(QStringLiteral("reminded_ms")).toDouble()));
    item->setData(Qt::UserRole + 4,
                  t.value(QStringLiteral("title")).toString());
    // R27-6 开始/回执（start）/类型/状态（roles +7..+10）
    item->setData(Qt::UserRole + 7,
                  static_cast<qint64>(
                      t.value(QStringLiteral("start_ms")).toDouble()));
    item->setData(Qt::UserRole + 8,
                  static_cast<qint64>(
                      t.value(QStringLiteral("start_reminded_ms"))
                          .toDouble()));
    item->setData(Qt::UserRole + 9,
                  t.value(QStringLiteral("type")).toString());
    item->setData(Qt::UserRole + 10,
                  t.value(QStringLiteral("status")).toString());
    if (!prov.isEmpty()) {
      // 外部任务（R27-2）：🌐 标注 provider·键原文，现解析 detail URL
      //（provider 未登记/键不合法=无跳转，行仍在、双击给状态提示）
      const QString raw = t.value(QStringLiteral("ext_key")).toString();
      QString project, key;
      split_ext_key(raw, project, key);
      item->setData(Qt::UserRole + 5,
                    providers_.detail_url(prov, key, project));
      const auto* p = providers_.provider(prov);
      item->setText(QStringLiteral("%1🌐 %2  %3·%4")
                        .arg(done ? QStringLiteral("[x] ")
                                  : QStringLiteral("[ ] "),
                             t.value(QStringLiteral("title")).toString(),
                             p != nullptr ? p->name() : prov, raw));
    } else {
      const QString creator =
          t.value(QStringLiteral("creator")).toString();
      const bool self = creator == account_;
      const qint64 due = item->data(Qt::UserRole + 2).toLongLong();
      QString when;
      if (due > 0) {
        when = QStringLiteral("  提醒 %1")
                   .arg(QDateTime::fromMSecsSinceEpoch(due)
                            .toString(QStringLiteral("MM-dd HH:mm")));
      }
      const qint64 start = item->data(Qt::UserRole + 7).toLongLong();
      if (start > 0) {
        when += QStringLiteral("  开始 %1")
                    .arg(QDateTime::fromMSecsSinceEpoch(start)
                             .toString(QStringLiteral("MM-dd HH:mm")));
      }
      const QString status = item->data(Qt::UserRole + 10).toString();
      item->setText(QStringLiteral("%1%2 ·%3  %4%5%6")
                        .arg(done ? QStringLiteral("[x] ")
                                  : QStringLiteral("[ ] "),
                             t.value(QStringLiteral("title")).toString(),
                             item->data(Qt::UserRole + 9).toString(),
                             self ? QStringLiteral("自建")
                                  : QStringLiteral("由 %1 分配")
                                        .arg(creator),
                             status == QStringLiteral("进行中")
                                 ? QStringLiteral("（进行中）")
                                 : QString(),
                             when));
    }
    if (done) {
      item->setForeground(Qt::gray);
    }
  }
  // 我派出的折叠进列表尾（只读视角：完成与否看得到，动作在对方清单）
  for (const auto& v : assigned) {
    const auto t = v.toObject();
    auto* item = new QListWidgetItem(
        QStringLiteral("[→] %1  派给 %2%3")
            .arg(t.value(QStringLiteral("title")).toString(),
                 t.value(QStringLiteral("owner")).toString(),
                 t.value(QStringLiteral("done")).toBool()
                     ? QStringLiteral("（已完成）")
                     : QString()),
        list_);
    item->setData(Qt::UserRole, t.value(QStringLiteral("id")).toDouble());
    item->setData(Qt::UserRole + 1,
                  t.value(QStringLiteral("done")).toBool());
    // 派出行不可在本人侧勾完成/提醒（服务端也会拒——owner 才是动作主体）
    item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
  }
  // 外部拉取会话缓存行（⇣ 只读镜像）缀尾，随每次重渲染对齐外部现态
  render_ext_rows();
  // 日历视图：重标到期日＋重施筛选（30s 轮询刷新重建列表，筛选态保持）
  update_day_marks();
  apply_day_filter();
  // 任务清单 sec-head 计数（我的/派给我/外部拉取镜像）
  int ext_n = 0;
  for (auto it = ext_cache_.constBegin(); it != ext_cache_.constEnd(); ++it) {
    ext_n += it.value().size();
  }
  if (counts_ != nullptr) {
    counts_->setText(QStringLiteral("我的 %1 · 派给我 %2 · 外部 %3")
                         .arg(static_cast<int>(mine.size()))
                         .arg(static_cast<int>(assigned.size()))
                         .arg(ext_n));
  }
  // R27-6 看板同批数据重渲染（隐藏时也保持最新，切换即现）＋状态按钮复位
  render_kanban();
  btn_status_->setText(QStringLiteral("开始 / 暂停"));
  set_status(QStringLiteral("清单已刷新（%1 项）").arg(mine.size()));
}

void TaskDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  // 错误红色：QSS 不好控主题，用调色板直改（恢复用空参刷新路径重设）
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText,
             error ? QColor(Qt::red)
                   : list_->palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString TaskDialog::status_text() const { return status_->text(); }

int TaskDialog::task_count() const { return list_->count(); }
} // namespace memex::client
