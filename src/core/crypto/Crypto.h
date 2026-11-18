#pragma once

#include "core/platform/Common.h"

#include <initializer_list>
#include <memory>

// Windows CNG / DPAPI 封装，不引入第三方密码库
namespace bm::crypto {

Bytes RandomBytes(size_t size);

Bytes Sha256(std::span<const uint8_t> data);
Bytes Sha256Concat(std::initializer_list<std::span<const uint8_t>> parts);
Bytes HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> data);
// 与 .NET HKDF.DeriveKey(SHA256, ikm, output, salt, info) 完全一致
Bytes HkdfSha256(std::span<const uint8_t> ikm, std::span<const uint8_t> salt, std::span<const uint8_t> info, size_t length);

class AesGcm {
 public:
  explicit AesGcm(std::span<const uint8_t> key);
  ~AesGcm();
  AesGcm(const AesGcm&) = delete;
  AesGcm& operator=(const AesGcm&) = delete;

  void Seal(std::span<const uint8_t> nonce, std::span<const uint8_t> plaintext, std::span<const uint8_t> aad,
            std::span<uint8_t> ciphertext, std::span<uint8_t> tag) const;
  bool Open(std::span<const uint8_t> nonce, std::span<const uint8_t> ciphertext, std::span<const uint8_t> tag,
            std::span<const uint8_t> aad, std::span<uint8_t> plaintext) const;

 private:
  void* key_ = nullptr;
};

// P-256 签名身份；公钥按 .NET 的 91 字节 SubjectPublicKeyInfo 交换
class EcdsaKey {
 public:
  EcdsaKey() = default;
  ~EcdsaKey();
  EcdsaKey(const EcdsaKey&) = delete;
  EcdsaKey& operator=(const EcdsaKey&) = delete;

  static std::unique_ptr<EcdsaKey> Generate();
  static std::unique_ptr<EcdsaKey> ImportPkcs8(std::span<const uint8_t> der);

  Bytes ExportPkcs8() const;
  Bytes ExportSubjectPublicKeyInfo() const;
  // 32 字节哈希进，P1363（r||s，64 字节）签名出
  Bytes SignHash(std::span<const uint8_t> hash) const;
  static bool VerifyHash(std::span<const uint8_t> publicDer, std::span<const uint8_t> hash, std::span<const uint8_t> signature);

 private:
  void* key_ = nullptr;
  bool hasPrivate_ = false;
};

// P-256 临时协商密钥
class EcdhKey {
 public:
  EcdhKey() = default;
  ~EcdhKey();
  EcdhKey(const EcdhKey&) = delete;
  EcdhKey& operator=(const EcdhKey&) = delete;

  static std::unique_ptr<EcdhKey> Generate();
  Bytes ExportSubjectPublicKeyInfo() const;
  // 32 字节 X 坐标（大端），与 .NET DeriveRawSecretAgreement 一致
  Bytes DeriveSecret(std::span<const uint8_t> peerDer) const;

 private:
  void* key_ = nullptr;
};

Bytes DpapiProtect(std::span<const uint8_t> data);
Bytes DpapiUnprotect(std::span<const uint8_t> data);

}  // namespace bm::crypto
