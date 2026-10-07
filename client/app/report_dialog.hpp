// 二期·日报周报窗口：个人日报写/看（当日重复提交=服务端 upsert 更新）
// ＋直属上级看下属（团队聚合；判权服务端 az report:read 现裁——直属
// 上级专属，org-admin 不兜底）。周报=日报按周过滤的聚合视图（客户端
// 过滤展示，不单设表）。独立文件面会话（与 R23-3 文件助手同构）。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

#include <QtGlobal>

class QDateEdit;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;

namespace memex::client {

class FilesClient;

class ReportDialog : public QDialog {
  Q_OBJECT
 public:
  explicit ReportDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;

  // 程序化入口（测试共用）：内容空拒（本地门，服务端同门）
  bool write_report(const QString& date, const QString& content);
  // 拉我的日报＋团队聚合（一次双拉）
  void refresh();
  // 团队视图「仅本周」开关（周报=按周过滤聚合；按钮与测试共用同一入口）
  void toggle_team_week();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int report_count() const;   // 我的日报行数
  int team_count() const;     // 团队聚合行数（仅本周开关生效后）
  QListWidget* list() const { return list_; }
  QListWidget* team_list() const { return team_list_; }
  // 编辑器现值（日期/内容——选中行回填观察点）
  QDateEdit* date_edit() const { return date_; }
  QPlainTextEdit* content_edit() const { return content_; }

 private:
  void build_ui();
  void populate(const QJsonArray& reports);
  void populate_team(const QJsonArray& team);
  void set_status(const QString& text, bool error = false);
  // 本周一~周日（YYYY-MM-DD；周报聚合视图的过滤窗）
  static QString week_start();
  static QString week_end();

  FilesClient* client_;
  QDateEdit* date_;
  QPlainTextEdit* content_;
  QLabel* status_;
  QListWidget* list_;
  QListWidget* team_list_;
  QPushButton* btn_connect_;
  QPushButton* btn_save_;
  QPushButton* btn_refresh_;
  QPushButton* btn_team_week_;
  bool team_week_only_{true}; // 团队视图「仅本周」默认开（周报=按周聚合）
  QString account_;
};

} // namespace memex::client
