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

// —— R25-4 工具凭据面：服务端静态加密（AES-256-GCM，密钥=部署密钥
// SHA-256；凭据永不回客户端，代理调用时仅内存内解密）——
// 32B 密钥（raw bytes；secret 空=返回空串＝凭据面未启用）
std::string derive_tool_cred_key(const std::string& secret);
// 密封：hex(nonce12 || ct || tag16)；失败返回空串（口令/参数非法）
std::string gcm_seal(const std::string& key32, const std::string& plaintext);
// 开封：验签通过回明文；篡改/错钥/非法串返回空串
std::string gcm_open(const std::string& key32, const std::string& packed_hex);

} // namespace memex::server
