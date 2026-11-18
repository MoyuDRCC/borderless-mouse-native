#include "core/crypto/Crypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

// 哈希、HMAC、HKDF、AES-GCM 与 DPAPI；P-256 密钥在 CryptoKeys.cpp
namespace bm::crypto {

namespace {

void Check(NTSTATUS status, const char* message) {
  if (!BCRYPT_SUCCESS(status)) throw std::runtime_error(message);
}

class Sha256Hasher {
 public:
  explicit Sha256Hasher(std::span<const uint8_t> key = {}) {
    Check(BCryptOpenAlgorithmProvider(&provider_, BCRYPT_SHA256_ALGORITHM, nullptr,
                                      key.empty() ? 0 : BCRYPT_ALG_HANDLE_HMAC_FLAG),
          "SHA-256 算法不可用");
    Check(BCryptCreateHash(provider_, &hash_, nullptr, 0, const_cast<PUCHAR>(key.data()),
                           static_cast<ULONG>(key.size()), 0),
          "SHA-256 上下文创建失败");
  }
  ~Sha256Hasher() {
    if (hash_) BCryptDestroyHash(hash_);
    if (provider_) BCryptCloseAlgorithmProvider(provider_, 0);
  }
  Sha256Hasher(const Sha256Hasher&) = delete;
  Sha256Hasher& operator=(const Sha256Hasher&) = delete;

  void Update(std::span<const uint8_t> data) {
    Check(BCryptHashData(hash_, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0), "哈希计算失败");
  }

  Bytes Finish() {
    Bytes digest(32);
    Check(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0), "哈希计算失败");
    return digest;
  }

 private:
  BCRYPT_ALG_HANDLE provider_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

}  // namespace

Bytes RandomBytes(size_t size) {
  Bytes result(size);
  Check(BCryptGenRandom(nullptr, result.data(), static_cast<ULONG>(result.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
        "系统随机数生成失败");
  return result;
}

Bytes Sha256(std::span<const uint8_t> data) {
  Sha256Hasher hasher;
  hasher.Update(data);
  return hasher.Finish();
}

Bytes Sha256Concat(std::initializer_list<std::span<const uint8_t>> parts) {
  Sha256Hasher hasher;
  for (std::span<const uint8_t> part : parts) hasher.Update(part);
  return hasher.Finish();
}

Bytes HmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> data) {
  Sha256Hasher hasher(key);
  hasher.Update(data);
  return hasher.Finish();
}

Bytes HkdfSha256(std::span<const uint8_t> ikm, std::span<const uint8_t> salt, std::span<const uint8_t> info, size_t length) {
  // RFC 5869：extract 得到 prk，expand 逐块拼接到目标长度；空盐按全零处理
  if (length > 255 * 32) throw std::invalid_argument("HKDF 输出过长");
  Bytes zeroSalt(32, 0);
  Bytes prk = HmacSha256(salt.empty() ? std::span<const uint8_t>(zeroSalt) : salt, ikm);
  Bytes result;
  Bytes block;
  for (uint8_t counter = 1; result.size() < length; counter++) {
    Bytes input = block;
    input.insert(input.end(), info.begin(), info.end());
    input.push_back(counter);
    block = HmacSha256(prk, input);
    size_t take = std::min(block.size(), length - result.size());
    result.insert(result.end(), block.begin(), block.begin() + static_cast<ptrdiff_t>(take));
  }
  SecureZero(prk);
  SecureZero(block);
  return result;
}

AesGcm::AesGcm(std::span<const uint8_t> key) {
  if (key.size() != 32) throw std::invalid_argument("AES-256-GCM 需要 32 字节密钥");
  BCRYPT_ALG_HANDLE provider = nullptr;
  Check(BCryptOpenAlgorithmProvider(&provider, BCRYPT_AES_ALGORITHM, nullptr, 0), "AES 算法不可用");
  wchar_t chaining[] = BCRYPT_CHAIN_MODE_GCM;
  Check(BCryptSetProperty(provider, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(chaining), sizeof(chaining), 0),
        "AES-GCM 模式设置失败");
  BCRYPT_KEY_HANDLE handle = nullptr;
  Check(BCryptGenerateSymmetricKey(provider, &handle, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                   static_cast<ULONG>(key.size()), 0),
        "AES-GCM 密钥导入失败");
  BCryptCloseAlgorithmProvider(provider, 0);
  key_ = handle;
}

AesGcm::~AesGcm() {
  if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

void AesGcm::Seal(std::span<const uint8_t> nonce, std::span<const uint8_t> plaintext, std::span<const uint8_t> aad,
                  std::span<uint8_t> ciphertext, std::span<uint8_t> tag) const {
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = const_cast<PUCHAR>(nonce.data());
  info.cbNonce = static_cast<ULONG>(nonce.size());
  info.pbAuthData = const_cast<PUCHAR>(aad.data());
  info.cbAuthData = static_cast<ULONG>(aad.size());
  info.pbTag = tag.data();
  info.cbTag = static_cast<ULONG>(tag.size());
  ULONG written = 0;
  Check(BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), const_cast<PUCHAR>(plaintext.data()),
                      static_cast<ULONG>(plaintext.size()), &info, nullptr, 0, ciphertext.data(),
                      static_cast<ULONG>(ciphertext.size()), &written, 0),
        "AES-GCM 加密失败");
}

bool AesGcm::Open(std::span<const uint8_t> nonce, std::span<const uint8_t> ciphertext, std::span<const uint8_t> tag,
                  std::span<const uint8_t> aad, std::span<uint8_t> plaintext) const {
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
  BCRYPT_INIT_AUTH_MODE_INFO(info);
  info.pbNonce = const_cast<PUCHAR>(nonce.data());
  info.cbNonce = static_cast<ULONG>(nonce.size());
  info.pbAuthData = const_cast<PUCHAR>(aad.data());
  info.cbAuthData = static_cast<ULONG>(aad.size());
  info.pbTag = const_cast<PUCHAR>(tag.data());
  info.cbTag = static_cast<ULONG>(tag.size());
  ULONG written = 0;
  return BCRYPT_SUCCESS(BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), const_cast<PUCHAR>(ciphertext.data()),
                                      static_cast<ULONG>(ciphertext.size()), &info, nullptr, 0, plaintext.data(),
                                      static_cast<ULONG>(plaintext.size()), &written, 0));
}

namespace {

Bytes DpapiTransform(std::span<const uint8_t> data, bool protect) {
  DATA_BLOB input{static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data())};
  DATA_BLOB output{};
  BOOL ok = protect ? CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
                    : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
  if (!ok) throw std::runtime_error(protect ? "设备私钥加密失败" : "设备私钥解密失败");
  Bytes result(output.pbData, output.pbData + output.cbData);
  SecureZero(std::span<uint8_t>(output.pbData, output.cbData));
  LocalFree(output.pbData);
  return result;
}

}  // namespace

Bytes DpapiProtect(std::span<const uint8_t> data) { return DpapiTransform(data, true); }
Bytes DpapiUnprotect(std::span<const uint8_t> data) { return DpapiTransform(data, false); }

}  // namespace bm::crypto
