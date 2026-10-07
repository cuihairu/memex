// R24-3 密码箱客户端解锁面：PBKDF2-HMAC-SHA256 600000 轮（wingman 口径）
// →KEK，AES-256-GCM 包 DEK（nonce12||ct32||tag16 单串 b64）；条目密文
// secret_ct=b64(ct||tag)＋secret_nonce=b64(nonce)。全部本地派生——
// 箱密码/明文永不触服务端；服务端只见 b64 密文与包裹块（全程密文）。
#pragma once

#include <QByteArray>
#include <QString>

namespace memex::vault {

// PBKDF2 派生 32B KEK（salt 传 b64 串；iters 服务端建箱时定，≥10000）
QByteArray derive_kek(const QString& password, const QString& salt_b64,
                      int iters);
// 随机 b64（建箱盐 16B、DEK 32B、条目 nonce 12B）
QString random_b64(int bytes);
// KEK 包 DEK：b64(nonce12||ct32||tag16)；解包失败=空串（口令错）
QString wrap_dek(const QByteArray& kek, const QByteArray& dek);
QByteArray unwrap_dek(const QByteArray& kek, const QString& wrapped_b64);
// 条目 secret 加解密（明文=JSON{password,url,note}；失败=空串）
QString encrypt_secret(const QByteArray& dek, const QString& plaintext,
                       QString* nonce_b64);
QString decrypt_secret(const QByteArray& dek, const QString& ct_b64,
                       const QString& nonce_b64);

} // namespace memex::vault
