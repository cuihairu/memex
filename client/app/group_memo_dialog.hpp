// R24-2 群备忘录窗口：条目化共享知识（标题+正文）的列表/搜索/编辑/
// 历史回滚/开放编辑开关。独立文件面会话（与 R23-3 文件助手同构：协作面
// 同源账号、文件面独立端口；权限按服务端裁决——写=群主/管理员或开放编辑
// 时全员，删恒归群主/管理员）。界面按 prototypes/desktop-group-memo.html
//（左列表列 430px，右内联编辑卡 + 内联修订历史面板）。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

#include <QtGlobal>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
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
  // 展开内联修订历史面板（拉修订史、选中笔可回滚）
  void open_history();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int stream_count() const;
  QListWidget* stream() const { return stream_; }
  QLineEdit* search_box() const { return search_; }
  bool open_edit_checked() const;  // 开关勾态（列表回包同步后的现值）
  // 内联历史面板当前条数（未展开/已关闭＝-1）
  int history_count() const;
  QListWidget* history_list() const { return hist_list_; }
  // 回滚历史面板中选中的修订笔（「回滚到选中这笔」按钮与测试共用入口）
  void rollback_selected_rev();
  // R24-4 疑似密码提示现挂否（最近一次提交触发；正常内容自动回落 false）
  bool secret_hint_shown() const { return secret_hint_; }

 private:
  void build_ui();
  // 读入内联历史面板条目（内联面板无对话框生命周期，连接挂本窗口）
  void populate_history(const QJsonArray& revisions);
  // 收起内联历史面板（回滚落笔/关闭；收起后无历史可回滚）
  void close_history();
  // R24-4 手工路径疑似密码二次确认（无疑似或用户坚持＝放行）
  bool confirm_secret_memo(const QString& title, const QString& content);
  void set_status(const QString& text, bool error = false);
  void end_edit();
  // 「只看我建的」客户端过滤（作者≠当前账号的行隐藏）
  void apply_mine_filter();
  // 「保存」可用态：卡片标题与正文均非空
  void update_save_enabled();
  // 卡片头注「条目 #N/新建 · 开放成员编辑（已开/未开）」
  void update_edit_note(const QString& override_text = QString());

  FilesClient* client_;
  quint64 gid_{0};
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_;
  QLineEdit* password_;
  QLabel* gid_chip_{nullptr};  // 连接行群号（set_group 后显示）
  QLineEdit* search_;
  QCheckBox* mine_only_{nullptr};  // 「只看我建的」客户端过滤（原型 .chk）
  QCheckBox* open_edit_;
  QLabel* status_;
  QListWidget* stream_;
  // 内联编辑卡（原型 edit-col：标题 + 正文 + 动作行）
  QLabel* card_title_{nullptr};    // 「备忘录条目 · <标题/新建>」
  QLineEdit* title_edit_{nullptr};
  QPlainTextEdit* content_edit_{nullptr};
  QLabel* edit_head_note_{nullptr};  // 「条目 #N · 开放成员编辑（已开/未开）」
  // 内联修订历史面板
  QLabel* hist_head_{nullptr};    // 「条目修订历史（#N）」
  QListWidget* hist_list_{nullptr};
  QPushButton* btn_roll_{nullptr};
  QWidget* hist_panel_{nullptr};
  qint64 editing_id_{0};       // 0=新建；>0=正在改的条目 id
  bool secret_hint_{false};    // R24-4 疑似密码提示挂起（列表刷新时随状态行带出）
  qint64 hist_memo_id_{0};     // 历史面板当前查看的条目 id（回滚目标）
  QPushButton* btn_connect_;
  QPushButton* btn_new_;
  QPushButton* btn_save_;  // 原型 btnrow 四键外的功能键：提交卡片现值
  QPushButton* btn_edit_;
  QPushButton* btn_history_;
  QPushButton* btn_delete_;
  QPushButton* btn_refresh_;
};

} // namespace memex::client
