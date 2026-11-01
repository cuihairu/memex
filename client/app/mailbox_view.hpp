// 邮箱·统一收件箱（原型 desktop-mailbox.html）：告警 / 通知 / 公告三类
// 条目进同一个收件箱——console（标题＋副题＋类型页签计数＋只看未读＋搜索
// ＋全部标记已读＋五列条目表＋未读脚注＋边界注）＋infopanel（条目详情
// kv＋已读确认台账＋告警处理留痕）。
// 设计铁律（mailbox.md）：邮箱＝视图＋已读状态，不是新通道——本视图只
// 经 FilesClient 拉取（条目只 pull 不 push）；清空 / 标记已读只动邮箱
// 状态与留痕，消息归档行 / 公告行 / 告警台账行一概不动（服务端口径）；
// 未读数以服务端为唯一来源（rail 徽章与页签同源同数——unread_changed）。
// 「去会话 / 去任务 / 去审批 / 去群」不在此视图内做路由：emit
// jump_requested(ref) 由 MainWindow 落位（正文留在各自来源）。
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QString>
#include <QWidget>

#include <QtGlobal>

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace memex::client {

class FilesClient;

class MailboxView : public QWidget {
  Q_OBJECT
 public:
  explicit MailboxView(QWidget* parent = nullptr);

  // 注入文件面客户端（MainWindow 懒建后调用；重复调用先解旧连接）
  void set_client(FilesClient* client);
  FilesClient* client() const { return client_; }

  // 拉条目列表（进入视图 / 条件变更 / 动作回执后；未登录静默不拉）
  void refresh();
  // 轻量面：只拉未读计数（rail 徽章刷新用，不动条目表）
  void refresh_unread();

  // —— 走查/测试观察点 ——
  bool view_active() const { return !isHidden(); }
  int row_count() const;                 // 当前行数（过滤后）
  const QJsonArray& items() const { return items_; } // 最近一次回包
  QJsonObject counts() const { return counts_; }     // 四路未读计数
  qint64 selected_id() const { return selected_id_; }
  QString foot_text() const;             // rt-foot 文案（断言用）
  QString status_text() const { return status_ ? status_->text() : QString(); }

 signals:
  // 未读总数变化（服务端 counts.all；rail 徽章与页签同源同数）
  void unread_changed(int total);
  // 「去会话/去任务/去审批/去群」跳转（ref=服务端条目 ref/jump_url；
  // group:N→群会话、approvals→审批、tasks→任务；其余由主窗酌情）
  void jump_requested(const QString& ref);

 private:
  void build_ui();
  void restyle();                 // 主题令牌 → 样式（构造一次＋theme_changed）
  void request_list();            // 按当前条件发拉取（type/unread/q）
  void populate_rows();           // items_ → 五列行（全量重建）
  void populate_tabs();           // counts → 页签文案
  void select_item(qint64 id);    // 行点击 → 详情面板（公告拉确认台账）
  void refresh_detail();          // 详情/台账/留痕随 items_ 与选中态重建
  void show_status(const QString& text, bool error = false);
  void on_items(const QJsonArray& items, const QJsonObject& counts);

  FilesClient* client_{nullptr};
  QJsonObject counts_;            // {all,warning,notice,announce} 未读计数
  QJsonArray items_;              // 最近一次条目回包
  QString type_filter_;           // 空=全部 | warning | notice | announce
  bool unread_only_{false};
  QString query_;
  qint64 selected_id_{0};

  QLabel* sub_title_{nullptr};
  QPushButton* tab_btns_[4] = {nullptr, nullptr, nullptr, nullptr};
  QPushButton* unread_toggle_{nullptr};
  QLineEdit* search_{nullptr};
  QPushButton* mark_all_btn_{nullptr};
  QWidget* table_host_{nullptr};   // 表头＋行容器（滚动区内）
  QVBoxLayout* rows_lay_{nullptr};
  QLabel* empty_hint_{nullptr};
  QLabel* foot_{nullptr};
  QLabel* status_{nullptr};
  QWidget* detail_panel_{nullptr}; // infopanel（条目详情＋台账＋留痕）
  QVBoxLayout* detail_lay_{nullptr};
};

}  // namespace memex::client
