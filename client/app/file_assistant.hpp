// R23-3 块2：文件助手会话窗口（备忘录文本 + 收件箱统一收件）。
// 独立文件面会话：与协作面同源账号口令、服务端文件面独立端口；
// 字节面（上传/下载）同走 FileServer——客户端永不直连对象存储（铁律）。
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

class FileAssistantDialog : public QDialog {
  Q_OBJECT
 public:
  explicit FileAssistantDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;

  // 输入框现有文本入库：编辑态（editing_id_>0）改既有条目、否则新建；
  // 空文本不发
  bool submit_memo();
  // 程序化入口（快捷键/测试共用）：写正文后走 submit_memo
  bool submit_text(const QString& text);
  // 选中备忘录进入编辑态（等同点「编辑备忘录」，再点取消）；
  // 无选中或选中的不是备忘录返回 false
  bool edit_selected();
  // 刷新收件箱混排流
  void refresh();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int stream_count() const;
  QListWidget* stream() const { return stream_; }

 private:
  void build_ui();
  void populate(const QJsonArray& items);
  void set_status(const QString& text, bool error = false);
  void end_edit();

  FilesClient* client_;
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_;
  QLineEdit* password_;
  QLineEdit* input_;
  QLabel* status_;
  QListWidget* stream_;
  QPushButton* btn_connect_;
  QPushButton* btn_submit_;
  QPushButton* btn_edit_;
  QPushButton* btn_delete_;
  QPushButton* btn_download_;
  QPushButton* btn_upload_;
  QPushButton* btn_refresh_;
  qint64 editing_id_{0}; // 0=新建；>0=正在改的备忘录 id
};

} // namespace memex::client
