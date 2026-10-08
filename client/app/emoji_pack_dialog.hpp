// 表情包云素材管理（需求批②）：服务端个人素材面——上传 GIF/PNG/JPG、
// 清单、删除、下载到本地表情目录（发送走既有自定义表情文件通道）。
// 连接行与 FileAssistantDialog 同惯例（与协作面同源账号口令、文件面
// 独立端口）；素材判权＝仅本人（服务端统一裁决）。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

#include <QtGlobal>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace memex::client {

class FilesClient;

class EmojiPackDialog : public QDialog {
  Q_OBJECT
 public:
  // local_dir＝下载落点（主窗自定义表情目录——下载即可在面板发送）
  explicit EmojiPackDialog(QWidget* parent = nullptr,
                           const QString& local_dir = QString());

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;
  // 拉清单
  void refresh();
  // 程序化上传（快捷键/测试共用；name 空＝取文件基名）
  bool upload_from(const QString& file_path, const QString& name = QString());
  // 下载当前选中到本地表情目录（按钮与测试共用同一入口）
  void download_selected();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int asset_count() const;
  QListWidget* list();

 private:
  void build_ui();
  void populate(const QJsonArray& assets);
  void set_status(const QString& text, bool error = false);

  FilesClient* client_;
  QString local_dir_;
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_;
  QLineEdit* password_;
  QListWidget* assets_;
  QLabel* status_;
  QPushButton* btn_connect_;
  QPushButton* btn_upload_;
  QPushButton* btn_delete_;
  QPushButton* btn_download_;
  QPushButton* btn_refresh_;
};

} // namespace memex::client
