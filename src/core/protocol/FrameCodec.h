#pragma once

#include "core/platform/Common.h"

namespace bm {

// 与 .NET 版协议 3 保持一致；新增类型必须同时在 IsDataMessage 里登记
enum class MessageType : uint16_t {
  ClientHello = 0x01,
  ServerHello = 0x02,
  IdentityProof = 0x03,
  TrustState = 0x04,
  PairingDecision = 0x05,
  KeyConfirmation = 0x06,
  Heartbeat = 0x07,
  Input = 0x10,
  ControlStart = 0x11,
  ControlEnd = 0x12,
  YieldControl = 0x13,
  AppInfo = 0x14,
  InputBlocked = 0x15,
  Ping = 0x16,
  Pong = 0x17,
  ClipboardState = 0x20,
  ClipboardStart = 0x21,
  ClipboardChunk = 0x22,
  TransferCancel = 0x23,
  FileRequest = 0x24,
  FileData = 0x25,
  TransferCredit = 0x26,
  Topology = 0x40,
};

constexpr uint32_t kFrameMagic = 0x53574D42;
constexpr uint16_t kProtocolVersion = 3;
constexpr size_t kHeaderSize = 20;
constexpr size_t kMaxPayload = 64 * 1024;
constexpr size_t kNonceSize = 12;
constexpr size_t kTagSize = 16;
constexpr size_t kMaxPlainPayload = kMaxPayload - kTagSize;

struct FrameHeader {
  MessageType type;
  uint32_t payloadLength;
  uint64_t sequence;
};

// 固定头是 AES-GCM 的认证数据
void WriteFrameHeader(std::span<uint8_t> buffer, MessageType type, uint32_t payloadLength, uint64_t sequence);
bool TryReadFrameHeader(std::span<const uint8_t> buffer, FrameHeader& header);

bool IsDataMessage(MessageType type);
bool IsBulkMessage(MessageType type);
const char* MessageTypeName(MessageType type);

}  // namespace bm
