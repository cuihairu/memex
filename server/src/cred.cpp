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

} // namespace memex::server
