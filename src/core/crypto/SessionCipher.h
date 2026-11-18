#pragma once

#include "core/platform/Common.h"
#include "core/protocol/FrameCodec.h"

#include <memory>
#include <string>

namespace bm {

namespace crypto {
class AesGcm;
}

// 两个方向各用独立密钥与 nonce 前缀；接收只接受严格下一序号
class SessionCipher {
 public:
  // initiator 决定哪一侧用客户端密钥发送；材料布局与 .NET 版逐字节一致
  SessionCipher(std::span<const uint8_t> secret, std::span<const uint8_t> transcriptHash, bool initiator);
  ~SessionCipher();
  SessionCipher(const SessionCipher&) = delete;
  SessionCipher& operator=(const SessionCipher&) = delete;

  // 整帧：帧头 + 密文 + 认证标签；帧头同时是 GCM 的认证数据
  Bytes Seal(MessageType type, std::span<const uint8_t> plaintext);
  Bytes Open(std::span<const uint8_t> header, std::span<const uint8_t> encrypted);
  // 角色域拼接上下文的 HMAC，用于证明双方握有同一会话密钥
  Bytes Confirmation(bool initiator, std::span<const uint8_t> context) const;

  const std::string& ShortAuthenticationCode() const { return shortCode_; }

 private:
  std::unique_ptr<crypto::AesGcm> send_;
  std::unique_ptr<crypto::AesGcm> receive_;
  uint8_t sendPrefix_[4] = {};
  uint8_t receivePrefix_[4] = {};
  Bytes confirmationKey_;
  std::string shortCode_;
  uint64_t sendSequence_ = 0;
  uint64_t receiveSequence_ = 0;
};

}  // namespace bm
