#include "theme_settings_page.hpp"

#include <QButtonGroup>
#include <QLabel>
#include <QRadioButton>
#include <QVBoxLayout>

#include "theme.hpp"

namespace memex::client {
namespace {

// 模式名 → 界面文案（跟随系统排最前，自定义主题标注为扩展主题）
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

  group_ = new QButtonGroup(this);

  // 色带预览：直接从令牌取色（颜色令牌化的可视证据）
  swatch_ = new QWidget(this);
  swatch_->setObjectName(QStringLiteral("themeSettingsSwatch"));
  swatch_->setMinimumHeight(18);
  layout->addWidget(swatch_);

  layout->addStretch(1);

  for (const QString& mode : manager_->modes()) {
    auto* radio = new QRadioButton(mode_label(mode), this);
    radio->setObjectName(QStringLiteral("themeMode_%1").arg(mode));
    group_->addButton(radio);
    layout->addWidget(radio);
    // 点选即刻切换：ThemeManager 落盘并重应用，随后 theme_changed 回灌选中态
    connect(radio, &QRadioButton::clicked, this,
            [this, mode] { manager_->set_mode(mode); });
  }

  // 系统亮暗变化（跟随模式）与代码侧切换都汇到同一处刷新
  connect(manager_, &ThemeManager::theme_changed, this,
          [this](const QString&) { sync_from_manager(); });

  sync_from_manager();
}

void ThemeSettingsPage::sync_from_manager() {
  const QString mode = manager_->mode();
  for (auto* button : group_->buttons()) {
    if (auto* radio = qobject_cast<QRadioButton*>(button)) {
      const bool want = radio->objectName() ==
                        QStringLiteral("themeMode_%1").arg(mode);
      if (radio->isChecked() != want) {
        radio->setChecked(want);  // 不发 clicked，避免回灌 set_mode
      }
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