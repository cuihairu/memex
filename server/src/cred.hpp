// 服务端凭据：PBKDF2-HMAC-SHA256 口令摘要与随机盐（libcrypto）。
// 设备指纹不做服务端再哈希校验（客户端已送 SHA-256 hex，原样留档）。
#pragma once

#include <array>
#include <string>

namespace memex::server {

// 生成 16 字节随机盐（hex 返回）。熵源不可用返回空串。
std::string random_salt_hex();

// PBKDF2-HMAC-SHA256 摘要（hex 返回）。iterations ≥ 10000。
std::string pbkdf2_sha256_hex(const std::string& password,
                              const std::string& salt_hex, int iterations);

// SHA-256 摘要（hex 返回），用于工具与自检。
std::string sha256_hex(const std::string& data);

} // namespace memex::server
