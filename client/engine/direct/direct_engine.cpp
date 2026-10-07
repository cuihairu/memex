#include "direct_engine.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
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

  // 平台-8：设备身份（Ed25519，缺失即生成落库）。身份未就绪即拒绝启动
  // （fail-closed——没有身份就无法通过握手，不退回明文直连）。
  identity_ = DeviceIdentity::load(store_.get());
  if (!identity_.valid()) {
    qWarning() << "[直连引擎] 设备身份未就绪";
    store_->close();
    store_.reset();
    return false;
  }

  transport_ = std::make_unique<DirectTransport>();
  transport_->set_device_id(device_id_);
  transport_->set_secure(&identity_, store_.get());
  if (!transport_->listen()) {
    qWarning() << "[直连引擎] TCP 监听失败";
    transport_.reset();
    identity_ = DeviceIdentity{};
    store_.reset();
    return false;
  }

  FileTransferOptions fopts;
  fopts.download_dir = download_dir_;
  file_service_ = std::make_unique<FileTransferService>(fopts);
  file_service_->set_device_id(device_id_);
  file_service_->set_secure(&identity_, store_.get());

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
    identity_ = DeviceIdentity{};
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
            m.sync_state = "LOCAL"; // 直连域不进服务端同步（平台-9）
            store_->append(m);
            emit message_received(from_id, text, ts_ms);
          });

  connect(transport_.get(), &DirectTransport::delivered, this,
          [this](quint64 seq, bool ok) { emit text_delivered(seq, ok); });

  // 文件传输：连接移交与信号转发（QString 化），目录作业链在 file_finished 驱动；
  // 移交携带该连接的安全信道（已握手），数据面续用其密钥
  connect(transport_.get(), &DirectTransport::file_incoming, this,
          [this](QTcpSocket* socket, const memex::protocol::Message& meta,
                 std::shared_ptr<SecureChannel> ch) {
            file_service_->handle_incoming(socket, meta, std::move(ch));
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
  pending_authz_.clear();
  discovery_.reset();
  transport_.reset();
  file_service_.reset();
  store_.reset();
  identity_ = DeviceIdentity{}; // 信道已随 transport_ 亡，释放身份密钥
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
  m.sync_state = "LOCAL"; // 直连域不进服务端同步（平台-9）
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
  // 平台-10 授权门：接线即先问服务端四问（蓝图§十九），裁决回流才起传；
  // 进度/终态/取消全程以关联号贯穿（file_finished(false)＝拒绝或失败）
  if (file_authorizer_) {
    const QFileInfo fi(local_path);
    if (!fi.isFile()) return {};
    const quint64 req = ++authz_seq_;
    PendingAuthz p;
    p.target = target;
    p.is_dir = false;
    p.path = fi.absoluteFilePath();
    pending_authz_.emplace(req, std::move(p));
    FileAuthzRequest r;
    r.req = req;
    r.to_account = target.account;
    r.size = static_cast<quint64>(fi.size());
    r.name = fi.fileName().toStdString();
    r.forward = false; // 文件来源追踪未建，再转发声明面留后续（如实口径）
    file_authorizer_(r);
    return std::to_string(req);
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

  std::vector<std::pair<QString, QString>> files;
  QDirIterator it(root_abs, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString local = it.next();
    files.emplace_back(local, root.relativeFilePath(local));
  }
  if (files.empty()) return {};
  std::sort(files.begin(), files.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });

  // 平台-10 授权门：目录作业按作业级一次查询（合计尺寸；策略面不依赖
  // 逐文件内容，逐文件查询同答多余）
  if (file_authorizer_) {
    const quint64 req = ++authz_seq_;
    PendingAuthz p;
    p.target = target;
    p.is_dir = true;
    p.rel = root_abs;
    p.files = std::move(files);
    quint64 total = 0;
    for (const auto& [local, rel] : p.files) {
      (void)rel;
      total += static_cast<quint64>(QFileInfo(local).size());
    }
    pending_authz_.emplace(req, std::move(p));
    FileAuthzRequest r;
    r.req = req;
    r.to_account = target.account;
    r.size = total;
    r.name = root.dirName().toStdString();
    r.forward = false;
    file_authorizer_(r);
    return QString::number(req);
  }

  if (!start_dir_job(root_abs, target, root_abs, std::move(files))) return {};
  return root_abs;
}

// 平台-10 授权裁决回流：允＝真正起传（关联号贯穿为传输/作业 ID），
// 拒＝终态失败（服务端理由随文上抛）
void DirectEngine::file_authz_resolved(quint64 req, bool allowed,
                                       const QString& reason) {
  const auto it = pending_authz_.find(req);
  if (it == pending_authz_.end()) return;
  PendingAuthz p = std::move(it->second);
  pending_authz_.erase(it);
  const QString req_str = QString::number(req);
  if (!allowed) {
    if (p.is_dir) {
      emit directory_finished(req_str, false);
    }
    emit file_finished(req_str, false,
                       QStringLiteral("授权拒绝（服务端）：") + reason);
    return;
  }
  if (p.is_dir) {
    start_dir_job(req_str, p.target, p.rel, std::move(p.files));
    return;
  }
  const std::string tid = file_service_->send_file(
      p.target.address, p.target.tcp_port, p.target.device_id, p.path, {},
      req_str.toStdString());
  if (tid.empty()) {
    emit file_finished(req_str, false, QStringLiteral("发送失败"));
  }
}

void DirectEngine::set_file_authorizer(
    std::function<void(const FileAuthzRequest&)> authorizer) {
  file_authorizer_ = std::move(authorizer);
}

// 目录作业注册与首发起：job_id 即对外作业 ID（门控=授权关联号，
// 未门控=根目录绝对路径——directory_finished 原样回报）
bool DirectEngine::start_dir_job(const QString& job_id, const Peer& target,
                                 const QString& root,
                                 std::vector<std::pair<QString, QString>> files) {
  DirJob job;
  job.peer_id = target.device_id;
  job.root = root;
  job.files = std::move(files);
  if (job.files.empty()) return false;
  dir_jobs_.emplace(job_id, std::move(job));
  send_next_dir_file(job_id);
  return true;
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
  // 平台-10：授权待决中的关联号亦可取消——撤单即终态失败，不再起传
  //（transfer_id 也可能是 UUID／根路径，非纯数字则跳过此查）
  {
    quint64 req = 0;
    if (!transfer_id.empty() &&
        transfer_id.find_first_not_of("0123456789") == std::string::npos) {
      req = std::stoull(transfer_id);
    }
    if (req > 0 && pending_authz_.erase(req) > 0) {
      emit file_finished(QString::fromStdString(transfer_id), false,
                         QStringLiteral("本地取消"));
      return;
    }
  }
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
