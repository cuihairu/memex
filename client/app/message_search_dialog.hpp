// 聊天记录模糊搜索窗：跨会话检索本机聊天记录（子串匹配 + 多关键词 AND +
// 相关度排序，口径见 LocalStore::search_messages）。命中列表按相关度倒序，
// 点击命中＝打开对应会话（MainWindow 按对端键解析会话类型）。
// 搜索只读本机库，不触网、不写库。
#pragma once

#include <QDialog>
#include <QString>

#include <cstdint>

class QLabel;
class QLineEdit;
class QListWidget;

namespace memex::client {

class LocalStore;

class MessageSearchDialog : public QDialog {
  Q_OBJECT
 public:
  explicit MessageSearchDialog(QWidget* parent = nullptr);

  // 绑定本地库（每次打开窗时注入当前库；空=不可搜索）
  void bind_store(LocalStore* store);
  // 程序化入口（测试共用）：按现值检索并回填列表
  void run_search();
  // 选中命中并发出打开信号（等同双击/回车选中行）
  void open_selected();

  // —— 走查/测试观察点 ——
  QLineEdit* search_box() const { return search_; }
  QListWidget* results() const { return list_; }
  int result_count() const;
  QString status_text() const;
  // 当前选中命中的对端键与消息 id（未选中＝空）
  QString selected_peer() const;
  QString selected_msg_id() const;

 signals:
  // 打开命中所在会话（peer＝对端键，msg_id 非空＝尽量定位到该条）
  void open_message(const QString& peer, const QString& msg_id);

 private:
  void build_ui();
  void set_status(const QString& text, bool error = false);

  LocalStore* store_{nullptr};
  QLineEdit* search_{nullptr};
  QListWidget* list_{nullptr};
  QLabel* status_{nullptr};
};

} // namespace memex::client
