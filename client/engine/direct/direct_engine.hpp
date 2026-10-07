// 直连引擎（T1.1／T1.2／T1.3）：未登录零配置直连态。
// UDP 广播发现 + TCP 点对点文本（送达确认）+ 文件传输（分块、断点续传、
// SHA-256 校验）+ 本地 SQLite 历史（source="direct"）。
#pragma once

#include <QList>
#include <QString>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <core/local_store.hpp>

#include "direct_transport.hpp"
#include "discovery.hpp"
#include "file_transfer.hpp"
#include "secure_channel.hpp"

namespace memex::client {

// 平台-10 直连文件旁路授权查询（蓝图§十九四问的上行面）：文件不经服务
// 器，判权必须经服务器——授权面由应用层接线（协作引擎 → 服务端统一
// AuthorizationService），裁决经 file_authz_resolved 回流关单。
struct FileAuthzRequest {
  quint64 req{0};          // 关联号（进度/终态/取消全程以此 ID 贯穿）
  std::string to_account;  // 接收方账号（直连发现宣告；空＝未登录对端）
  quint64 size{0};         // 文件字节数（目录作业＝合计）
  std::string name;        // 文件名（目录作业＝目录名）
  bool forward{false};     // 本次为收到文件的再转发
};

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

  // 异步发送文件；返回传输 ID（失败为空）。进度与终态经 file_progress /
  // file_finished 回报。须在 start() 前设置接收目录（set_download_dir）。
  std::string send_file(const std::string& peer_device_id,
                        const QString& local_path);

  // 目录传输：遍历（含子目录）按相对路径逐文件串行发送；返回作业 ID
  // （＝根目录绝对路径，失败为空）。终态经 directory_finished 回报。
  QString send_directory(const std::string& peer_device_id,
                         const QString& dir_path);

  // 中止发送中的文件或目录作业；对端保留部分文件，重发自动续传。
  void cancel_transfer(const std::string& transfer_id);

  // 接收落地目录（默认 应用数据目录/files，可启动前覆盖）
  void set_download_dir(const QString& dir);

  QList<StoredMessage> history(const QString& peer, int limit = 200) const;

  // 本地库（start() 后有效）。双态共用同一份本地库：协作引擎挂接同一库，
  // 界面按 source 字段合并展示直连与协作历史（T2.4 模式切换）。
  LocalStore* store() const { return store_.get(); }

  std::string status_text() const;
  const std::string& device_id() const { return device_id_; }

  // 平台-8：本端设备身份公钥（64 hex；start() 失败或未启动为空）。
  // 同库重启取回同一身份——对端 TOFU 定针跨会话稳定。
  std::string identity_pub_hex() const { return identity_.pub_hex(); }

  // 本端协作账号（T4.2）：登录／登出时同步进发现宣告（仅作对端显示与
  // 跨态判定；空=未登录）。
  void set_collab_account(const std::string& account);

  // —— 平台-10 文件旁路授权门 ——
  // 接线授权面后，send_file／send_directory 先发查询（立即返回关联号），
  // 裁决经 file_authz_resolved 关单：允→真正起传；拒→file_finished(false)。
  // 未接线＝直通（现状；仅测试路径，生产主窗口必须接线）。
  void set_file_authorizer(
      std::function<void(const FileAuthzRequest&)> authorizer);
  // 裁决回流：req＝FileAuthzRequest::req；reason 拒绝时随终态上抛。
  void file_authz_resolved(quint64 req, bool allowed, const QString& reason);

signals:
  void message_received(const QString& from_id, const QString& text, qint64 ts_ms);
  void text_delivered(quint64 seq, bool ok);
  void peers_changed();
  void file_progress(const QString& transfer_id, quint64 bytes_done,
                     quint64 bytes_total);
  void file_finished(const QString& transfer_id, bool ok, const QString& error);
  // 接收侧：文件完整落地
  void file_received(const QString& transfer_id, const QString& final_path);
  void directory_finished(const QString& job_id, bool ok);

private:
  struct DirJob {
    std::string peer_id;
    QString root;
    std::vector<std::pair<QString, QString>> files; // {本地路径, 相对路径}
    std::size_t index{0};
  };
  // 授权待决发送（平台-10）：裁决允后据此真正起传
  struct PendingAuthz {
    Peer target;
    bool is_dir{false};
    QString path;      // 单文件本地路径
    QString rel;       // 目录作业根目录
    std::vector<std::pair<QString, QString>> files; // 目录扫描结果
  };

  void send_next_dir_file(const QString& job_id);
  // 目录作业注册与首发起（job_id＝对外作业 ID：未门控=根路径，门控=关联号）
  bool start_dir_job(const QString& job_id, const Peer& target,
                     const QString& root,
                     std::vector<std::pair<QString, QString>> files);

  std::string device_id_;
  std::string device_name_;
  QString db_path_;
  QString download_dir_;
  std::unique_ptr<LocalStore> store_;
  // 设备身份（Ed25519）：成员序在 transport_/file_service_ 之前析构在后，
  // 信道持有指针随它们先亡（声明序＝逆析构序）
  DeviceIdentity identity_;
  std::unique_ptr<DirectTransport> transport_;
  std::unique_ptr<DiscoveryService> discovery_;
  std::unique_ptr<FileTransferService> file_service_;
  std::map<QString, DirJob> dir_jobs_;            // 作业 ID → 目录作业
  std::map<std::string, QString> transfer_job_;   // 在途传输 ID → 作业 ID
  // 平台-10 授权门
  std::function<void(const FileAuthzRequest&)> file_authorizer_;
  std::map<quint64, PendingAuthz> pending_authz_;
  std::atomic<std::uint64_t> seq_counter_{0};
  std::uint64_t authz_seq_{0};
  bool running_{false};
};

} // namespace memex::client
