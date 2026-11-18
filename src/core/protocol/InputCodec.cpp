#include "core/protocol/InputCodec.h"

#include "core/input/InputEvent.h"

#include <stdexcept>

namespace bm {

namespace {

void Put16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

void Put32(uint8_t* p, uint32_t value) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

void Put64(uint8_t* p, uint64_t value) {
  for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

int32_t Get32(const uint8_t* p) {
  uint32_t value = 0;
  for (int i = 0; i < 4; i++) value |= static_cast<uint32_t>(p[i]) << (8 * i);
  return static_cast<int32_t>(value);
}

uint64_t Get64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) value |= static_cast<uint64_t>(p[i]) << (8 * i);
  return value;
}

// 四种输入帧共用的前缀：类型字节 + 控制代次，代次为零视为未取得租约
Bytes Create(NetInputKind kind, size_t length, uint64_t controlId) {
  if (controlId == 0) throw std::out_of_range("控制代次不能为零");
  Bytes buffer(length, 0);
  buffer[0] = static_cast<uint8_t>(kind);
  Put64(&buffer[1], controlId);
  return buffer;
}

// 与 .NET 相同：只接受 1..254 的虚拟键值
bool ValidVirtualKey(int virtualKey) { return virtualKey > 0 && virtualKey < 0xFF; }

}  // namespace

Bytes InputCodec::Position(int x, int y, uint64_t controlId) {
  Bytes buffer = Create(NetInputKind::Position, kPositionSize, controlId);
  // 保留全部 int32 位，大桌面的零基位置不能被截断成 short
  Put32(&buffer[kInputPrefixSize], static_cast<uint32_t>(x));
  Put32(&buffer[kInputPrefixSize + 4], static_cast<uint32_t>(y));
  return buffer;
}

Bytes InputCodec::Button(int button, bool down, uint64_t controlId) {
  if (button < static_cast<int>(MouseButton::Left) || button > static_cast<int>(MouseButton::X2))
    throw std::out_of_range("鼠标按钮号非法");
  Bytes buffer = Create(NetInputKind::Button, kButtonSize, controlId);
  buffer[kInputPrefixSize] = static_cast<uint8_t>(button);
  buffer[kInputPrefixSize + 1] = down ? uint8_t{1} : uint8_t{0};
  return buffer;
}

Bytes InputCodec::Wheel(int delta, bool horizontal, uint64_t controlId) {
  Bytes buffer = Create(NetInputKind::Wheel, kWheelSize, controlId);
  Put32(&buffer[kInputPrefixSize], static_cast<uint32_t>(delta));
  buffer[kInputPrefixSize + 4] = horizontal ? uint8_t{1} : uint8_t{0};
  return buffer;
}

Bytes InputCodec::Key(int virtualKey, int scanCode, bool extended, bool down, uint64_t controlId) {
  if (!ValidVirtualKey(virtualKey)) throw std::out_of_range("虚拟键值非法");
  if (scanCode < 0 || scanCode > 0xFFFF) throw std::out_of_range("扫描码非法");
  Bytes buffer = Create(NetInputKind::Key, kKeySize, controlId);
  Put16(&buffer[kInputPrefixSize], static_cast<uint16_t>(virtualKey));
  Put16(&buffer[kInputPrefixSize + 2], static_cast<uint16_t>(scanCode));
  constexpr uint8_t kDownFlag = 0x01;
  constexpr uint8_t kExtendedFlag = 0x02;
  buffer[kInputPrefixSize + 4] = static_cast<uint8_t>((down ? kDownFlag : 0) | (extended ? kExtendedFlag : 0));
  return buffer;
}

bool InputCodec::TryDecode(std::span<const uint8_t> payload, NetInput& input) {
  input = NetInput{};
  if (payload.size() < kInputPrefixSize) return false;
  uint64_t controlId = Get64(payload.data() + 1);
  if (controlId == 0) return false;
  constexpr uint8_t kDownFlag = 0x01;
  constexpr uint8_t kExtendedFlag = 0x02;
  switch (static_cast<NetInputKind>(payload[0])) {
    case NetInputKind::Position:
      if (payload.size() != kPositionSize) return false;
      input = NetInput{NetInputKind::Position, Get32(payload.data() + kInputPrefixSize),
                       Get32(payload.data() + kInputPrefixSize + 4), false, 0, false, controlId};
      return true;
    case NetInputKind::Button: {
      if (payload.size() != kButtonSize) return false;
      uint8_t button = payload[kInputPrefixSize];
      uint8_t down = payload[kInputPrefixSize + 1];
      if (button > static_cast<uint8_t>(MouseButton::X2) || down > 1) return false;
      input = NetInput{NetInputKind::Button, button, 0, down == 1, 0, false, controlId};
      return true;
    }
    case NetInputKind::Wheel: {
      if (payload.size() != kWheelSize) return false;
      uint8_t horizontal = payload[kInputPrefixSize + 4];
      if (horizontal > 1) return false;
      input = NetInput{NetInputKind::Wheel, Get32(payload.data() + kInputPrefixSize), 0, horizontal == 1, 0, false,
                       controlId};
      return true;
    }
    case NetInputKind::Key: {
      if (payload.size() != kKeySize) return false;
      int virtualKey = Get16(payload.data() + kInputPrefixSize);
      int scanCode = Get16(payload.data() + kInputPrefixSize + 2);
      uint8_t flags = payload[kInputPrefixSize + 4];
      if (!ValidVirtualKey(virtualKey) || (flags & ~(kDownFlag | kExtendedFlag)) != 0) return false;
      input = NetInput{NetInputKind::Key, virtualKey, 0, (flags & kDownFlag) != 0, scanCode,
                       (flags & kExtendedFlag) != 0, controlId};
      return true;
    }
    default:
      return false;
  }
}

}  // namespace bm
