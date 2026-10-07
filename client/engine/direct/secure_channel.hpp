// 平台-8 直连安全：设备身份 + 认证握手 + X25519 会话密钥 + AEAD
// （消息与文件共用的加密边界；发现宣告只做发现，不担身份认证——
//  UDP 宣告不带身份，身份只在 TCP 握手上认证）。
//
// 握手（两帧明文 HELLO，双向签名，TOFU 定针；协议版本 v2）：
//   发起方 → 响应方: HELLO{from=A, to=B, v2, identity_pub, eph_pub, nonce, sig}
//   响应方 → 发起方: HELLO{from=B, to=A, v2, identity_pub, eph_pub, nonce, sig}
//   init 文本 = "memex-direct-v1" ‖ u32be(2) ‖ A ‖ B ‖ pubA ‖ ephA ‖ nonceA
//   resp 文本 = "memex-direct-v1" ‖ u32be(2) ‖ B ‖ A ‖ pubB ‖ ephB ‖ nonceB ‖ init 文本
//   双方各自签 resp/init 文本（Ed25519 直签原文），签前先验持证、再对定针。
// 身份＝Ed25519 长期密钥对（种子存本地库，对外仅公钥）；定针＝TOFU：
// 首触记下 device_id↔pub，此后不符即拒——换钥须显式清针，冒充当场断，
// 无明文回退（握手不成即连接失效，发送侧回报失败）。
// 会话密钥＝HKDF-SHA256(ikm=X25519(临时 ECDH), salt=nonceA‖nonceB,
// info="memex-direct-v1/key"‖握手文本) → 每方向 32B 密钥 ‖ 4B nonce 前缀。
//
// 会话帧 = [4B BE 长度 = 8+pt+16][8B BE 计数][AES-256-GCM(ct‖tag16)]；
// nonce = 方向前缀4B ‖ 计数8B（密钥按连接新派生、计数连接内不复用）；
// 接收计数严格递增（TCP 保序，错序/重放/截断即断）。握手之后线路无明文。
#pragma once

#include <QString>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <memex/protocol/frame.hpp>

namespace memex::client {

class LocalStore;

// 设备身份（Ed25519 长期密钥对）：load() 从本地库读，缺失即生成落库。
// 种子不出对象、不进日志；对外只有公钥。持密钥不可复制（可移动）。
class DeviceIdentity {
public:
  DeviceIdentity() = default;
  ~DeviceIdentity();
  DeviceIdentity(DeviceIdentity&&) noexcept;
  DeviceIdentity& operator=(DeviceIdentity&&) noexcept;
  DeviceIdentity(const DeviceIdentity&) = delete;
  DeviceIdentity& operator=(const DeviceIdentity&) = delete;

  // 库里没有则生成并落库；失败返回无效对象（valid()==false）。
  static DeviceIdentity load(LocalStore* store);

  bool valid() const { return pkey_ != nullptr; }
  const std::string& pub() const { return pub_; } // 32B 原始字节
  std::string pub_hex() const;                    // 64 hex
  // Ed25519 直签原文；失败返回空。
  std::string sign(std::string_view msg) const;

private:
  void* pkey_{nullptr}; // EVP_PKEY*（头文件不暴露 OpenSSL）
  std::string pub_;     // 32B
};

// 直连安全信道：一条 TCP 连接一个实例（连接即会话，连接断即终）。
// 纯同步拉式 API（无 socket 依赖，可单测对拨）：start → feed → protect。
class SecureChannel {
public:
  enum class Role { kInitiator, kResponder };

  // self=本端身份；peer_id 仅发起方填（所拨对端设备标识，进签名文本）。
  SecureChannel(Role role, const DeviceIdentity* self, LocalStore* store,
                std::string self_id, std::string peer_id);
  ~SecureChannel();
  SecureChannel(const SecureChannel&) = delete;
  SecureChannel& operator=(const SecureChannel&) = delete;

  // 发起方启动：返回 HELLO1 帧字节（响应方恒为空串）。
  std::string start();

  struct Fed {
    bool failed{false};       // 信道已失效（reason 定因；此后一律拒）
    QString reason;
    bool established{false};  // 本轮完成握手（发起方应在此后发首帧应用数据）
    std::string reply;        // 须写出的字节（响应方 HELLO2；其余场景空）
    std::vector<std::string> payloads; // 应用载荷（握手帧不产出）
  };
  // 喂入原始字节（容忍任意切分）；failed 后调用恒返回 failed。
  Fed feed(std::string_view raw);

  // 应用载荷 → 会话帧字节（须 established 且载荷非空；失败返回空串）。
  std::string protect(std::string_view payload);

  bool established() const { return established_; }
  bool failed() const { return failed_; }
  QString fail_reason() const { return reason_; }
  // 已认证对端设备标识：发起方＝所拨 peer_id；响应方＝HELLO1 里验签＋定针
  // 通过的 from（应用层 from/to 与此比对，防已认证对端冒名）。
  const std::string& peer_id() const { return peer_id_; }

private:
  void fail(const QString& reason);
  // 握手帧处理（明文阶段唯一放行类型）；false=信道已失效
  bool on_hello_frame(const std::string& payload, Fed* out);
  bool ensure_eph();      // 生成本端临时 X25519 密钥对（一次）
  bool derive_keys();     // ECDH+HKDF 派生双向密钥（须双方字段齐）
  std::string make_hello(const std::string& init_or_resp_text,
                         const std::string& to_id) const;

  Role role_;
  const DeviceIdentity* self_{nullptr};
  LocalStore* store_{nullptr};
  std::string self_id_;
  std::string peer_id_; // 发起方：所拨对端；响应方：HELLO1 后回填

  bool sent_hello_{false}; // 发起方已发 HELLO1
  std::string our_nonce_;  // 16B
  void* our_eph_{nullptr}; // EVP_PKEY*（X25519 临时，用即弃）
  std::string our_eph_pub_;
  std::string init_text_; // 发起方启动时算；响应方由 HELLO1 重算
  std::string resp_text_; // 响应方 HELLO1 时算；发起方由 HELLO2 重算

  std::string their_id_;
  std::string their_pub_;  // 32B（待定针校验）
  std::string their_eph_;  // 32B
  std::string their_nonce_; // 16B

  bool established_{false};
  bool failed_{false};
  QString reason_;
  std::string send_key_, recv_key_;       // 32B
  std::string send_prefix_, recv_prefix_; // 4B
  std::uint64_t send_ctr_{1};
  std::uint64_t recv_ctr_{1};

  memex::protocol::FrameDecoder decoder_; // 外层 4B 长度成帧
};

} // namespace memex::client
