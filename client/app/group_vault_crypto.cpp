#include "group_vault_crypto.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <vector>

namespace memex::vault {

namespace {
constexpr int kKeckBytes = 32; // AES-256 密钥长度
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;

bool rand_bytes(QByteArray* out) {
  out->resize(out->size());
  return RAND_bytes(reinterpret_cast<unsigned char*>(out->data()),
                    out->size()) == 1;
}
} // namespace

QByteArray derive_kek(const QString& password, const QString& salt_b64,
                      int iters) {
  const QByteArray salt = QByteArray::fromBase64(salt_b64.toLatin1());
  if (salt.isEmpty() || iters < 10000) return {};
  const QByteArray pass = password.toUtf8();
  QByteArray out(kKeckBytes, Qt::Uninitialized);
  if (PKCS5_PBKDF2_HMAC(pass.constData(), pass.size(),
                        reinterpret_cast<const unsigned char*>(salt.constData()),
                        salt.size(), iters, EVP_sha256(), out.size(),
                        reinterpret_cast<unsigned char*>(out.data())) != 1) {
    return {};
  }
  return out;
}

QString random_b64(int bytes) {
  QByteArray b(bytes, Qt::Uninitialized);
  if (!rand_bytes(&b)) return {};
  return QString::fromLatin1(b.toBase64());
}

namespace {
// AES-256-GCM 加解密共享：返回 false=EVP 失败（或 tag 校验不过）
bool gcm_crypt(bool encrypt, const QByteArray& key, const QByteArray& nonce,
                const QByteArray& in, QByteArray* out, QByteArray* tag) {
  if (key.size() != kKeckBytes || nonce.size() != kNonceBytes) return false;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  bool ok = false;
  do {
    if (encrypt) {
      if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr,
                             nullptr) != 1) break;
      if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(),
                              nullptr) != 1) break;
      if (EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                             reinterpret_cast<const unsigned char*>(
                                 key.constData()),
                             reinterpret_cast<const unsigned char*>(
                                 nonce.constData())) != 1) break;
      out->resize(in.size() + kTagBytes); // 有余量给 final
      int len = 0, fin = 0;
      if (EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(out->data()),
                            &len, reinterpret_cast<const unsigned char*>(
                                      in.constData()),
                            in.size()) != 1) break;
      if (EVP_EncryptFinal_ex(
              ctx, reinterpret_cast<unsigned char*>(out->data()) + len,
              &fin) != 1) break;
      out->resize(len + fin);
      tag->resize(kTagBytes);
      if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagBytes,
                              tag->data()) != 1) break;
      ok = true;
    } else {
      if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr,
                             nullptr) != 1) break;
      if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(),
                              nullptr) != 1) break;
      if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                             reinterpret_cast<const unsigned char*>(
                                 key.constData()),
                             reinterpret_cast<const unsigned char*>(
                                 nonce.constData())) != 1) break;
      out->resize(in.size());
      int len = 0, fin = 0;
      if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(out->data()),
                            &len, reinterpret_cast<const unsigned char*>(
                                      in.constData()),
                            in.size()) != 1) break;
      if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes,
                              const_cast<char*>(tag->constData())) != 1) break;
      // tag 不符（口令错/密文篡改）在此失败
      if (EVP_DecryptFinal_ex(
              ctx, reinterpret_cast<unsigned char*>(out->data()) + len,
              &fin) != 1) break;
      out->resize(len + fin);
      ok = true;
    }
  } while (false);
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) out->clear();
  return ok;
}
} // namespace

QString wrap_dek(const QByteArray& kek, const QByteArray& dek) {
  if (dek.size() != kKeckBytes) return {};
  const QString nonce = random_b64(kNonceBytes);
  if (nonce.isEmpty()) return {};
  QByteArray ct, tag;
  if (!gcm_crypt(true, kek, QByteArray::fromBase64(nonce.toLatin1()), dek,
                  &ct, &tag)) {
    return {};
  }
  // 单串打包：nonce12||ct32||tag16（服务端 wrapped_dek 原样存取）
  return QString::fromLatin1(
      (QByteArray::fromBase64(nonce.toLatin1()) + ct + tag).toBase64());
}

QByteArray unwrap_dek(const QByteArray& kek, const QString& wrapped_b64) {
  const QByteArray packed = QByteArray::fromBase64(wrapped_b64.toLatin1());
  if (packed.size() != kNonceBytes + kKeckBytes + kTagBytes) return {};
  const QByteArray nonce = packed.left(kNonceBytes);
  const QByteArray ct = packed.mid(kNonceBytes, kKeckBytes);
  QByteArray tag = packed.right(kTagBytes);
  QByteArray out;
  if (!gcm_crypt(false, kek, nonce, ct, &out, &tag)) return {};
  return out;
}

QString encrypt_secret(const QByteArray& dek, const QString& plaintext,
                       QString* nonce_b64) {
  const QString nonce = random_b64(kNonceBytes);
  if (nonce.isEmpty()) return {};
  QByteArray ct, tag;
  const QByteArray pt = plaintext.toUtf8();
  if (!gcm_crypt(true, dek, QByteArray::fromBase64(nonce.toLatin1()), pt, &ct,
                  &tag)) {
    return {};
  }
  if (nonce_b64) *nonce_b64 = nonce;
  return QString::fromLatin1((ct + tag).toBase64());
}

QString decrypt_secret(const QByteArray& dek, const QString& ct_b64,
                       const QString& nonce_b64) {
  const QByteArray packed = QByteArray::fromBase64(ct_b64.toLatin1());
  const QByteArray nonce = QByteArray::fromBase64(nonce_b64.toLatin1());
  if (nonce.size() != kNonceBytes || packed.size() <= kTagBytes) return {};
  const QByteArray ct = packed.left(packed.size() - kTagBytes);
  QByteArray tag = packed.right(kTagBytes);
  QByteArray out;
  if (!gcm_crypt(false, dek, nonce, ct, &out, &tag)) return {};
  return QString::fromUtf8(out);
}

} // namespace memex::vault
