// 直连引擎（T1.1／T1.2）：未登录零配置直连态。
// UDP 广播发现 + TCP 点对点文本（送达确认）+ 本地 SQLite 历史（source="direct"）。
// 文件传输按 T1.3 接入。
#pragma once

#include <QList>
#include <QString>

#include <atomic>
#include <memory>
#include <string>

#include <core/local_store.hpp>

#include "direct_transport.hpp"
#include "discovery.hpp"

namespace memex::client {

class DirectEngine : public QObject {
  Q_OBJECT

public:
  // device_id / db_path 为空时取默认：QSettings 持久标识、应用数据目录本地库。
  explicit DirectEngine(const std::string& device_id = {},
                        const QString& db_path = {},
                        QObject* parent = nullptr);
  ~DirectEngine() override;

  bool start();
  void stop();
  bool running() const;

  QList<Peer> peers() const;
  Peer peer(const std::string& device_id) const;
  bool has_peer(const std::string& device_id) const;

  // 异步发送文本；结果经 text_delivered(seq, ok)。失败返回 false（seq=0）。
  quint64 send_text(const std::string& peer_device_id, const std::string& text);

  QList<StoredMessage> history(const QString& peer, int limit = 200) const;

  std::string status_text() const;
  const std::string& device_id() const { return device_id_; }

signals:
  void message_received(const QString& from_id, const QString& text, qint64 ts_ms);
  void text_delivered(quint64 seq, bool ok);
  void peers_changed();

private:
  std::string device_id_;
  std::string device_name_;
  QString db_path_;
  std::unique_ptr<LocalStore> store_;
  std::unique_ptr<DirectTransport> transport_;
  std::unique_ptr<DiscoveryService> discovery_;
  std::atomic<std::uint64_t> seq_counter_{0};
  bool running_{false};
};

} // namespace memex::client
