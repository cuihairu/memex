// group_vault_dialog.cpp 拆分模块（LAY-017）：包裹块 pack/unpack 静态方法（加解密提示）；公共面见 common.hpp，方法声明在 group_vault_dialog.hpp。
#include "common.hpp"

namespace memex::client {

QString GroupVaultDialog::pack_secret(const QString& password,
                                      const QString& url,
                                      const QString& note) {
  return QString::fromUtf8(
      QJsonDocument(QJsonObject{{QStringLiteral("password"), password},
                                {QStringLiteral("url"), url},
                                {QStringLiteral("note"), note}})
          .toJson(QJsonDocument::Compact));
}

bool GroupVaultDialog::unpack_secret(const QString& plaintext,
                                     QString* password, QString* url,
                                     QString* note) {
  if (plaintext.isEmpty()) return false;
  const QJsonDocument doc = QJsonDocument::fromJson(plaintext.toUtf8());
  if (!doc.isObject()) return false;
  const QJsonObject o = doc.object();
  *password = o.value(QStringLiteral("password")).toString();
  *url = o.value(QStringLiteral("url")).toString();
  *note = o.value(QStringLiteral("note")).toString();
  return true;
}

} // namespace memex::client
