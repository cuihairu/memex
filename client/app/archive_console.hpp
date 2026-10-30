// 管理台·归档检索与导出（原型 desktop-search.html）：auditor 专属整窗
// 控制台——条件检索（人员/时间窗/关键词）＋五列命中表（高亮/会话对端/
// 归档时间/标记/操作）＋查看上下文＋撤回事件＋导出留证＋查阅日志台账。
// 判权全在服务端（az audit:read——auditor 有效角色；被拒尝试留痕可对账），
// 本窗只提交条件、展示结果、把查阅行为交给服务端留痕（每次检索/上下文/
// 撤回事件/导出各落一条 audit_reads 行——铁律：明文呈现均留痕）。
// 与「会话审计」对话框（desktop-audit）分立：同后端、两原型两形态。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

#include <QtGlobal>

class QCheckBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace memex::client {

class FilesClient;

class ArchiveConsole : public QDialog {
  Q_OBJECT
 public:
  explicit ArchiveConsole(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;

  // 程序化入口（测试共用）：条件透传（空=不过滤）；须已连接
  bool run_search(const QString& account, const QString& keyword,
                  qint64 since_ms, qint64 until_ms);
  // 「查看上下文」／「撤回事件」程序化入口（行按钮同路径）
  void open_context(const QString& msg_id);
  void open_recall_event(const QString& msg_id);
  // 「导出留证」程序化入口：服务端重跑＋留痕后把证据文件写到 path
  //（与按钮同路径——按钮只是多一步文件对话框选路径）
  void export_to(const QString& path);

  // —— 走查/测试观察点 ——
  int result_count() const;
  QListWidget* reads_list() const { return reads_list_; }
  const QJsonArray& results() const { return results_; }
  QString status_text() const;
  QString evidence_text() const;

 private:
  void build_ui();
  void restyle();            // 主题令牌 → 本窗样式（构造时一次）
  void populate_results(const QJsonArray& messages);
  void populate_reads(const QJsonArray& reads);
  void set_status(const QString& text, bool error = false);
  QString highlight(const QString& text) const;  // 关键词命中 → 富文本
  QString row_pair(const QJsonObject& m) const;  // 会话/对端列文案

  FilesClient* client_;
  QLineEdit* host_{nullptr};
  QLineEdit* port_{nullptr};
  QLineEdit* conn_account_{nullptr};
  QLineEdit* conn_password_{nullptr};
  QPushButton* btn_connect_{nullptr};
  QWidget* conn_row_{nullptr};
  QLabel* badge_{nullptr};           // 「● 协作服务 <host>」／「○ 未连接」
  QLineEdit* account_{nullptr};      // 人员（账号，可空＝全部）
  QCheckBox* use_window_{nullptr};
  QDateEdit* since_{nullptr};
  QDateEdit* until_{nullptr};
  QLineEdit* keyword_{nullptr};
  QPushButton* btn_search_{nullptr};
  QWidget* rows_host_{nullptr};      // 命中行容器（滚动区内）
  QVBoxLayout* rows_lay_{nullptr};
  QLabel* foot_count_{nullptr};
  QPushButton* btn_export_{nullptr};
  QLabel* evidence_{nullptr};        // 导出留证行（证据包路径＋条数＋操作人）
  QListWidget* reads_list_{nullptr};
  QLabel* status_{nullptr};
  QJsonArray results_;               // 最近一次检索回包（导出/上下文按钮共用）
  QString last_export_path_;         // 最近一次证据文件路径（evidence 行回显）
};

}  // namespace memex::client
