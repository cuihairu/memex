// 直连态文件传输（T1.3）：分块字节流 + 偏移协商断点续传 + 整文件 SHA-256 校验。
// 控制面复用 common 帧协议（kFileMeta / kFileResume / kFileDone，JSON 帧）；
// 数据面为二进制块（u64 offset + u32 len + 数据，大端），单文件独占一条 TCP
// 连接（复用直连端口段，随发现宣告）。中断保留 .memex-part，重发自动从
// 偏移续传；收满做整文件哈希比对，通过后原子改名落地。
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

namespace memex::client {

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

  // 发起文件发送（异步）：返回传输 ID，失败为空。rel_path 为空取文件名；
  // 目录传输由上层按相对路径逐文件调用。
  std::string send_file(const QHostAddress& target, quint16 target_port,
                        const std::string& peer_id, const QString& local_path,
                        const QString& rel_path = {});

  // 中止发送。对端感知连接断开后保留 .memex-part，重发即续传。
  void cancel(const std::string& transfer_id);

  // 接入 DirectTransport 移交的文件连接（已解出 kFileMeta，此后字节流不走帧解码）。
  void handle_incoming(QTcpSocket* socket, const memex::protocol::Message& meta);

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
    memex::protocol::FrameDecoder decoder;
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
    QByteArray buf;
  };

  Outgoing* outgoing(const std::string& id);
  Incoming* incoming(QTcpSocket* socket);

  void pump(const std::string& id);
  void finish_outgoing(const std::string& id, bool ok, const QString& error);
  void fail_incoming(QTcpSocket* socket, const QString& error);
  void finalize_incoming(QTcpSocket* socket);
  void reply(QTcpSocket* socket, const memex::protocol::Message& msg);

  FileTransferOptions opts_;
  std::string device_id_;
  std::map<std::string, Outgoing> outgoing_;
  std::map<QTcpSocket*, Incoming> incoming_;
};

} // namespace memex::client
