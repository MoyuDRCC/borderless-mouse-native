#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "core/platform/Common.h"

// 剪贴板与文件传输的线路编码：批次 ID、偏移、代次全部小端，与 .NET 协议 3 逐字节一致
namespace bm::net {

// 与 .NET ClipboardContentKind 相同取值
enum class ClipboardContentKind : uint8_t { Text = 1, Image = 2, Files = 3 };

// 批次 ID 是 16 字节不透明标识：.NET 把 Guid 的线路字节原样写入，这里按同一块搬运
struct TransferId {
  std::array<uint8_t, 16> bytes{};
  bool IsEmpty() const { return bytes == std::array<uint8_t, 16>{}; }
  friend bool operator==(const TransferId&, const TransferId&) = default;
};

struct TransferIdHash {
  size_t operator()(const TransferId& id) const;
};

// 32 位十六进制形态：用于接收目录名与日志
std::string ToHex(const TransferId& id);
TransferId NewTransferId();

// 载荷里的文本一律严格 UTF-8，与 .NET 的 UTF8Encoding(throwOnInvalidBytes: true) 一致
bool IsValidUtf8(std::string_view text);

// 小端读写；边界由调用方在校验长度之后保证
inline uint16_t ReadUInt16LE(std::span<const uint8_t> buffer, size_t offset) {
  return static_cast<uint16_t>(buffer[offset] | (buffer[offset + 1] << 8));
}

inline uint32_t ReadUInt32LE(std::span<const uint8_t> buffer, size_t offset) {
  return static_cast<uint32_t>(buffer[offset]) | (static_cast<uint32_t>(buffer[offset + 1]) << 8) |
         (static_cast<uint32_t>(buffer[offset + 2]) << 16) | (static_cast<uint32_t>(buffer[offset + 3]) << 24);
}

inline int32_t ReadInt32LE(std::span<const uint8_t> buffer, size_t offset) {
  return static_cast<int32_t>(ReadUInt32LE(buffer, offset));
}

inline uint64_t ReadUInt64LE(std::span<const uint8_t> buffer, size_t offset) {
  uint64_t value = 0;
  for (size_t index = 0; index < 8; index++) value |= static_cast<uint64_t>(buffer[offset + index]) << (8 * index);
  return value;
}

inline int64_t ReadInt64LE(std::span<const uint8_t> buffer, size_t offset) {
  return static_cast<int64_t>(ReadUInt64LE(buffer, offset));
}

inline void WriteUInt16LE(std::span<uint8_t> buffer, size_t offset, uint16_t value) {
  buffer[offset] = static_cast<uint8_t>(value);
  buffer[offset + 1] = static_cast<uint8_t>(value >> 8);
}

inline void WriteUInt32LE(std::span<uint8_t> buffer, size_t offset, uint32_t value) {
  for (size_t index = 0; index < 4; index++) buffer[offset + index] = static_cast<uint8_t>(value >> (8 * index));
}

inline void WriteInt32LE(std::span<uint8_t> buffer, size_t offset, int32_t value) {
  WriteUInt32LE(buffer, offset, static_cast<uint32_t>(value));
}

inline void WriteUInt64LE(std::span<uint8_t> buffer, size_t offset, uint64_t value) {
  for (size_t index = 0; index < 8; index++) buffer[offset + index] = static_cast<uint8_t>(value >> (8 * index));
}

inline void WriteInt64LE(std::span<uint8_t> buffer, size_t offset, int64_t value) {
  WriteUInt64LE(buffer, offset, static_cast<uint64_t>(value));
}

constexpr size_t kChunkSize = 32 * 1024;
// 全会话共享窗口，换批次不重置信用，旧块也必须处理并归还额度
constexpr int kCreditWindow = 8;
constexpr int64_t kMaxContentLength = 64 * 1024 * 1024;
constexpr int64_t kMaxManifestLength = 8 * 1024 * 1024;

struct ClipboardState {
  bool enabled = false;
  uint64_t epoch = 0;
};

struct TransferStart {
  TransferId id;
  uint64_t receiverEpoch = 0;
  uint64_t senderEpoch = 0;
  ClipboardContentKind kind = ClipboardContentKind::Text;
  int64_t totalLength = 0;
};

struct TransferChunk {
  TransferId id;
  int64_t offset = 0;
  bool last = false;
  Bytes data;
};

struct TransferCancellation {
  TransferId id;
  std::string reason;
};

// 信用消息：对端累计已处理的数据块数
Bytes EncodeCredit(uint64_t consumed);
bool TryParseCredit(std::span<const uint8_t> payload, uint64_t& consumed);

// 状态消息：本端接收代次与开关；代次为零非法
Bytes EncodeState(bool enabled, uint64_t epoch);
bool TryParseState(std::span<const uint8_t> payload, ClipboardState& state);

// 批次公告：接收方代次、发送方代次、内容类型与文档总长
Bytes EncodeStart(const TransferStart& start);
bool TryParseStart(std::span<const uint8_t> payload, TransferStart& start);

// 文档块：批次 ID、精确偏移、尾块标记与数据
Bytes EncodeChunk(const TransferId& id, int64_t offset, bool last, std::span<const uint8_t> data);
bool TryParseChunk(std::span<const uint8_t> payload, TransferChunk& chunk);

// 取消消息：说明按 Unicode 标量截断到 256 个
Bytes EncodeCancel(const TransferId& id, std::string_view reason);
bool TryParseCancel(std::span<const uint8_t> payload, TransferCancellation& cancellation);

// 数据块与请求共用的批次 ID 前缀
bool TryGetTransferId(std::span<const uint8_t> payload, TransferId& id);

// 编码前的同一套长度约束，非法即抛 std::invalid_argument
void ValidateChunk(const TransferId& id, int64_t offset, bool last, size_t length);

}  // namespace bm::net
