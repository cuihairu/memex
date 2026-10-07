// R24-2 群备忘录窗口：条目化共享知识（标题+正文）的列表/搜索/编辑/
// 历史回滚/开放编辑开关。独立文件面会话（与 R23-3 文件助手同构：协作面
// 同源账号、文件面独立端口；权限按服务端裁决——写=群主/管理员或开放编辑
// 时全员，删恒归群主/管理员）。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

#include <QtGlobal>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace memex::client {

class FilesClient;

class GroupMemoDialog : public QDialog {
  Q_OBJECT
 public:
  explicit GroupMemoDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;
  // 切目标群（懒建复用同一窗口时换群：清列表重拉）
  void set_group(quint64 gid, const QString& group_name);

  // 程序化入口（测试共用）：editing_id_==0 新建、>0 保存修改；
  // 标题或正文为空不发
  bool submit_entry(const QString& title, const QString& content);
  // 选中条目进编辑态（等同点「编辑」，再进一次＝取消）
  bool edit_selected();
  // 按搜索框现值拉列表（空＝全部）
  void refresh();
  // 打开历史与回滚子对话框（拉修订史、选中笔可回滚）
  void open_history();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int stream_count() const;
  QListWidget* stream() const { return stream_; }
  QLineEdit* search_box() const { return search_; }
  bool open_edit_checked() const; // 开关勾态（列表回包同步后的现值）
  // 历史子对话框当前条数（未打开过/已关＝-1）
  int history_count() const;
  QListWidget* history_list() const { return hist_list_; }
  // 回滚历史窗中选中的修订笔（「回滚到选中这笔」按钮与测试共用入口）
  void rollback_selected_rev();

 private:
  void build_ui();
  // 标题+正文编辑子对话框（手工新建/编辑共用；OK 回传两字段，全非空）
  bool edit_dialog(QString* title, QString* content,
                   const QString& init_title, const QString& init_content);
  void set_status(const QString& text, bool error = false);
  void end_edit();

  FilesClient* client_;
  quint64 gid_{0};
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_;
  QLineEdit* password_;
  QLineEdit* search_;
  QCheckBox* open_edit_;
  QLabel* status_;
  QListWidget* stream_;
  QPushButton* btn_connect_;
  QPushButton* btn_new_;
  QPushButton* btn_edit_;
  QPushButton* btn_history_;
  QPushButton* btn_delete_;
  QPushButton* btn_refresh_;
  qint64 editing_id_{0}; // 0=新建；>0=正在改的条目 id
  // 历史子对话框列表（非模态存活期观察点；关闭置空）
  QListWidget* hist_list_{nullptr};
  qint64 hist_memo_id_{0}; // 历史窗当前查看的条目 id（回滚目标）
};

} // namespace memex::client
