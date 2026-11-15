// files_client 拆分模块（LAY-007）：本域成员函数定义自原文件逐字搬移；
// 公共面（超时常量）见 common.hpp。对外 API 仍是 files_client.hpp。
#include "engine/collab/files_client.hpp"
#include "engine/collab/files/common.hpp"

namespace memex::client {

// —— 品牌物料（二期）——

void FilesClient::fetch_branding() {
  send_json(QStringLiteral("branding.fetch"), QStringLiteral("GET"),
            QStringLiteral("/files/branding"), {},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) emit branding_fetched(resp);
            });
}

void FilesClient::save_branding(const QString& company_name,
                                const QString& accent,
                                const QString& slogan) {
  // 三件齐发（服务端口径：显式空串=清空、缺省不动——设置页表单语义
  // 「留空=清空该字段」与服务端显式空串对齐）
  send_json(QStringLiteral("branding.set"), QStringLiteral("POST"),
            QStringLiteral("/files/branding"),
            {{QStringLiteral("company_name"), company_name},
             {QStringLiteral("accent"), accent},
             {QStringLiteral("slogan"), slogan}},
            [this](bool ok, int, const QJsonObject& resp, const QString&) {
              if (ok) {
                emit branding_saved(static_cast<qint64>(
                    resp.value(QStringLiteral("version")).toDouble()));
              }
            });
}

void FilesClient::clear_brand_logo() {
  send_json(QStringLiteral("branding.clear_logo"), QStringLiteral("POST"),
            QStringLiteral("/files/branding"),
            {{QStringLiteral("clear_logo"), true}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit brand_logo_cleared();
            });
}

void FilesClient::clear_brand_splash() {
  send_json(QStringLiteral("branding.clear_splash"), QStringLiteral("POST"),
            QStringLiteral("/files/branding"),
            {{QStringLiteral("clear_splash"), true}},
            [this](bool ok, int, const QJsonObject&, const QString&) {
              if (ok) emit brand_splash_cleared();
            });
}

// 素材上传（PNG 原始字节 octet-stream；send_json 只走 JSON 面，照
// upload_inbox 直挂 NAM；类型/大小/尺寸校验在服务端，语义化 413/415
// 文案经统一失败通道回状态行）
void FilesClient::upload_brand_logo(const QString& file_path) {
  const QString op = QStringLiteral("branding.logo");
  if (token_.isEmpty()) {
    fail(op, 0, QStringLiteral("未登录"));
    return;
  }
  QFile f(file_path);
  if (!f.open(QIODevice::ReadOnly)) {
    fail(op, 0, QStringLiteral("文件打开失败"));
    return;
  }
  const QByteArray bytes = f.readAll();
  QNetworkRequest req(
      QUrl(base_url() + QStringLiteral("/files/branding/logo")));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("image/png"));
  QNetworkReply* rep = nam_->post(req, bytes);
  connect(rep, &QNetworkReply::finished, this, [this, rep, op] {
    rep->deleteLater();
    const int status =
        rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};
    if (rep->error() != QNetworkReply::NoError || status < 200 ||
        status >= 300) {
      QString msg = obj.value(QStringLiteral("error")).toString();
      if (msg.isEmpty()) msg = rep->errorString();
      fail(op, status, msg);
      return;
    }
    emit brand_logo_uploaded(static_cast<qint64>(
        obj.value(QStringLiteral("version")).toDouble()));
  });
}

void FilesClient::upload_brand_splash(const QString& file_path) {
  const QString op = QStringLiteral("branding.splash");
  if (token_.isEmpty()) {
    fail(op, 0, QStringLiteral("未登录"));
    return;
  }
  QFile f(file_path);
  if (!f.open(QIODevice::ReadOnly)) {
    fail(op, 0, QStringLiteral("文件打开失败"));
    return;
  }
  const QByteArray bytes = f.readAll();
  QNetworkRequest req(
      QUrl(base_url() + QStringLiteral("/files/branding/splash")));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  req.setHeader(QNetworkRequest::ContentTypeHeader,
                QStringLiteral("image/png"));
  QNetworkReply* rep = nam_->post(req, bytes);
  connect(rep, &QNetworkReply::finished, this, [this, rep, op] {
    rep->deleteLater();
    const int status =
        rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};
    if (rep->error() != QNetworkReply::NoError || status < 200 ||
        status >= 300) {
      QString msg = obj.value(QStringLiteral("error")).toString();
      if (msg.isEmpty()) msg = rep->errorString();
      fail(op, status, msg);
      return;
    }
    emit brand_splash_uploaded(static_cast<qint64>(
        obj.value(QStringLiteral("version")).toDouble()));
  });
}

// —— 表情包素材（需求批②）——

void FilesClient::emoji_upload(const QString& file_path, const QString& name) {
  const QString op = QStringLiteral("emoji.upload");
  if (token_.isEmpty()) {
    fail(op, 0, QStringLiteral("未登录"));
    return;
  }
  QFile f(file_path);
  if (!f.open(QIODevice::ReadOnly)) {
    fail(op, 0, QStringLiteral("文件打开失败"));
    return;
  }
  const QString suffix = QFileInfo(file_path).suffix().toLower();
  QString content_type = QStringLiteral("image/png");
  if (suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg")) {
    content_type = QStringLiteral("image/jpeg");
  } else if (suffix == QStringLiteral("gif")) {
    content_type = QStringLiteral("image/gif");
  }
  const QByteArray bytes = f.readAll();
  // name 进 query（magic 与 1MiB 上限校验在服务端）
  QNetworkRequest req(QUrl(base_url() +
                           QStringLiteral("/files/emoji/upload?name=") +
                           QString::fromUtf8(
                               QUrl::toPercentEncoding(name.toUtf8()))));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  req.setHeader(QNetworkRequest::ContentTypeHeader, content_type);
  QNetworkReply* rep = nam_->post(req, bytes);
  connect(rep, &QNetworkReply::finished, this, [this, rep, op] {
    rep->deleteLater();
    const int status =
        rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
    const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};
    if (rep->error() != QNetworkReply::NoError || status < 200 ||
        status >= 300) {
      QString msg = obj.value(QStringLiteral("error")).toString();
      if (msg.isEmpty()) msg = rep->errorString();
      fail(op, status, msg);
      return;
    }
    emit emoji_uploaded(static_cast<qint64>(
        obj.value(QStringLiteral("id")).toDouble()));
  });
}

void FilesClient::emoji_list() {
  send_json(QStringLiteral("emoji.list"), QStringLiteral("GET"),
            QStringLiteral("/files/emoji/list"), {},
            [this](bool ok, int status, const QJsonObject& body,
                   const QString& error) {
              if (!ok) return;
              emit emoji_listed(
                  body.value(QStringLiteral("assets")).toArray());
            });
}

void FilesClient::emoji_download(qint64 id, const QString& name,
                                 const QString& save_dir) {
  const QString op = QStringLiteral("emoji.download");
  if (token_.isEmpty()) {
    fail(op, 0, QStringLiteral("未登录"));
    return;
  }
  QDir dir(save_dir);
  if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
    fail(op, 0, QStringLiteral("保存目录创建失败"));
    return;
  }
  // 重名加序号（-1/-2…），不覆盖已有文件
  QString path = dir.filePath(name);
  const QFileInfo fi(name);
  for (int n = 1; QFileInfo::exists(path); ++n) {
    path = dir.filePath(fi.completeBaseName() + QStringLiteral("-%1").arg(n) +
                        (fi.suffix().isEmpty()
                             ? QString()
                             : QStringLiteral(".") + fi.suffix()));
  }
  QNetworkRequest req(QUrl(base_url() +
                           QStringLiteral("/files/emoji/download?id=") +
                           QString::number(id)));
  req.setTransferTimeout(kByteTimeoutMs);
  req.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
  QNetworkReply* rep = nam_->get(req);
  auto out = std::make_shared<QSaveFile>(path);
  if (!out->open(QIODevice::WriteOnly)) {
    rep->abort();
    rep->deleteLater();
    fail(op, 0, QStringLiteral("本地文件创建失败"));
    return;
  }
  connect(rep, &QNetworkReply::readyRead, this, [rep, out] {
    if (rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() !=
        200) {
      return; // 非 200（JSON 错误体）不写入
    }
    out->write(rep->readAll());
  });
  connect(rep, &QNetworkReply::finished, this, [this, rep, out, op, path] {
    rep->deleteLater();
    const int status =
        rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (rep->error() != QNetworkReply::NoError || status != 200) {
      out->cancelWriting();
      const QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
      const QJsonObject obj = doc.isObject() ? doc.object() : QJsonObject{};
      QString msg = obj.value(QStringLiteral("error")).toString();
      if (msg.isEmpty()) msg = rep->errorString();
      fail(op, status, msg);
      return;
    }
    if (!out->commit()) {
      fail(op, 0, QStringLiteral("本地写入失败"));
      return;
    }
    emit emoji_downloaded(path);
  });
}

void FilesClient::emoji_delete(qint64 id) {
  QJsonObject body;
  body[QStringLiteral("id")] = id;
  send_json(QStringLiteral("emoji.delete"), QStringLiteral("POST"),
            QStringLiteral("/files/emoji/delete"), body,
            [this, id](bool ok, int, const QJsonObject&, const QString&) {
              if (!ok) return;
              emit emoji_deleted(id);
            });
}


} // namespace memex::client
