#include "cred.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <stdexcept>

namespace memex::server {

namespace {
constexpr int kSaltBytes = 16;
constexpr int kPbkdf2Iterations = 60000; // 内网办公场景与登录时延的折中
} // namespace

std::string to_hex(const unsigned char* p, std::size_t n) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(kHex[p[i] >> 4]);
    out.push_back(kHex[p[i] & 0xF]);
  }
  return out;
}

std::string random_salt_hex() {
  std::array<unsigned char, kSaltBytes> buf{};
  if (RAND_bytes(buf.data(), static_cast<int>(buf.size())) != 1) return {};
  return to_hex(buf.data(), buf.size());
}

int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// hex → bytes；非法字符返回空
std::string from_hex(const std::string& hex) {
  if (hex.size() % 2 != 0) return {};
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hex_val(hex[i]), lo = hex_val(hex[i + 1]);
    if (hi < 0 || lo < 0) return {};
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

std::string pbkdf2_sha256_hex(const std::string& password,
                              const std::string& salt_hex, int iterations) {
  const std::string salt = from_hex(salt_hex);
  if (salt.empty() || iterations < 10000) {
    throw std::invalid_argument("盐非法或迭代次数过低");
  }
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int out_len = 0;
  if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                        reinterpret_cast<const unsigned char*>(salt.data()),
                        static_cast<int>(salt.size()), iterations,
                        EVP_sha256(), sizeof(out), out) != 1) {
    throw std::runtime_error("PBKDF2 计算失败");
  }
  return to_hex(out, 32); // SHA-256 输出固定 32 字节
}

std::string sha256_hex(const std::string& data) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (EVP_Digest(data.data(), data.size(), md, &md_len, EVP_sha256(),
                 nullptr) != 1) {
    throw std::runtime_error("SHA-256 计算失败");
  }
  return to_hex(md, md_len);
}

// —— R25-4 工具凭据面（与群密码箱同构的 GCM 封装，但密钥在服务端：代理
// 调用无人在线解密，主密钥随部署配置；客户端只见掩码永不取回明文）——

std::string derive_tool_cred_key(const std::string& secret) {
  if (secret.empty()) return {}; // 未配主密钥＝凭据面未启用
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int md_len = 0;
  if (EVP_Digest(secret.data(), secret.size(), md, &md_len, EVP_sha256(),
                 nullptr) != 1) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(md), 32);
}

namespace {
constexpr int kGcmNonceBytes = 12;
constexpr int kGcmTagBytes = 16;
} // namespace

std::string gcm_seal(const std::string& key32, const std::string& plaintext) {
  if (key32.size() != 32 || plaintext.empty()) return {};
  unsigned char nonce[kGcmNonceBytes];
  if (RAND_bytes(nonce, sizeof(nonce)) != 1) return {};
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return {};
  std::string out;
  do {
    unsigned char tag[kGcmTagBytes];
    int len = 0, total = 0;
    std::string packed;
    packed.resize(plaintext.size() + kGcmTagBytes);
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) !=
        1) break;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kGcmNonceBytes,
                            nullptr) != 1) break;
    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                           reinterpret_cast<const unsigned char*>(key32.data()),
                           nonce) != 1) break;
    if (EVP_EncryptUpdate(
            ctx, reinterpret_cast<unsigned char*>(packed.data()), &len,
            reinterpret_cast<const unsigned char*>(plaintext.data()),
            static_cast<int>(plaintext.size())) != 1)
      break;
    total = len;
    if (EVP_EncryptFinal_ex(
            ctx, reinterpret_cast<unsigned char*>(packed.data()) + total,
            &len) != 1)
      break;
    total += len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kGcmTagBytes, tag) !=
        1)
      break;
    packed.resize(static_cast<std::size_t>(total));
    std::string raw(reinterpret_cast<const char*>(nonce), sizeof(nonce));
    raw += packed;
    raw.append(reinterpret_cast<const char*>(tag), sizeof(tag));
    out = to_hex(reinterpret_cast<const unsigned char*>(raw.data()),
                 raw.size());
  } while (false);
  EVP_CIPHER_CTX_free(ctx);
  return out;
}

std::string gcm_open(const std::string& key32, const std::string& packed_hex) {
  if (key32.size() != 32) return {};
  const std::string raw = from_hex(packed_hex);
  if (raw.size() <=
      static_cast<std::size_t>(kGcmNonceBytes + kGcmTagBytes)) {
    return {}; // 至少 nonce+tag；空明文不合法（写入面校验非空）
  }
  const unsigned char* nonce =
      reinterpret_cast<const unsigned char*>(raw.data());
  const unsigned char* tag =
      reinterpret_cast<const unsigned char*>(raw.data() + raw.size()) -
      kGcmTagBytes;
  const unsigned char* ct =
      reinterpret_cast<const unsigned char*>(raw.data() + kGcmNonceBytes);
  const int ct_len = static_cast<int>(raw.size()) - kGcmNonceBytes -
                     kGcmTagBytes;
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (!ctx) return {};
  std::string out;
  bool ok = false;
  do {
    int len = 0, total = 0;
    out.resize(raw.size()); // 明文 ≤ 密文长度（同界安全）
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) !=
        1) break;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kGcmNonceBytes,
                            nullptr) != 1) break;
    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                           reinterpret_cast<const unsigned char*>(key32.data()),
                           nonce) != 1) break;
    if (EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()),
                          &len, ct, ct_len) != 1)
      break;
    total = len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kGcmTagBytes,
                            const_cast<unsigned char*>(tag)) != 1)
      break;
    // 验签败（篡改/错钥）＝Final <0 → 不置 ok → 空串
    if (EVP_DecryptFinal_ex(
            ctx, reinterpret_cast<unsigned char*>(out.data()) + total,
            &len) != 1)
      break;
    total += len;
    out.resize(static_cast<std::size_t>(total));
    ok = true;
  } while (false);
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) out.clear();
  return out;
}

} // namespace memex::server
