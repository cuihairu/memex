// 二期·品牌物料设置页（设计稿 docs/design/品牌物料.md §4）：连接文件面
// →回显当前配置→编辑保存。判权全在服务端（branding-manage=org-admin，
// 403 状态行明示）；保存成功后触发 BrandKit 本机 fetch（设置即时生效＝
// 本机先见）。独立文件面会话（与审批/群工具窗口同构）。
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QPixmap>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;

namespace memex::client {

class FilesClient;

class BrandSettingsDialog : public QDialog {
  Q_OBJECT
 public:
  explicit BrandSettingsDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;

  // —— 程序化入口（测试共用；服务端裁决一切判权/校验）——
  // 拉当前配置回显（展示区＋编辑区初始值）
  void refresh();
  // 保存文本三件（空行=清空该字段；accent 非空须 #rrggbb，坏形态本地
  // 先拒不发网——服务端同款裁决兜底）
  bool save_text();
  // 程序化填表（测试缝；与行内编辑同一字段）
  void set_company(const QString& text);
  void set_accent(const QString& text);
  void set_slogan(const QString& text);
  // 素材文件选中（测试注入路径绕开 QFileDialog；PNG 预览即时可见）
  bool pick_logo(const QString& path);
  bool pick_splash(const QString& path);
  // 上传已选素材（两件都未选=no-op 回 true；服务端校验类型/大小/尺寸）
  bool upload_assets();
  bool clear_logo();
  bool clear_splash();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  QPixmap logo_preview() const;

 private:
  void build_ui();
  void apply_branding(const QJsonObject& branding);
  void set_status(const QString& text, bool error = false);

  FilesClient* client_;
  QString conn_host_;        // 已连接目标（保存后本机 BrandKit fetch 用）
  quint16 files_port_{0};
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_box_;
  QLineEdit* password_;
  QPushButton* btn_connect_;
  QLabel* brand_now_;        // 展示区：当前配置概要
  QLineEdit* company_box_;   // 编辑区：公司名
  QLineEdit* accent_box_;    // 编辑区：主题色 #rrggbb（＋取色按钮）
  QLineEdit* slogan_box_;    // 编辑区：登录页文案
  QLineEdit* logo_path_;     // 编辑区：logo 文件路径＋选择按钮
  QLineEdit* splash_path_;
  QLabel* logo_preview_;     // 本地选中即时预览
  QLabel* splash_preview_;
  QPushButton* btn_pick_logo_;
  QPushButton* btn_pick_splash_;
  QPushButton* btn_save_text_;
  QPushButton* btn_upload_;
  QPushButton* btn_clear_logo_;
  QPushButton* btn_clear_splash_;
  QLabel* status_;
};

} // namespace memex::client
