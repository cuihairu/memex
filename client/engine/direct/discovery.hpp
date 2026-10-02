// 直连态发现服务（T1.1）：UDP 2425 周期宣告与同网段在线表。
// 宣告载荷为 JSON 数据报（不带长度前缀——数据报本身即边界）；
// 接收侧严格校验 magic 与字段，畸形报文丢弃不崩溃。
#pragma once

#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

#include <map>
#include <string>

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
};

struct Peer {
  std::string device_id;
  std::string name;
  quint16 tcp_port{0};       // 直连 TCP 端口（T1.2 起使用）
  QHostAddress address;
  quint64 last_seen_ms{0};
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

  QList<Peer> peers() const;
  quint16 listen_port() const { return opts_.port; }

signals:
  void peerJoined(const memex::client::Peer& peer);
  void peerLeft(const std::string& device_id);

private slots:
  void on_ready_read();
  void announce();
  void sweep();

private:
  std::string device_id_;
  std::string device_name_;
  DiscoveryOptions opts_;
  QUdpSocket socket_;
  QTimer announce_timer_;
  QTimer sweep_timer_;
  std::map<std::string, Peer> peers_;
  bool running_{false};
  quint16 tcp_port_{0};
};

} // namespace memex::client
