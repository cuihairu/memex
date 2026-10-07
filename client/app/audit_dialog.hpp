// 二期·会话审计窗口：归档在线检索（按人/关键词/时间窗，条件全可空=
// 全量）＋查阅日志台账自阅。持 auditor 有效角色方可查（平台-6 口径
// SecurityAuditor≠SystemAdmin）；每次检索服务端落查阅日志，被拒尝试
// 也留痕（audit.denied 可对账）。独立文件面会话（与 R23-3 同构）。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

#include <QtGlobal>

class QCheckBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace memex::client {

class FilesClient;

class AuditDialog : public QDialog {
  Q_OBJECT
 public:
  explicit AuditDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;

  // 程序化入口（测试共用）：条件全部透传（空=不过滤，服务端全量语义）
  bool run_search(const QString& account, const QString& keyword,
                  qint64 since_ms, qint64 until_ms);
  // 拉查阅日志（台账自阅）
  void refresh_reads();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int result_count() const;
  int read_count() const;
  QListWidget* list() const { return list_; }
  QListWidget* reads_list() const { return reads_list_; }

 private:
  void build_ui();
  void populate_results(const QJsonArray& messages);
  void populate_reads(const QJsonArray& reads);
  void set_status(const QString& text, bool error = false);

  FilesClient* client_;
  QLineEdit* account_;
  QLineEdit* keyword_;
  QCheckBox* use_window_;
  QDateEdit* since_;
  QDateEdit* until_;
  QLabel* status_;
  QListWidget* list_;
  QListWidget* reads_list_;
  QPushButton* btn_connect_;
  QPushButton* btn_search_;
  QPushButton* btn_reads_;
};

} // namespace memex::client
