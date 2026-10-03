#include "theme.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleHints>

#include <cmath>

namespace memex::client {
namespace {

// 品牌橙（R19 主色）：两主题恒同，只随底色调 hover
constexpr auto kBrandOrange = "#e16531";
constexpr auto kSettingsKey = "appearance/theme_mode";

ThemeTokens light_tokens() {
  ThemeTokens t;
  t.surface = QColor(QStringLiteral("#faf7f3"));
  t.surface_alt = QColor(QStringLiteral("#f4efe8"));
  t.surface_raised = QColor(QStringLiteral("#ffffff"));
  t.border = QColor(QStringLiteral("#e8e0d6"));
  t.divider = QColor(QStringLiteral("#efe8de"));
  t.text = QColor(QStringLiteral("#332b24"));
  // muted 不用旧界面的 #9b8f86——它在浅底上只有 2.75:1，低于可读门槛；
  // 令牌层按对比度守住（test_theme 断言 ≥3:1）
  t.text_muted = QColor(QStringLiteral("#7d7169"));
  t.brand = QColor(QString::fromUtf8(kBrandOrange));
  t.brand_hover = QColor(QStringLiteral("#c9541f"));
  // brand_text＝浅底上的品牌色文本（描边按钮文字、本机保存徽标）：
  // 直接压暗品牌橙到 5.3:1，保住「橙色系文字」观感又守住可读门槛
  t.brand_text = QColor(QStringLiteral("#a05a26"));
  t.on_brand = QColor(QStringLiteral("#ffffff"));
  t.brand_tint = QColor(QStringLiteral("#f6e3d7"));
  t.brand_wash = QColor(QStringLiteral("#fdeee2"));
  t.brand_wash_text = QColor(QStringLiteral("#8a4a1f"));
  t.success = QColor(QStringLiteral("#6f8f6a"));
  t.warning = QColor(QStringLiteral("#c98a3c"));
  t.success_wash = QColor(QStringLiteral("#eaf3e7"));
  t.success_text = QColor(QStringLiteral("#3f6b3a"));
  t.danger = QColor(QStringLiteral("#c0492f"));
  t.disabled_bg = QColor(QStringLiteral("#d9cfc4"));
  t.bubble_out = QColor(QStringLiteral("#e16531"));
  t.bubble_out_text = QColor(QStringLiteral("#ffffff"));
  t.bubble_in = QColor(QStringLiteral("#f0ebe5"));
  t.bubble_in_text = QColor(QStringLiteral("#332b24"));
  t.chat_bg = QColor(QStringLiteral("#ffffff"));
  t.selection = QColor(QStringLiteral("#f6e3d7"));
  t.input_bg = QColor(QStringLiteral("#ffffff"));
  return t;
}

ThemeTokens dark_tokens() {
  ThemeTokens t;
  t.surface = QColor(QStringLiteral("#191614"));
  t.surface_alt = QColor(QStringLiteral("#221e1b"));
  t.surface_raised = QColor(QStringLiteral("#2b2622"));
  t.border = QColor(QStringLiteral("#3b342d"));
  t.divider = QColor(QStringLiteral("#322c27"));
  t.text = QColor(QStringLiteral("#f2ece5"));
  t.text_muted = QColor(QStringLiteral("#a3978a"));
  t.brand = QColor(QString::fromUtf8(kBrandOrange));
  t.brand_hover = QColor(QStringLiteral("#ef7a4a"));
  // 暗底上的品牌色文字须提亮才可读（brand 本身在暗底只有 3.5:1）
  t.brand_text = QColor(QStringLiteral("#f0a07c"));
  t.on_brand = QColor(QStringLiteral("#ffffff"));
  t.brand_tint = QColor(QStringLiteral("#3a2a20"));
  t.brand_wash = QColor(QStringLiteral("#2e241d"));
  t.brand_wash_text = QColor(QStringLiteral("#e9a97f"));
  t.success = QColor(QStringLiteral("#8fae88"));
  t.warning = QColor(QStringLiteral("#d9a066"));
  t.success_wash = QColor(QStringLiteral("#22301f"));
  t.success_text = QColor(QStringLiteral("#a8cfa4"));
  t.danger = QColor(QStringLiteral("#d9705a"));
  t.disabled_bg = QColor(QStringLiteral("#3a352f"));
  t.bubble_out = QColor(QString::fromUtf8(kBrandOrange));
  t.bubble_out_text = QColor(QStringLiteral("#ffffff"));
  t.bubble_in = QColor(QStringLiteral("#2b2622"));
  t.bubble_in_text = QColor(QStringLiteral("#f2ece5"));
  t.chat_bg = QColor(QStringLiteral("#201c19"));
  t.selection = QColor(QStringLiteral("#3a2a20"));
  t.input_bg = QColor(QStringLiteral("#2b2622"));
  return t;
}

bool valid(const QColor& c) { return c.isValid() && c.alpha() > 0; }

// WCAG 相对亮度（sRGB 线性化）——用于无 colorScheme 提示时的明暗兜底判断
double linearized(double c) {
  return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor& c) {
  return 0.2126 * linearized(c.redF()) + 0.7152 * linearized(c.greenF()) +
         0.0722 * linearized(c.blueF());
}

}  // namespace

QHash<QString, QColor> ThemeTokens::as_map() const {
  return {{QStringLiteral("surface"), surface},
          {QStringLiteral("surface_alt"), surface_alt},
          {QStringLiteral("surface_raised"), surface_raised},
          {QStringLiteral("border"), border},
          {QStringLiteral("divider"), divider},
          {QStringLiteral("text"), text},
          {QStringLiteral("text_muted"), text_muted},
          {QStringLiteral("brand"), brand},
          {QStringLiteral("brand_hover"), brand_hover},
          {QStringLiteral("brand_text"), brand_text},
          {QStringLiteral("on_brand"), on_brand},
          {QStringLiteral("brand_tint"), brand_tint},
          {QStringLiteral("brand_wash"), brand_wash},
          {QStringLiteral("brand_wash_text"), brand_wash_text},
          {QStringLiteral("success"), success},
          {QStringLiteral("warning"), warning},
          {QStringLiteral("success_wash"), success_wash},
          {QStringLiteral("success_text"), success_text},
          {QStringLiteral("danger"), danger},
          {QStringLiteral("disabled_bg"), disabled_bg},
          {QStringLiteral("bubble_out"), bubble_out},
          {QStringLiteral("bubble_out_text"), bubble_out_text},
          {QStringLiteral("bubble_in"), bubble_in},
          {QStringLiteral("bubble_in_text"), bubble_in_text},
          {QStringLiteral("chat_bg"), chat_bg},
          {QStringLiteral("selection"), selection},
          {QStringLiteral("input_bg"), input_bg}};
}

bool ThemeTokens::is_complete() const {
  const QHash<QString, QColor> map = as_map();
  for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
    if (!valid(it.value())) return false;
  }
  return true;
}

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {
  mode_ = read_persisted_mode();
  reload();
}

ThemeManager::~ThemeManager() = default;

ThemeManager& ThemeManager::instance() {
  static ThemeManager manager;
  return manager;
}

QStringList ThemeManager::builtin_themes() {
  return {QString::fromUtf8(kLight), QString::fromUtf8(kDark)};
}

ThemeTokens ThemeManager::tokens_for(const QString& name) {
  if (name == QLatin1String(kDark)) return dark_tokens();
  return light_tokens();
}

QString ThemeManager::read_persisted_mode() const {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  const QString stored =
      settings.value(QString::fromUtf8(kSettingsKey)).toString();
  if (stored.isEmpty()) return QString::fromUtf8(kFollowSystem);
  if (stored == QLatin1String(kFollowSystem)) return stored;
  if (stored == QLatin1String(kLight) || stored == QLatin1String(kDark)) {
    return stored;
  }
  // 自定义主题：注册表里没有这个名字（换机／重装后扩展主题可能已不在）→
  // 回落跟随系统，不让界面停在无法解析的取值上
  const ThemeManager* self = this;
  return self->custom_.contains(stored) ? stored
                                        : QString::fromUtf8(kFollowSystem);
}

void ThemeManager::persist_mode(const QString& mode) const {
  QSettings settings(QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
  settings.setValue(QString::fromUtf8(kSettingsKey), mode);
}

bool ThemeManager::has_theme(const QString& name) const {
  return builtin_themes().contains(name) || custom_.contains(name);
}

void ThemeManager::register_theme(const QString& name,
                                  const ThemeTokens& tokens) {
  if (name.isEmpty() || name == QLatin1String(kFollowSystem)) return;
  // 品牌橙是全产品唯一主色：扩展主题也不得改主色，否则多主题扩展位会漂移
  ThemeTokens copy = tokens;
  copy.brand = QColor(QString::fromUtf8(kBrandOrange));
  custom_.insert(name, copy);
}

QStringList ThemeManager::modes() const {
  QStringList list{QString::fromUtf8(kFollowSystem)};
  list += builtin_themes();
  for (auto it = custom_.constBegin(); it != custom_.constEnd(); ++it) {
    list << it.key();
  }
  return list;
}

void ThemeManager::set_mode(const QString& mode) {
  QString next = mode;
  if (next != QLatin1String(kFollowSystem) && !has_theme(next)) {
    next = QString::fromUtf8(kFollowSystem);
  }
  if (next == mode_) return;
  mode_ = next;
  persist_mode(mode_);
  reload();
}

QString ThemeManager::resolve_theme() const {
  if (mode_ != QLatin1String(kFollowSystem)) return mode_;
  return system_is_dark() ? QString::fromUtf8(kDark) : QString::fromUtf8(kLight);
}

QString ThemeManager::effective_theme() const { return resolve_theme(); }

bool ThemeManager::system_is_dark() const {
  if (dark_probe_) return dark_probe_();
  if (auto* hints = QGuiApplication::styleHints()) {
    if (hints->colorScheme() != Qt::ColorScheme::Unknown) {
      return hints->colorScheme() == Qt::ColorScheme::Dark;
    }
  }
  // 老平台／无头环境没有 colorScheme 提示：退回当前调色板自身的明暗关系
  // （窗口底比文本暗即暗色主题），不依赖固定阈值
  const QPalette palette = QGuiApplication::palette();
  return luminance(palette.color(QPalette::Active, QPalette::Window)) <
         luminance(palette.color(QPalette::Active, QPalette::WindowText));
}

void ThemeManager::set_system_dark_probe(std::function<bool()> probe) {
  dark_probe_ = std::move(probe);
  reload();
}

void ThemeManager::reload() {
  const QString name = resolve_theme();
  const ThemeTokens next = custom_.contains(name) ? custom_.value(name)
                                                  : tokens_for(name);
  tokens_ = next;
  if (applied_) push_to_app();
}

QString ThemeManager::stylesheet_for(const ThemeTokens& tokens) {
  // 模板里只写 %令牌名%，末尾统一替换——新增令牌自动进 QSS，
  // 不会因为漏改样式而悄悄留硬编码色。
  QString qss = QStringLiteral(
      "QWidget { color: %text%; }\n"
      "QMainWindow, QDialog { background: %surface%; }\n"
      "QTextBrowser, QPlainTextEdit, QTextEdit { background: %chat_bg%; "
      "color: %text%; border: 1px solid %border%; border-radius: 8px; }\n"
      "QListWidget, QTreeWidget, QTableWidget { background: %surface_alt%; "
      "color: %text%; border: 1px solid %border%; border-radius: 8px; }\n"
      "QListWidget::item:selected, QTreeWidget::item:selected { "
      "background: %selection%; color: %text%; }\n"
      "QLineEdit, QSpinBox, QComboBox { background: %input_bg%; "
      "color: %text%; border: 1px solid %border%; border-radius: 8px; "
      "padding: 6px 8px; }\n"
      "QLineEdit:focus, QSpinBox:focus, QComboBox:focus { "
      "border: 1px solid %brand%; }\n"
      "QComboBox QAbstractItemView { background: %surface_raised%; "
      "color: %text%; selection-background-color: %brand%; "
      "selection-color: %on_brand%; border: 1px solid %border%; }\n"
      "QPushButton { background: %surface_raised%; color: %text%; "
      "border: 1px solid %border%; border-radius: 8px; padding: 6px 12px; }\n"
      "QPushButton:hover { border: 1px solid %brand_hover%; "
      "color: %brand_hover%; }\n"
      "QPushButton:pressed { background: %brand_tint%; }\n"
      "QPushButton:disabled { background: %disabled_bg%; color: %text_muted%; "
      "border-color: %divider%; }\n"
      "QToolButton { color: %text%; }\n"
      "QToolButton:hover { color: %brand_hover%; }\n"
      "QHeaderView::section { background: %surface_alt%; color: %text_muted%; "
      "border: none; border-bottom: 1px solid %border%; padding: 6px 8px; }\n"
      "QToolTip { background: %surface_raised%; color: %text%; "
      "border: 1px solid %border%; }\n"
      "QMenu { background: %surface_raised%; color: %text%; "
      "border: 1px solid %border%; }\n"
      "QMenu::item:selected { background: %selection%; color: %text%; }\n"
      "QGroupBox { border: 1px solid %border%; border-radius: 8px; "
      "margin-top: 12px; }\n"
      "QGroupBox::title { subcontrol-origin: margin; left: 10px; "
      "padding: 0 4px; color: %text_muted%; }\n"
      "QTabBar::tab { background: %surface_alt%; color: %text_muted%; "
      "padding: 6px 12px; border: 1px solid %border%; }\n"
      "QTabBar::tab:selected { background: %surface_raised%; color: %text%; "
      "border-bottom: 1px solid %brand%; }\n"
      "QTabWidget::pane { border: 1px solid %border%; }\n"
      "QCheckBox, QRadioButton { color: %text%; spacing: 6px; }\n"
      "QCheckBox::indicator, QRadioButton::indicator { "
      "border: 1px solid %border%; background: %input_bg%; }\n"
      "QSplitter::handle { background: %divider%; }\n"
      "QScrollBar:vertical, QScrollBar:horizontal { background: transparent; "
      "margin: 0; }\n"
      "QScrollBar::handle { background: %divider%; border-radius: 4px; "
      "min-height: 24px; min-width: 24px; }\n"
      "QScrollBar::handle:hover { background: %brand_tint%; }\n"
      "QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; }\n"
      "QScrollBar::add-page, QScrollBar::sub-page { background: transparent; "
      "}\n"
      "QStatusBar { background: %surface_alt%; color: %text_muted%; }\n"
      "QProgressBar { background: %surface_alt%; color: %text%; "
      "border: 1px solid %border%; border-radius: 6px; text-align: center; }\n"
      "QProgressBar::chunk { background: %brand%; }");

  const auto map = tokens.as_map();
  for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
    qss.replace(QStringLiteral("%") + it.key() + QStringLiteral("%"),
                it.value().name());
  }
  return qss;
}

void ThemeManager::push_to_app() {
  if (!app_) return;
  QPalette palette = app_->palette();
  palette.setColor(QPalette::Window, tokens_.surface);
  palette.setColor(QPalette::WindowText, tokens_.text);
  palette.setColor(QPalette::Base, tokens_.surface_raised);
  palette.setColor(QPalette::AlternateBase, tokens_.surface_alt);
  palette.setColor(QPalette::Text, tokens_.text);
  palette.setColor(QPalette::Button, tokens_.surface_raised);
  palette.setColor(QPalette::ButtonText, tokens_.text);
  palette.setColor(QPalette::Highlight, tokens_.brand);
  palette.setColor(QPalette::HighlightedText, tokens_.on_brand);
  palette.setColor(QPalette::ToolTipBase, tokens_.surface_raised);
  palette.setColor(QPalette::ToolTipText, tokens_.text);
  palette.setColor(QPalette::Link, tokens_.brand);
  palette.setColor(QPalette::PlaceholderText, tokens_.text_muted);
  app_->setPalette(palette);
  app_->setStyleSheet(stylesheet_for(tokens_));
  emit theme_changed(effective_theme());
}

void ThemeManager::apply(QApplication* app) {
  app_ = app;
  applied_ = true;
  if (!watching_system_) {
    watching_system_ = true;
    // 系统亮暗变化：只在跟随模式下重应用，手动选择不被系统变化覆盖
    if (auto* hints = QGuiApplication::styleHints()) {
      QObject::connect(hints, &QStyleHints::colorSchemeChanged, this, [this] {
        if (mode_ == QLatin1String(kFollowSystem)) reload();
      });
    }
  }
  push_to_app();
}

}  // namespace memex::client