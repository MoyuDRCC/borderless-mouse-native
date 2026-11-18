#pragma once

#include "core/platform/Common.h"
#include "core/input/DesktopGeometry.h"

#include <string>

namespace bm {

class DeviceIdentity;

namespace crypto {
class EcdhKey;
}

// Hello 解析结果；两个公钥都是 91 字节规范 SPKI
struct HelloData {
  std::string name;
  DesktopGeometry desktop;
  int port = 0;
  Bytes identityKey;
  Bytes ephemeralKey;
};

// 算法与字段顺序固定，不做协商降级；签名覆盖两个原始 Hello 帧与角色域
struct HandshakeProtocol {
  static Bytes EncodeHello(std::string_view name, const DesktopGeometry& desktop, int port,
                           const DeviceIdentity& identity, const crypto::EcdhKey& ephemeral);
  static HelloData ParseHello(std::span<const uint8_t> payload);
  // transcript = SHA256(域串 || 客户端 Hello 帧 || 服务端 Hello 帧)
  static Bytes Transcript(std::span<const uint8_t> clientFrame, std::span<const uint8_t> serverFrame);
  static Bytes ProofHash(std::span<const uint8_t> transcript, bool initiator);
  // 身份证明只认持久的 P-256 签名身份，64 字节 P1363 签名
  static void VerifyProof(std::span<const uint8_t> publicKey, std::span<const uint8_t> proof,
                          std::span<const uint8_t> transcript, bool initiator);
};

}  // namespace bm
