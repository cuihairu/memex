// 协作引擎：登录协作服务端的长连接（T2.1 登录与互踢；T2.2 心跳／重连／离线补投／消息路由；
// T2.5 断线中断期消息本地暂存、恢复后按消息标识去重补传归档）。
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>

#include <memex/protocol/frame.hpp>
#include <memex/protocol/messages.hpp>

namespace memex::client {

class LocalStore;

class CollabEngine : public QObject {
  Q_OBJECT

public:
  explicit CollabEngine(QObject* parent = nullptr);
  ~CollabEngine() override;

  // 设备指纹：machine-id 与主机名的 SHA-256（同机稳定、跨机不撞）。
  static QString device_fingerprint();
  // 设备名：主机名。
  static QString device_name();

  bool is_logged_in() const { return logged_in_; }
  QString account() const { return account_; }
  // 登录口令（内存常驻供重连；需求批⑫ 文件面同源自动登录复用，不出进程）
  QString password() const { return password_; }
  QStringList online_accounts() const { return online_accounts_; }
  std::string status_text() const;

  // 设置心跳参数：interval_ms（PING 间隔）、missed_max（连续丢包阈值，触发判死重连）。
  void set_heartbeat(int interval_ms, int missed_max);

  // 绑定本地库（用于协作消息落库与 msg_id 去重）。由外部传入并管理生命周期。
  void attach_store(LocalStore* store);

public slots:
  // 连接并登录（desktop 主设备）；结果异步回报：logged_in／login_failed。
  void login(const QString& host, quint16 port, const QString& account,
             const QString& password);
  // 主动登出：发 LOGOUT 后断开（T2.4 切换形态入口）。
  void logout();
  // 发送文本消息（协作态）：返回本地序列号（sent 回执以此 seq 关联）。
  quint64 send_text(const QString& to, const QString& text);
  // 振屏（需求批⑥，协作单聊）：空体 NUDGE 经服务端归档留痕并在线即投
  //（不进离线补投）；本地落 "[振屏]" 标记行。返回 0＝未登录/目标空。
  quint64 send_nudge(const QString& to);
  // 撤回一条协作态消息（服务端校验权限后全网标记，原文留痕不清）。
  void recall_text(const QString& to, const QString& msg_id);
  // 请求组织架构（部门树＋成员资料，T3.1）；结果经 org_received 送达。
  void query_org();
  // 常用联系人（T4.5）：登录后自动拉取；star/unstar/touch 都触发全量重推
  void fav_query();
  void fav_cmd(const QString& op, const QString& peer);
  // 个性签名设置/清除（需求批⑪）：op=set_signature，空串=清除；回执经
  // profile_result（服务端受理回执，长度门 120 字）
  void set_signature(const QString& signature);
  // —— 群聊（T4.1）：命令走 GROUP_CMD，回执经 group_result；群列表 query_groups ——
  void create_group(const QString& name, const QStringList& members);
  void invite_group(quint64 group_id, const QStringList& members);
  void leave_group(quint64 group_id);
  void announce_group(quint64 group_id, const QString& announcement);
  // 公告编辑历史查询（R24-1）：回执经 announcement_history_received
  void announce_history(quint64 group_id);
  void query_groups();
  // —— 跨态会话日志（T4.2）：op=start（建立）／end（结束）——
  // 只上报时间、双方与时长，不含任何消息内容；须登录态。
  void cross_log(const QString& op, const QString& peer_device,
                 const QString& peer_name, qint64 started_ms, qint64 ended_ms);
  // —— 已读回执与在线状态（T4.3）——
  // 服务端 msg_id 派生式（session.cpp 同式 sha256(from:seq)）：发出即回填
  // 本地行（send_text），送达/已读回执通知按它命中；界面层同式取用。
  static QString msg_id_for(const QString& account, quint64 seq);
  // 上报某条收到的协作消息已读（服务端留痕＋通知发送方）；须登录态。
  // 是否上报（隐私开关）由界面层裁决——引擎只管机制。
  void mark_read(const QString& msg_id);
  // 批量查询自己发出消息的回执态（需求批⑦ 归档扩：离线期错过的
  // 送达/已读通知经服务端台账补查）；回包经 receipts_received。
  void query_receipts(const QStringList& msg_ids);
  // 会话历史按日期范围查询（需求批⑧）：peer=对端账号或群键 group:N，
  // 时间窗毫秒含端点。回包落本地索引（msg_id 去重）后经 history_received
  // 通知（gap 补齐——服务端有的本地没有的行就此同步）。
  void query_history(const QString& peer, qint64 from_ms, qint64 until_ms);
  // 主动拉取在线账号表（登录/登出/互踢变更由服务端推送，无需轮询）。
  void query_presence();
  // —— 直连文件旁路授权（平台-10，蓝图§十九四问）——
  // 文件不经服务器，判权必须经服务器：发送前问统一 AuthorizationService。
  // req＝调用方关联号（原样回带）；未登录即 fail-closed 本地拒（不发查询）。
  void file_authz(quint64 req, const QString& to, quint64 size,
                  const QString& name, const QString& sha256, bool forward);

signals:
  void logged_in(const QString& account, const QString& display_name);
  void login_failed(const QString& reason);
  // 单点在线被踢（第二台同类型设备登录）：reason 供提示文案。
  void kicked(const QString& reason, const QString& replaced_by);
  void connection_lost();
  // 重连成功（自动重登后恢复在线，离线消息补投已由服务端推送）。
  void reconnected();
  // 收到协作文本消息：from、text、ts_ms、msg_id（服务端分配，去重键）。
  void message_received(const QString& from, const QString& text, qint64 ts_ms,
                        const QString& msg_id);
  // 发送方受理回执：seq（本地 send_text 返回值）、ok（服务端已接收并入队）。
  void text_delivered(quint64 seq, bool ok);
  // 一条协作消息被撤回（本地与对端副本都置标记，原文保留）。
  void message_recalled(const QString& from, const QString& msg_id);
  // 收到协作振屏（需求批⑥）：界面层抖窗＋提示音；本地已落 "[振屏]" 标记行
  void nudge_received(const QString& from, qint64 ts_ms);
  // 组织架构数据（T3.1）：JSON——
  // {"departments":[{"path":"公司/研发部"}],
  //  "members":[{"account","display_name","title","department_path","manager","role"}],
  //  "policies":[{"department_path":"","allow_anonymous":true,
  //              "allow_cross_state":true,"new_device_approval":false,
  //              "allow_cross_dept_file":false,"allow_forward_file":true}]}（T3.4）
  void org_received(const QString& org_json);
  // 直连文件旁路授权裁决（平台-10）：req＝请求关联号、allowed、reason＝
  // 命中规则名、forwardable＝发送方生效策略的再转发开关（第四问答复）。
  void file_authz_result(quint64 req, bool allowed, const QString& reason,
                         bool forwardable);
  // 常用联系人全量（T4.5）：JSON [{"peer","starred","last_ms"}]（已排序）
  void fav_received(const QString& fav_json);
  // 资料命令回执（需求批⑪）：op 原样回带、ok=受理结果、reason=失败原因
  void profile_result(bool ok, const QString& reason, const QString& op);
  // 群命令回执（T4.1）：op（create/invite/leave/announce/announce_history）、
  // ok、reason、群号
  void group_result(bool ok, const QString& reason, const QString& op,
                    quint64 group_id);
  // 公告编辑历史（R24-1）：JSON [{"editor","content","ts_ms"}]（倒序，
  // content 空串=该次为清除）；群号随查
  void announcement_history_received(quint64 group_id,
                                     const QString& history_json);
  // 群列表（T4.1）：JSON [{"group_id":N,"name","owner","announcement","members":[…]}]
  void groups_received(const QString& groups_json);
  // 收到群消息：群键（"group:N"）、发送者账号、正文、时间、msg_id
  void group_message_received(const QString& group_key, const QString& sender,
                              const QString& text, qint64 ts_ms,
                              const QString& msg_id);
  // 已读回执（T4.3）：我发出的协作消息被接收方已读——msg_id、已读方、已读时刻
  void message_read(const QString& msg_id, const QString& reader, qint64 read_ms);
  // 送达回执（需求批⑦）：我发出的协作消息已被接收方客户端收取——
  // msg_id、送达方账号、送达时刻（群消息＝逐成员各一条）
  void message_delivered(const QString& msg_id, const QString& delivered_to,
                         qint64 delivered_ms);
  // 回执态查询回包（需求批⑦）：JSON [{"msg_id","delivered_to":[…],
  // "readers":[…]}]——仅自己发出消息的条目（服务端裁决）
  void receipts_received(const QString& receipts_json);
  // 日期范围查询回包（需求批⑧）：peer/from_ms/until_ms 回带，added=本次
  // 新入库条数（已在库的按 msg_id 去重不计）
  void history_received(const QString& peer, qint64 from_ms, qint64 until_ms,
                        int added);
  // 在线账号表（T4.3）：登录/登出/互踢/断开变更即推送，含自己
  void presence_changed(const QStringList& online_accounts);
  // 收到通知（T4.10，webhook 推入）：from（＝"通知"）、标题、正文、紧急程度
  // （1 普通／2 重要／3 紧急，Notice::Urgency 数值）、跳转、ts_ms、msg_id。
  // 正文展示走 message_received／group_message_received（同一条只渲染一次），
  // 本信号专供分级推送（桌面通知／置顶弹窗）与通知偏好判定。
  void notice_received(const QString& from, const QString& title,
                       const QString& content, int urgency,
                       const QString& jump_url, qint64 ts_ms,
                       const QString& msg_id);

private:
  // 在途／待补传消息（T2.5）：断线中断期本地暂存，恢复后按原 seq 补传，
  // 服务端按 msg_id=sha256(from:seq) 去重归档——重复补传不产生重复归档。
  struct PendingSend {
    std::string to;
    std::string text;
    quint64 seq{0};
    qint64 ts_ms{0};
    qint64 sent_at_ms{0}; // 已发未回执的超时计时（0=尚未发出）
    bool nudge{false};    // 振屏（需求批⑥）：重发按 NUDGE 空体重建
  };

  void send_login_frame();
  void handle_frame(const QByteArray& payload);
  // unexpected=true：意外断开——在途消息转待补传（不判失败）；
  // false：主动登出／被踢／重新登录——清队列并回报失败。
  void teardown(bool unexpected);
  void schedule_reconnect();
  void check_delivery_timeouts();
  void start_heartbeat();
  void stop_heartbeat();
  void send_frame(const memex::protocol::Message& msg);
  void handle_text(const memex::protocol::Message& msg);
  void handle_notice(const memex::protocol::Message& msg); // T4.10 通知
  void handle_ack(const memex::protocol::Message& msg);
  void flush_pending_sends(); // 重连成功后补传暂存消息

  QTcpSocket* socket_{nullptr};
  memex::protocol::FrameDecoder decoder_;
  LocalStore* store_{nullptr};
  QString account_;
  QString password_;
  QString host_;
  quint16 port_{0};
  bool logged_in_{false};
  bool kicking_{false};            // 互踢／登出引发的断开，不再报连接丢失
  bool manual_logout_{false};      // 主动登出，不触发重连
  bool reconnecting_{false};       // 正在重连中
  int reconnect_backoff_ms_{1000}; // 重连退避（ms），指数级增至 30s
  QTimer reconnect_timer_;

  int heartbeat_interval_ms_{0};   // 0=未启用
  int heartbeat_max_missed_{0};
  int heartbeat_missed_{0};
  QTimer heartbeat_timer_;

  quint64 next_seq_{1};
  QHash<quint64, PendingSend> inflight_; // seq -> 在途消息（已发未回执；超时转待补传）
  QList<PendingSend> pending_reconnect_; // 断线中断期暂存，恢复后补传
  QTimer delivery_timer_;

  QStringList online_accounts_; // 最近一次在线账号表（T4.3；推送即刷新）

  static constexpr qint64 kDeliveryTimeoutMs = 10000;
  static constexpr int kMaxBackoffMs = 30000;
};

} // namespace memex::client