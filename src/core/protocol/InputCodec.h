#pragma once

#include "core/platform/Common.h"

namespace bm {

// 线上输入类型。所有载荷都以类型字节和非零控制代次开头，整数一律小端
enum class NetInputKind : uint8_t { Position = 0, Button = 1, Wheel = 2, Key = 3 };

// 位置使用零基桌面偏移；键盘保留虚拟键、完整 16 位扫描码与扩展位
struct NetInput {
  NetInputKind kind = NetInputKind::Position;
  int a = 0;          // 位置 X / 按钮号 / 滚轮增量 / 虚拟键
  int b = 0;          // 位置 Y
  bool flag = false;  // 按下 / 水平滚动
  int scanCode = 0;
  bool extended = false;
  uint64_t controlId = 0;
};

// 前缀是类型 1 字节加控制代次 8 字节，四种输入帧长度固定
constexpr size_t kInputPrefixSize = 9;
constexpr size_t kPositionSize = kInputPrefixSize + 8;
constexpr size_t kButtonSize = kInputPrefixSize + 2;
constexpr size_t kWheelSize = kInputPrefixSize + 5;
constexpr size_t kKeySize = kInputPrefixSize + 5;

struct InputCodec {
  static Bytes Position(int x, int y, uint64_t controlId);
  static Bytes Button(int button, bool down, uint64_t controlId);
  static Bytes Wheel(int delta, bool horizontal, uint64_t controlId);
  static Bytes Key(int virtualKey, int scanCode, bool extended, bool down, uint64_t controlId);
  // 仅接受精确长度、有效类型、非零控制代次及已定义的标志位
  static bool TryDecode(std::span<const uint8_t> payload, NetInput& input);
};

}  // namespace bm
