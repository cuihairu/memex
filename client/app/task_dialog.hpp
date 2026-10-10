// R27-1 个人任务清单窗口：自建/完成/提醒 + 他人分配（服务端按权限模型
// 「同群/同部门才能互派」判权）。独立文件面会话（与 R23-3 文件助手同构：
// 协作面同源账号、文件面独立端口）。提醒=到期待办经通知中心强提醒，
// 回执落服务端只提醒一次；30s 自动轮询，关窗=hide 后台驻留（连接与
// 轮询持续，后台提醒常驻——R27-1 留尾收口），重开即现窗不重建。
// R27-3 拉取接线：已配置凭据（设置面→TaskProviderStore 加密落盘）的
// L2 provider 直拉外部任务列表展示（⇣ 行，只读——登记后才能在清单标记）；
// 外部拉取不经 memex 服务端，断连也可用。
// R27-1 余量 日历视图：月历标到期日＋点日筛选该日到期（默认收起）。
#pragma once

#include <QDate>
#include <QDialog>
#include <QHash>
#include <QJsonArray>
#include <QString>
#include <QStringList>

#include <QtGlobal>

#include "engine/task/task_provider.hpp"

class QCalendarWidget;
class QComboBox;
class QDateTimeEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace memex::client {

class FilesClient;
class TaskHttp;
class TaskProviderStore;

class TaskDialog : public QDialog {
  Q_OBJECT
 public:
  // http/store 测试注入点（缺省=QtNetworkTaskHttp＋单例存储）
  explicit TaskDialog(QWidget* parent = nullptr, TaskHttp* http = nullptr,
                      TaskProviderStore* store = nullptr);
  ~TaskDialog() override;

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;
  // 分配目标候选（组织架构账号；开窗前由主窗喂入）
  void set_assignees(const QStringList& accounts);

  // 程序化入口（测试共用）：标题为空不发；assignee 空=自建、
  // due_ms<=0=不设提醒
  bool add_task(const QString& title, const QString& note, qint64 due_ms,
                const QString& assignee);
  // R27-2 外部任务登记（程序化入口）：键书写「project#键」或完整链接
  //（无 # = 整串为键）。detailUrl 必带——provider 未知/解析不出 URL
  // 本地拒不发网；标题空取键原文兜底。外部条目个人登记（不转派），
  // 完成勾选=memex 本地进度标记（回写外部是 L3，随 R27-3 实做）。
  bool add_external_task(const QString& provider_id, const QString& key_input,
                         const QString& title);
  // 选中条目勾完成/回退（勾选框双击同效）
  bool toggle_selected_done();
  // 撤回选中条目（清单主人或分配人，服务端裁决）
  bool delete_selected();
  // 拉列表（我的清单＋我派出的）
  void refresh();
  // 到期待办检查：未完成、已到期、未提醒过 → 通知中心强提醒＋服务端回
  // 执（只提醒一次）。轮询定时器与测试共用同一入口。
  void check_due();

  // —— R27-3 拉取接线 ——
  // 凭据 store 重建 live provider（已配置且可读者入拉取下拉）
  void rebuild_live_providers();
  // 拉取指定 provider 的外部任务（按钮与测试共用同一入口）；结果进
  // 会话缓存并渲染 ⇣ 只读行
  void pull_external(const QString& provider_id);
  // 可拉取 provider id（live 已配置＋声明 L2）
  QStringList pull_provider_ids() const;

  // —— R27-1 余量 日历视图 ——
  // 月历标到期日（品牌橙加粗），点日筛该日到期任务；无到期数据的行
  //（派出行/⇣ 行/未设提醒项）筛选态下隐藏。再点同日或清筛选回全量。
  QCalendarWidget* calendar() const { return calendar_; }
  QPushButton* btn_calendar() const { return btn_calendar_; }
  // 点日（点按与测试共用）：d 无效=清筛选回全量；同日再点=切换清
  void toggle_day_filter(const QDate& d);
  // 当前筛选日（无效=未筛选）
  QDate day_filter() const { return day_filter_; }
  // 筛选后可见任务行数
  int visible_task_count() const;

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int task_count() const;      // 列表总条数（我的清单＋我派出的）
  QListWidget* list() const { return list_; }
  // 选中条目 id（未选中=-1）
  qint64 selected_id() const;
  // 选中条目的外部详情 URL（本地任务/未选中/解析不出=空串）
  QString selected_detail_url() const;
  // 登记下拉条数与模板设置按钮（接线面测试共用）
  int ext_combo_count() const;
  QPushButton* btn_templates() const { return btn_templates_; }

 private:
  void build_ui();
  void refresh_ext_combo(); // 登记下拉（预设＋自定义模板）
  void populate(const QJsonArray& mine, const QJsonArray& assigned);
  // 会话缓存的外部拉取行渲染（⇣ 行；UserRole+6=true 只读标记）
  void render_ext_rows();
  bool selected_row_pulled() const;
  // 日历视图：重标到期日（先清全表再标，刷新后重入不叠色）
  void update_day_marks();
  // 按 day_filter_ 施加行可见性；回可见行数
  int apply_day_filter();
  void set_status(const QString& text, bool error = false);
  // 登记键原文 → （project, key）——「project#键」或整串为键
  static void split_ext_key(const QString& raw, QString& project,
                            QString& key);

  FilesClient* client_;
  QString account_;
  TaskProviderRegistry providers_;
  TaskHttp* http_{nullptr};       // 非拥有则注入；自建则随窗同亡
  bool http_owned_{false};
  TaskProviderStore* store_;
  QHash<QString, TaskProvider*> live_; // 凭据重建的 live provider（本窗拥有）
  QHash<QString, QVector<ExternalTask>> ext_cache_; // 拉取会话缓存
  QLineEdit* host_;
  QLineEdit* port_;
  QLineEdit* account_box_;
  QLineEdit* password_;
  QLabel* status_;
  QListWidget* list_;
  QLineEdit* title_;
  QLineEdit* note_;
  QDateTimeEdit* due_;
  QComboBox* assignee_;
  QComboBox* ext_provider_;
  QLineEdit* ext_key_;
  QComboBox* pull_provider_;
  QPushButton* btn_connect_;
  QPushButton* btn_add_;
  QPushButton* btn_add_ext_;
  QPushButton* btn_pull_;
  QPushButton* btn_settings_;
  QPushButton* btn_templates_;
  QPushButton* btn_toggle_;
  QPushButton* btn_delete_;
  QPushButton* btn_refresh_;
  QPushButton* btn_calendar_;
  QCalendarWidget* calendar_;
  QDate day_filter_; // 无效=未筛选
};

} // namespace memex::client
