// 直连态文件传输（T1.3）：分块字节流 + 偏移协商断点续传 + 整文件 SHA-256 校验。
// 控制面与数据面统一走连接级安全信道（SecureChannel，平台-8）：先握手
// （Ed25519 身份 + X25519 会话 + TOFU 定针）再收发，控制帧与数据块均为
// AEAD 密文载荷（数据块＝一个载荷：u64 offset + u32 len + 数据，大端），
// 握手不成即连接失效，无明文回退。单文件独占一条 TCP 连接（复用直连端口
// 段，随发现宣告）。中断保留 .memex-part，重发自动从偏移续传；收满做整
// 文件哈希比对，通过后原子改名落地。
#pragma once

#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

#include <map>
#include <memory>
#include <string>

#include <memex/protocol/frame.hpp>
#include <memex/protocol/messages.hpp>

#include "secure_channel.hpp"

namespace memex::client {

class LocalStore;

struct FileTransferOptions {
  QString download_dir;           // 接收落地根目录
  int chunk_size = 256 * 1024;    // 数据块大小
  int window_bytes = 1024 * 1024; // 发送在途窗口（未落网卡字节上限）
  int timeout_ms = 30000;         // 无进展超时（收发两侧同值）
};

class FileTransferService : public QObject {
  Q_OBJECT

public:
  explicit FileTransferService(FileTransferOptions opts,
                               QObject* parent = nullptr);
  ~FileTransferService() override;

  void set_device_id(std::string id) { device_id_ = std::move(id); }
  void set_download_dir(const QString& dir) { opts_.download_dir = dir; }
  // 平台-8：安全信道材料（身份与本地库）。未设置时拒发新传输
  // （fail-closed，不退回明文）；接收侧信道随连接移交而来。
  void set_secure(const DeviceIdentity* self, LocalStore* store);

  // 发起文件发送（异步）：返回传输 ID，失败为空。rel_path 为空取文件名；
  // 目录传输由上层按相对路径逐文件调用。
  std::string send_file(const QHostAddress& target, quint16 target_port,
                        const std::string& peer_id, const QString& local_path,
                        const QString& rel_path = {});

  // 中止发送。对端感知连接断开后保留 .memex-part，重发即续传。
  void cancel(const std::string& transfer_id);

  // 接入 DirectTransport 移交的文件连接（已解出 kFileMeta 且握手已立；
  // 此后数据面经同一信道解封，每个数据块一个密文载荷）。
  void handle_incoming(QTcpSocket* socket, const memex::protocol::Message& meta,
                       std::shared_ptr<SecureChannel> ch);

  void stop();

signals:
  void file_progress(const std::string& transfer_id, quint64 bytes_done,
                     quint64 bytes_total);
  void file_finished(const std::string& transfer_id, bool ok,
                     const QString& error);
  // 接收侧：文件完整落地
  void file_received(const std::string& transfer_id, const QString& final_path);

private:
  // 发送侧：一条独占连接，控制面（meta/resume/done）走 JSON 帧，数据面走二进制块
  struct Outgoing {
    std::string id;
    std::string peer_id;
    QString local_path;
    QString rel_path;
    quint64 total{0};
    quint64 offset{0};   // 已写入 socket 的数据偏移（自文件头计）
    bool resumed{false}; // 已收到 kFileResume 并打开文件
    QString sha256;
    std::unique_ptr<QFile> file;
    QTcpSocket* socket{nullptr};
    QTimer* timer{nullptr};
    std::shared_ptr<SecureChannel> ch; // 连接级安全信道（发起方）
    bool meta_sent{false};             // 握手已立、FILE_META 已写出
  };

  // 接收侧：缓冲区内做二进制块状态机；.memex-part 保留供续传
  struct Incoming {
    std::string id;
    std::string peer_id;
    QString rel_path;
    QString final_path;
    QString part_path;
    quint64 total{0};
    quint64 expect{0}; // 下一块的期望偏移（＝已落盘字节数）
    QString sha256;
    std::unique_ptr<QFile> file;
    std::unique_ptr<QCryptographicHash> hasher;
    QTcpSocket* socket{nullptr};
    QTimer* timer{nullptr};
    std::shared_ptr<SecureChannel> ch; // 连接级安全信道（响应方，随连接移交）
  };

  Outgoing* outgoing(const std::string& id);
  Incoming* incoming(QTcpSocket* socket);

  void pump(const std::string& id);
  void finish_outgoing(const std::string& id, bool ok, const QString& error);
  void fail_incoming(QTcpSocket* socket, const QString& error);
  void finalize_incoming(QTcpSocket* socket);
  // 经信道密文回包；信道不可用返回 false（调用方按连接中断收尾）
  bool reply(QTcpSocket* socket, const memex::protocol::Message& msg,
             const std::shared_ptr<SecureChannel>& ch);

  FileTransferOptions opts_;
  std::string device_id_;
  const DeviceIdentity* self_{nullptr};
  LocalStore* store_{nullptr};
  std::map<std::string, Outgoing> outgoing_;
  std::map<QTcpSocket*, Incoming> incoming_;
};

} // namespace memex::client
