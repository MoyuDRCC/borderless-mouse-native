#pragma once

#include "core/platform/Common.h"

#include <memory>
#include <string>

namespace bm {

namespace crypto {
class EcdsaKey;
}

// 规范完整的 P-256 SubjectPublicKeyInfo 固定为 91 字节
constexpr size_t kP256PublicKeySize = 91;

// 签名与协商公钥都只接受规范、完整的 P-256 公钥
bool IsP256PublicKey(std::span<const uint8_t> der);

// 设备签名身份；私钥只以当前 Windows 用户的 DPAPI 密文保存
class DeviceIdentity {
 public:
  // protectedKey 为空表示首次运行新建身份；已有私钥解不开时抛异常，不偷偷生成另一台设备
  static std::unique_ptr<DeviceIdentity> LoadOrCreate(const std::string* protectedKey);
  ~DeviceIdentity();
  DeviceIdentity(const DeviceIdentity&) = delete;
  DeviceIdentity& operator=(const DeviceIdentity&) = delete;

  // Id = SHA256(SPKI) 的大写十六进制，与 .NET 版同一台设备算出同一串
  const std::string& Id() const { return id_; }
  const std::string& PublicKey() const { return publicKey_; }
  const std::string& ProtectedPrivateKey() const { return protectedKey_; }
  // 握手编码要反复用到原始 SPKI，构造时缓存一份
  const Bytes& PublicKeyDer() const { return publicDer_; }

  // 32 字节哈希进，P1363（r||s，64 字节）签名出
  Bytes Sign(std::span<const uint8_t> hash) const;

 private:
  DeviceIdentity(std::unique_ptr<crypto::EcdsaKey> key, std::string protectedKey);

  std::unique_ptr<crypto::EcdsaKey> key_;
  Bytes publicDer_;
  std::string publicKey_;
  std::string id_;
  std::string protectedKey_;
};

}  // namespace bm
