// 二期·品牌物料设置页（实现）。判权与校验全在服务端（branding-manage=
// org-admin；accent #rrggbb 形态门、PNG 类型/大小/尺寸）——客户端只做
// 本地形态预检（accent 坏形态不发网）与提交展示；403 状态行明示归属。
#include "brand_settings_page.hpp"

#include <QColorDialog>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include "brand_kit.hpp"
#include "engine/collab/files_client.hpp"

namespace memex::client {

BrandSettingsDialog::BrandSettingsDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("品牌物料"));
  resize(560, 520);
  client_ = new FilesClient(this);
  build_ui();

  connect(client_, &FilesClient::logged_in, this, [this] {
    set_status(QStringLiteral("已连接（") + client_->account() +
               QStringLiteral("）"));
    btn_connect_->setEnabled(true);
    refresh();
  });
  connect(client_, &FilesClient::login_failed, this,
          [this](const QString& r) {
            set_status(QStringLiteral("连接失败：") + r, true);
            btn_connect_->setEnabled(true);
          });
  connect(client_, &FilesClient::branding_fetched, this,
          [this](const QJsonObject& b) { apply_branding(b); });
  // 保存/清除/上传成功：状态行明示＋本机 BrandKit 即时拉新（设置即时
  // 生效＝本机先见；离线缓存同步更新）
  connect(client_, &FilesClient::branding_saved, this, [this](qint64 v) {
    set_status(QStringLiteral("已保存（版本 ") + QString::number(v) +
               QStringLiteral("）"));
    BrandKit::instance().fetch(conn_host_, files_port_);
  });
  connect(client_, &FilesClient::brand_logo_cleared, this, [this] {
    logo_path_->clear();
    logo_preview_->clear();
    set_status(QStringLiteral("logo 已清除"));
    BrandKit::instance().fetch(conn_host_, files_port_);
  });
  connect(client_, &FilesClient::brand_splash_cleared, this, [this] {
    splash_path_->clear();
    splash_preview_->clear();
    set_status(QStringLiteral("splash 已清除"));
    BrandKit::instance().fetch(conn_host_, files_port_);
  });
  connect(client_, &FilesClient::brand_logo_uploaded, this,
          [this](qint64 v) {
            set_status(QStringLiteral("logo 已更新（版本 ") +
                       QString::number(v) + QStringLiteral("）"));
            BrandKit::instance().fetch(conn_host_, files_port_);
          });
  connect(client_, &FilesClient::brand_splash_uploaded, this,
          [this](qint64 v) {
            set_status(QStringLiteral("splash 已更新（版本 ") +
                       QString::number(v) + QStringLiteral("）"));
            BrandKit::instance().fetch(conn_host_, files_port_);
          });
  // 统一失败通道：品牌操作失败状态行明示（403 归属文案／服务端语义化
  // 413/415 文案透传）
  connect(client_, &FilesClient::request_failed, this,
          [this](const QString& op, int status, const QString& error) {
            if (!op.startsWith(QStringLiteral("branding"))) return;
            set_status(QStringLiteral("失败（") + QString::number(status) +
                           QStringLiteral("）：") + error,
                       true);
          });
}

void BrandSettingsDialog::build_ui() {
  auto* layout = new QVBoxLayout(this);

  // 连接区（与群工具窗口同构；文件口预填上次值）
  auto* conn = new QHBoxLayout;
  host_ = new QLineEdit(this);
  host_->setPlaceholderText(QStringLiteral("服务器地址"));
  port_ = new QLineEdit(this);
  port_->setPlaceholderText(QStringLiteral("文件端口"));
  port_->setMaximumWidth(90);
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  port_->setText(settings.value(QStringLiteral("files_port"),
                                QStringLiteral("24362"))
                     .toString());
  account_box_ = new QLineEdit(this);
  account_box_->setPlaceholderText(QStringLiteral("账号"));
  account_box_->setMaximumWidth(110);
  password_ = new QLineEdit(this);
  password_->setPlaceholderText(QStringLiteral("口令"));
  password_->setEchoMode(QLineEdit::Password);
  btn_connect_ = new QPushButton(QStringLiteral("连接"), this);
  conn->addWidget(host_);
  conn->addWidget(port_);
  conn->addWidget(account_box_);
  conn->addWidget(password_);
  conn->addWidget(btn_connect_);
  layout->addLayout(conn);

  // 展示区：当前配置概要（连接后回显）
  brand_now_ = new QLabel(QStringLiteral("当前：未连接"), this);
  brand_now_->setWordWrap(true);
  layout->addWidget(brand_now_);

  // 编辑区：文本三件＋素材两件
  auto* form = new QFormLayout;
  company_box_ = new QLineEdit(this);
  company_box_->setPlaceholderText(QStringLiteral("留空=清空公司名"));
  form->addRow(QStringLiteral("公司名"), company_box_);

  auto* accent_row = new QHBoxLayout;
  accent_box_ = new QLineEdit(this);
  accent_box_->setPlaceholderText(QStringLiteral("#rrggbb，留空=清空"));
  accent_row->addWidget(accent_box_);
  auto* btn_color = new QPushButton(QStringLiteral("取色…"), this);
  accent_row->addWidget(btn_color);
  form->addRow(QStringLiteral("主题色"), accent_row);

  slogan_box_ = new QLineEdit(this);
  slogan_box_->setPlaceholderText(QStringLiteral("登录页文案，留空=清空"));
  form->addRow(QStringLiteral("登录页文案"), slogan_box_);

  auto* logo_row = new QHBoxLayout;
  logo_path_ = new QLineEdit(this);
  logo_path_->setPlaceholderText(QStringLiteral("PNG 文件路径"));
  logo_row->addWidget(logo_path_);
  btn_pick_logo_ = new QPushButton(QStringLiteral("选择…"), this);
  logo_row->addWidget(btn_pick_logo_);
  form->addRow(QStringLiteral("logo（≤512KiB，建议 512×512）"), logo_row);
  logo_preview_ = new QLabel(this);
  logo_preview_->setFixedSize(72, 72);
  form->addRow(QString(), logo_preview_);

  auto* splash_row = new QHBoxLayout;
  splash_path_ = new QLineEdit(this);
  splash_path_->setPlaceholderText(QStringLiteral("PNG 文件路径（可选件）"));
  splash_row->addWidget(splash_path_);
  btn_pick_splash_ = new QPushButton(QStringLiteral("选择…"), this);
  splash_row->addWidget(btn_pick_splash_);
  form->addRow(QStringLiteral("splash（≤512KiB，建议 1024×768）"),
               splash_row);
  splash_preview_ = new QLabel(this);
  splash_preview_->setFixedSize(160, 90);
  form->addRow(QString(), splash_preview_);

  layout->addLayout(form);

  // 动作区：保存文本／上传素材／清除两件
  auto* actions = new QHBoxLayout;
  btn_save_text_ = new QPushButton(QStringLiteral("保存文本"), this);
  btn_upload_ = new QPushButton(QStringLiteral("上传素材"), this);
  btn_clear_logo_ = new QPushButton(QStringLiteral("清除 logo"), this);
  btn_clear_splash_ = new QPushButton(QStringLiteral("清除 splash"), this);
  actions->addWidget(btn_save_text_);
  actions->addWidget(btn_upload_);
  actions->addWidget(btn_clear_logo_);
  actions->addWidget(btn_clear_splash_);
  actions->addStretch(1);
  layout->addLayout(actions);

  status_ = new QLabel(this);
  layout->addWidget(status_);

  connect(btn_connect_, &QPushButton::clicked, this, [this] {
    connect_to(host_->text(), port_->text().toUShort(),
               account_box_->text(), password_->text());
  });
  connect(btn_color, &QPushButton::clicked, this, [this] {
    const QColor c = QColorDialog::getColor(
        QColor(accent_box_->text()), this, QStringLiteral("选择主题色"));
    if (c.isValid()) accent_box_->setText(c.name(QColor::HexRgb));
  });
  connect(btn_pick_logo_, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 logo"), QString(),
        QStringLiteral("PNG 图片 (*.png)"));
    if (!path.isEmpty()) pick_logo(path);
  });
  connect(btn_pick_splash_, &QPushButton::clicked, this, [this] {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择 splash"), QString(),
        QStringLiteral("PNG 图片 (*.png)"));
    if (!path.isEmpty()) pick_splash(path);
  });
  connect(btn_save_text_, &QPushButton::clicked, this,
          [this] { save_text(); });
  connect(btn_upload_, &QPushButton::clicked, this,
          [this] { upload_assets(); });
  connect(btn_clear_logo_, &QPushButton::clicked, this,
          [this] { clear_logo(); });
  connect(btn_clear_splash_, &QPushButton::clicked, this,
          [this] { clear_splash(); });
}

void BrandSettingsDialog::connect_to(const QString& host, quint16 files_port,
                                     const QString& acc,
                                     const QString& pass) {
  if (host.isEmpty() || acc.isEmpty()) {
    set_status(QStringLiteral("服务器地址与账号不能为空"), true);
    return;
  }
  if (files_port == 0) {
    set_status(QStringLiteral("文件面端口非法"), true);
    return;
  }
  conn_host_ = host;
  files_port_ = files_port;
  QSettings settings(QStringLiteral("memex"), QStringLiteral("collab"));
  settings.setValue(QStringLiteral("files_port"), QString::number(files_port));
  btn_connect_->setEnabled(false);
  client_->login(host, files_port, acc, pass);
}

bool BrandSettingsDialog::is_connected() const {
  return client_->is_logged_in();
}

void BrandSettingsDialog::refresh() { client_->fetch_branding(); }

void BrandSettingsDialog::set_company(const QString& text) {
  company_box_->setText(text);
}

void BrandSettingsDialog::set_accent(const QString& text) {
  accent_box_->setText(text);
}

void BrandSettingsDialog::set_slogan(const QString& text) {
  slogan_box_->setText(text);
}

bool BrandSettingsDialog::save_text() {
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  // accent 本地形态预检（7 字符 #rrggbb，大小写皆可＝与服务端口径一致；
  // 坏形态不发网，服务端同款裁决兜底；显式空串=清空放行）
  const QString accent = accent_box_->text().trimmed();
  bool accent_ok = accent.isEmpty();
  if (!accent_ok) {
    accent_ok = accent.size() == 7 && accent[0] == QLatin1Char('#');
    for (int i = 1; accent_ok && i < 7; ++i) {
      const QString hex = QStringLiteral("0123456789abcdefABCDEF");
      accent_ok = hex.contains(accent[i]);
    }
  }
  if (!accent_ok) {
    set_status(QStringLiteral("主题色须为 #rrggbb 形态"), true);
    return false;
  }
  client_->save_branding(company_box_->text().trimmed(), accent,
                         slogan_box_->text().trimmed());
  return true;
}

bool BrandSettingsDialog::pick_logo(const QString& path) {
  QPixmap pm(path);
  if (pm.isNull()) {
    set_status(QStringLiteral("图片打不开（须 PNG）"), true);
    return false;
  }
  logo_path_->setText(path);
  logo_preview_->setPixmap(
      pm.scaled(logo_preview_->size(), Qt::KeepAspectRatio,
                Qt::SmoothTransformation));
  return true;
}

bool BrandSettingsDialog::pick_splash(const QString& path) {
  QPixmap pm(path);
  if (pm.isNull()) {
    set_status(QStringLiteral("图片打不开（须 PNG）"), true);
    return false;
  }
  splash_path_->setText(path);
  splash_preview_->setPixmap(
      pm.scaled(splash_preview_->size(), Qt::KeepAspectRatio,
                Qt::SmoothTransformation));
  return true;
}

bool BrandSettingsDialog::upload_assets() {
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  const QString logo = logo_path_->text().trimmed();
  const QString splash = splash_path_->text().trimmed();
  if (logo.isEmpty() && splash.isEmpty()) {
    set_status(QStringLiteral("未选择素材文件"), true);
    return false;
  }
  if (!logo.isEmpty()) client_->upload_brand_logo(logo);
  if (!splash.isEmpty()) client_->upload_brand_splash(splash);
  return true;
}

bool BrandSettingsDialog::clear_logo() {
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->clear_brand_logo();
  return true;
}

bool BrandSettingsDialog::clear_splash() {
  if (!is_connected()) {
    set_status(QStringLiteral("未连接文件面"), true);
    return false;
  }
  client_->clear_brand_splash();
  return true;
}

QString BrandSettingsDialog::status_text() const { return status_->text(); }

QPixmap BrandSettingsDialog::logo_preview() const {
  return logo_preview_->pixmap();
}

void BrandSettingsDialog::apply_branding(const QJsonObject& b) {
  const QString company = b.value(QStringLiteral("company_name")).toString();
  const QString accent = b.value(QStringLiteral("accent")).toString();
  const QString slogan = b.value(QStringLiteral("slogan")).toString();
  const bool has_logo = b.value(QStringLiteral("has_logo")).toBool();
  const bool has_splash = b.value(QStringLiteral("has_splash")).toBool();
  const qint64 version = static_cast<qint64>(
      b.value(QStringLiteral("version")).toDouble(0));
  brand_now_->setText(
      QStringLiteral("当前：公司名 %1 · 主题色 %2 · 文案 %3 · logo %4 · "
                     "splash %5 · 版本 %6")
          .arg(company.isEmpty() ? QStringLiteral("（未配）") : company,
               accent.isEmpty() ? QStringLiteral("（未配）") : accent,
               slogan.isEmpty() ? QStringLiteral("（未配）") : slogan,
               has_logo ? QStringLiteral("有") : QStringLiteral("无"),
               has_splash ? QStringLiteral("有") : QStringLiteral("无"),
               QString::number(version)));
  // 编辑区初始值=当前配置（占位提示已注明留空语义）
  company_box_->setText(company);
  accent_box_->setText(accent);
  slogan_box_->setText(slogan);
}

void BrandSettingsDialog::set_status(const QString& text, bool error) {
  status_->setText(text);
  status_->setStyleSheet(error ? QStringLiteral("color:#c0392b;")
                               : QString());
}

} // namespace memex::client
