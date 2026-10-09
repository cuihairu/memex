#include "task_template_settings.hpp"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

#include <utility>

namespace memex::client {
namespace {
// 内置预设 id（撞了挂了也是死件——provider() 先命中预设）
bool is_builtin_id(const QString& id) {
  return id == QStringLiteral("github-issue") ||
         id == QStringLiteral("github-pr") ||
         id == QStringLiteral("gitlab-issue") || id == QStringLiteral("jira") ||
         id == QStringLiteral("linear") || id == QStringLiteral("url");
}
} // namespace

TaskTemplateSettingsDialog::TaskTemplateSettingsDialog(QWidget* parent,
                                                       QString org,
                                                       QString app)
    : QDialog(parent), org_(std::move(org)), app_(std::move(app)) {
  setWindowTitle(QStringLiteral("任务设置（自定义模板）"));
  resize(480, 360);
  build_ui();
  reload_list();
}

void TaskTemplateSettingsDialog::build_ui() {
  auto* layout = new QVBoxLayout(this);

  list_ = new QListWidget(this);
  layout->addWidget(list_, 1);

  auto* form = new QFormLayout;
  name_ = new QLineEdit(this);
  name_->setPlaceholderText(QStringLiteral("显示名（如 禅道任务）"));
  id_ = new QLineEdit(this);
  id_->setPlaceholderText(QStringLiteral("provider id（如 zentao）"));
  template_in_ = new QLineEdit(this);
  template_in_->setPlaceholderText(
      QStringLiteral("URL 模板，必含 {key}，可选 {project}"));
  form->addRow(QStringLiteral("名称"), name_);
  form->addRow(QStringLiteral("id"), id_);
  form->addRow(QStringLiteral("URL 模板"), template_in_);
  layout->addLayout(form);

  auto* ops = new QHBoxLayout;
  btn_add_ = new QPushButton(QStringLiteral("添加模板"), this);
  btn_delete_ = new QPushButton(QStringLiteral("删除选中"), this);
  ops->addWidget(btn_add_);
  ops->addWidget(btn_delete_);
  ops->addStretch(1);
  layout->addLayout(ops);

  status_ = new QLabel(this);
  layout->addWidget(status_);

  connect(btn_add_, &QPushButton::clicked, this, [this] {
    if (add_template(name_->text().trimmed(), id_->text().trimmed(),
                     template_in_->text().trimmed())) {
      name_->clear();
      id_->clear();
      template_in_->clear();
    }
  });
  connect(btn_delete_, &QPushButton::clicked, this, [this] {
    const auto* item = list_->currentItem();
    if (item == nullptr) {
      set_status(QStringLiteral("先选中一条模板"), true);
      return;
    }
    if (QMessageBox::question(this, QStringLiteral("删除模板"),
                              QStringLiteral("确定删除该自定义模板？")) ==
        QMessageBox::Yes) {
      remove_template(item->data(Qt::UserRole).toString());
    }
  });
}

bool TaskTemplateSettingsDialog::add_template(const QString& name,
                                             const QString& id,
                                             const QString& url_template) {
  if (id.isEmpty() || url_template.isEmpty()) {
    set_status(QStringLiteral("id 与 URL 模板必填"), true);
    return false;
  }
  if (!url_template.contains(QStringLiteral("{key}"))) {
    set_status(QStringLiteral("模板须含 {key} 槽"), true);
    return false;
  }
  if (is_builtin_id(id)) {
    set_status(QStringLiteral("id 撞内置预设，换一个"), true);
    return false;
  }
  auto list = custom_templates(org_, app_);
  for (const auto& t : list) {
    if (t.id == id) {
      set_status(QStringLiteral("id 已存在（先删旧的再改）"), true);
      return false;
    }
  }
  list.append({id, name.isEmpty() ? id : name, url_template});
  save_custom_templates(list, org_, app_);
  reload_list();
  set_status(QStringLiteral("模板已保存（任务清单即时生效）"));
  return true;
}

bool TaskTemplateSettingsDialog::remove_template(const QString& id) {
  auto list = custom_templates(org_, app_);
  for (int i = 0; i < list.size(); ++i) {
    if (list[i].id == id) {
      list.remove(i);
      save_custom_templates(list, org_, app_);
      reload_list();
      set_status(QStringLiteral("模板已删除"));
      return true;
    }
  }
  set_status(QStringLiteral("未找到该模板"), true);
  return false;
}

void TaskTemplateSettingsDialog::reload_list() {
  list_->clear();
  for (const auto& t : custom_templates(org_, app_)) {
    auto* item = new QListWidgetItem(
        QStringLiteral("%1（%2）  %3").arg(t.name, t.id, t.url_template), list_);
    item->setData(Qt::UserRole, t.id);
  }
}

void TaskTemplateSettingsDialog::set_status(const QString& text,
                                            bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText, error ? QColor(Qt::red)
                                         : palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString TaskTemplateSettingsDialog::status_text() const {
  return status_->text();
}

int TaskTemplateSettingsDialog::template_count() const {
  return list_->count();
}

} // namespace memex::client
