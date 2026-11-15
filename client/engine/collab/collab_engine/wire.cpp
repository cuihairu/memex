// collab_engine 拆分模块（LAY-013）：文件判权请求＋帧发送通道（send_frame/send_login_frame）——方法体自原文件逐字搬移；公共面见 common.hpp，声明在 collab_engine.hpp。
#include "common.hpp"

namespace memex::client {
void CollabEngine::file_authz(quint64 req, const QString& to, quint64 size,
                              const QString& name, const QString& sha256,
                              bool forward) {
  if (!logged_in_) {
    emit file_authz_result(req, false,
                           QStringLiteral("deny:server-unreachable"), false);
    return;
  }
  Message m;
  m.set_type(MsgType::FILE_AUTHZ);
  m.set_seq(req); // 关联号原样回带
  m.set_from(account_.toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* fa = m.mutable_file_authz();
  fa->set_to(to.toStdString());
  fa->set_size(size);
  fa->set_name(name.toStdString());
  fa->set_sha256(sha256.toStdString());
  fa->set_forward(forward);
  send_frame(m);
}

void CollabEngine::send_frame(const Message& msg) {
  const std::string frame = memex::protocol::encode(msg);
  socket_->write(QByteArray(frame.data(), static_cast<qsizetype>(frame.size())));
}

void CollabEngine::send_login_frame() {
  Message m;
  m.set_type(MsgType::LOGIN);
  m.set_from(device_name().toStdString());
  m.set_to("server");
  m.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* in = m.mutable_login();
  in->set_account(account_.toStdString());
  in->set_password(password_.toStdString());
  in->set_device_fingerprint(device_fingerprint().toStdString());
  in->set_device_kind("desktop");
  in->set_device_name(device_name().toStdString());
  in->set_client_version(MEMEX_VERSION);
  send_frame(m);
}

} // namespace memex::client
