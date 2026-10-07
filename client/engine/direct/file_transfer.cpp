#include "file_transfer.hpp"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace memex::client {

using memex::protocol::Message;
using memex::protocol::MsgType;
using memex::protocol::ProtocolError;

namespace {

// 数据块长度上限（接收侧防线），与 common 帧上限同量级
constexpr quint32 kMaxChunkSize = 4 * 1024 * 1024;
// 续传部分文件后缀；中断保留，重发续传，哈希不符才删除
const QLatin1String kPartSuffix(".memex-part");
// 控制帧长度前缀与块头（u64 offset + u32 len）尺寸
constexpr qsizetype kChunkHeaderSize = 12;

void append_u64(QByteArray& out, std::uint64_t v) {
  for (int i = 7; i >= 0; --i) out.append(char((v >> (8 * i)) & 0xFF));
}

void append_u32(QByteArray& out, std::uint32_t v) {
  for (int i = 3; i >= 0; --i) out.append(char((v >> (8 * i)) & 0xFF));
}

std::uint64_t read_u64(const char* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | std::uint8_t(p[i]);
  return v;
}

std::uint32_t read_u32(const char* p) {
  std::uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 8) | std::uint8_t(p[i]);
  return v;
}

QString sha256_hex(const QByteArray& raw) {
  return QString::fromLatin1(raw.toHex());
}

// 单文件全量哈希：分块读，避免整载内存（百 MB 级文件也只占常量缓冲）
QString file_sha256(const QString& path, bool* ok) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    if (ok) *ok = false;
    return {};
  }
  QCryptographicHash h(QCryptographicHash::Sha256);
  while (!f.atEnd()) h.addData(f.read(1 << 20));
  if (ok) *ok = true;
  return sha256_hex(h.result());
}

// 接收侧相对路径消毒：禁绝对路径、.. 段与盘符/反斜杠，防落地越界
QString sanitize_rel_path(const QString& rel) {
  const QStringList parts = rel.split(QChar('/'), Qt::SkipEmptyParts);
  QStringList clean;
  for (const QString& seg : parts) {
    if (seg == QLatin1String(".") || seg == QLatin1String("..")) return {};
    if (seg.contains(QChar(':')) || seg.contains(QChar('\\'))) return {};
    clean.push_back(seg);
  }
  if (clean.isEmpty()) return {};
  return clean.join(QChar('/'));
}

} // namespace

FileTransferService::FileTransferService(FileTransferOptions opts,
                                         QObject* parent)
    : QObject(parent), opts_(std::move(opts)) {}

FileTransferService::~FileTransferService() { stop(); }

void FileTransferService::set_secure(const DeviceIdentity* self,
                                     LocalStore* store) {
  self_ = self;
  store_ = store;
}

// ---------- 发送侧 ----------

std::string FileTransferService::send_file(const QHostAddress& target,
                                           quint16 target_port,
                                           const std::string& peer_id,
                                           const QString& local_path,
                                           const QString& rel_path) {
  if (!self_ || !store_) {
    // 平台-8：安全材料未注入即拒发（不退回明文）
    qWarning() << "[文件传输] 安全材料未就绪，拒发";
    return {};
  }
  const QFileInfo fi(local_path);
  if (!fi.isFile() || !fi.isReadable()) {
    qWarning() << "[文件传输] 本地文件不可读：" << local_path;
    return {};
  }
  bool hash_ok = false;
  const QString sha = file_sha256(fi.absoluteFilePath(), &hash_ok);
  if (!hash_ok) {
    qWarning() << "[文件传输] 哈希计算失败：" << local_path;
    return {};
  }

  Outgoing o;
  o.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
  o.peer_id = peer_id;
  o.local_path = fi.absoluteFilePath();
  o.rel_path = rel_path.isEmpty() ? fi.fileName() : rel_path;
  o.total = static_cast<quint64>(fi.size());
  o.sha256 = sha;
  // 每连接一个安全信道（发起方）：先握手后发元数据
  o.ch = std::make_shared<SecureChannel>(SecureChannel::Role::kInitiator,
                                         self_, store_, device_id_, peer_id);

  const std::string id = o.id;
  outgoing_.emplace(id, std::move(o));
  Outgoing* p = outgoing(id);

  p->socket = new QTcpSocket(this);
  p->timer = new QTimer(p->socket);
  p->timer->setSingleShot(true);

  connect(p->timer, &QTimer::timeout, this, [this, id] {
    finish_outgoing(id, false, QStringLiteral("传输超时"));
  });

  // 连接即启动握手（HELLO1）
  connect(p->socket, &QTcpSocket::connected, this, [this, id] {
    Outgoing* p = outgoing(id);
    if (!p || !p->ch) {
      finish_outgoing(id, false, QStringLiteral("安全信道未就绪"));
      return;
    }
    const std::string hello = p->ch->start();
    if (hello.empty()) {
      finish_outgoing(id, false, QStringLiteral("握手启动失败"));
      return;
    }
    p->socket->write(QByteArray(hello.data(),
                                static_cast<qsizetype>(hello.size())));
  });

  // 握手回执 + 控制面回包：kFileResume 定起点，kFileDone 定终态
  connect(p->socket, &QTcpSocket::readyRead, this, [this, id] {
    Outgoing* p = outgoing(id);
    if (!p || !p->ch) return;
    const QByteArray data = p->socket->readAll();
    p->timer->start(opts_.timeout_ms);
    const SecureChannel::Fed fed = p->ch->feed(
        std::string_view(data.constData(),
                         static_cast<std::size_t>(data.size())));
    if (fed.failed) {
      finish_outgoing(id, false,
                      QStringLiteral("握手失败：%1").arg(fed.reason));
      return;
    }
    if (!fed.reply.empty()) {
      p->socket->write(QByteArray(fed.reply.data(),
                                  static_cast<qsizetype>(fed.reply.size())));
    }
    if (fed.established && !p->meta_sent) {
      // 握手已立：发元数据（size/sha256 供对端做续传协商与完整性校验）
      Message meta;
      meta.set_type(MsgType::FILE_META);
      meta.set_from(device_id_);
      meta.set_to(p->peer_id);
      auto* fm = meta.mutable_file_meta();
      fm->set_transfer_id(p->id);
      fm->set_rel_path(p->rel_path.toStdString());
      fm->set_name(QFileInfo(p->local_path).fileName().toStdString());
      fm->set_size(p->total);
      fm->set_sha256(p->sha256.toStdString());
      if (!reply(p->socket, meta, p->ch)) {
        finish_outgoing(id, false, QStringLiteral("元数据封装失败"));
        return;
      }
      p->meta_sent = true;
    }
    for (const auto& payload : fed.payloads) {
      Message m;
      try {
        m = memex::protocol::decode_payload(payload);
      } catch (const ProtocolError& e) {
        finish_outgoing(id, false, QStringLiteral("对端控制帧解析失败：%1")
                                       .arg(QString::fromUtf8(e.what())));
        return;
      }
      if (m.type() == MsgType::FILE_RESUME && !p->resumed) {
        if (!m.has_file_resume()) {
          finish_outgoing(id, false, QStringLiteral("续传响应缺 offset"));
          return;
        }
        const quint64 offset = m.file_resume().offset();
        if (offset > p->total) {
          finish_outgoing(id, false, QStringLiteral("对端续传偏移越界"));
          return;
        }
        p->file = std::make_unique<QFile>(p->local_path);
        if (!p->file->open(QIODevice::ReadOnly) ||
            !p->file->seek(static_cast<qint64>(offset))) {
          finish_outgoing(id, false, QStringLiteral("本地文件读取失败"));
          return;
        }
        p->resumed = true;
        p->offset = offset;
        emit file_progress(id, offset, p->total);
        pump(id);
      } else if (m.type() == MsgType::FILE_DONE && p->resumed) {
        const bool ok = m.has_file_done() && m.file_done().ok();
        finish_outgoing(id, ok,
                        ok ? QString() : QStringLiteral("对端校验未通过"));
        return;
      }
    }
  });

  connect(p->socket, &QTcpSocket::bytesWritten, this, [this, id](qint64) {
    Outgoing* p = outgoing(id);
    if (p && p->resumed) pump(id);
  });

  connect(p->socket, &QTcpSocket::errorOccurred, this, [this, id](auto) {
    finish_outgoing(id, false, QStringLiteral("连接错误"));
  });
  connect(p->socket, &QTcpSocket::disconnected, this, [this, id] {
    finish_outgoing(id, false, QStringLiteral("连接中断"));
  });

  p->timer->start(opts_.timeout_ms);
  p->socket->connectToHost(target, target_port);
  return id;
}

// 在途窗口：socket 待写字节低于窗口才续读文件，防大文件撑爆发送缓冲
void FileTransferService::pump(const std::string& id) {
  Outgoing* p = outgoing(id);
  if (!p || !p->resumed || !p->ch) return;
  while (p->offset < p->total &&
         p->socket->bytesToWrite() <
             static_cast<qint64>(opts_.window_bytes)) {
    const quint64 remain = p->total - p->offset;
    const int want = static_cast<int>(
        std::min<quint64>(static_cast<quint64>(opts_.chunk_size), remain));
    const QByteArray data = p->file->read(want);
    if (data.isEmpty()) {
      finish_outgoing(id, false, QStringLiteral("本地文件读取失败"));
      return;
    }
    // 数据块＝一个载荷：[u64 offset][u32 len][数据]，整体经信道密文成帧
    QByteArray chunk;
    chunk.reserve(static_cast<qsizetype>(kChunkHeaderSize) + data.size());
    append_u64(chunk, p->offset);
    append_u32(chunk, static_cast<std::uint32_t>(data.size()));
    chunk.append(data);
    const std::string wire = p->ch->protect(
        std::string_view(chunk.constData(),
                         static_cast<std::size_t>(chunk.size())));
    if (wire.empty()) {
      finish_outgoing(id, false, QStringLiteral("数据块封装失败"));
      return;
    }
    p->socket->write(
        QByteArray(wire.data(), static_cast<qsizetype>(wire.size())));
    p->offset += static_cast<quint64>(data.size());
    emit file_progress(id, p->offset, p->total);
  }
}

void FileTransferService::cancel(const std::string& transfer_id) {
  finish_outgoing(transfer_id, false, QStringLiteral("本地取消"));
}

void FileTransferService::finish_outgoing(const std::string& id, bool ok,
                                          const QString& error) {
  const auto it = outgoing_.find(id);
  if (it == outgoing_.end()) return;
  Outgoing& o = it->second;
  if (o.timer) o.timer->stop();
  if (o.socket) {
    o.socket->disconnect(this);
    o.socket->abort();
    o.socket->deleteLater();
  }
  if (o.file) o.file->close();
  outgoing_.erase(it);
  if (!ok) {
    qWarning() << "[文件传输] 发送结束（未完成）"
               << QString::fromStdString(id) << "：" << error;
  }
  emit file_finished(id, ok, error);
}

// ---------- 接收侧 ----------

void FileTransferService::handle_incoming(QTcpSocket* socket,
                                          const Message& meta,
                                          std::shared_ptr<SecureChannel> ch) {
  if (!ch || !ch->established() || ch->failed()) {
    qWarning() << "[文件传输] 连接未过安全握手，拒绝文件";
    socket->abort();
    socket->deleteLater();
    return;
  }
  // 身份绑定：元数据 from/to 必须与已认证信道一致
  if (meta.from() != ch->peer_id() || meta.to() != device_id_) {
    qWarning() << "[文件传输] 元数据身份与信道不符，拒绝";
    socket->abort();
    socket->deleteLater();
    return;
  }
  if (opts_.download_dir.isEmpty()) {
    qWarning() << "[文件传输] 未配置接收目录，拒绝文件";
    socket->abort();
    socket->deleteLater();
    return;
  }
  if (!meta.has_file_meta() || meta.file_meta().transfer_id().empty() ||
      meta.file_meta().rel_path().empty() || meta.file_meta().sha256().empty()) {
    qWarning() << "[文件传输] 元数据缺字段，拒绝";
    socket->abort();
    socket->deleteLater();
    return;
  }
  const auto& b = meta.file_meta();
  const QString rel = sanitize_rel_path(QString::fromStdString(b.rel_path()));
  if (rel.isEmpty()) {
    qWarning() << "[文件传输] 相对路径非法，拒绝";
    socket->abort();
    socket->deleteLater();
    return;
  }

  Incoming in;
  in.id = b.transfer_id();
  in.peer_id = meta.from();
  in.rel_path = rel;
  in.final_path = QDir(opts_.download_dir).filePath(rel);
  in.part_path = in.final_path + QString(kPartSuffix);
  in.total = b.size();
  in.sha256 = QString::fromStdString(b.sha256());
  in.socket = socket;
  in.ch = std::move(ch); // 数据面续用同一信道
  in.hasher = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
  in.file = std::make_unique<QFile>(in.part_path);

  // 幂等重发：完整文件已存在且哈希一致 → 直达终态，不再收数据
  if (QFile::exists(in.final_path)) {
    bool have = false;
    const QString have_sha = file_sha256(in.final_path, &have);
    if (have && have_sha == in.sha256) {
      Message done;
      done.set_type(MsgType::FILE_DONE);
      done.set_from(device_id_);
      done.set_to(in.peer_id);
      done.mutable_file_done()->set_ok(true);
      done.mutable_file_done()->set_sha256(in.sha256.toStdString());
      reply(socket, done, in.ch);
      emit file_progress(in.id, in.total, in.total);
      emit file_received(in.id, in.final_path);
      emit file_finished(in.id, true, {});
      // 优雅关闭：abort 会丢掉刚写入的回执帧
      socket->disconnect(this);
      socket->flush();
      socket->disconnectFromHost();
      socket->deleteLater();
      return;
    }
    QFile::remove(in.final_path); // 内容不符则重建
  }

  // 断点续传：.memex-part 在则从其尺寸续传，前缀先入哈希；越界损坏则重建
  quint64 offset = 0;
  if (QFile::exists(in.part_path)) {
    const QFileInfo pi(in.part_path);
    if (pi.size() <= static_cast<qint64>(in.total)) {
      offset = static_cast<quint64>(pi.size());
      QFile prefix(in.part_path);
      if (prefix.open(QIODevice::ReadOnly)) {
        while (!prefix.atEnd()) in.hasher->addData(prefix.read(1 << 20));
      } else {
        offset = 0;
      }
    } else {
      QFile::remove(in.part_path);
    }
  }

  QDir().mkpath(QFileInfo(in.final_path).absolutePath());
  // ReadWrite 不动既有内容（WriteOnly 在部分平台打开即清零，续传基线会丢）；
  // 偏移为 0 时显式 Truncate 确保全新起点；打开后校验 size 未回退。
  QIODevice::OpenMode mode = QIODevice::ReadWrite;
  if (offset == 0) mode |= QIODevice::Truncate;
  bool opened = in.file->open(mode) &&
                in.file->size() == static_cast<qint64>(offset) &&
                in.file->seek(static_cast<qint64>(offset));
  if (!opened && offset > 0) {
    // 前缀丢失（打开即截断等平台差异）：退回整收
    offset = 0;
    in.hasher = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    in.file->close();
    opened = in.file->open(QIODevice::WriteOnly | QIODevice::Truncate) &&
             in.file->seek(0);
  }
  if (!opened) {
    Message done;
    done.set_type(MsgType::FILE_DONE);
    done.set_from(device_id_);
    done.set_to(in.peer_id);
    done.mutable_file_done()->set_ok(false);
    done.mutable_file_done()->set_error("无法写盘");
    reply(socket, done, in.ch);
    qWarning() << "[文件传输] 落地文件打开失败：" << in.part_path;
    socket->disconnect(this);
    socket->flush();
    socket->disconnectFromHost();
    socket->deleteLater();
    return;
  }
  in.expect = offset;

  const std::string id = in.id;
  const std::string peer_id = in.peer_id;
  incoming_.emplace(socket, std::move(in));
  Incoming* p = incoming(socket);

  p->timer = new QTimer(socket);
  p->timer->setSingleShot(true);
  connect(p->timer, &QTimer::timeout, this,
          [this, socket] { fail_incoming(socket, QStringLiteral("传输超时")); });

  // 数据面：经安全信道解封，每个密文载荷＝一个数据块
  // （u64 offset + u32 len + 数据，大端；len 严格等于载荷余长）
  connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
    Incoming* p = incoming(socket);
    if (!p || !p->ch) return;
    const QByteArray data = socket->readAll();
    p->timer->start(opts_.timeout_ms);
    const SecureChannel::Fed fed = p->ch->feed(
        std::string_view(data.constData(),
                         static_cast<std::size_t>(data.size())));
    if (fed.failed) {
      fail_incoming(socket,
                    QStringLiteral("安全信道失效：%1").arg(fed.reason));
      return;
    }
    for (const auto& payload : fed.payloads) {
      if (payload.size() < static_cast<std::size_t>(kChunkHeaderSize)) {
        fail_incoming(socket, QStringLiteral("数据块帧非法"));
        return;
      }
      const quint64 off = read_u64(payload.data());
      const quint32 len = read_u32(payload.data() + 8);
      if (len == 0 || len > kMaxChunkSize ||
          static_cast<std::size_t>(len) !=
              payload.size() - static_cast<std::size_t>(kChunkHeaderSize)) {
        fail_incoming(socket, QStringLiteral("数据块帧非法"));
        return;
      }
      if (off != p->expect) {
        fail_incoming(socket, QStringLiteral("数据块偏移不连续"));
        return;
      }
      const QByteArray chunk(payload.data() + kChunkHeaderSize,
                             static_cast<qsizetype>(len));
      p->hasher->addData(chunk);
      if (p->file->write(chunk) != static_cast<qint64>(len)) {
        fail_incoming(socket, QStringLiteral("写盘失败"));
        return;
      }
      p->expect += len;
      emit file_progress(p->id, p->expect, p->total);
      if (p->expect == p->total) {
        finalize_incoming(socket);
        return;
      }
    }
  });

  // 中断不删 .memex-part：正是断点续传的依据
  connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
    fail_incoming(socket, QStringLiteral("连接中断"));
  });
  connect(socket, &QTcpSocket::errorOccurred, this, [this, socket](auto) {
    fail_incoming(socket, QStringLiteral("连接错误"));
  });

  p->timer->start(opts_.timeout_ms);
  emit file_progress(id, p->expect, p->total);

  if (p->expect == p->total) {
    // 部分文件已完整（前次收满但未及改名）：前缀哈希已在 hasher，直接终态
    finalize_incoming(socket);
    return;
  }
  Message resume;
  resume.set_type(MsgType::FILE_RESUME);
  resume.set_from(device_id_);
  resume.set_to(peer_id);
  resume.mutable_file_resume()->set_transfer_id(id);
  resume.mutable_file_resume()->set_offset(p->expect);
  reply(socket, resume, p->ch);
}

// 收满：整文件哈希比对 → 原子改名落地 → 终态回执
void FileTransferService::finalize_incoming(QTcpSocket* socket) {
  const auto it = incoming_.find(socket);
  if (it == incoming_.end()) return;
  Incoming& in = it->second;
  in.file->close();
  const QString got = sha256_hex(in.hasher->result());
  const bool ok = (got == in.sha256);
  if (ok) {
    QFile::remove(in.final_path);
    if (!QFile::rename(in.part_path, in.final_path)) {
      fail_incoming(socket, QStringLiteral("落地改名失败"));
      return;
    }
    Message done;
    done.set_type(MsgType::FILE_DONE);
    done.set_from(device_id_);
    done.set_to(in.peer_id);
    done.mutable_file_done()->set_ok(true);
    done.mutable_file_done()->set_sha256(in.sha256.toStdString());
    reply(socket, done, in.ch);
    emit file_progress(in.id, in.total, in.total);
    emit file_received(in.id, in.final_path);
    emit file_finished(in.id, true, {});
  } else {
    // 哈希不符：坏数据不可续传，删部分文件要求整发重来
    QFile::remove(in.part_path);
    Message done;
    done.set_type(MsgType::FILE_DONE);
    done.set_from(device_id_);
    done.set_to(in.peer_id);
    done.mutable_file_done()->set_ok(false);
    done.mutable_file_done()->set_error("哈希不一致");
    reply(socket, done, in.ch);
    emit file_finished(in.id, false, QStringLiteral("哈希不一致"));
  }
  // 回执先行落网（flush），再优雅关闭；abort 会把刚写的回执帧一起丢掉
  socket->disconnect(this);
  socket->flush();
  socket->disconnectFromHost();
  socket->deleteLater();
  incoming_.erase(it);
}

void FileTransferService::fail_incoming(QTcpSocket* socket,
                                        const QString& error) {
  const auto it = incoming_.find(socket);
  if (it == incoming_.end()) return;
  const std::string id = it->second.id;
  if (it->second.timer) it->second.timer->stop();
  if (it->second.file) it->second.file->close();
  socket->disconnect(this);
  socket->abort();
  socket->deleteLater();
  incoming_.erase(it);
  qWarning() << "[文件传输] 接收中断" << QString::fromStdString(id) << "："
             << error << "（保留 .memex-part 供续传）";
  emit file_finished(id, false, error);
}

// ---------- 公共 ----------

bool FileTransferService::reply(QTcpSocket* socket, const Message& msg,
                                const std::shared_ptr<SecureChannel>& ch) {
  if (!ch || ch->failed()) {
    qWarning() << "[文件传输] 回包时信道已失效";
    return false;
  }
  const std::string wire =
      ch->protect(memex::protocol::encode_payload(msg));
  if (wire.empty()) {
    qWarning() << "[文件传输] 回包封装失败";
    return false;
  }
  socket->write(QByteArray(wire.data(), static_cast<qsizetype>(wire.size())));
  return true;
}

void FileTransferService::stop() {
  for (auto& [id, o] : outgoing_) {
    if (o.timer) o.timer->stop();
    if (o.socket) {
      o.socket->disconnect(this);
      o.socket->abort();
      o.socket->deleteLater();
    }
    if (o.file) o.file->close();
  }
  outgoing_.clear();
  for (auto& [socket, in] : incoming_) {
    if (in.timer) in.timer->stop();
    if (in.file) in.file->close();
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
  }
  incoming_.clear();
}

FileTransferService::Outgoing* FileTransferService::outgoing(
    const std::string& id) {
  const auto it = outgoing_.find(id);
  return it == outgoing_.end() ? nullptr : &it->second;
}

FileTransferService::Incoming* FileTransferService::incoming(
    QTcpSocket* socket) {
  const auto it = incoming_.find(socket);
  return it == incoming_.end() ? nullptr : &it->second;
}

} // namespace memex::client
