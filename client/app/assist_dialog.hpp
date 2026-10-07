// 二期·远程协助窗口：双角色同窗——「我发起协助」（发起/启动/结束＋
// 远端画面＋点画面发鼠标事件）与「别人协助我」（待批 consent＋实批
// 子集／共享中持续可见＋随时结束=撤权＋推帧共享）。模型层（平台-11）
// 的 consent/audit 红线与部门放行开关全在服务端，客户端只做门面；
// 媒体走文件面 HTTP 中继（最小闭环，WebRTC P2P 留后续）。独立文件面
// 会话（与 R23-3 同构）；500ms 轮询拉会话台账=过程持续可见。
#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QString>

#include <QtGlobal>

class QLabel;
class QLineEdit;
class QListWidget;
class QCheckBox;
class QPushButton;
class QTimer;

namespace memex::client {

class FilesClient;

class AssistDialog : public QDialog {
  Q_OBJECT
 public:
  explicit AssistDialog(QWidget* parent = nullptr);

  // 连接文件面（连接按钮与测试共用同一入口）
  void connect_to(const QString& host, quint16 files_port,
                  const QString& account, const QString& password);
  bool is_connected() const;

  // —— 程序化入口（按钮与测试同源）——
  // 发起协助（perms 空=按勾选；显式给出=程序化入口直传）
  void request_assist(const QString& target,
                      const QStringList& perms = {});
  // 批/拒最早的待批请求（perms 空=按实批勾选；服务端仍校验 ⊆ 申请集）
  void approve_pending(bool allow, const QStringList& perms);
  // 启动/结束选中会话（发起方侧）
  void start_selected();
  void end_selected(bool with_reason = true);
  // 受控方侧共享开关（推帧定时器起停）
  void start_sharing();
  void stop_sharing();
  // 测试注帧（离屏环境 QScreen 抓屏不可靠——绕真抓屏直推合成帧）
  void push_frame_once(const QString& jpeg_b64);
  // 程序化选中会话行（与点击同语义；启动/结束/审计按选中行走）
  void select_session(int row);
  // 程序化点远端画面（构造同款鼠标事件走真 eventFilter 路径）
  void click_frame(double nx, double ny);
  // 拉选中会话的审计链（「看审计链」按钮同源）
  void show_audit();

  // —— 走查/测试观察点 ——
  QString status_text() const;
  int session_count() const;   // 会话台账行数
  int pending_count() const;   // 待批请求行数
  int audit_count() const;     // 审计链行数
  int input_log_count() const; // 受控侧收到的输入事件行数
  QString session_text(int row) const;
  QString selected_session() const;
  qint64 frame_seq_seen() const; // 远端画面最近帧号（0=未收到）
  QString share_text() const;  // 共享中指示文本
  QStringList grant_checks() const; // 实批勾选（勾中项）

 private:
 protected:
  // 画面点选→发鼠标事件（归一化坐标随事件发网；mouse 权限位服务端门）
  bool eventFilter(QObject* watched, QEvent* ev) override;

 private:
  void build_ui();
  void set_status(const QString& text, bool error = false);
  void refresh_sessions(); // 台账拉取＋两侧列表重建＋共享/查看态推导
  QString first_pending_id() const;
  QString active_id_as_target() const;
  QString active_id_as_requester() const;

  FilesClient* client_;
  QLineEdit* target_edit_;
  QCheckBox* chk_view_;
  QCheckBox* chk_mouse_;
  QCheckBox* chk_kb_;
  QCheckBox* gview_;
  QCheckBox* gmouse_;
  QCheckBox* gkb_;
  QListWidget* sessions_;
  QListWidget* pending_;
  QListWidget* audit_;
  QListWidget* inputs_;
  QLabel* frame_;
  QLabel* share_label_;
  QPushButton* btn_connect_;
  QPushButton* btn_request_;
  QPushButton* btn_approve_;
  QPushButton* btn_deny_;
  QPushButton* btn_start_;
  QPushButton* btn_end_;
  QPushButton* btn_audit_;
  QPushButton* btn_share_;
  QPushButton* btn_refresh_;
  QTimer* timer_;
  QLabel* status_;
  QString account_;
  qint64 frame_seq_{0};
  bool sharing_{false};
};

} // namespace memex::client
