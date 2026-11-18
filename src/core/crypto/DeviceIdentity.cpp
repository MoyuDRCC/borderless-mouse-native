#include "core/crypto/DeviceIdentity.h"

#include "core/crypto/Crypto.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bm {

namespace {

// SPKI 的固定前缀：ecPublicKey + prime256v1 + 66 字节未压缩点
constexpr uint8_t kSpkiPrefix[] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01, 0x06,
                                   0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00, 0x04};

}  // namespace

bool IsP256PublicKey(std::span<const uint8_t> der) {
  return der.size() == kP256PublicKeySize && std::equal(std::begin(kSpkiPrefix), std::end(kSpkiPrefix), der.begin());
}

DeviceIdentity::DeviceIdentity(std::unique_ptr<crypto::EcdsaKey> key, std::string protectedKey)
    : key_(std::move(key)), protectedKey_(std::move(protectedKey)) {
  publicDer_ = key_->ExportSubjectPublicKeyInfo();
  if (!IsP256PublicKey(publicDer_)) throw std::runtime_error("设备公钥不是规范的 P-256 公钥");
  publicKey_ = ToBase64(publicDer_);
  id_ = ToHex(crypto::Sha256(publicDer_));
}

DeviceIdentity::~DeviceIdentity() { SecureZero(publicDer_); }

std::unique_ptr<DeviceIdentity> DeviceIdentity::LoadOrCreate(const std::string* protectedKey) {
  if (protectedKey == nullptr || protectedKey->empty()) {
    std::unique_ptr<crypto::EcdsaKey> created = crypto::EcdsaKey::Generate();
    Bytes privateDer = created->ExportPkcs8();
    std::string text;
    try {
      text = ToBase64(crypto::DpapiProtect(privateDer));
    } catch (...) {
      SecureZero(privateDer);
      throw;
    }
    SecureZero(privateDer);
    return std::unique_ptr<DeviceIdentity>(new DeviceIdentity(std::move(created), std::move(text)));
  }

  // 已存在的私钥解不开时必须让用户处理，不能悄悄生成另一台“设备”
  std::unique_ptr<DeviceIdentity> identity;
  try {
    Bytes plain = crypto::DpapiUnprotect(FromBase64(*protectedKey));
    try {
      identity.reset(new DeviceIdentity(crypto::EcdsaKey::ImportPkcs8(plain), *protectedKey));
    } catch (...) {
      SecureZero(plain);
      throw;
    }
    SecureZero(plain);
  } catch (const std::exception&) {
    throw std::runtime_error("已有设备身份私钥损坏，或不属于当前 Windows 用户，无法加载；未创建新身份。");
  }
  return identity;
}

Bytes DeviceIdentity::Sign(std::span<const uint8_t> hash) const { return key_->SignHash(hash); }

}  // namespace bm
