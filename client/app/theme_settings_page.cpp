#include "theme_settings_page.hpp"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

#include "theme.hpp"

namespace memex::client {
namespace {

// 模式名 → 界面文案（跟随系统排最前；⑬ 默认 20 套名字本身即可读中文，
// 原样展示；未注册的扩展主题才加注）
QString mode_label(const QString& mode) {
  if (mode == QLatin1String(ThemeManager::kFollowSystem)) {
    return QStringLiteral("跟随系统");
  }
  if (mode == QLatin1String(ThemeManager::kLight)) {
    return QStringLiteral("亮色");
  }
  if (mode == QLatin1String(ThemeManager::kDark)) {
    return QStringLiteral("暗色");
  }
  if (ThemeManager::builtin_themes().contains(mode)) return mode;
  return QStringLiteral("%1（扩展主题）").arg(mode);
}

QString theme_label(const QString& theme) {
  if (theme == QLatin1String(ThemeManager::kLight)) {
    return QStringLiteral("亮色");
  }
  if (theme == QLatin1String(ThemeManager::kDark)) {
    return QStringLiteral("暗色");
  }
  return theme;
}

}  // namespace

ThemeSettingsPage::ThemeSettingsPage(ThemeManager* manager, QWidget* parent)
    : QWidget(parent),
      manager_(manager ? manager : &ThemeManager::instance()) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(10);

  auto* title = new QLabel(QStringLiteral("外观"), this);
  title->setObjectName(QStringLiteral("themeSettingsTitle"));
  layout->addWidget(title);

  effective_label_ = new QLabel(this);
  effective_label_->setObjectName(QStringLiteral("themeSettingsEffective"));
  layout->addWidget(effective_label_);

  // 主题列表：23+ 项（跟随系统＋内置 22＋扩展主题），radio 群排不下，
  // ⑬ 起改 QListWidget；item 的 UserRole 携带模式名
  list_ = new QListWidget(this);
  list_->setObjectName(QStringLiteral("themeModes"));
  for (const QString& mode : manager_->modes()) {
    auto* item = new QListWidgetItem(mode_label(mode), list_);
    item->setData(Qt::UserRole, mode);
  }
  layout->addWidget(list_);

  // 色带预览：直接从令牌取色（颜色令牌化的可视证据）
  swatch_ = new QWidget(this);
  swatch_->setObjectName(QStringLiteral("themeSettingsSwatch"));
  swatch_->setMinimumHeight(18);
  layout->addWidget(swatch_);

  layout->addStretch(1);

  // 点选即刻切换：ThemeManager 落盘并重应用，随后 theme_changed 回灌选中态
  connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
    manager_->set_mode(it->data(Qt::UserRole).toString());
  });

  // 系统亮暗变化（跟随模式）与代码侧切换都汇到同一处刷新
  connect(manager_, &ThemeManager::theme_changed, this,
          [this](const QString&) { sync_from_manager(); });

  sync_from_manager();
}

void ThemeSettingsPage::sync_from_manager() {
  const QString mode = manager_->mode();
  for (int i = 0; i < list_->count(); ++i) {
    if (list_->item(i)->data(Qt::UserRole).toString() == mode) {
      // setCurrentRow 不发 itemClicked，不会回灌 set_mode
      list_->setCurrentRow(i);
      break;
    }
  }
  effective_label_->setText(
      QStringLiteral("当前生效：%1").arg(theme_label(manager_->effective_theme())));
  swatch_->setStyleSheet(
      QStringLiteral("background: %1; border-radius: 4px;")
          .arg(manager_->tokens().brand.name()));
}

bool ThemeSettingsPage::select_mode(const QString& mode) {
  if (mode != QLatin1String(ThemeManager::kFollowSystem) &&
      !manager_->has_theme(mode)) {
    return false;
  }
  manager_->set_mode(mode);
  sync_from_manager();
  return true;
}

QString ThemeSettingsPage::effective_theme() const {
  return manager_->effective_theme();
}

}  // namespace memex::client
