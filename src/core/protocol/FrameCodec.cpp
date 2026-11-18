#include "core/protocol/FrameCodec.h"

#include <cstring>

namespace bm {

namespace {
void Put16(uint8_t* p, uint16_t value) { p[0] = static_cast<uint8_t>(value); p[1] = static_cast<uint8_t>(value >> 8); }
void Put32(uint8_t* p, uint32_t value) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
void Put64(uint8_t* p, uint64_t value) {
  for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Get32(const uint8_t* p) {
  uint32_t value = 0;
  for (int i = 0; i < 4; i++) value |= static_cast<uint32_t>(p[i]) << (8 * i);
  return value;
}
uint64_t Get64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) value |= static_cast<uint64_t>(p[i]) << (8 * i);
  return value;
}
}  // namespace

void WriteFrameHeader(std::span<uint8_t> buffer, MessageType type, uint32_t payloadLength, uint64_t sequence) {
  Put32(buffer.data(), kFrameMagic);
  Put16(buffer.data() + 4, kProtocolVersion);
  Put16(buffer.data() + 6, static_cast<uint16_t>(type));
  Put32(buffer.data() + 8, payloadLength);
  Put64(buffer.data() + 12, sequence);
}

bool TryReadFrameHeader(std::span<const uint8_t> buffer, FrameHeader& header) {
  if (buffer.size() != kHeaderSize || Get32(buffer.data()) != kFrameMagic || Get16(buffer.data() + 4) != kProtocolVersion)
    return false;
  uint32_t length = Get32(buffer.data() + 8);
  if (length > kMaxPayload) return false;
  header = FrameHeader{static_cast<MessageType>(Get16(buffer.data() + 6)), length, Get64(buffer.data() + 12)};
  return true;
}

bool IsDataMessage(MessageType type) {
  switch (type) {
    case MessageType::Heartbeat:
    case MessageType::Input:
    case MessageType::ControlStart:
    case MessageType::ControlEnd:
    case MessageType::YieldControl:
    case MessageType::AppInfo:
    case MessageType::InputBlocked:
    case MessageType::Ping:
    case MessageType::Pong:
    case MessageType::ClipboardState:
    case MessageType::ClipboardStart:
    case MessageType::ClipboardChunk:
    case MessageType::TransferCancel:
    case MessageType::FileRequest:
    case MessageType::FileData:
    case MessageType::TransferCredit:
    case MessageType::Topology: return true;
    default: return false;
  }
}

bool IsBulkMessage(MessageType type) {
  return type == MessageType::ClipboardStart || type == MessageType::ClipboardChunk || type == MessageType::FileData;
}

const char* MessageTypeName(MessageType type) {
  switch (type) {
    case MessageType::ClientHello: return "ClientHello";
    case MessageType::ServerHello: return "ServerHello";
    case MessageType::IdentityProof: return "IdentityProof";
    case MessageType::TrustState: return "TrustState";
    case MessageType::PairingDecision: return "PairingDecision";
    case MessageType::KeyConfirmation: return "KeyConfirmation";
    case MessageType::Heartbeat: return "Heartbeat";
    case MessageType::Input: return "Input";
    case MessageType::ControlStart: return "ControlStart";
    case MessageType::ControlEnd: return "ControlEnd";
    case MessageType::YieldControl: return "YieldControl";
    case MessageType::AppInfo: return "AppInfo";
    case MessageType::InputBlocked: return "InputBlocked";
    case MessageType::Ping: return "Ping";
    case MessageType::Pong: return "Pong";
    case MessageType::ClipboardState: return "ClipboardState";
    case MessageType::ClipboardStart: return "ClipboardStart";
    case MessageType::ClipboardChunk: return "ClipboardChunk";
    case MessageType::TransferCancel: return "TransferCancel";
    case MessageType::FileRequest: return "FileRequest";
    case MessageType::FileData: return "FileData";
    case MessageType::TransferCredit: return "TransferCredit";
    case MessageType::Topology: return "Topology";
  }
  return "未知";
}

}  // namespace bm
