#include "core/protocol/HandshakeProtocol.h"

#include "core/crypto/Crypto.h"
#include "core/crypto/DeviceIdentity.h"

#include <windows.h>

#include <cwctype>
#include <cstring>
#include <stdexcept>

namespace bm {

namespace {

constexpr size_t kRandomSize = 32;
constexpr size_t kMonitorSize = 16;  // left/top/width/height 各 int32
// 2 字节名字长度 + 两个 91 字节公钥 + 32 字节随机数 + 桌面宽高 + 端口 + 显示器数量
constexpr size_t kFixedHelloSize = 2 + kP256PublicKeySize * 2 + kRandomSize + 12;

constexpr char kHandshakeDomain[] = "BorderlessMouse signed handshake";
constexpr char kClientIdentity[] = "BorderlessMouse client identity";
constexpr char kServerIdentity[] = "BorderlessMouse server identity";

void Put16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

void Put32(uint8_t* p, uint32_t value) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

int32_t Get32(const uint8_t* p) {
  uint32_t value = 0;
  for (int i = 0; i < 4; i++) value |= static_cast<uint32_t>(p[i]) << (8 * i);
  return static_cast<int32_t>(value);
}

// .NET 用严格 UTF-8 解码设备名：非法字节必须让握手失败，而不是替换成问号
std::wstring StrictUtf8ToWide(std::span<const uint8_t> bytes) {
  if (bytes.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()),
                                        nullptr, 0);
  if (length <= 0) throw std::runtime_error("设备名不是合法的 UTF-8");
  std::wstring text(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()),
                      static_cast<int>(bytes.size()), text.data(), length);
  return text;
}

// 设备名必须 1 至 255 字节、非全空白且不含控制字符
void ValidateName(std::span<const uint8_t> encoded) {
  if (encoded.empty() || encoded.size() > 255) throw std::runtime_error("设备名必须是1至255字节且不含控制字符的 UTF-8 文本");
  const std::wstring text = StrictUtf8ToWide(encoded);
  bool visible = false;
  for (wchar_t ch : text) {
    if (ch <= 0x1F || (ch >= 0x7F && ch <= 0x9F)) throw std::runtime_error("设备名必须是1至255字节且不含控制字符的 UTF-8 文本");
    if (!std::iswspace(static_cast<wint_t>(ch))) visible = true;
  }
  if (!visible) throw std::runtime_error("设备名必须是1至255字节且不含控制字符的 UTF-8 文本");
}

}  // namespace

Bytes HandshakeProtocol::EncodeHello(std::string_view name, const DesktopGeometry& desktop, int port,
                                     const DeviceIdentity& identity, const crypto::EcdhKey& ephemeral) {
  const std::span<const uint8_t> nameBytes(reinterpret_cast<const uint8_t*>(name.data()), name.size());
  ValidateName(nameBytes);
  if (port < 1 || port > 65535) throw std::out_of_range("监听端口非法");
  const Bytes& identityKey = identity.PublicKeyDer();
  Bytes ephemeralKey = ephemeral.ExportSubjectPublicKeyInfo();
  if (!IsP256PublicKey(ephemeralKey)) throw std::runtime_error("本机临时公钥不是规范的 P-256 公钥");

  const std::vector<RectI>& monitors = desktop.Monitors();
  Bytes hello(kFixedHelloSize + nameBytes.size() + kMonitorSize * monitors.size(), 0);
  Put16(&hello[0], static_cast<uint16_t>(nameBytes.size()));
  size_t offset = 2;
  std::memcpy(&hello[offset], nameBytes.data(), nameBytes.size());
  offset += nameBytes.size();
  std::memcpy(&hello[offset], identityKey.data(), kP256PublicKeySize);
  offset += kP256PublicKeySize;
  std::memcpy(&hello[offset], ephemeralKey.data(), kP256PublicKeySize);
  offset += kP256PublicKeySize;
  const Bytes random = crypto::RandomBytes(kRandomSize);
  std::memcpy(&hello[offset], random.data(), kRandomSize);
  offset += kRandomSize;
  Put32(&hello[offset], static_cast<uint32_t>(desktop.Size().width));
  offset += 4;
  Put32(&hello[offset], static_cast<uint32_t>(desktop.Size().height));
  offset += 4;
  Put16(&hello[offset], static_cast<uint16_t>(port));
  offset += 2;
  Put16(&hello[offset], static_cast<uint16_t>(monitors.size()));
  offset += 2;
  for (const RectI& monitor : monitors) {
    Put32(&hello[offset], static_cast<uint32_t>(monitor.left));
    Put32(&hello[offset + 4], static_cast<uint32_t>(monitor.top));
    Put32(&hello[offset + 8], static_cast<uint32_t>(monitor.Width()));
    Put32(&hello[offset + 12], static_cast<uint32_t>(monitor.Height()));
    offset += kMonitorSize;
  }
  return hello;
}

HelloData HandshakeProtocol::ParseHello(std::span<const uint8_t> payload) {
  if (payload.size() < kFixedHelloSize) throw std::runtime_error("Hello 载荷被截断");
  const size_t nameLength = Get16(payload.data());
  if (payload.size() < kFixedHelloSize + nameLength) throw std::runtime_error("Hello 长度不一致");
  size_t offset = 2;
  const std::span<const uint8_t> nameBytes = payload.subspan(offset, nameLength);
  ValidateName(nameBytes);
  HelloData hello;
  hello.name = std::string(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
  offset += nameLength;
  hello.identityKey.assign(payload.begin() + static_cast<ptrdiff_t>(offset),
                           payload.begin() + static_cast<ptrdiff_t>(offset + kP256PublicKeySize));
  offset += kP256PublicKeySize;
  hello.ephemeralKey.assign(payload.begin() + static_cast<ptrdiff_t>(offset),
                            payload.begin() + static_cast<ptrdiff_t>(offset + kP256PublicKeySize));
  offset += kP256PublicKeySize + kRandomSize;  // 随机数只参与避免重放，不参与校验
  const Size2i size{Get32(payload.data() + offset), Get32(payload.data() + offset + 4)};
  offset += 8;
  const int port = Get16(payload.data() + offset);
  offset += 2;
  const size_t monitorCount = Get16(payload.data() + offset);
  offset += 2;
  if (monitorCount < 1 || monitorCount > static_cast<size_t>(DesktopGeometry::kMaxMonitors) ||
      payload.size() != offset + monitorCount * kMonitorSize)
    throw std::runtime_error("Hello 显示器数量或长度不一致");
  std::vector<RectI> monitors(monitorCount);
  for (RectI& monitor : monitors) {
    const int left = Get32(payload.data() + offset);
    const int top = Get32(payload.data() + offset + 4);
    const int width = Get32(payload.data() + offset + 8);
    const int height = Get32(payload.data() + offset + 12);
    monitor = RectI{left, top, left + width, top + height};
    offset += kMonitorSize;
  }
  if (port == 0) throw std::runtime_error("Hello 监听端口非法");
  if (!IsP256PublicKey(hello.identityKey)) throw std::runtime_error("只接受规范的完整 P-256 公钥");
  if (!IsP256PublicKey(hello.ephemeralKey)) throw std::runtime_error("只接受规范的完整 P-256 公钥");
  // 桌面几何自带完整校验：超出范围、空显示器或外接尺寸不一致都会拒绝
  hello.desktop = DesktopGeometry(size, std::move(monitors));
  hello.port = port;
  return hello;
}

Bytes HandshakeProtocol::Transcript(std::span<const uint8_t> clientFrame, std::span<const uint8_t> serverFrame) {
  return crypto::Sha256Concat({std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(kHandshakeDomain),
                                                        sizeof(kHandshakeDomain) - 1),
                               clientFrame, serverFrame});
}

Bytes HandshakeProtocol::ProofHash(std::span<const uint8_t> transcript, bool initiator) {
  const char* role = initiator ? kClientIdentity : kServerIdentity;
  return crypto::Sha256Concat({std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(role), std::strlen(role)),
                               transcript});
}

void HandshakeProtocol::VerifyProof(std::span<const uint8_t> publicKey, std::span<const uint8_t> proof,
                                    std::span<const uint8_t> transcript, bool initiator) {
  if (proof.size() != 64) throw std::runtime_error("设备持久身份签名验证失败");
  if (!crypto::EcdsaKey::VerifyHash(publicKey, ProofHash(transcript, initiator), proof))
    throw std::runtime_error("设备持久身份签名验证失败");
}

}  // namespace bm
