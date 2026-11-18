#include "core/protocol/TransferCodec.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "core/crypto/Crypto.h"

namespace bm::net {
namespace {

// 批次取值校验：ID 非空、两侧代次非零、类型合法、文档长度在对应上限内
bool ValidStart(const TransferStart& start) {
  if (start.id.IsEmpty() || start.receiverEpoch == 0 || start.senderEpoch == 0) return false;
  if (start.kind != ClipboardContentKind::Text && start.kind != ClipboardContentKind::Image &&
      start.kind != ClipboardContentKind::Files)
    return false;
  // 清单与普通内容使用不同的资源上限
  const int64_t limit = start.kind == ClipboardContentKind::Files ? kMaxManifestLength : kMaxContentLength;
  return start.totalLength >= 0 && start.totalLength <= limit;
}

// 取消说明最多编码 256 个 Unicode 标量：只在字符边界上截断
std::string_view TruncateRunes(std::string_view text, size_t limit) {
  size_t offset = 0;
  size_t runes = 0;
  while (offset < text.size() && runes < limit) {
    const uint8_t lead = static_cast<uint8_t>(text[offset]);
    size_t length = 1;
    if ((lead & 0xE0) == 0xC0) length = 2;
    else if ((lead & 0xF0) == 0xE0) length = 3;
    else if ((lead & 0xF8) == 0xF0) length = 4;
    offset = std::min(text.size(), offset + length);
    runes++;
  }
  return text.substr(0, offset);
}

}  // namespace

size_t TransferIdHash::operator()(const TransferId& id) const {
  uint64_t hash = 1469598103934665603ull;
  for (const uint8_t byte : id.bytes) hash = (hash ^ byte) * 1099511628211ull;
  return static_cast<size_t>(hash);
}

std::string ToHex(const TransferId& id) { return bm::ToHex(id.bytes); }

TransferId NewTransferId() {
  TransferId id;
  do {
    const Bytes random = crypto::RandomBytes(id.bytes.size());
    std::memcpy(id.bytes.data(), random.data(), id.bytes.size());
  } while (id.IsEmpty());  // 全零代表空批次，重新抽取
  return id;
}

bool IsValidUtf8(std::string_view text) {
  if (text.empty()) return true;
  // 严格模式拒绝超长编码、代理项与超过 U+10FFFF 的码点
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}

Bytes EncodeCredit(uint64_t consumed) {
  Bytes payload(8);
  WriteUInt64LE(payload, 0, consumed);
  return payload;
}

bool TryParseCredit(std::span<const uint8_t> payload, uint64_t& consumed) {
  if (payload.size() != 8) return false;
  consumed = ReadUInt64LE(payload, 0);
  return consumed > 0;
}

Bytes EncodeState(bool enabled, uint64_t epoch) {
  if (epoch == 0) throw std::invalid_argument("剪贴板状态代次不能为零");
  Bytes payload(9);
  payload[0] = enabled ? 1 : 0;
  WriteUInt64LE(payload, 1, epoch);
  return payload;
}

bool TryParseState(std::span<const uint8_t> payload, ClipboardState& state) {
  if (payload.size() != 9 || payload[0] > 1) return false;
  const uint64_t epoch = ReadUInt64LE(payload, 1);
  if (epoch == 0) return false;
  state.enabled = payload[0] == 1;
  state.epoch = epoch;
  return true;
}

Bytes EncodeStart(const TransferStart& start) {
  if (!ValidStart(start)) throw std::invalid_argument("剪贴板批次或文档长度非法");
  Bytes payload(41);
  std::memcpy(payload.data(), start.id.bytes.data(), start.id.bytes.size());
  WriteUInt64LE(payload, 16, start.receiverEpoch);
  WriteUInt64LE(payload, 24, start.senderEpoch);
  payload[32] = static_cast<uint8_t>(start.kind);
  WriteInt64LE(payload, 33, start.totalLength);
  return payload;
}

bool TryParseStart(std::span<const uint8_t> payload, TransferStart& start) {
  if (payload.size() != 41) return false;
  TransferStart parsed;
  std::memcpy(parsed.id.bytes.data(), payload.data(), parsed.id.bytes.size());
  parsed.receiverEpoch = ReadUInt64LE(payload, 16);
  parsed.senderEpoch = ReadUInt64LE(payload, 24);
  parsed.kind = static_cast<ClipboardContentKind>(payload[32]);
  parsed.totalLength = ReadInt64LE(payload, 33);
  if (!ValidStart(parsed)) return false;
  start = parsed;
  return true;
}

Bytes EncodeChunk(const TransferId& id, int64_t offset, bool last, std::span<const uint8_t> data) {
  ValidateChunk(id, offset, last, data.size());
  Bytes payload(25 + data.size());
  std::memcpy(payload.data(), id.bytes.data(), id.bytes.size());
  WriteInt64LE(payload, 16, offset);
  payload[24] = last ? 1 : 0;
  if (!data.empty()) std::memcpy(payload.data() + 25, data.data(), data.size());
  return payload;
}

bool TryParseChunk(std::span<const uint8_t> payload, TransferChunk& chunk) {
  // 数据块长度上限就是一批的大小；空块的 last 标记必须为真
  if (payload.size() < 25 || payload.size() > kChunkSize + 25 || payload[24] > 1) return false;
  TransferChunk parsed;
  std::memcpy(parsed.id.bytes.data(), payload.data(), parsed.id.bytes.size());
  parsed.offset = ReadInt64LE(payload, 16);
  parsed.last = payload[24] == 1;
  if (parsed.id.IsEmpty() || parsed.offset < 0 || (payload.size() == 25 && !parsed.last)) return false;
  parsed.data.assign(payload.begin() + 25, payload.end());
  chunk = std::move(parsed);
  return true;
}

Bytes EncodeCancel(const TransferId& id, std::string_view reason) {
  if (id.IsEmpty()) throw std::invalid_argument("取消消息缺少批次 ID");
  const std::string_view text = TruncateRunes(reason, 256);
  Bytes payload(20 + text.size());
  std::memcpy(payload.data(), id.bytes.data(), id.bytes.size());
  WriteInt32LE(payload, 16, static_cast<int32_t>(text.size()));
  if (!text.empty()) std::memcpy(payload.data() + 20, text.data(), text.size());
  return payload;
}

bool TryParseCancel(std::span<const uint8_t> payload, TransferCancellation& cancellation) {
  if (payload.size() < 20 || payload.size() > 1044) return false;
  TransferCancellation parsed;
  std::memcpy(parsed.id.bytes.data(), payload.data(), parsed.id.bytes.size());
  const int32_t length = ReadInt32LE(payload, 16);
  if (parsed.id.IsEmpty() || length < 0 || payload.size() != 20 + static_cast<size_t>(length)) return false;
  parsed.reason.assign(reinterpret_cast<const char*>(payload.data() + 20), static_cast<size_t>(length));
  if (!IsValidUtf8(parsed.reason)) return false;
  cancellation = std::move(parsed);
  return true;
}

bool TryGetTransferId(std::span<const uint8_t> payload, TransferId& id) {
  if (payload.size() < 16) return false;
  TransferId parsed;
  std::memcpy(parsed.bytes.data(), payload.data(), parsed.bytes.size());
  if (parsed.IsEmpty()) return false;
  id = parsed;
  return true;
}

void ValidateChunk(const TransferId& id, int64_t offset, bool last, size_t length) {
  if (id.IsEmpty() || offset < 0 || length > kChunkSize || (length == 0 && !last))
    throw std::invalid_argument("数据块的批次、偏移或长度非法");
}

}  // namespace bm::net
