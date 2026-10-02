#include "direct_engine.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUuid>

#include <algorithm>

namespace memex::client {

DirectEngine::DirectEngine(const std::string& device_id, const QString& db_path,
                           QObject* parent)
    : QObject(parent) {
  if (!device_id.empty()) {
    device_id_ = device_id;
  } else {
    QSettings settings(QCoreApplication::organizationName(),
                       QCoreApplication::applicationName());
    auto id = settings.value(QStringLiteral("direct/device_id")).toString();
    if (id.isEmpty()) {
      id = QUuid::createUuid().toString(QUuid::WithoutBraces);
      settings.setValue(QStringLiteral("direct/device_id"), id);
    }
    device_id_ = id.toStdString();
  }
  device_name_ = QSysInfo::machineHostName().toStdString();

  db_path_ = db_path;
  if (db_path_.isEmpty()) {
    const QString data_dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    db_path_ = data_dir + QStringLiteral("/memex-local.db");
  }
  download_dir_ =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
      QStringLiteral("/files");
}

DirectEngine::~DirectEngine() { stop(); }

bool DirectEngine::start() {
  if (running_) return true;

  store_ = std::make_unique<LocalStore>();
  if (!store_->open(db_path_)) {
    qWarning() << "[直连引擎] 本地库打开失败：" << db_path_;
    store_.reset();
    return false;
  }

  transport_ = std::make_unique<DirectTransport>();
  transport_->set_device_id(device_id_);
  if (!transport_->listen()) {
    qWarning() << "[直连引擎] TCP 监听失败";
    transport_.reset();
    store_.reset();
    return false;
  }

  FileTransferOptions fopts;
  fopts.download_dir = download_dir_;
  file_service_ = std::make_unique<FileTransferService>(fopts);
  file_service_->set_device_id(device_id_);

  discovery_ = std::make_unique<DiscoveryService>(device_id_, device_name_);
  discovery_->set_tcp_port(transport_->port());
  // 测试钩子：并发测试进程各占独立 UDP 口互不串扰（默认仍为协议端口 2425）
  DiscoveryOptions dopts;
  const int discovery_port = qEnvironmentVariableIntValue("MEMEX_TEST_DISCOVERY_PORT");
  if (discovery_port >= 1024 && discovery_port <= 65535) {
    dopts.port = static_cast<quint16>(discovery_port);
  }
  if (!discovery_->start(dopts)) {
    discovery_.reset();
    file_service_.reset();
    transport_->stop();
    transport_.reset();
    store_.reset();
    return false;
  }

  connect(discovery_.get(), &DiscoveryService::peerJoined, this,
          [this](const Peer&) { emit peers_changed(); });
  connect(discovery_.get(), &DiscoveryService::peerLeft, this,
          [this](const std::string&) { emit peers_changed(); });
  // 对端登录态变化（T4.2）：跨态判定与账号显示随宣告刷新
  connect(discovery_.get(), &DiscoveryService::peerUpdated, this,
          [this](const Peer&) { emit peers_changed(); });

  connect(transport_.get(), &DirectTransport::text_received, this,
          [this](const QString& from_id, const QString& /*to_id*/, quint64 seq,
                 qint64 ts_ms, const QString& text) {
            // 本地落库：来源＝直连（仅本机，不入服务端归档）
            StoredMessage m;
            m.peer = from_id.toStdString();
            m.from = from_id.toStdString();
            m.to = device_id_;
            m.seq = seq;
            m.ts_ms = ts_ms;
            m.text = text.toStdString();
            m.source = "direct";
            store_->append(m);
            emit message_received(from_id, text, ts_ms);
          });

  connect(transport_.get(), &DirectTransport::delivered, this,
          [this](quint64 seq, bool ok) { emit text_delivered(seq, ok); });

  // 文件传输：连接移交与信号转发（QString 化），目录作业链在 file_finished 驱动
  connect(transport_.get(), &DirectTransport::file_incoming, this,
          [this](QTcpSocket* socket, const memex::protocol::Message& meta) {
            file_service_->handle_incoming(socket, meta);
          });
  connect(file_service_.get(), &FileTransferService::file_progress, this,
          [this](const std::string& id, quint64 done, quint64 total) {
            emit file_progress(QString::fromStdString(id), done, total);
          });
  connect(file_service_.get(), &FileTransferService::file_received, this,
          [this](const std::string& id, const QString& path) {
            emit file_received(QString::fromStdString(id), path);
          });
  connect(file_service_.get(), &FileTransferService::file_finished, this,
          [this](const std::string& id, bool ok, const QString& error) {
            // 目录作业链：成功续发下一文件，失败终止整作业（部分文件保留供续传）
            const auto tj = transfer_job_.find(id);
            if (tj == transfer_job_.end()) {
              emit file_finished(QString::fromStdString(id), ok, error);
              return;
            }
            const QString job_id = tj->second;
            transfer_job_.erase(tj);
            const auto job_it = dir_jobs_.find(job_id);
            if (job_it != dir_jobs_.end() && !ok) {
              dir_jobs_.erase(job_it);
            } else if (job_it != dir_jobs_.end()) {
              ++job_it->second.index;
              send_next_dir_file(job_id);
            }
            emit file_finished(QString::fromStdString(id), ok, error);
          });

  running_ = true;
  qDebug().noquote() << QString::fromStdString("[直连引擎] 启动：设备 " + device_id_ +
                                               "，TCP " +
                                               std::to_string(transport_->port()));
  return true;
}

void DirectEngine::stop() {
  if (!running_) return;
  running_ = false;
  if (discovery_) discovery_->stop();
  if (transport_) transport_->stop();
  if (file_service_) file_service_->stop();
  if (store_) store_->close();
  dir_jobs_.clear();
  transfer_job_.clear();
  discovery_.reset();
  transport_.reset();
  file_service_.reset();
  store_.reset();
}

bool DirectEngine::running() const { return running_; }

QList<Peer> DirectEngine::peers() const {
  return discovery_ ? discovery_->peers() : QList<Peer>{};
}

Peer DirectEngine::peer(const std::string& device_id) const {
  const auto list = peers();
  for (const Peer& p : list) {
    if (p.device_id == device_id) return p;
  }
  return {};
}

bool DirectEngine::has_peer(const std::string& device_id) const {
  return !peer(device_id).device_id.empty();
}

void DirectEngine::set_collab_account(const std::string& account) {
  if (discovery_) discovery_->set_account(account);
}

quint64 DirectEngine::send_text(const std::string& peer_device_id,
                                const std::string& text) {
  if (!running_) return 0;
  const Peer target = peer(peer_device_id);
  if (target.device_id.empty() || target.tcp_port == 0) {
    qWarning() << "[直连引擎] 对端不可达："
               << QString::fromStdString(peer_device_id);
    return 0;
  }

  const std::uint64_t seq = ++seq_counter_;
  const qint64 ts_ms = QDateTime::currentMSecsSinceEpoch();

  // 发送方本地落库（送达失败仍保留本机记录，与聊天界面语义一致）
  StoredMessage m;
  m.peer = peer_device_id;
  m.from = device_id_;
  m.to = peer_device_id;
  m.seq = seq;
  m.ts_ms = ts_ms;
  m.text = text;
  m.source = "direct";
  store_->append(m);

  transport_->send_text(target.address, target.tcp_port, peer_device_id, seq, text);
  return static_cast<quint64>(seq);
}

QList<StoredMessage> DirectEngine::history(const QString& peer, int limit) const {
  return store_ ? store_->history(peer, limit) : QList<StoredMessage>{};
}

std::string DirectEngine::send_file(const std::string& peer_device_id,
                                    const QString& local_path) {
  if (!running_ || !file_service_) return {};
  const Peer target = peer(peer_device_id);
  if (target.device_id.empty() || target.tcp_port == 0) {
    qWarning() << "[直连引擎] 对端不可达："
               << QString::fromStdString(peer_device_id);
    return {};
  }
  return file_service_->send_file(target.address, target.tcp_port,
                                  peer_device_id, local_path, {});
}

QString DirectEngine::send_directory(const std::string& peer_device_id,
                                     const QString& dir_path) {
  if (!running_ || !file_service_) return {};
  const Peer target = peer(peer_device_id);
  if (target.device_id.empty() || target.tcp_port == 0) {
    qWarning() << "[直连引擎] 对端不可达："
               << QString::fromStdString(peer_device_id);
    return {};
  }
  const QDir root(dir_path);
  if (!root.exists()) return {};
  const QString root_abs = root.absolutePath();

  DirJob job;
  job.peer_id = peer_device_id;
  job.root = root_abs;
  QDirIterator it(root_abs, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString local = it.next();
    job.files.emplace_back(local, root.relativeFilePath(local));
  }
  if (job.files.empty()) return {};
  std::sort(job.files.begin(), job.files.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

  dir_jobs_.emplace(root_abs, std::move(job));
  send_next_dir_file(root_abs);
  return root_abs;
}

void DirectEngine::send_next_dir_file(const QString& job_id) {
  const auto job_it = dir_jobs_.find(job_id);
  if (job_it == dir_jobs_.end()) return;
  DirJob& job = job_it->second;
  if (job.index >= job.files.size()) {
    dir_jobs_.erase(job_it);
    emit directory_finished(job_id, true);
    return;
  }
  const Peer target = peer(job.peer_id);
  if (target.device_id.empty() || target.tcp_port == 0) {
    dir_jobs_.erase(job_it);
    emit directory_finished(job_id, false);
    return;
  }
  const auto& [local, rel] = job.files[job.index];
  const std::string tid = file_service_->send_file(
      target.address, target.tcp_port, job.peer_id, local, rel);
  if (tid.empty()) {
    dir_jobs_.erase(job_it);
    emit directory_finished(job_id, false);
    return;
  }
  transfer_job_[tid] = job_id;
}

void DirectEngine::cancel_transfer(const std::string& transfer_id) {
  if (!file_service_) return;
  // 作业 ID 亦可作取消目标：止其当前在途文件，失败链自动终止整作业
  const auto job_it = dir_jobs_.find(QString::fromStdString(transfer_id));
  if (job_it != dir_jobs_.end()) {
    for (const auto& [tid, jid] : transfer_job_) {
      if (jid == job_it->first) {
        file_service_->cancel(tid);
        return;
      }
    }
    return;
  }
  file_service_->cancel(transfer_id);
}

void DirectEngine::set_download_dir(const QString& dir) {
  download_dir_ = dir;
}

std::string DirectEngine::status_text() const {
  if (!running_) return "直连态 · 停止";
  const auto n = peers().size();
  if (n == 0) return "直连态 · 运行中（未发现设备）";
  return "直连态 · " + std::to_string(n) + " 台在线";
}

} // namespace memex::client
