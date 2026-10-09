#include "theme_settings_page.hpp"

#include <algorithm>

#include <QCheckBox>
#include <QColorDialog>
#include <QCoreApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include "theme.hpp"

namespace memex::client {
namespace {

// ⑮ 皮肤包对话框记忆目录（此前仓库无文件对话框目录记忆惯例，⑮ 自建）
constexpr auto kSkinDirKey = "ui/skin_pack_dir";

QSettings app_settings() {
  return QSettings(QCoreApplication::organizationName(),
                   QCoreApplication::applicationName());
}

// 模式名 → 界面文案（跟随系统排最前；⑬ 默认 20 套名字本身即可读中文，
// 原样展示；自定义槽与未注册的扩展主题加注；⑮ 皮肤包主题加注）
QString mode_label(const QString& mode, const ThemeManager* manager) {
  if (mode == QLatin1String(ThemeManager::kFollowSystem)) {
    return QStringLiteral("跟随系统");
  }
  if (mode == QLatin1String(ThemeManager::kLight)) {
    return QStringLiteral("亮色");
  }
  if (mode == QLatin1String(ThemeManager::kDark)) {
    return QStringLiteral("暗色");
  }
  if (mode == ThemeManager::customThemeName()) {
    return QStringLiteral("自定义（改色）");
  }
  if (ThemeManager::builtin_themes().contains(mode)) return mode;
  if (manager && manager->skin_pack_themes().contains(mode)) {
    return QStringLiteral("%1（皮肤包）").arg(mode);
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

// ⑭ 放行令牌的界面文案（色钮 tooltip 与测试断言共用口径）
QString token_label(const QString& token) {
  static const QHash<QString, QString> labels = {
      {QStringLiteral("surface"), QStringLiteral("窗口底色")},
      {QStringLiteral("surface_alt"), QStringLiteral("侧栏／列表底")},
      {QStringLiteral("surface_raised"), QStringLiteral("卡片／面板底")},
      {QStringLiteral("border"), QStringLiteral("边框")},
      {QStringLiteral("text"), QStringLiteral("主文字")},
      {QStringLiteral("text_muted"), QStringLiteral("次要文字")},
      {QStringLiteral("chat_bg"), QStringLiteral("聊天区底")},
      {QStringLiteral("input_bg"), QStringLiteral("输入框底")}};
  return labels.value(token);
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

  // 主题列表：24+ 项（跟随系统＋内置 22＋自定义槽＋扩展主题＋皮肤包），
  // radio 群排不下，⑬ 起改 QListWidget；item 的 UserRole 携带模式名
  list_ = new QListWidget(this);
  list_->setObjectName(QStringLiteral("themeModes"));
  for (const QString& mode : manager_->modes()) {
    auto* item = new QListWidgetItem(mode_label(mode, manager_), list_);
    item->setData(Qt::UserRole, mode);
  }
  layout->addWidget(list_);

  // 色带预览：直接从令牌取色（颜色令牌化的可视证据）
  swatch_ = new QWidget(this);
  swatch_->setObjectName(QStringLiteral("themeSettingsSwatch"));
  swatch_->setMinimumHeight(18);
  layout->addWidget(swatch_);

  // —— ⑭ 面板改色：8 枚中性令牌（品牌橙与语义族锁死不放行）——
  auto* color_box = new QGroupBox(QStringLiteral("面板改色（自定义）"), this);
  color_box->setObjectName(QStringLiteral("customColorsBox"));
  auto* grid = new QGridLayout(color_box);
  grid->setContentsMargins(8, 8, 8, 8);
  grid->setSpacing(6);
  const QStringList tokens = ThemeManager::customizable_tokens();
  for (int i = 0; i < tokens.size(); ++i) {
    const QString& token = tokens.at(i);
    auto* btn = new QPushButton(this);
    btn->setObjectName(QStringLiteral("customColor_%1").arg(token));
    btn->setToolTip(
        QStringLiteral("%1（点击改色；品牌色与状态色不可改）")
            .arg(token_label(token)));
    btn->setMinimumSize(48, 22);
    // 点色钮弹 QColorDialog；选定即走 apply_custom_color（与测试同路径）
    connect(btn, &QPushButton::clicked, this, [this, token] {
      const QColor initial = manager_->tokens().as_map().value(token);
      const QColor picked =
          QColorDialog::getColor(initial, this, token_label(token));
      if (picked.isValid()) apply_custom_color(token, picked);
    });
    color_buttons_.insert(token, btn);
    grid->addWidget(btn, i / 4, i % 4);
  }
  reset_colors_ = new QPushButton(QStringLiteral("恢复默认"), this);
  reset_colors_->setObjectName(QStringLiteral("resetCustomColors"));
  // 「恢复默认」＝清覆盖层并回到基线内置主题（ThemeManager 同路径）
  connect(reset_colors_, &QPushButton::clicked, this, [this] {
    manager_->clear_custom_overrides();
    sync_from_manager();
  });
  grid->addWidget(reset_colors_, 2, 0, 1, 4);
  layout->addWidget(color_box);

  // —— ⑭ 毛玻璃特效：全局单开关（默认关；平台不支持自动降级不透明）——
  auto* effect_box = new QGroupBox(QStringLiteral("特效"), this);
  effect_box->setObjectName(QStringLiteral("effectsBox"));
  auto* effect_layout = new QVBoxLayout(effect_box);
  frosted_check_ = new QCheckBox(QStringLiteral("毛玻璃特效（Windows 亚克力／Mica）"),
                                 this);
  frosted_check_->setObjectName(QStringLiteral("frostedEffectCheck"));
  // 复选框勾选走 set_frosted（与测试程序化驱动同路径）
  connect(frosted_check_, &QCheckBox::toggled, this,
          [this](bool on) { set_frosted(on); });
  effect_layout->addWidget(frosted_check_);
  frosted_hint_ = new QLabel(this);
  frosted_hint_->setObjectName(QStringLiteral("frostedEffectHint"));
  effect_layout->addWidget(frosted_hint_);
  layout->addWidget(effect_box);

  // —— ⑮ 皮肤包：导入／导出（对话框记忆目录自建 ui/skin_pack_dir）——
  auto* skin_box = new QGroupBox(QStringLiteral("皮肤包"), this);
  skin_box->setObjectName(QStringLiteral("skinPackBox"));
  auto* skin_layout = new QVBoxLayout(skin_box);
  auto* skin_row = new QHBoxLayout();
  import_skins_ = new QPushButton(QStringLiteral("导入皮肤包…"), this);
  import_skins_->setObjectName(QStringLiteral("importSkinButton"));
  export_skins_ = new QPushButton(QStringLiteral("导出当前皮肤…"), this);
  export_skins_->setObjectName(QStringLiteral("exportSkinButton"));
  skin_row->addWidget(import_skins_);
  skin_row->addWidget(export_skins_);
  skin_layout->addLayout(skin_row);
  skin_status_ = new QLabel(this);
  skin_status_->setObjectName(QStringLiteral("skinStatus"));
  skin_status_->setWordWrap(true);
  skin_layout->addWidget(skin_status_);
  connect(import_skins_, &QPushButton::clicked, this, [this] {
    const QString dir =
        app_settings().value(QString::fromUtf8(kSkinDirKey)).toString();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入皮肤包"), dir,
        QStringLiteral("皮肤包 (*.zip)"));
    if (!path.isEmpty()) import_skin_from_path(path);
  });
  connect(export_skins_, &QPushButton::clicked, this, [this] {
    const QString dir =
        app_settings().value(QString::fromUtf8(kSkinDirKey)).toString();
    const SkinPackManifest manifest = manager_->current_skin_manifest();
    const QString suggested =
        dir.isEmpty()
            ? QStringLiteral("%1-%2.zip").arg(manifest.name, manifest.version)
            : QStringLiteral("%1/%2-%3.zip")
                  .arg(dir, manifest.name, manifest.version);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出当前皮肤"), suggested,
        QStringLiteral("皮肤包 (*.zip)"));
    if (!path.isEmpty()) export_skin_to_path(path);
  });
  layout->addWidget(skin_box);

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
  // ⑮ 皮肤包安装会扩展 modes()：列表与注册表对不上时整体重建（QHash
  // 无序，比较用集合不用顺序）
  const QStringList modes = manager_->modes();
  QSet<QString> listed;
  for (int i = 0; i < list_->count(); ++i) {
    listed.insert(list_->item(i)->data(Qt::UserRole).toString());
  }
  if (listed.size() != modes.size() ||
      std::any_of(modes.cbegin(), modes.cend(),
                  [&listed](const QString& m) { return !listed.contains(m); })) {
    list_->clear();
    for (const QString& m : modes) {
      auto* item = new QListWidgetItem(mode_label(m, manager_), list_);
      item->setData(Qt::UserRole, m);
    }
  }
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
  // ⑭ 色钮显示当前生效色（基线或覆盖后的值），恢复默认可用性随覆盖层
  const QHash<QString, QColor> live = manager_->tokens().as_map();
  for (auto it = color_buttons_.constBegin(); it != color_buttons_.constEnd();
       ++it) {
    it.value()->setStyleSheet(
        QStringLiteral("background: %1; border: 1px solid %2;")
            .arg(live.value(it.key()).name())
            .arg(manager_->tokens().border.name()));
  }
  reset_colors_->setEnabled(manager_->has_custom_overrides());
  // ⑭ 毛玻璃：勾选态回灌（blockSignals 防 toggled 回灌 set_frosted）；
  // 不支持平台的降级提示走生效态而非开关态
  frosted_check_->blockSignals(true);
  frosted_check_->setChecked(manager_->frosted_effect_enabled());
  frosted_check_->blockSignals(false);
  if (!manager_->frosted_effect_enabled()) {
    frosted_hint_->setText(
        QStringLiteral("默认关闭；开启后需系统支持（Windows 11）"));
  } else if (manager_->frosted_effect_active()) {
    frosted_hint_->setText(QStringLiteral("已启用"));
  } else {
    frosted_hint_->setText(
        QStringLiteral("当前系统不支持，已降级为不透明背景"));
  }
}

bool ThemeSettingsPage::apply_custom_color(const QString& token,
                                           const QColor& color) {
  const bool ok = manager_->set_custom_override(token, color);
  sync_from_manager();
  return ok;
}

bool ThemeSettingsPage::set_frosted(bool enabled) {
  const bool active = manager_->set_frosted_effect_enabled(enabled);
  sync_from_manager();
  return active;
}

// —— 需求批⑮：皮肤包导入／导出 ——

bool ThemeSettingsPage::import_skin_from_path(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    skin_status_->setText(
        QStringLiteral("无法读取皮肤包文件：%1").arg(path));
    return false;
  }
  QString error;
  const bool ok = manager_->install_skin_pack(file.readAll(), &error);
  if (ok) {
    // 导入成功即选中新主题（install_skin_pack 内已 set_mode），状态行报包名
    skin_status_->setText(
        QStringLiteral("皮肤包导入成功：%1").arg(manager_->mode()));
    app_settings().setValue(QString::fromUtf8(kSkinDirKey),
                            QFileInfo(path).absolutePath());
  } else {
    skin_status_->setText(error);
  }
  sync_from_manager();
  return ok;
}

bool ThemeSettingsPage::export_skin_to_path(const QString& path) {
  QString error;
  const SkinPackManifest manifest = manager_->current_skin_manifest(&error);
  if (!error.isEmpty()) {
    skin_status_->setText(error);
    return false;
  }
  QString export_error;
  const QByteArray bytes = export_skin_pack(manifest, &export_error);
  if (bytes.isEmpty()) {
    skin_status_->setText(export_error);
    return false;
  }
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    skin_status_->setText(
        QStringLiteral("无法写入目标文件：%1").arg(path));
    return false;
  }
  if (file.write(bytes) != bytes.size()) {
    skin_status_->setText(
        QStringLiteral("写入皮肤包不完整：%1").arg(path));
    return false;
  }
  app_settings().setValue(QString::fromUtf8(kSkinDirKey),
                          QFileInfo(path).absolutePath());
  skin_status_->setText(QStringLiteral("已导出皮肤包：%1（%2 v%3）")
                            .arg(path, manifest.name, manifest.version));
  return true;
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
