#include "secure_channel.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <algorithm>

#include <core/local_store.hpp>
#include <memex/protocol/messages.hpp>

namespace memex::client {

using memex::protocol::DecodeStatus;
using memex::protocol::Message;
using memex::protocol::MsgType;

namespace {

constexpr std::uint32_t kDirectSecureVersion = 2; // 握手协议版本
constexpr std::size_t kIdentityKeyBytes = 32;     // Ed25519 / X25519 公钥长度
constexpr std::size_t kHandshakeNonceBytes = 16;  // 握手盐
constexpr std::size_t kSigBytes = 64;             // Ed25519 签名长度
constexpr std::size_t kAeadTagBytes = 16;         // GCM 标签
constexpr std::size_t kCtrBytes = 8;              // 帧计数（明文段）
constexpr char kHkdfInfoPrefix[] = "memex-direct-v1/key";

std::string hex_of(const unsigned char* p, std::size_t n) {
  return QByteArray(reinterpret_cast<const char*>(p), qsizetype(n))
      .toHex()
      .toStdString();
}

std::string unhex(const std::string& h) {
  return QByteArray::fromHex(QByteArray::fromStdString(h)).toStdString();
}

std::string sha256(std::string_view data) {
  QCryptographicHash h(QCryptographicHash::Sha256);
  h.addData(QByteArray(data.data(), qsizetype(data.size())));
  return h.result().toStdString();
}

void put_u32be(std::string* out, std::uint32_t v) {
  for (int i = 3; i >= 0; --i) out->push_back(char((v >> (8 * i)) & 0xFF));
}

std::string be64(std::uint64_t v) {
  std::string s(8, '\0');
  for (int i = 7; i >= 0; --i) s[static_cast<std::size_t>(7 - i)] =
                                   char((v >> (8 * i)) & 0xFF);
  return s;
}

// 握手文本（签名原文）：版本域分隔 ‖ 双方标识 ‖ 身份公钥 ‖ 临时公钥 ‖ 盐，
// resp 侧再叠 init 文本哈希——签名覆盖全部握手字段，防替换/降级。
std::string build_init_text(const std::string& from, const std::string& to,
                            const std::string& pub, const std::string& eph,
                            const std::string& nonce) {
  std::string s("memex-direct-v1");
  put_u32be(&s, kDirectSecureVersion);
  s += from;
  s += to;
  s += pub;
  s += eph;
  s += nonce;
  return s;
}

std::string build_resp_text(const std::string& from, const std::string& to,
                            const std::string& pub, const std::string& eph,
                            const std::string& nonce,
                            const std::string& init_hash) {
  std::string s("memex-direct-v1");
  put_u32be(&s, kDirectSecureVersion);
  s += from;
  s += to;
  s += pub;
  s += eph;
  s += nonce;
  s += init_hash;
  return s;
}

bool ed25519_verify(const std::string& pub32, std::string_view msg,
                    const std::string& sig64) {
  EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(
      EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char*>(pub32.data()), pub32.size());
  if (!pkey) return false;
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  const bool ok =
      md != nullptr &&
      EVP_DigestVerifyInit(md, nullptr, nullptr, nullptr, pkey) == 1 &&
      EVP_DigestVerify(md,
                       reinterpret_cast<const unsigned char*>(sig64.data()),
                       sig64.size(),
                       reinterpret_cast<const unsigned char*>(msg.data()),
                       msg.size()) == 1;
  if (md) EVP_MD_CTX_free(md);
  EVP_PKEY_free(pkey);
  return ok;
}

bool gcm_seal(const std::string& key, const std::string& nonce12,
              std::string_view pt, std::string* out) {
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  std::string buf(pt.size() + kAeadTagBytes, '\0');
  int len = 0;
  int fin = 0;
  bool ok =
      EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) ==
          1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
      EVP_EncryptInit_ex(
          ctx, nullptr, nullptr,
          reinterpret_cast<const unsigned char*>(key.data()),
          reinterpret_cast<const unsigned char*>(nonce12.data())) == 1 &&
      (pt.empty() || EVP_EncryptUpdate(
                         ctx, reinterpret_cast<unsigned char*>(buf.data()),
                         &len, reinterpret_cast<const unsigned char*>(pt.data()),
                         static_cast<int>(pt.size())) == 1) &&
      EVP_EncryptFinal_ex(
          ctx, reinterpret_cast<unsigned char*>(buf.data()) + len, &fin) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kAeadTagBytes,
                          buf.data() + len + fin) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return false;
  buf.resize(static_cast<std::size_t>(len + fin) + kAeadTagBytes);
  *out = std::move(buf);
  return true;
}

bool gcm_open(const std::string& key, const std::string& nonce12,
              std::string_view ct_tag, std::string* out) {
  if (ct_tag.size() < kAeadTagBytes) return false;
  const std::size_t ctlen = ct_tag.size() - kAeadTagBytes;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return false;
  std::string buf(ctlen + kAeadTagBytes, '\0');
  int len = 0;
  int fin = 0;
  bool ok =
      EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) ==
          1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
      EVP_DecryptInit_ex(
          ctx, nullptr, nullptr,
          reinterpret_cast<const unsigned char*>(key.data()),
          reinterpret_cast<const unsigned char*>(nonce12.data())) == 1 &&
      (ctlen == 0 || EVP_DecryptUpdate(
                         ctx, reinterpret_cast<unsigned char*>(buf.data()),
                         &len,
                         reinterpret_cast<const unsigned char*>(ct_tag.data()),
                         static_cast<int>(ctlen)) == 1) &&
      EVP_CIPHER_CTX_ctrl(
          ctx, EVP_CTRL_GCM_SET_TAG, kAeadTagBytes,
          const_cast<char*>(ct_tag.data() + ctlen)) == 1 &&
      EVP_DecryptFinal_ex(
          ctx, reinterpret_cast<unsigned char*>(buf.data()) + len, &fin) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) return false;
  buf.resize(static_cast<std::size_t>(len + fin));
  *out = std::move(buf);
  return true;
}

// RFC 5869 HKDF-SHA256（OpenSSL EVP_KDF）：展开 72B＝每方向 密钥32‖前缀4
std::string hkdf_sha256(std::string_view ikm, std::string_view salt,
                        std::string_view info, std::size_t out_len) {
  EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
  if (!kdf) return {};
  EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
  EVP_KDF_free(kdf);
  if (!ctx) return {};
  char digest[] = "SHA256";
  OSSL_PARAM params[5];
  params[0] =
      OSSL_PARAM_construct_utf8_string("digest", digest, 0);
  params[1] = OSSL_PARAM_construct_octet_string(
      "key", const_cast<char*>(ikm.data()), ikm.size());
  params[2] = OSSL_PARAM_construct_octet_string(
      "salt", const_cast<char*>(salt.data()), salt.size());
  params[3] = OSSL_PARAM_construct_octet_string(
      "info", const_cast<char*>(info.data()), info.size());
  params[4] = OSSL_PARAM_construct_end();
  std::string out(out_len, '\0');
  const bool ok =
      EVP_KDF_CTX_set_params(ctx, params) == 1 &&
      EVP_KDF_derive(ctx, reinterpret_cast<unsigned char*>(out.data()),
                     out.size(), nullptr) == 1;
  EVP_KDF_CTX_free(ctx);
  if (!ok) return {};
  return out;
}

// TOFU 定针：首触记录、已录比对，不符即拒；写入失败按拒处理（fail-closed）。
enum class PinCheck { kNew, kMatch, kMismatch, kError };

PinCheck verify_pin(LocalStore* store, const std::string& device_id,
                    const std::string& pub_hex) {
  const std::string pinned = store->peer_identity_pub(device_id);
  if (!pinned.empty()) {
    return pinned == pub_hex ? PinCheck::kMatch : PinCheck::kMismatch;
  }
  if (!store->pin_peer_identity(device_id, pub_hex)) return PinCheck::kError;
  const std::string again = store->peer_identity_pub(device_id);
  if (again.empty()) return PinCheck::kError;
  return again == pub_hex ? PinCheck::kNew : PinCheck::kMismatch;
}

} // namespace

// ---------- DeviceIdentity ----------

DeviceIdentity::~DeviceIdentity() {
  if (pkey_) EVP_PKEY_free(static_cast<EVP_PKEY*>(pkey_));
}

DeviceIdentity::DeviceIdentity(DeviceIdentity&& o) noexcept
    : pkey_(o.pkey_), pub_(std::move(o.pub_)) {
  o.pkey_ = nullptr;
}

DeviceIdentity& DeviceIdentity::operator=(DeviceIdentity&& o) noexcept {
  if (this != &o) {
    if (pkey_) EVP_PKEY_free(static_cast<EVP_PKEY*>(pkey_));
    pkey_ = o.pkey_;
    pub_ = std::move(o.pub_);
    o.pkey_ = nullptr;
  }
  return *this;
}

std::string DeviceIdentity::pub_hex() const {
  return QByteArray(pub_.data(), qsizetype(pub_.size())).toHex().toStdString();
}

DeviceIdentity DeviceIdentity::load(LocalStore* store) {
  DeviceIdentity id;
  if (!store) return id;
  std::string seed_hex;
  std::string stored_pub_hex;
  if (!store->read_identity(&seed_hex, &stored_pub_hex)) {
    unsigned char seed[32];
    if (RAND_bytes(seed, sizeof(seed)) != 1) {
      qWarning() << "[直连安全] 随机数生成失败，设备身份未就绪";
      return id;
    }
    EVP_PKEY* probe = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                   seed, sizeof(seed));
    unsigned char pub[32];
    std::size_t plen = sizeof(pub);
    const bool derived =
        probe != nullptr &&
        EVP_PKEY_get_raw_public_key(probe, pub, &plen) == 1 && plen == 32;
    if (probe) EVP_PKEY_free(probe);
    if (!derived) {
      std::fill(seed, seed + sizeof(seed), 0);
      qWarning() << "[直连安全] 身份密钥生成失败";
      return id;
    }
    seed_hex = hex_of(seed, sizeof(seed));
    stored_pub_hex = hex_of(pub, sizeof(pub));
    std::fill(seed, seed + sizeof(seed), 0); // 种子只在库内存活
    if (!store->save_identity(seed_hex, stored_pub_hex) ||
        !store->read_identity(&seed_hex, &stored_pub_hex)) {
      qWarning() << "[直连安全] 设备身份落库失败";
      return id;
    }
    qInfo() << "[直连安全] 首次生成设备身份（Ed25519）并落库";
  }
  const std::string seed = unhex(seed_hex);
  if (seed.size() != 32) {
    qWarning() << "[直连安全] 身份种子损坏";
    return id;
  }
  EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
      EVP_PKEY_ED25519, nullptr,
      reinterpret_cast<const unsigned char*>(seed.data()), seed.size());
  if (!pkey) {
    qWarning() << "[直连安全] 身份密钥载入失败";
    return id;
  }
  unsigned char pub[32];
  std::size_t plen = sizeof(pub);
  if (EVP_PKEY_get_raw_public_key(pkey, pub, &plen) != 1 || plen != 32) {
    EVP_PKEY_free(pkey);
    qWarning() << "[直连安全] 身份公钥导出失败";
    return id;
  }
  const std::string derived_hex = hex_of(pub, sizeof(pub));
  if (derived_hex != stored_pub_hex) {
    // 库内公钥列仅作参考（一切校验以握手携带、签名验证为准），以种子派生为准
    qWarning() << "[直连安全] 库内公钥与种子派生不一致（以派生为准）";
  }
  id.pkey_ = pkey;
  id.pub_.assign(reinterpret_cast<const char*>(pub), sizeof(pub));
  return id;
}

std::string DeviceIdentity::sign(std::string_view msg) const {
  if (!pkey_) return {};
  EVP_MD_CTX* md = EVP_MD_CTX_new();
  if (!md) return {};
  std::string sig(kSigBytes, '\0');
  std::size_t len = kSigBytes;
  const bool ok =
      EVP_DigestSignInit(md, nullptr, nullptr, nullptr,
                         static_cast<EVP_PKEY*>(pkey_)) == 1 &&
      EVP_DigestSign(md, reinterpret_cast<unsigned char*>(sig.data()), &len,
                     reinterpret_cast<const unsigned char*>(msg.data()),
                     msg.size()) == 1 &&
      len == kSigBytes;
  EVP_MD_CTX_free(md);
  if (!ok) return {};
  return sig;
}

// ---------- SecureChannel ----------

SecureChannel::SecureChannel(Role role, const DeviceIdentity* self,
                             LocalStore* store, std::string self_id,
                             std::string peer_id)
    : role_(role), self_(self), store_(store), self_id_(std::move(self_id)),
      peer_id_(std::move(peer_id)) {
  if (!self_ || !self_->valid()) {
    fail(QStringLiteral("设备身份未就绪"));
    return;
  }
  if (!store_) {
    fail(QStringLiteral("本地库未就绪"));
    return;
  }
  our_nonce_.resize(kHandshakeNonceBytes);
  if (RAND_bytes(reinterpret_cast<unsigned char*>(our_nonce_.data()),
                 static_cast<int>(our_nonce_.size())) != 1) {
    fail(QStringLiteral("随机数生成失败"));
  }
}

SecureChannel::~SecureChannel() {
  if (our_eph_) EVP_PKEY_free(static_cast<EVP_PKEY*>(our_eph_));
}

void SecureChannel::fail(const QString& reason) {
  if (failed_) return;
  failed_ = true;
  reason_ = reason;
}

bool SecureChannel::ensure_eph() {
  if (our_eph_) return true;
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
  EVP_PKEY* pkey = nullptr;
  const bool ok = ctx != nullptr && EVP_PKEY_keygen_init(ctx) == 1 &&
                  EVP_PKEY_keygen(ctx, &pkey) == 1;
  if (ctx) EVP_PKEY_CTX_free(ctx);
  if (!ok || !pkey) {
    if (pkey) EVP_PKEY_free(pkey);
    fail(QStringLiteral("临时密钥生成失败"));
    return false;
  }
  std::string pub(kIdentityKeyBytes, '\0');
  std::size_t plen = pub.size();
  if (EVP_PKEY_get_raw_public_key(pkey,
                                  reinterpret_cast<unsigned char*>(pub.data()),
                                  &plen) != 1 ||
      plen != pub.size()) {
    EVP_PKEY_free(pkey);
    fail(QStringLiteral("临时公钥导出失败"));
    return false;
  }
  our_eph_ = pkey;
  our_eph_pub_ = std::move(pub);
  return true;
}

std::string SecureChannel::make_hello(const std::string& text,
                                      const std::string& to_id) const {
  const std::string sig = self_->sign(text);
  if (sig.empty()) return {};
  Message h;
  h.set_type(MsgType::HELLO);
  h.set_from(self_id_);
  h.set_to(to_id);
  h.set_ts_ms(QDateTime::currentMSecsSinceEpoch());
  auto* hh = h.mutable_hello();
  hh->set_proto_version(kDirectSecureVersion);
  hh->set_identity_pub(self_->pub());
  hh->set_eph_pub(our_eph_pub_);
  hh->set_nonce(our_nonce_);
  hh->set_sig(sig);
  return memex::protocol::encode(h);
}

std::string SecureChannel::start() {
  if (failed_ || role_ != Role::kInitiator || sent_hello_) return {};
  if (!ensure_eph()) return {};
  init_text_ = build_init_text(self_id_, peer_id_, self_->pub(),
                               our_eph_pub_, our_nonce_);
  const std::string frame = make_hello(init_text_, peer_id_);
  if (frame.empty()) {
    fail(QStringLiteral("握手签名生成失败"));
    return {};
  }
  sent_hello_ = true;
  return frame;
}

bool SecureChannel::derive_keys() {
  if (!our_eph_) {
    fail(QStringLiteral("临时密钥未就绪"));
    return false;
  }
  EVP_PKEY* peer = EVP_PKEY_new_raw_public_key(
      EVP_PKEY_X25519, nullptr,
      reinterpret_cast<const unsigned char*>(their_eph_.data()),
      their_eph_.size());
  if (!peer) {
    fail(QStringLiteral("对端临时公钥非法"));
    return false;
  }
  EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(static_cast<EVP_PKEY*>(our_eph_),
                                       nullptr);
  std::string shared(kIdentityKeyBytes, '\0');
  std::size_t slen = shared.size();
  const bool ok =
      ctx != nullptr && EVP_PKEY_derive_init(ctx) == 1 &&
      EVP_PKEY_derive_set_peer(ctx, peer) == 1 &&
      EVP_PKEY_derive(ctx, reinterpret_cast<unsigned char*>(shared.data()),
                      &slen) == 1 &&
      slen == shared.size();
  if (ctx) EVP_PKEY_CTX_free(ctx);
  EVP_PKEY_free(peer);
  if (!ok) {
    fail(QStringLiteral("X25519 密钥协商失败"));
    return false;
  }
  if (std::all_of(shared.begin(), shared.end(),
                  [](char c) { return c == '\0'; })) {
    fail(QStringLiteral("X25519 共享密钥退化"));
    return false;
  }
  // salt＝发起方盐在前（双方各自按角色拼装，字节序一致）
  const std::string salt =
      role_ == Role::kInitiator ? our_nonce_ + their_nonce_
                                : their_nonce_ + our_nonce_;
  const std::string info =
      std::string(kHkdfInfoPrefix) + sha256(init_text_ + resp_text_);
  const std::string okm = hkdf_sha256(shared, salt, info, 72);
  std::fill(shared.begin(), shared.end(), '\0'); // 共享密钥用毕即清
  if (okm.size() != 72) {
    fail(QStringLiteral("会话密钥派生失败"));
    return false;
  }
  const std::string k_i2r = okm.substr(0, 32);
  const std::string k_r2i = okm.substr(32, 32);
  const std::string p_i2r = okm.substr(64, 4);
  const std::string p_r2i = okm.substr(68, 4);
  if (role_ == Role::kInitiator) {
    send_key_ = k_i2r;
    recv_key_ = k_r2i;
    send_prefix_ = p_i2r;
    recv_prefix_ = p_r2i;
  } else {
    send_key_ = k_r2i;
    recv_key_ = k_i2r;
    send_prefix_ = p_r2i;
    recv_prefix_ = p_i2r;
  }
  return true;
}

bool SecureChannel::on_hello_frame(const std::string& payload, Fed* out) {
  Message msg;
  try {
    msg = memex::protocol::decode_payload(payload);
  } catch (const memex::protocol::ProtocolError&) {
    fail(QStringLiteral("握手帧解析失败"));
    return false;
  }
  if (msg.type() != MsgType::HELLO) {
    fail(QStringLiteral("未握手即收应用数据（明文拒绝）"));
    return false;
  }
  if (!msg.has_hello()) {
    fail(QStringLiteral("握手帧缺 hello 字段"));
    return false;
  }
  const auto& h = msg.hello();
  if (h.proto_version() < kDirectSecureVersion) {
    fail(QStringLiteral("直连协议版本不支持（需≥%1 安全握手）")
             .arg(kDirectSecureVersion));
    return false;
  }
  if (h.identity_pub().size() != kIdentityKeyBytes ||
      h.eph_pub().size() != kIdentityKeyBytes ||
      h.nonce().size() != kHandshakeNonceBytes ||
      h.sig().size() != kSigBytes) {
    fail(QStringLiteral("握手字段长度非法"));
    return false;
  }
  if (msg.from().empty()) {
    fail(QStringLiteral("握手缺设备标识"));
    return false;
  }
  if (msg.from() == self_id_) {
    fail(QStringLiteral("对端标识与本机相同"));
    return false;
  }
  const std::string pub_hex = hex_of(
      reinterpret_cast<const unsigned char*>(h.identity_pub().data()),
      h.identity_pub().size());

  if (role_ == Role::kResponder) {
    if (msg.to() != self_id_) {
      fail(QStringLiteral("握手目标不是本机"));
      return false;
    }
    init_text_ = build_init_text(msg.from(), msg.to(), h.identity_pub(),
                                 h.eph_pub(), h.nonce());
    // 持证校验用原始字节公钥（pub_hex 只用于定针比对）
    if (!ed25519_verify(h.identity_pub(), init_text_, h.sig())) {
      fail(QStringLiteral("握手签名无效（持证校验拒）"));
      return false;
    }
    const PinCheck pin = verify_pin(store_, msg.from(), pub_hex);
    if (pin == PinCheck::kMismatch) {
      fail(QStringLiteral("对端身份与定针不符（拒绝冒充或未确认换钥）"));
      return false;
    }
    if (pin == PinCheck::kError) {
      fail(QStringLiteral("对端定针写入失败"));
      return false;
    }
    their_id_ = msg.from();
    their_pub_ = pub_hex;
    their_eph_ = h.eph_pub();
    their_nonce_ = h.nonce();
    peer_id_ = their_id_; // 响应方：HELLO1 后回填已认证对端
    if (!ensure_eph()) return false;
    resp_text_ = build_resp_text(self_id_, their_id_, self_->pub(),
                                 our_eph_pub_, our_nonce_,
                                 sha256(init_text_));
    out->reply = make_hello(resp_text_, their_id_);
    if (out->reply.empty()) {
      fail(QStringLiteral("握手签名生成失败"));
      return false;
    }
    if (!derive_keys()) return false;
    established_ = true;
    return true;
  }

  // 发起方收 HELLO2：绑定所拨对端标识 + 验签 + 定针
  if (msg.from() != peer_id_) {
    fail(QStringLiteral("响应方标识与所拨不符"));
    return false;
  }
  if (msg.to() != self_id_) {
    fail(QStringLiteral("握手回执目标不是本机"));
    return false;
  }
  resp_text_ = build_resp_text(msg.from(), msg.to(), h.identity_pub(),
                               h.eph_pub(), h.nonce(), sha256(init_text_));
  if (!ed25519_verify(h.identity_pub(), resp_text_, h.sig())) {
    fail(QStringLiteral("握手签名无效（持证校验拒）"));
    return false;
  }
  const PinCheck pin = verify_pin(store_, msg.from(), pub_hex);
  if (pin == PinCheck::kMismatch) {
    fail(QStringLiteral("对端身份与定针不符（拒绝冒充或未确认换钥）"));
    return false;
  }
  if (pin == PinCheck::kError) {
    fail(QStringLiteral("对端定针写入失败"));
    return false;
  }
  their_id_ = msg.from();
  their_pub_ = pub_hex;
  their_eph_ = h.eph_pub();
  their_nonce_ = h.nonce();
  if (!derive_keys()) return false;
  established_ = true;
  return true;
}

SecureChannel::Fed SecureChannel::feed(std::string_view raw) {
  Fed out;
  if (failed_) {
    out.failed = true;
    out.reason = reason_;
    return out;
  }
  std::vector<std::string> frames;
  const DecodeStatus st = decoder_.feed(raw, frames);
  if (st == DecodeStatus::kZeroLength) {
    fail(QStringLiteral("非法帧（零长度）"));
  } else if (st == DecodeStatus::kTooLarge) {
    fail(QStringLiteral("非法帧（超长）"));
  }
  const bool was_established = established_;
  for (const auto& payload : frames) {
    if (failed_) break;
    if (!established_) {
      if (!on_hello_frame(payload, &out)) break;
      continue;
    }
    // —— 会话帧解封：计数严格递增 + GCM 认证 ——
    if (payload.size() < kCtrBytes + kAeadTagBytes) {
      fail(QStringLiteral("密文帧过短"));
      break;
    }
    std::uint64_t ctr = 0;
    for (std::size_t i = 0; i < kCtrBytes; ++i) {
      ctr = (ctr << 8) | static_cast<std::uint8_t>(payload[i]);
    }
    if (ctr != recv_ctr_) {
      fail(QStringLiteral("帧计数错序（重放或截断）"));
      break;
    }
    const std::string nonce = recv_prefix_ + be64(ctr);
    std::string pt;
    if (!gcm_open(recv_key_, nonce,
                  std::string_view(payload).substr(kCtrBytes), &pt)) {
      fail(QStringLiteral("密文认证失败（篡改或密钥不符）"));
      break;
    }
    ++recv_ctr_;
    out.payloads.push_back(std::move(pt));
  }
  out.failed = failed_;
  out.reason = reason_;
  out.established = established_ && !was_established;
  return out;
}

std::string SecureChannel::protect(std::string_view payload) {
  if (failed_ || !established_) return {};
  if (payload.empty() ||
      payload.size() + kCtrBytes + kAeadTagBytes >
          memex::protocol::kMaxFrameSize) {
    return {};
  }
  const std::uint64_t ctr = send_ctr_++;
  const std::string nonce = send_prefix_ + be64(ctr);
  std::string sealed;
  if (!gcm_seal(send_key_, nonce, payload, &sealed)) {
    fail(QStringLiteral("密文封装失败"));
    return {};
  }
  std::string body;
  body.reserve(kCtrBytes + sealed.size());
  body.append(be64(ctr));
  body.append(sealed);
  return memex::protocol::encode_frame(body);
}

} // namespace memex::client
