#pragma once

#include "core/input/DesktopGeometry.h"

namespace bm {

enum class InputEventKind { RelativeActivity, Cursor, InjectedCursor, Button, Wheel, Key };

// 钩子串行域内同步处理的输入；Cursor 是唯一位置来源
struct InputEvent {
  InputEventKind kind = InputEventKind::RelativeActivity;
  int deltaX = 0;
  int deltaY = 0;
  int data = 0;
  bool down = false;
  int virtualKey = 0;
  bool horizontal = false;
  int scanCode = 0;
  bool extended = false;
  bool physical = true;
  uint32_t time = 0;
};

enum class MouseButton { Left, Right, Middle, X1, X2 };

// 显式选择源端指针语义，不按 RawInput 到达顺序猜测设备类型
enum class PointerMode { SystemMouse, AbsoluteDevice };

// 非阻塞的远端输入出口；位置为远端虚拟桌面零基坐标
class IInputSink {
 public:
  virtual ~IInputSink() = default;
  virtual const DesktopGeometry& RemoteGeometry() const = 0;
  virtual bool IsClosed() const = 0;
  // 仍持有可用控制租约；被远端撤回后立即为 false
  virtual bool IsActive() const = 0;
  virtual uint64_t ControlId() const = 0;
  virtual void BeginControl() = 0;
  // 远端 DPI / 本机 DPI，1 表示 1:1
  virtual double PointerScale() const = 0;
  virtual bool MoveTo(int x, int y) = 0;
  virtual bool Button(MouseButton button, bool down) = 0;
  virtual bool Wheel(int delta, bool horizontal) = 0;
  virtual bool Key(int virtualKey, int scanCode, bool extended, bool down) = 0;
  virtual void ReleaseControl() = 0;
};

}  // namespace bm
