#include "core/input/InputService.h"

#include "core/input/CursorHider.h"
#include "core/platform/EventLog.h"
#include "core/platform/Win32Input.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace bm {

namespace {

void AppendError(std::string& messages, const char* text) {
  if (!messages.empty()) messages += "；";
  messages += text;
}

}  // namespace

bool InputService::Route(const InputEvent& input) {
  // Magic 移动在钩子回调内立即更新基线，先于紧随其后的物理事件；不触发本机抢占
  if (input.kind == InputEventKind::InjectedCursor) {
    lastCursor_ = Point2i{input.deltaX, input.deltaY};
    return false;
  }
  bool suppress = false;
  bool wasRemote = target_.load() != nullptr;
  long sourceRevision = sourcePositionRevision_;
  try {
    TrackPhysical(input);
    PrepareButtonCycle(input);
    RestoreSourceCursor();
    // 远端一旦撤回租约就立即回到本机：不等异步回切命令，避免这一瞬间的按键被吞掉却无处提交
    if (auto current = target_.load(); current && !current->IsActive()) ExitRemote();
    switch (input.kind) {
      case InputEventKind::Key: suppress = RouteKey(input); break;
      case InputEventKind::Button: suppress = RouteButton(input); break;
      case InputEventKind::Wheel: suppress = RouteWheel(input); break;
      case InputEventKind::Cursor:
        suppress = restorePoint_.has_value() || (wasRemote && target_.load() == nullptr) || RouteCursor(input);
        break;
      case InputEventKind::RelativeActivity: suppress = RouteRelativeActivity(input); break;
      default: suppress = false; break;
    }
    if (input.physical && !disposing_ &&
        (input.kind == InputEventKind::Cursor || input.kind == InputEventKind::Key ||
         input.kind == InputEventKind::Button || input.kind == InputEventKind::Wheel)) {
      if (localInputDetected) localInputDetected();
    }
  } catch (const std::exception& error) {
    // 只抑制已经提交远端的事件。未提交的当前事件仍交给本机，故障同时撤销共享并报告
    if (input.kind == InputEventKind::Key) {
      KeyStroke key{input.virtualKey, input.scanCode, input.extended};
      suppress |= remoteKeys_.contains(key) || suppressedKeys_.contains(key);
    } else if (input.kind == InputEventKind::Button) {
      MouseButton button = static_cast<MouseButton>(input.data);
      suppress |= remoteButtons_.contains(button) || releasedRemoteButtons_.contains(button) ||
                  suppressedButtons_.contains(button) || IsolatePendingButton(input);
    } else if (input.kind == InputEventKind::Cursor) {
      suppress |= wasRemote || restorePoint_.has_value();
    }
    Fail(error.what());
    if (input.kind == InputEventKind::Key) {
      KeyStroke key{input.virtualKey, input.scanCode, input.extended};
      if (!suppress) {
        if (input.down) localKeys_.insert(key); else localKeys_.erase(key);
      } else if (!input.down) {
        suppressedKeys_.erase(key);
      }
    } else if (input.kind == InputEventKind::Button) {
      MouseButton button = static_cast<MouseButton>(input.data);
      if (!suppress) {
        if (input.down) localButtons_.insert(button); else localButtons_.erase(button);
      } else if (!input.down) {
        suppressedButtons_.erase(button);
      }
    }
  }
  // 同步回切可能发生在恢复、撤租约或异常分支；当前旧 pt 不能覆盖新落点
  if (input.kind == InputEventKind::Cursor && sourceRevision != sourcePositionRevision_) suppress = true;
  FinishDisposalWhenReleased();
  return suppress;
}

bool InputService::RouteKey(const InputEvent& input) {
  KeyStroke key{input.virtualKey, input.scanCode, input.extended};
  if (input.down && rawReleasedKeys_.erase(key)) {
    releasedRemoteKeys_.erase(key);
    releasedLocalKeys_.erase(key);
  }
  if (!input.down) rawReleasedKeys_.erase(key);
  // 校准移出的旧归属仍隔离重复 Down；直到 Up 才结束，不能把仍按住的键交给新目标
  if (releasedRemoteKeys_.contains(key)) {
    if (!input.down) releasedRemoteKeys_.erase(key);
    return true;
  }
  if (releasedLocalKeys_.contains(key)) {
    if (!input.down) releasedLocalKeys_.erase(key);
    return false;
  }
  if (suppressedKeys_.contains(key)) {
    if (!input.down) suppressedKeys_.erase(key);
    return true;
  }
  // 紧急 Esc 只在远控时归本服务；本机组合完整放行，已在本机按下的 Esc 也保持原归属
  if (target_.load() && !localKeys_.contains(key) && input.physical && input.down && input.virtualKey == VK_ESCAPE &&
      HasModifier(VK_CONTROL, 0xA2, 0xA3) && HasModifier(VK_MENU, 0xA4, 0xA5)) {
    suppressedKeys_.insert(key);
    ExitRemote();
    return true;
  }
  // Win 键结束远控
  if (input.physical && input.down && (input.virtualKey == VK_LWIN || input.virtualKey == VK_RWIN)) ExitRemote();
  if (localKeys_.contains(key)) {
    if (!input.down) localKeys_.erase(key);
    return false;
  }
  bool ownedRemotely = remoteKeys_.contains(key);
  auto target = target_.load();
  if (target && (input.down || ownedRemotely)) {
    if (target->Key(key.virtualKey, key.scanCode, key.extended, input.down)) {
      // 只有明确提交后才记录 Down；Up 无论本次提交结果如何，都继承原 Down 的归属
      if (input.down) remoteKeys_.insert(key); else remoteKeys_.erase(key);
      return true;
    }
    // 会话关闭或发送队列拒绝：这是键盘无声失效的唯一出口，必须留下证据
    if (input.down) LogWarn("按键未能下发到远端，已返回本机");
    ExitRemote();
    if (ownedRemotely) {
      if (!input.down) suppressedKeys_.erase(key);
      return true;
    }
  }
  if (input.down) localKeys_.insert(key); else localKeys_.erase(key);
  return false;
}

bool InputService::HasModifier(int generic, int left, int right) const {
  if (initialKeys_.contains(generic) || initialKeys_.contains(left) || initialKeys_.contains(right)) return true;
  for (const KeyStroke& key : physicalKeys_)
    if (key.virtualKey == generic || key.virtualKey == left || key.virtualKey == right) return true;
  return false;
}

bool InputService::RouteButton(const InputEvent& input) {
  if (IsolatePendingButton(input)) return true;
  MouseButton button = static_cast<MouseButton>(input.data);
  if (releasedRemoteButtons_.contains(button)) {
    if (!input.down) releasedRemoteButtons_.erase(button);
    return true;
  }
  if (releasedLocalButtons_.contains(button)) {
    if (!input.down) releasedLocalButtons_.erase(button);
    return false;
  }
  if (suppressedButtons_.contains(button)) {
    if (!input.down) suppressedButtons_.erase(button);
    return true;
  }
  if (localButtons_.contains(button)) {
    if (!input.down) localButtons_.erase(button);
    return false;
  }
  bool ownedRemotely = remoteButtons_.contains(button);
  auto target = target_.load();
  if (target && (input.down || ownedRemotely)) {
    if (target->Button(button, input.down)) {
      if (input.down) remoteButtons_.insert(button); else remoteButtons_.erase(button);
      return true;
    }
    ExitRemote();
    if (ownedRemotely) {
      if (!input.down) suppressedButtons_.erase(button);
      return true;
    }
  }
  if (input.down) localButtons_.insert(button); else localButtons_.erase(button);
  return false;
}

void InputService::PrepareButtonCycle(const InputEvent& input) {
  if (input.kind != InputEventKind::Button) return;
  MouseButton button = static_cast<MouseButton>(input.data);
  // 新周期必须先清理旧 Raw Up 墓碑；即使随后恢复失败，也不能让旧墓碑吞掉新周期的 Up
  if (input.down && rawReleasedButtons_.erase(button)) {
    releasedRemoteButtons_.erase(button);
    releasedLocalButtons_.erase(button);
  }
  if (!input.down) rawReleasedButtons_.erase(button);
}

bool InputService::IsolatePendingButton(const InputEvent& input) {
  MouseButton button = static_cast<MouseButton>(input.data);
  if (!input.down || !restorePoint_ || localButtons_.contains(button) || releasedLocalButtons_.contains(button) ||
      releasedRemoteButtons_.contains(button))
    return false;
  // 恢复失败时不把新点击交给停放点，也不在随后回定位时拖动；只吞到对应 Up，不重放
  suppressedButtons_.insert(button);
  return true;
}

bool InputService::RouteWheel(const InputEvent& input) {
  auto target = target_.load();
  if (!target) return false;
  if (target->Wheel(input.data, input.horizontal)) return true;
  ExitRemote();
  return false;
}

bool InputService::RouteCursor(const InputEvent& input) {
  Point2i cursor{input.deltaX, input.deltaY};
  lastCursor_ = cursor;
  auto target = target_.load();
  if (!target) {
    // 本机鼠标动了：把可能罩在停放点上的隐藏窗口收起来，避免再次划过时消失
    if (cursorHider_) cursorHider_->Hide();
    return false;
  }
  if (pointerMode_ == PointerMode::AbsoluteDevice) {
    // 绝对指针只覆盖落点，不把同一坐标解释为反复的相对位移；按钮继承此前提交的位置
    auto [mappedX, mappedY] =
        target->RemoteGeometry().MapFrom(cursor.x - desktop_.left, cursor.y - desktop_.top, geometry_.Size());
    remoteX_ = mappedX;
    remoteY_ = mappedY;
    Point2i visible = target->RemoteGeometry().ToPixel(remoteX_, remoteY_);
    if (!target->MoveTo(visible.x, visible.y)) ExitRemote();
    return true;
  }
  // 与 MWB 一样使用系统处理后的屏幕增量。被吞的物理移动不会更新实际光标，
  // 连续两个相同 pt 仍可能是两次相同增量，因此不能用上一条 LL 的 pt 作差或去重
  POINT actual{};
  if (!GetCursorPos(&actual)) win::ThrowLastError("GetCursorPos");
  int deltaX = cursor.x - actual.x;
  int deltaY = cursor.y - actual.y;
  if (deltaX != 0 || deltaY != 0) MoveRemote(deltaX, deltaY);
  // 即使本次发送失败已回切，也不能让旧源坐标覆盖刚恢复的本机位置
  return true;
}

bool InputService::RouteRelativeActivity(const InputEvent& input) {
  // RawInput 仅证明本机边缘有相对设备向外移动；不生产也不重放远端位移
  // 回切前已排队的旧 Raw 包不能在恢复锚点后立即触发再次进入
  uint64_t now = TickMs();
  uint64_t createdAt = now - (static_cast<uint32_t>(now) - input.time);
  if (target_.load() || createdAt <= localInputSince_) return false;
  POINT actual{};
  if (!GetCursorPos(&actual)) return false;
  Point2i cursor{actual.x, actual.y};
  lastCursor_ = cursor;
  CheckLocalEdge(cursor, input.deltaX, input.deltaY);
  return false;
}

bool InputService::CanCross() const {
  return enabled_ && !disposing_ && !suspended_ && inputDesktopAvailable_ && !restorePoint_ && initialKeys_.empty() &&
         physicalKeys_.empty() && physicalButtons_.empty() && remoteKeys_.empty() && remoteButtons_.empty() &&
         localKeys_.empty() && localButtons_.empty() && suppressedKeys_.empty() && suppressedButtons_.empty();
}

bool InputService::CanSwitch(bool selectedDirectly) const { return CanCross() && switchGate_.CanSwitch(selectedDirectly); }

void InputService::CheckLocalEdge(Point2i cursor, double deltaX, double deltaY) {
  switchGate_.Observe(cursor.x, cursor.y);
  if (!CanSwitch()) return;
  Point2i offset{cursor.x - desktop_.left, cursor.y - desktop_.top};
  int side = geometry_.GetEdgeSide(offset, deltaX, deltaY);
  if (side < 0) return;
  const InputDevice* next = topology_->Neighbor(localDevice_, side);
  if (!next || !next->sink || next->sink->IsClosed()) return;
  anchor_ = ClampToDesktop(cursor);
  EnterRemote(next->name, next->sink, side, offset.x, offset.y, geometry_.Size());
}

bool InputService::MoveRemote(double deltaX, double deltaY) {
  auto target = target_.load();
  if (!target) return false;
  double scale = target->PointerScale();
  if (scale > 0 && scale != 1) {
    // 目标电脑鼠标 DPI 与本机不同：按比例换算，余数留到下一次，长距离移动不会因取整变慢
    deltaX = TakeWhole(deltaX * scale, scaleCarryX_);
    deltaY = TakeWhole(deltaY * scale, scaleCarryY_);
  }
  const DesktopGeometry& geometry = target->RemoteGeometry();
  Size2i remoteSize = geometry.Size();
  int side = -1;
  std::tie(remoteX_, remoteY_) = geometry.Move(remoteX_, remoteY_, deltaX, deltaY, side);
  switchGate_.Observe(remoteX_, remoteY_);
  if (side >= 0 && CanSwitch()) {
    const InputDevice* next = topology_->Neighbor(CurrentDeviceName(), side);
    if (next && next->name == localDevice_) {
      auto [x, y] = geometry_.EntryPoint(side, remoteX_, remoteY_, remoteSize);
      Point2i visible = geometry_.ToPixel(x, y);
      ExitRemote(Point2i{desktop_.left + visible.x, desktop_.top + visible.y});
      return true;
    }
    if (next && next->sink && !next->sink->IsClosed()) return EnterRemote(next->name, next->sink, side, remoteX_, remoteY_, remoteSize);
  }
  Point2i position = geometry.ToPixel(remoteX_, remoteY_);
  if (target->MoveTo(position.x, position.y)) return true;
  ExitRemote();
  return false;
}

double InputService::TakeWhole(double value, double& carry) {
  double total = carry + value;
  double whole = std::trunc(total);
  carry = total - whole;
  return whole;
}

bool InputService::EnterRemote(const std::string& name, const std::shared_ptr<IInputSink>& sink, int side, double x, double y,
                               Size2i from) {
  auto [mappedX, mappedY] = sink->RemoteGeometry().EntryPoint(side, x, y, from);
  return ActivateTarget(name, sink, mappedX, mappedY, false);
}

bool InputService::ActivateTarget(const std::string& name, const std::shared_ptr<IInputSink>& sink, double x, double y, bool selectedDirectly,
                                  PointerMode mode) {
  if (!CanSwitch(selectedDirectly) || sink->IsClosed()) return false;
  std::tie(x, y) = sink->RemoteGeometry().ClampD(x, y);
  Point2i visible = sink->RemoteGeometry().ToPixel(x, y);
  bool wasRemote = target_.load() != nullptr;
  PointerMode previousMode = pointerMode_;
  if (auto previous = target_.load()) previous->ReleaseControl();
  sink->BeginControl();
  bool accepted = false;
  try {
    accepted = sink->MoveTo(visible.x, visible.y);
  } catch (...) {
    // 本次进入未完成，也必须结束已经开始的租约
    sink->ReleaseControl();
    throw;
  }
  if (!accepted) {
    sink->ReleaseControl();
    ExitRemote();
    return false;
  }
  remoteX_ = x;
  remoteY_ = y;
  scaleCarryX_ = 0;
  scaleCarryY_ = 0;
  SetCurrentDevice(name);
  targetSelectedDirectly_ = selectedDirectly;
  pointerMode_ = mode;
  target_.store(sink);
  Size2i remoteSize = sink->RemoteGeometry().Size();
  switchGate_.Enter(x, y, remoteSize.width, remoteSize.height);
  // 返回锚点与屏幕内停放点分离；避免源端长时间贴边而裁掉系统像素增量。
  // 只在首次进入或改变指针模式时回定位，远端之间漫游沿用同一实际光标原点
  if (!wasRemote || previousMode != mode) {
    Point2i source = mode == PointerMode::SystemMouse ? SourceParkingPoint() : anchor_;
    PositionSourceCursor(source);
    if (mode == PointerMode::SystemMouse && cursorHider_) cursorHider_->Show(source.x, source.y);
  }
  if (!wasRemote && remoteChanged) remoteChanged(true);
  return true;
}

void InputService::ExitRemote(std::optional<Point2i> destination) {
  auto target = target_.load();
  if (!target) return;
  // 先撤掉透明光标再挪回光标，避免回切瞬间停在一个不可见的形状上
  if (cursorHider_) cursorHider_->Hide();
  std::vector<KeyStroke> keys(remoteKeys_.begin(), remoteKeys_.end());
  std::vector<MouseButton> buttons(remoteButtons_.begin(), remoteButtons_.end());
  for (const KeyStroke& key : keys) suppressedKeys_.insert(key);
  for (MouseButton button : buttons) suppressedButtons_.insert(button);
  remoteKeys_.clear();
  remoteButtons_.clear();
  target_.store(nullptr);
  SetCurrentDevice(localDevice_);
  targetSelectedDirectly_ = false;
  pointerMode_ = PointerMode::SystemMouse;
  scaleCarryX_ = 0;
  scaleCarryY_ = 0;
  localInputSince_ = TickMs();
  lastCursor_ = ClampToDesktop(destination ? *destination : anchor_);
  RectI monitor = MonitorAt(geometry_, Point2i{lastCursor_.x - desktop_.left, lastCursor_.y - desktop_.top});
  switchGate_.Enter(lastCursor_.x, lastCursor_.y, monitor.Width(), monitor.Height());
  restorePoint_ = lastCursor_;

  // 先承诺本机与物理 Up 的归属，再逐项释放，单项发送失败也不能遗漏其他已按下状态
  std::string errors;
  try {
    RestoreSourceCursor();
  } catch (const std::exception& error) {
    AppendError(errors, error.what());
  }
  for (const KeyStroke& key : keys) {
    try {
      target->Key(key.virtualKey, key.scanCode, key.extended, false);
    } catch (const std::exception& error) {
      AppendError(errors, error.what());
    }
  }
  for (MouseButton button : buttons) {
    try {
      target->Button(button, false);
    } catch (const std::exception& error) {
      AppendError(errors, error.what());
    }
  }
  try {
    target->ReleaseControl();
  } catch (const std::exception& error) {
    AppendError(errors, error.what());
  }
  if (!errors.empty()) throw std::runtime_error("远端控制释放失败：" + errors);
  if (!disposing_ && remoteChanged) remoteChanged(false);
}

}  // namespace bm
