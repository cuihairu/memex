#include "task_provider_settings.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

#include "task_provider_store.hpp"

namespace memex::client {

TaskProviderSettingsDialog::TaskProviderSettingsDialog(
    TaskProviderStore* store, QWidget* parent)
    : QDialog(parent), store_(store) {
  setWindowTitle(QStringLiteral("任务设置（外部任务）"));
  resize(520, 320);
  build_ui();
  refresh_gate();
  show_provider(provider_->currentData().toString());
}

void TaskProviderSettingsDialog::build_ui() {
  auto* layout = new QVBoxLayout(this);

  // 口令门：首建/解锁共用一口令框；解锁态出现换口令
  auto* top = new QHBoxLayout;
  passphrase_ = new QLineEdit(this);
  passphrase_->setEchoMode(QLineEdit::Password);
  passphrase_->setPlaceholderText(QStringLiteral("凭据口令（本地加密用）"));
  passphrase_->setMaximumWidth(170);
  btn_unlock_ = new QPushButton(
      store_->has_store() ? QStringLiteral("解锁") : QStringLiteral("创建并解锁"),
      this);
  btn_change_pass_ = new QPushButton(QStringLiteral("更换口令"), this);
  btn_change_pass_->setEnabled(false);
  top->addWidget(passphrase_);
  top->addWidget(btn_unlock_);
  top->addWidget(btn_change_pass_);
  layout->addLayout(top);

  auto* gate = new QLabel(
      store_->has_store()
          ? QStringLiteral("口令保护凭据包；解锁后才可读写 provider 配置")
          : QStringLiteral("首次使用：建一个口令保护凭据包（加密落盘）"),
      this);
  layout->addWidget(gate);

  // provider 区（动态字段）
  provider_ = new QComboBox(this);
  provider_->addItem(QStringLiteral("GitHub Issues"), QStringLiteral("github-issue"));
  provider_->addItem(QStringLiteral("钉钉待办"), QStringLiteral("dingtalk-todo"));
  provider_->addItem(QStringLiteral("飞书任务"), QStringLiteral("feishu-task"));
  layout->addWidget(new QLabel(QStringLiteral("provider（凭据仅本地加密存储）"), this));
  layout->addWidget(provider_);
  provider_hint_ = new QLabel(this);
  layout->addWidget(provider_hint_);
  fields_holder_ = new QVBoxLayout;
  layout->addLayout(fields_holder_);
  layout->addStretch(1);

  auto* ops = new QHBoxLayout;
  btn_save_ = new QPushButton(QStringLiteral("保存该 provider"), this);
  btn_clear_ = new QPushButton(QStringLiteral("清除该 provider"), this);
  btn_save_->setEnabled(false);
  btn_clear_->setEnabled(false);
  ops->addWidget(btn_save_);
  ops->addWidget(btn_clear_);
  ops->addStretch(1);
  layout->addLayout(ops);

  status_ = new QLabel(this);
  layout->addWidget(status_);

  connect(btn_unlock_, &QPushButton::clicked, this, [this] {
    if (unlock_with(passphrase_->text())) {
      passphrase_->clear();
      build_fields(provider_->currentData().toString());
    }
  });
  connect(btn_change_pass_, &QPushButton::clicked, this, [this] {
    bool ok = false;
    const QString next = QInputDialog::getText(
        this, QStringLiteral("更换凭据口令"),
        QStringLiteral("输入新口令（整包重加密）"), QLineEdit::Password,
        QString(), &ok);
    if (ok && !next.isEmpty()) {
      if (change_passphrase(next)) {
        set_status(QStringLiteral("口令已更换（整包重加密落盘）"));
      } else {
        set_status(QStringLiteral("更换口令失败"), true);
      }
    }
  });
  connect(provider_, &QComboBox::currentIndexChanged, this,
          [this] { build_fields(provider_->currentData().toString()); });
  connect(btn_save_, &QPushButton::clicked, this, [this] {
    const QString id = provider_->currentData().toString();
    const QJsonObject fields = current_fields();
    if (save_current_provider(fields)) build_fields(id);
  });
  connect(btn_clear_, &QPushButton::clicked, this, [this] {
    store_->remove(provider_->currentData().toString());
    build_fields(provider_->currentData().toString());
  });
  connect(store_, &TaskProviderStore::changed, this, [this] {
    build_fields(provider_->currentData().toString());
  });
}

void TaskProviderSettingsDialog::refresh_gate() {
  const bool unlocked = store_->is_unlocked();
  btn_unlock_->setText(store_->has_store() ? QStringLiteral("解锁")
                                           : QStringLiteral("创建并解锁"));
  btn_unlock_->setEnabled(!unlocked);
  btn_change_pass_->setEnabled(unlocked);
  btn_save_->setEnabled(unlocked);
  btn_clear_->setEnabled(unlocked);
  passphrase_->setEnabled(!unlocked);
}

bool TaskProviderSettingsDialog::unlock_with(const QString& passphrase) {
  const bool ok = store_->has_store() ? store_->unlock(passphrase)
                                      : store_->create(passphrase);
  if (ok) {
    set_status(QStringLiteral("凭据已解锁（会话内存）"));
  } else {
    set_status(store_->has_store() ? QStringLiteral("口令不正确或凭据已损坏")
                                   : QStringLiteral("建包失败"),
               true);
  }
  refresh_gate();
  return ok;
}

bool TaskProviderSettingsDialog::save_current_provider(
    const QJsonObject& fields) {
  const QString id = provider_->currentData().toString();
  for (const auto& spec : field_specs(id)) {
    if (spec.required && fields.value(spec.key).toString().trimmed().isEmpty()) {
      set_status(QStringLiteral("必填项不能为空：") + spec.label, true);
      return false;
    }
  }
  store_->save(id, fields);
  set_status(QStringLiteral("已保存（加密落盘）：") + provider_name(id));
  return true;
}

bool TaskProviderSettingsDialog::change_passphrase(const QString& next) {
  return store_->change_passphrase(next);
}

void TaskProviderSettingsDialog::show_provider(const QString& provider_id) {
  const int idx = provider_->findData(provider_id);
  if (idx >= 0) provider_->setCurrentIndex(idx);
  build_fields(provider_id);
}

void TaskProviderSettingsDialog::build_fields(const QString& provider_id) {
  Q_ASSERT(fields_holder_ != nullptr);
  auto* holder = fields_holder_;
  while (QLayoutItem* it = holder->takeAt(0)) {
    if (auto* w = it->widget()) w->deleteLater();
    delete it;
  }
  field_edits_.clear();
  auto* form = new QFormLayout;
  const QJsonObject saved = store_->config(provider_id);
  for (const auto& spec : field_specs(provider_id)) {
    auto* edit = new QLineEdit(this);
    if (spec.secret) edit->setEchoMode(QLineEdit::Password);
    edit->setText(saved.value(spec.key).toString());
    form->addRow(
        spec.label + (spec.required ? QStringLiteral("（必填）")
                                    : QStringLiteral("（可空）")),
        edit);
    field_edits_.append(edit);
  }
  holder->addLayout(form);
  provider_hint_->setText(provider_name(provider_id) +
                          (store_->contains(provider_id)
                               ? QStringLiteral("  ✓ 已配置")
                               : QStringLiteral("  （未配置）")));
}

QJsonObject TaskProviderSettingsDialog::current_fields() const {
  QJsonObject out;
  const QString id = provider_->currentData().toString();
  const auto specs = field_specs(id);
  for (int i = 0; i < specs.size() && i < field_edits_.size(); ++i) {
    out.insert(specs[i].key, field_edits_[i]->text().trimmed());
  }
  return out;
}

void TaskProviderSettingsDialog::set_status(const QString& text, bool error) {
  status_->setText(error ? QStringLiteral("⚠ %1").arg(text) : text);
  QPalette p = status_->palette();
  p.setColor(QPalette::WindowText, error ? QColor(Qt::red)
                                         : palette().color(QPalette::WindowText));
  status_->setPalette(p);
}

QString TaskProviderSettingsDialog::status_text() const { return status_->text(); }

QString TaskProviderSettingsDialog::current_provider_id() const {
  return provider_->currentData().toString();
}

QVector<TaskProviderSettingsDialog::FieldSpec>
TaskProviderSettingsDialog::field_specs(const QString& provider_id) {
  if (provider_id == QStringLiteral("github-issue")) {
    return {{QStringLiteral("repo"), QStringLiteral("仓库 org/repo"), false, true},
            {QStringLiteral("token"), QStringLiteral("PAT 令牌"), true, true}};
  }
  if (provider_id == QStringLiteral("dingtalk-todo")) {
    return {{QStringLiteral("app_key"), QStringLiteral("appKey"), false, true},
            {QStringLiteral("app_secret"), QStringLiteral("appSecret"), true, true},
            // unionId 获取流程属设置页后续件；留空按「未取到用户」走
            {QStringLiteral("union_id"), QStringLiteral("unionId"), false, false}};
  }
  return {{QStringLiteral("app_id"), QStringLiteral("app_id"), false, true},
          {QStringLiteral("app_secret"), QStringLiteral("app_secret"), true, true}};
}

QString TaskProviderSettingsDialog::provider_name(const QString& provider_id) {
  if (provider_id == QStringLiteral("github-issue"))
    return QStringLiteral("GitHub Issues");
  if (provider_id == QStringLiteral("dingtalk-todo"))
    return QStringLiteral("钉钉待办");
  if (provider_id == QStringLiteral("feishu-task"))
    return QStringLiteral("飞书任务");
  return provider_id;
}

} // namespace memex::client
