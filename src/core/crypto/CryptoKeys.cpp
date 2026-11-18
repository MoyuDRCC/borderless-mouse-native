#include "core/crypto/Crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

// P-256 密钥：CNG 密钥块与该曲线固定的 DER 编码（SubjectPublicKeyInfo / PKCS8）
namespace bm::crypto {

namespace {

constexpr uint8_t kTagInteger = 0x02;
constexpr uint8_t kTagBitString = 0x03;
constexpr uint8_t kTagOctetString = 0x04;
constexpr uint8_t kTagSequence = 0x30;
constexpr uint8_t kTagParameters = 0xA0;      // ECPrivateKey 的 [0] 曲线参数
constexpr uint8_t kTagPublicKey = 0xA1;       // ECPrivateKey 的 [1] 公钥
constexpr size_t kCurveBytes = 32;

constexpr uint8_t kOidEcPublicKey[] = {0x06, 0x07, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01};
constexpr uint8_t kOidPrime256v1[] = {0x06, 0x08, 0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07};

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// ---------- 极简 DER 编解码 ----------

Bytes DerEncode(std::span<const uint8_t> body, uint8_t tag) {
  Bytes out{tag};
  if (body.size() < 0x80) {
    out.push_back(static_cast<uint8_t>(body.size()));
  } else if (body.size() < 0x100) {
    out.push_back(0x81);
    out.push_back(static_cast<uint8_t>(body.size()));
  } else {
    out.push_back(0x82);
    out.push_back(static_cast<uint8_t>(body.size() >> 8));
    out.push_back(static_cast<uint8_t>(body.size() & 0xFF));
  }
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Bytes DerEncode(uint8_t tag, std::initializer_list<std::span<const uint8_t>> parts) {
  Bytes body;
  for (std::span<const uint8_t> part : parts) body.insert(body.end(), part.begin(), part.end());
  return DerEncode(body, tag);
}

Bytes DerInteger(uint8_t value) { return Bytes{0x02, 0x01, value}; }

// 一次解析一层 TLV：读到期望标签就交出内容，否则返回 false
class DerReader {
 public:
  explicit DerReader(std::span<const uint8_t> data) : data_(data) {}

  bool Read(uint8_t tag, std::span<const uint8_t>& body) {
    if (pos_ + 2 > data_.size() || data_[pos_] != tag) return false;
    size_t length = data_[pos_ + 1];
    size_t offset = pos_ + 2;
    if (length & 0x80) {
      size_t count = length & 0x7F;
      if (count == 0 || count > 2 || offset + count > data_.size()) return false;
      length = 0;
      for (size_t i = 0; i < count; i++) length = (length << 8) | data_[offset + i];
      offset += count;
    }
    if (offset + length > data_.size()) return false;
    body = data_.subspan(offset, length);
    pos_ = offset + length;
    return true;
  }

  bool AtEnd() const { return pos_ == data_.size(); }
  bool AtTag(uint8_t tag) const { return pos_ < data_.size() && data_[pos_] == tag; }

 private:
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
};

struct EcPoint {
  Bytes x;
  Bytes y;
  Bytes d;  // 公钥导出时为空
};

bool IsPrime256Algorithm(std::span<const uint8_t> algorithm) {
  return algorithm.size() == sizeof(kOidEcPublicKey) + sizeof(kOidPrime256v1) &&
         std::memcmp(algorithm.data(), kOidEcPublicKey, sizeof(kOidEcPublicKey)) == 0 &&
         std::memcmp(algorithm.data() + sizeof(kOidEcPublicKey), kOidPrime256v1, sizeof(kOidPrime256v1)) == 0;
}

Bytes EncodeAlgorithm() { return DerEncode(kTagSequence, {kOidEcPublicKey, kOidPrime256v1}); }

Bytes EncodePublicBits(const EcPoint& point) {
  Bytes bits{0x00, 0x04};  // 未用位数为 0，未压缩点
  bits.insert(bits.end(), point.x.begin(), point.x.end());
  bits.insert(bits.end(), point.y.begin(), point.y.end());
  return DerEncode(bits, kTagBitString);
}

Bytes EncodeSpki(const EcPoint& point) {
  return DerEncode(kTagSequence, {EncodeAlgorithm(), EncodePublicBits(point)});
}

EcPoint DecodeSpki(std::span<const uint8_t> der) {
  DerReader outer(der);
  std::span<const uint8_t> body;
  Require(outer.Read(kTagSequence, body) && outer.AtEnd(), "对端公钥不是规范的 P-256 公钥");
  DerReader fields(body);
  std::span<const uint8_t> algorithm;
  Require(fields.Read(kTagSequence, algorithm) && IsPrime256Algorithm(algorithm), "对端公钥不是规范的 P-256 公钥");
  std::span<const uint8_t> bits;
  Require(fields.Read(kTagBitString, bits) && fields.AtEnd(), "对端公钥不是规范的 P-256 公钥");
  Require(bits.size() == 2 + kCurveBytes * 2 && bits[0] == 0x00 && bits[1] == 0x04, "对端公钥不是规范的 P-256 公钥");
  EcPoint point;
  point.x.assign(bits.begin() + 2, bits.begin() + 2 + kCurveBytes);
  point.y.assign(bits.begin() + 2 + kCurveBytes, bits.end());
  return point;
}

Bytes EncodePkcs8(const EcPoint& point) {
  // ECPrivateKey 的 [0]、[1] 是显式标签，必须各自包一层
  Bytes taggedCurve = DerEncode(kOidPrime256v1, kTagParameters);
  Bytes taggedPublic = DerEncode(EncodePublicBits(point), kTagPublicKey);
  Bytes ecPrivateKey = DerEncode(
      kTagSequence, {DerInteger(1), DerEncode(point.d, kTagOctetString), taggedCurve, taggedPublic});
  return DerEncode(kTagSequence, {DerInteger(0), EncodeAlgorithm(), DerEncode(ecPrivateKey, kTagOctetString)});
}

EcPoint DecodePkcs8(std::span<const uint8_t> der) {
  DerReader outer(der);
  std::span<const uint8_t> body;
  Require(outer.Read(kTagSequence, body) && outer.AtEnd(), "设备私钥不是完整的 PKCS8 密钥");
  DerReader fields(body);
  std::span<const uint8_t> version;
  Require(fields.Read(kTagInteger, version) && version.size() == 1 && version[0] == 0, "设备私钥版本非法");
  std::span<const uint8_t> algorithm;
  Require(fields.Read(kTagSequence, algorithm) && IsPrime256Algorithm(algorithm), "设备私钥曲线不是 P-256");
  std::span<const uint8_t> wrapped;
  Require(fields.Read(kTagOctetString, wrapped) && fields.AtEnd(), "设备私钥载荷被截断");

  DerReader ecReader(wrapped);
  std::span<const uint8_t> ecBody;
  Require(ecReader.Read(kTagSequence, ecBody) && ecReader.AtEnd(), "设备私钥结构非法");
  DerReader ec(ecBody);
  std::span<const uint8_t> ecVersion;
  Require(ec.Read(kTagInteger, ecVersion) && ecVersion.size() == 1 && ecVersion[0] == 1, "设备私钥结构非法");
  std::span<const uint8_t> d;
  Require(ec.Read(kTagOctetString, d) && d.size() == kCurveBytes, "设备私钥长度非法");
  // [0] 曲线参数是可选的：CNG 导出私人密钥时省略它，外层算法标识已经指明 P-256
  if (ec.AtTag(kTagParameters)) {
    std::span<const uint8_t> curve;
    Require(ec.Read(kTagParameters, curve) && curve.size() == sizeof(kOidPrime256v1) &&
                std::memcmp(curve.data(), kOidPrime256v1, sizeof(kOidPrime256v1)) == 0,
            "设备私钥曲线不是 P-256");
  }
  std::span<const uint8_t> publicKey;
  Require(ec.Read(kTagPublicKey, publicKey) && ec.AtEnd(), "设备私钥缺少公钥部分");
  DerReader publicReader(publicKey);
  std::span<const uint8_t> bits;
  Require(publicReader.Read(kTagBitString, bits) && publicReader.AtEnd() && bits.size() == 2 + kCurveBytes * 2 &&
              bits[0] == 0x00 && bits[1] == 0x04,
          "设备私钥公钥非法");

  EcPoint point;
  point.x.assign(bits.begin() + 2, bits.begin() + 2 + kCurveBytes);
  point.y.assign(bits.begin() + 2 + kCurveBytes, bits.end());
  point.d.assign(d.begin(), d.end());
  return point;
}

// ---------- CNG 密钥块 ----------

class Provider {
 public:
  explicit Provider(LPCWSTR algorithm) {
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&handle_, algorithm, nullptr, 0))) throw std::runtime_error("P-256 算法不可用");
  }
  ~Provider() {
    if (handle_) BCryptCloseAlgorithmProvider(handle_, 0);
  }
  Provider(const Provider&) = delete;
  Provider& operator=(const Provider&) = delete;
  BCRYPT_ALG_HANDLE Get() const { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_ = nullptr;
};

Bytes ExportBlob(BCRYPT_KEY_HANDLE key, LPCWSTR type) {
  ULONG size = 0;
  Require(BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, type, nullptr, 0, &size, 0)), "导出 P-256 密钥失败");
  Bytes blob(size);
  Require(BCRYPT_SUCCESS(BCryptExportKey(key, nullptr, type, blob.data(), size, &size, 0)), "导出 P-256 密钥失败");
  blob.resize(size);
  return blob;
}

// BCRYPT_ECCKEY_BLOB 之后依次是 X、Y、d，各 32 字节
EcPoint ReadPoint(std::span<const uint8_t> blob, bool withPrivate) {
  Require(blob.size() == sizeof(BCRYPT_ECCKEY_BLOB) + kCurveBytes * (withPrivate ? 3 : 2), "P-256 密钥块长度非法");
  const uint8_t* body = blob.data() + sizeof(BCRYPT_ECCKEY_BLOB);
  EcPoint point;
  point.x.assign(body, body + kCurveBytes);
  point.y.assign(body + kCurveBytes, body + kCurveBytes * 2);
  if (withPrivate) point.d.assign(body + kCurveBytes * 2, body + kCurveBytes * 3);
  return point;
}

Bytes WriteBlob(ULONG magic, const EcPoint& point) {
  BCRYPT_ECCKEY_BLOB header{magic, kCurveBytes};
  Bytes blob(sizeof(header));
  std::memcpy(blob.data(), &header, sizeof(header));
  blob.insert(blob.end(), point.x.begin(), point.x.end());
  blob.insert(blob.end(), point.y.begin(), point.y.end());
  if (!point.d.empty()) blob.insert(blob.end(), point.d.begin(), point.d.end());
  return blob;
}

BCRYPT_KEY_HANDLE ImportKey(LPCWSTR algorithm, LPCWSTR blobType, std::span<const uint8_t> blob) {
  Provider provider(algorithm);
  BCRYPT_KEY_HANDLE key = nullptr;
  Require(BCRYPT_SUCCESS(BCryptImportKeyPair(provider.Get(), nullptr, blobType, &key, const_cast<PUCHAR>(blob.data()),
                                             static_cast<ULONG>(blob.size()), 0)),
          "导入 P-256 密钥失败");
  return key;
}

BCRYPT_KEY_HANDLE GenerateKey(LPCWSTR algorithm) {
  Provider provider(algorithm);
  BCRYPT_KEY_HANDLE key = nullptr;
  Require(BCRYPT_SUCCESS(BCryptGenerateKeyPair(provider.Get(), &key, 256, 0)), "生成 P-256 密钥失败");
  Require(BCRYPT_SUCCESS(BCryptFinalizeKeyPair(key, 0)), "生成 P-256 密钥失败");
  return key;
}

}  // namespace

EcdsaKey::~EcdsaKey() {
  if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

std::unique_ptr<EcdsaKey> EcdsaKey::Generate() {
  auto result = std::make_unique<EcdsaKey>();
  result->key_ = GenerateKey(BCRYPT_ECDSA_P256_ALGORITHM);
  result->hasPrivate_ = true;
  return result;
}

std::unique_ptr<EcdsaKey> EcdsaKey::ImportPkcs8(std::span<const uint8_t> der) {
  EcPoint point = DecodePkcs8(der);
  auto result = std::make_unique<EcdsaKey>();
  result->key_ = ImportKey(BCRYPT_ECDSA_P256_ALGORITHM, BCRYPT_ECCPRIVATE_BLOB,
                           WriteBlob(BCRYPT_ECDSA_PRIVATE_P256_MAGIC, point));
  result->hasPrivate_ = true;
  return result;
}

Bytes EcdsaKey::ExportPkcs8() const {
  if (!hasPrivate_) throw std::runtime_error("公钥无法导出为私钥");
  return EncodePkcs8(ReadPoint(ExportBlob(static_cast<BCRYPT_KEY_HANDLE>(key_), BCRYPT_ECCPRIVATE_BLOB), true));
}

Bytes EcdsaKey::ExportSubjectPublicKeyInfo() const {
  return EncodeSpki(ReadPoint(ExportBlob(static_cast<BCRYPT_KEY_HANDLE>(key_), BCRYPT_ECCPUBLIC_BLOB), false));
}

Bytes EcdsaKey::SignHash(std::span<const uint8_t> hash) const {
  BCRYPT_KEY_HANDLE key = static_cast<BCRYPT_KEY_HANDLE>(key_);
  ULONG size = 0;
  Require(BCRYPT_SUCCESS(BCryptSignHash(key, nullptr, const_cast<PUCHAR>(hash.data()), static_cast<ULONG>(hash.size()),
                                        nullptr, 0, &size, 0)),
          "身份签名失败");
  Bytes signature(size);
  Require(BCRYPT_SUCCESS(BCryptSignHash(key, nullptr, const_cast<PUCHAR>(hash.data()), static_cast<ULONG>(hash.size()),
                                        signature.data(), size, &size, 0)),
          "身份签名失败");
  signature.resize(size);
  return signature;
}

bool EcdsaKey::VerifyHash(std::span<const uint8_t> publicDer, std::span<const uint8_t> hash,
                          std::span<const uint8_t> signature) {
  BCRYPT_KEY_HANDLE key = ImportKey(BCRYPT_ECDSA_P256_ALGORITHM, BCRYPT_ECCPUBLIC_BLOB,
                                    WriteBlob(BCRYPT_ECDSA_PUBLIC_P256_MAGIC, DecodeSpki(publicDer)));
  NTSTATUS status = BCryptVerifySignature(key, nullptr, const_cast<PUCHAR>(hash.data()), static_cast<ULONG>(hash.size()),
                                          const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), 0);
  BCryptDestroyKey(key);
  return BCRYPT_SUCCESS(status);
}

EcdhKey::~EcdhKey() {
  if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

std::unique_ptr<EcdhKey> EcdhKey::Generate() {
  auto result = std::make_unique<EcdhKey>();
  result->key_ = GenerateKey(BCRYPT_ECDH_P256_ALGORITHM);
  return result;
}

Bytes EcdhKey::ExportSubjectPublicKeyInfo() const {
  return EncodeSpki(ReadPoint(ExportBlob(static_cast<BCRYPT_KEY_HANDLE>(key_), BCRYPT_ECCPUBLIC_BLOB), false));
}

Bytes EcdhKey::DeriveSecret(std::span<const uint8_t> peerDer) const {
  EcPoint point = DecodeSpki(peerDer);
  BCRYPT_KEY_HANDLE peer = ImportKey(BCRYPT_ECDH_P256_ALGORITHM, BCRYPT_ECCPUBLIC_BLOB,
                                     WriteBlob(BCRYPT_ECDH_PUBLIC_P256_MAGIC, point));
  BCRYPT_SECRET_HANDLE secret = nullptr;
  Require(BCRYPT_SUCCESS(BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(key_), peer, &secret, 0)), "P-256 协商失败");
  Bytes result(kCurveBytes);
  ULONG size = 0;
  NTSTATUS status = BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, result.data(),
                                    static_cast<ULONG>(result.size()), &size, 0);
  BCryptDestroySecret(secret);
  BCryptDestroyKey(peer);
  Require(BCRYPT_SUCCESS(status), "P-256 协商失败");
  // CNG 的原始秘密是小端 X 坐标，.NET 的 DeriveRawSecretAgreement 输出大端
  std::reverse(result.begin(), result.end());
  return result;
}

}  // namespace bm::crypto
