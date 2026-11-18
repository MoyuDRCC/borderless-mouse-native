#include "core/crypto/SessionCipher.h"

#include "core/crypto/Crypto.h"

#include <cstring>
#include <stdexcept>

namespace bm {

namespace {

// HKDF 的 info 域，与 .NET 版字面量完全相同
constexpr char kSessionKeyInfo[] = "BorderlessMouse session keys";
constexpr char kClientConfirmation[] = "BorderlessMouse client confirmation";
constexpr char kServerConfirmation[] = "BorderlessMouse server confirmation";

// nonce = 4 字节前缀 + 8 字节小端序号
void BuildNonce(uint8_t* nonce, const uint8_t* prefix, uint64_t sequence) {
  std::memcpy(nonce, prefix, 4);
  for (int i = 0; i < 8; i++) nonce[4 + i] = static_cast<uint8_t>(sequence >> (8 * i));
}

}  // namespace

SessionCipher::SessionCipher(std::span<const uint8_t> secret, std::span<const uint8_t> transcriptHash,
                             bool initiator) {
  // 110 字节材料：客户端密钥 32、客户端 nonce 前缀 4、服务端密钥 32、服务端前缀 4、确认密钥 32、核对码 6
  Bytes material = crypto::HkdfSha256(secret, transcriptHash,
                                      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(kSessionKeyInfo),
                                                               sizeof(kSessionKeyInfo) - 1),
                                      110);
  std::span<const uint8_t> clientKey(material.data(), 32);
  std::span<const uint8_t> serverKey(material.data() + 36, 32);
  send_ = std::make_unique<crypto::AesGcm>(initiator ? clientKey : serverKey);
  receive_ = std::make_unique<crypto::AesGcm>(initiator ? serverKey : clientKey);
  std::memcpy(sendPrefix_, material.data() + (initiator ? 32 : 68), 4);
  std::memcpy(receivePrefix_, material.data() + (initiator ? 68 : 32), 4);
  confirmationKey_.assign(material.begin() + 72, material.begin() + 104);
  shortCode_ = ToHex(std::span<const uint8_t>(material.data() + 104, 6));
  SecureZero(material);
}

SessionCipher::~SessionCipher() {
  SecureZero(confirmationKey_);
  SecureZero(std::span<uint8_t>(sendPrefix_, 4));
  SecureZero(std::span<uint8_t>(receivePrefix_, 4));
}

Bytes SessionCipher::Seal(MessageType type, std::span<const uint8_t> plaintext) {
  if (plaintext.size() > kMaxPlainPayload) throw std::out_of_range("明文载荷超过可用长度");
  if (sendSequence_ == UINT64_MAX) throw std::runtime_error("发送序号已耗尽，必须重新建立会话");
  const uint64_t sequence = ++sendSequence_;
  Bytes frame(kHeaderSize + plaintext.size() + kTagSize, 0);
  WriteFrameHeader(std::span<uint8_t>(frame.data(), kHeaderSize), type,
                   static_cast<uint32_t>(plaintext.size() + kTagSize), sequence);
  uint8_t nonce[kNonceSize] = {};
  BuildNonce(nonce, sendPrefix_, sequence);
  send_->Seal(nonce, plaintext, std::span<const uint8_t>(frame.data(), kHeaderSize),
              std::span<uint8_t>(frame.data() + kHeaderSize, plaintext.size()),
              std::span<uint8_t>(frame.data() + kHeaderSize + plaintext.size(), kTagSize));
  return frame;
}

Bytes SessionCipher::Open(std::span<const uint8_t> header, std::span<const uint8_t> encrypted) {
  FrameHeader info{};
  if (!TryReadFrameHeader(header, info) || info.payloadLength != encrypted.size() || encrypted.size() < kTagSize)
    throw std::runtime_error("加密帧长度或帧头非法");
  if (receiveSequence_ == UINT64_MAX || info.sequence != receiveSequence_ + 1)
    throw std::runtime_error("加密帧序号重复、跳号或回绕");
  uint8_t nonce[kNonceSize] = {};
  BuildNonce(nonce, receivePrefix_, info.sequence);
  const size_t length = encrypted.size() - kTagSize;
  Bytes plaintext(length);
  if (!receive_->Open(nonce, encrypted.first(length), encrypted.subspan(length, kTagSize), header, plaintext))
    throw std::runtime_error("加密帧认证失败");
  receiveSequence_ = info.sequence;
  return plaintext;
}

Bytes SessionCipher::Confirmation(bool initiator, std::span<const uint8_t> context) const {
  const char* role = initiator ? kClientConfirmation : kServerConfirmation;
  const size_t roleLength = std::strlen(role);
  Bytes input(role, role + roleLength);
  input.insert(input.end(), context.begin(), context.end());
  return crypto::HmacSha256(confirmationKey_, input);
}

}  // namespace bm
