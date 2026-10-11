// 直连态发现服务（T1.1）：UDP 2425 周期宣告与同网段在线表。
// 宣告载荷为 JSON 数据报（不带长度前缀——数据报本身即边界）；
// 接收侧严格校验 magic 与字段，畸形报文丢弃不崩溃。
// 飞秋/IPMSG 兼容（2026-10-11）：同一 socket 按首字节分流——'{' 走
// memex JSON，数字开头走 IPMSG 报文（发现 BR_ENTRY/ANSENTRY/BR_EXIT、
// 单聊 SENDMSG/RECVMSG）。memex 侧 IPMSG 身份 host 恒 "memex"——
// 收到 host=memex 的 IPMSG 包即对端是另一 memex（其已由 JSON 通道
// 登记），IPMSG 侧不再建行，防同机互见重复行。
#pragma once

#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

#include <map>
#include <functional>
#include <string>

#include "ipmsg.hpp"

namespace memex::client {

// 发现协议常量
inline constexpr char kDiscoveryMagic[] = "memex-discovery";
inline constexpr int kDiscoveryVersion = 1;
inline constexpr quint16 kDiscoveryPort = 2425; // 报告固定端口段 UDP 2425–2436 之首

struct DiscoveryOptions {
  quint16 port = kDiscoveryPort;
  int announce_interval_ms = 3000; // 周期宣告间隔
  int peer_timeout_ms = 10000;     // 超时未宣告判定离线
  int sweep_interval_ms = 1000;    // 离线巡检间隔
  // 飞秋/IPMSG 兼容（默认关）：开＝同口伴发 BR_ENTRY 广播并响应飞秋包；
  // 关＝IPMSG 报文静默丢弃（socket 只走 memex JSON）
  bool ipmsg_enabled = false;
};

struct Peer {
  std::string device_id;
  std::string name;
  quint16 tcp_port{0};       // 直连 TCP 端口（T1.2 起使用）
  QHostAddress address;
  quint64 last_seen_ms{0};
  // 对端协作账号（T4.2：登录端广播携带，仅作显示与跨态判定——
  // 不参与任何消息路由；空=未登录（未进协作态））
  std::string account;
  // 飞秋对端 UDP 回发端口（SENDMSG 目标；0=按协议端口 2425——真实
  // 客户端固定监听 2425，收发同口；测试 mock 用临时口收发对称）
  quint16 udp_port{0};
};

class DiscoveryService : public QObject {
  Q_OBJECT

public:
  DiscoveryService(std::string device_id, std::string device_name,
                   QObject* parent = nullptr);
  ~DiscoveryService() override;

  // 绑定失败（端口被占且不可共享）返回 false。
  bool start(const DiscoveryOptions& opts = {});
  void stop();
  bool running() const { return running_; }

  // 直连 TCP 接入端口，随监听器启动更新（记入后续宣告）。
  void set_tcp_port(quint16 port);

  // 本端协作账号（T4.2）：登录态写入后续宣告（空=未登录）；变更立即补宣告。
  // 仅作对端显示与跨态判定，不参与路由。
  void set_account(const std::string& account);

  // 地址黑名单过滤（用户令 2026-10-08 ⑤）：sender 被拒的发现包直接丢弃
  // ＝「匹配不响应」；空过滤器＝不拦。
  void set_filter(std::function<bool(const QHostAddress&)> f);

  QList<Peer> peers() const;
  quint16 listen_port() const { return opts_.port; }

  // 飞秋兼容开关（设置页即时生效）：开时立即补一条 BR_ENTRY 广播，
  // 飞秋在线表即时互见；关时后续 IPMSG 报文静默丢弃
  void set_ipmsg_enabled(bool on);
  bool ipmsg_enabled() const { return ipmsg_enabled_; }

  // 飞秋单聊：向已知飞秋对端发 SENDMSG（GBK 优先＝真飞秋码页口径，
  // 编不出才回退 UTF-8 并置 UTF8OPT；带 SENDCHECKOPT 要回执）。no＝调用方发送序号
  // （与本机 seq 共用一条单调序列，RECVMSG 回执按同号回填，界面上
  // 送达态可直接对齐）。返回实际发出的包号（0=关断/对端不存在/写失败；
  // UDP 写成功不代表送达——送达以 RECVMSG 回执为准，经 ipmsg_acked 上抛）。
  quint32 send_ipmsg_text(const std::string& device_id, const QString& text,
                          quint32 no);

signals:
  void peerJoined(const memex::client::Peer& peer);
  void peerLeft(const std::string& device_id);
  // 对端条目更新（账号等既有设备的字段变化，T4.2）
  void peerUpdated(const memex::client::Peer& peer);
  // 飞秋 SENDMSG 收文（已按 charset 解码；device_id 形如
  // "ipmsg:user@host"）；本地落库与上屏由 DirectEngine 接线
  void ipmsg_received(const std::string& device_id, const QString& text,
                      qint64 ts_ms);
  // 对端 RECVMSG 回执（我方 SENDMSG 送达确认；包号键控）
  void ipmsg_acked(quint32 packet_no);

private slots:
  void on_ready_read();
  void announce();
  void sweep();

private:
  // IPMSG 侧：周期 BR_ENTRY 广播（伴 JSON 宣告同拍）与收包处理
  void announce_ipmsg();
  void handle_ipmsg(const QHostAddress& sender, quint16 sender_port,
                    const QByteArray& data);
  // 收到即入列/刷新（BR_ENTRY/ANSENTRY/BR_ABSENCE/未知者 SENDMSG 同径）；
  // 新见=peerJoined，昵称变化=peerUpdated
  void ipmsg_upsert(const QHostAddress& sender, quint16 sender_port,
                    const QByteArray& user, const QByteArray& host,
                    const QByteArray& nick);
  std::string ipmsg_id(const QByteArray& user, const QByteArray& host) const;
  // 单播一条 IPMSG 报文（ANSENTRY/RECVMSG 回执走原包来源口）
  void ipmsg_send(const QHostAddress& addr, quint16 port,
                  const QByteArray& raw);
  // 本端飞秋头部身份：user＝协作账号（空则设备名，GB18030 本地编码）、
  // host 恒 "memex"（对端据此识别 memex 实例，IPMSG 侧不重复建行）
  void refresh_ipmsg_user();
  quint32 ipmsg_next_no() { return ++ipmsg_no_; }

  std::function<bool(const QHostAddress&)> filter_; // 发现包地址过滤
  std::string device_id_;
  std::string device_name_;
  std::string account_; // 本端协作账号（空=未登录）
  DiscoveryOptions opts_;
  QUdpSocket socket_;
  QTimer announce_timer_;
  QTimer sweep_timer_;
  std::map<std::string, Peer> peers_;
  bool running_{false};
  quint16 tcp_port_{0};
  // 飞秋兼容面：开关、本端 IPMSG 身份（头部 user/host）与发包计数器
  bool ipmsg_enabled_{false};
  QByteArray ipmsg_user_;
  QByteArray ipmsg_host_;
  quint32 ipmsg_no_{0};
};

} // namespace memex::client
