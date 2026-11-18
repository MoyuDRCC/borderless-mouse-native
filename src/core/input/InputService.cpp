#include "core/input/InputService.h"

#include "core/input/CursorHider.h"
#include "core/platform/EventLog.h"
#include "core/input/InputHookHost.h"
#include "core/platform/Win32Input.h"

#include <stdexcept>
#include <utility>

namespace bm {

size_t InputService::KeyStrokeHash::operator()(const KeyStroke& key) const noexcept {
  return (static_cast<size_t>(key.virtualKey) << 12) ^ (static_cast<size_t>(key.scanCode) << 1) ^ (key.extended ? 1u : 0u);
}

int InputService::PhysicalKey(const KeyStroke& key) { return win::PhysicalKeyId(key.virtualKey, key.scanCode, key.extended); }

RectI InputService::MonitorAt(const DesktopGeometry& geometry, Point2i offset) {
  for (const RectI& monitor : geometry.Monitors())
    if (monitor.Contains(offset.x, offset.y)) return monitor;
  throw std::out_of_range("光标不在任何显示器上");
}

InputService::InputService(std::string localDeviceName)
    : localDevice_(std::move(localDeviceName)),
      topology_(std::make_shared<const InputTopology>(localDevice_, std::vector<InputDevice>{{localDevice_, 0, 0, nullptr}})),
      currentDevice_(localDevice_) {
  hook_ = std::make_unique<InputHookHost>(
      [this](const InputEvent& input) { return Route(input); }, [this] { Initialize(); }, [this] { Cleanup(); },
      [this](const std::string& message) { Fail(message); }, [this](bool available) { MaintainState(available); },
      [this](bool keyboard, int virtualKey, int scanCode, bool extended, MouseButton button) {
        ObserveRawRelease(keyboard, virtualKey, scanCode, extended, button);
      });
}

InputService::~InputService() {
  Dispose();
  // 析构没有恢复输入的机会，不再等待物理 Up，直接卸载钩子
  hook_->RequestStop();
  hook_->WaitCompletion();
}

void InputService::Start() {
  if (started_) return;
  started_ = true;
  hook_->Start();
}

bool InputService::IsRemote() const {
  auto target = target_.load();
  return target && target->IsActive();
}

std::string InputService::CurrentDevice() const { return CurrentDeviceName(); }

void InputService::Post(std::function<void()> action) {
  if (disposed_.load()) return;
  hook_->Post(std::move(action));
}

void InputService::Dispose() {
  if (disposed_.exchange(true)) return;
  if (!started_) {
    hook_->Dispose();
    return;
  }
  // 立即关闭输出；若仍持有远端按键，保留钩子直至对应物理 Up，之后自动卸载
  hook_->Post([this] {
    enabled_ = false;
    disposing_ = true;
    ExitRemote();
    MaintainState(win::IsInputDesktopCurrent());
    FinishDisposalWhenReleased();
  });
}

void InputService::HideLocalCursor() {
  Post([this] {
    if (target_.load() || !cursorHider_) return;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    cursorHider_->Show(cursor.x, cursor.y);
  });
}

void InputService::SetHideCursor(bool enabled) {
  hideCursorWhileRemote = enabled;
  Post([this, enabled] {
    if (enabled == (cursorHider_ != nullptr)) return;
    if (enabled) {
      CreateCursorHider();
      return;
    }
    cursorHider_.reset();
  });
}

void InputService::ControlDevice(std::string deviceName, std::shared_ptr<IInputSink> sink, PointerMode mode) {
  Post([this, deviceName = std::move(deviceName), sink = std::move(sink), mode] {
    if (!CanSwitch(true) || sink->IsClosed() || (target_.load() == sink && pointerMode_ == mode)) return;
    if (!target_.load()) {
      POINT cursor{};
      if (!GetCursorPos(&cursor)) win::ThrowLastError("GetCursorPos");
      anchor_ = ClampToDesktop(Point2i{cursor.x, cursor.y});
    }
    auto [x, y] = sink->RemoteGeometry().MapFrom(anchor_.x - desktop_.left, anchor_.y - desktop_.top, geometry_.Size());
    ActivateTarget(deviceName, sink, x, y, true, mode);
  });
}

void InputService::SetEnabled(bool enabled) {
  Post([this, enabled] {
    enabled_ = enabled && !faulted_;
    if (!enabled_) ExitRemote();
  });
}

void InputService::SetSuspended(bool suspended) {
  Post([this, suspended] {
    suspended_ = suspended;
    reconcilePending_ = true;
    if (suspended_) ExitRemote();
    else MaintainState(win::IsInputDesktopCurrent());
  });
}

void InputService::PublishTopology(std::shared_ptr<const InputTopology> topology) {
  if (!topology || topology->LocalDeviceName() != localDevice_) throw std::invalid_argument("拓扑的本机标识不一致");
  Post([this, topology = std::move(topology)] {
    topology_ = topology;
    auto target = target_.load();
    if (!target) return;
    if (target->IsClosed() || (!targetSelectedDirectly_ && !topology_->Contains(CurrentDeviceName(), target))) ExitRemote();
  });
}

void InputService::ReturnHome(std::shared_ptr<IInputSink> expected) {
  Post([this, expected = std::move(expected)] {
    if (expected && target_.load() == expected) ExitRemote();
  });
}

void InputService::ReturnHome(std::shared_ptr<IInputSink> expected, uint64_t controlId) {
  Post([this, expected = std::move(expected), controlId] {
    if (expected && target_.load() == expected && controlId != 0 && expected->ControlId() == controlId) ExitRemote();
  });
}

void InputService::RefreshDesktopGeometry() { Post([this] { RefreshGeometry(); }); }

void InputService::Initialize() {
  RefreshGeometry();
  if (hideCursorWhileRemote) CreateCursorHider();
  POINT cursor{};
  GetCursorPos(&cursor);
  lastCursor_ = Point2i{cursor.x, cursor.y};
  MaintainState(win::IsInputDesktopCurrent());
}

void InputService::Cleanup() {
  ExitRemote();
  cursorHider_.reset();
}

void InputService::Fail(std::string message) {
  // 钩子或路由故障需重新建立健康输入管线，用户开关不能将故障实例重新放行
  faulted_ = true;
  enabled_ = false;
  try {
    ExitRemote();
  } catch (const std::exception& release) {
    message += "；远端释放失败：" + std::string(release.what());
  } catch (...) {
    message += "；远端释放失败：未知异常";
  }
  ReportFault(message);
  FinishDisposalWhenReleased();
}

void InputService::ReportFault(const std::string& message) {
  try {
    if (faulted) faulted(message);
  } catch (const std::exception& observer) {
    LogError("输入故障通知失败：" + message + "；" + observer.what());
  } catch (...) {
    LogError("输入故障通知失败：" + message);
  }
}

void InputService::TrackPhysical(const InputEvent& input) {
  if (!input.physical) return;
  if (input.kind == InputEventKind::Key) {
    KeyStroke key{input.virtualKey, input.scanCode, input.extended};
    initialKeys_.erase(PhysicalKey(key));
    if (input.down) physicalKeys_.insert(key); else physicalKeys_.erase(key);
  } else if (input.kind == InputEventKind::Button) {
    MouseButton button = static_cast<MouseButton>(input.data);
    if (input.down) physicalButtons_.insert(button); else physicalButtons_.erase(button);
  }
}

void InputService::MaintainState(bool inputDesktopAvailable) {
  inputDesktopAvailable_ = inputDesktopAvailable;
  if (!inputDesktopAvailable) {
    reconcilePending_ = true;
    ExitRemote();
    return;
  }
  RestoreSourceCursor();
  if (suspended_) {
    ExitRemote();
    return;
  }
  if (reconcilePending_) {
    ExitRemote();
    // LL 回调发生在异步键态更新之前，采样只能从消息泵命令或定时维护进入
    std::array<bool, 256> down{};
    for (int key = 1; key < 256; key++) down[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    if (!win::IsInputDesktopCurrent()) {
      inputDesktopAvailable_ = false;
      return;
    }
    ReconcilePhysical(down);
    reconcilePending_ = false;
    POINT cursor{};
    GetCursorPos(&cursor);
    lastCursor_ = Point2i{cursor.x, cursor.y};
  }
  FinishDisposalWhenReleased();
}

void InputService::ReconcilePhysical(const std::array<bool, 256>& down) {
  // 桌面恢复采样不作为吞键的绝对物理真值：移出的旧归属继续隔离重复 Down 与迟到 Up
  for (auto it = initialKeys_.begin(); it != initialKeys_.end();) {
    if (down[*it]) ++it; else it = initialKeys_.erase(it);
  }
  for (auto it = physicalKeys_.begin(); it != physicalKeys_.end();) {
    if (down[PhysicalKey(*it)]) ++it; else it = physicalKeys_.erase(it);
  }
  for (auto it = suppressedKeys_.begin(); it != suppressedKeys_.end();) {
    if (down[PhysicalKey(*it)]) {
      ++it;
      continue;
    }
    releasedRemoteKeys_.insert(*it);
    it = suppressedKeys_.erase(it);
  }
  for (auto it = localKeys_.begin(); it != localKeys_.end();) {
    if (down[PhysicalKey(*it)]) {
      ++it;
      continue;
    }
    releasedLocalKeys_.insert(*it);
    it = localKeys_.erase(it);
  }
  for (int key = 8; key < 255; key++) {
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) continue;
    if (!down[key]) continue;
    bool held = false;
    for (const KeyStroke& stroke : physicalKeys_) {
      if (PhysicalKey(stroke) == key) {
        held = true;
        break;
      }
    }
    if (!held) initialKeys_.insert(key);
  }
  bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
  const MouseButton buttons[5] = {MouseButton::Left, MouseButton::Right, MouseButton::Middle, MouseButton::X1, MouseButton::X2};
  for (MouseButton button : buttons) {
    int key = button == MouseButton::Left    ? (swapped ? 2 : 1)
              : button == MouseButton::Right ? (swapped ? 1 : 2)
              : button == MouseButton::Middle ? 4
              : button == MouseButton::X1    ? 5
                                             : 6;
    if (down[key]) {
      physicalButtons_.insert(button);
      continue;
    }
    physicalButtons_.erase(button);
    if (suppressedButtons_.erase(button)) releasedRemoteButtons_.insert(button);
    if (localButtons_.erase(button)) releasedLocalButtons_.insert(button);
  }
}

void InputService::ObserveRawRelease(bool keyboard, int virtualKey, int scanCode, bool extended, MouseButton button) {
  // Raw Up 独立于 LL 的吞键结果；只补发尚未释放的远端 Up，本机归属保持原样
  if (!keyboard) {
    rawReleasedButtons_.insert(button);
    physicalButtons_.erase(button);
    if (remoteButtons_.contains(button)) {
      InputEvent input;
      input.kind = InputEventKind::Button;
      input.data = static_cast<int>(button);
      input.down = false;
      RouteButton(input);
      releasedRemoteButtons_.insert(button);
      rawReleasedButtons_.insert(button);
    }
    if (suppressedButtons_.erase(button)) releasedRemoteButtons_.insert(button);
    if (localButtons_.erase(button)) releasedLocalButtons_.insert(button);
    FinishDisposalWhenReleased();
    return;
  }

  KeyStroke raw{virtualKey, scanCode, extended};
  int physical = PhysicalKey(raw);
  auto matches = [physical, scanCode, extended](const KeyStroke& held) {
    return PhysicalKey(held) == physical && (scanCode == 0 || held.scanCode == scanCode) && held.extended == extended;
  };
  auto collect = [this, &matches](const KeySet& source) {
    for (const KeyStroke& held : source)
      if (matches(held)) rawReleasedKeys_.insert(held);
  };
  collect(releasedRemoteKeys_);
  collect(releasedLocalKeys_);
  collect(suppressedKeys_);
  collect(localKeys_);
  collect(remoteKeys_);
  initialKeys_.erase(physical);
  for (auto it = physicalKeys_.begin(); it != physicalKeys_.end();) {
    if (matches(*it)) it = physicalKeys_.erase(it); else ++it;
  }
  KeySet remoteMatched;
  for (const KeyStroke& held : remoteKeys_)
    if (matches(held)) remoteMatched.insert(held);
  for (const KeyStroke& key : remoteMatched) {
    InputEvent input;
    input.kind = InputEventKind::Key;
    input.virtualKey = key.virtualKey;
    input.scanCode = key.scanCode;
    input.extended = key.extended;
    RouteKey(input);
    releasedRemoteKeys_.insert(key);
    rawReleasedKeys_.insert(key);
  }
  for (auto it = suppressedKeys_.begin(); it != suppressedKeys_.end();) {
    if (!matches(*it)) {
      ++it;
      continue;
    }
    releasedRemoteKeys_.insert(*it);
    it = suppressedKeys_.erase(it);
  }
  for (auto it = localKeys_.begin(); it != localKeys_.end();) {
    if (!matches(*it)) {
      ++it;
      continue;
    }
    releasedLocalKeys_.insert(*it);
    it = localKeys_.erase(it);
  }
  FinishDisposalWhenReleased();
}

void InputService::FinishDisposalWhenReleased() {
  if (!disposing_ || restorePoint_ || !suppressedKeys_.empty() || !suppressedButtons_.empty()) return;
  // 异步键态不能证明被吞 Down 已抬起；只有明确 LL/Raw Up 才允许卸载
  for (const KeyStroke& key : releasedRemoteKeys_)
    if (!rawReleasedKeys_.contains(key)) return;
  for (MouseButton button : releasedRemoteButtons_)
    if (!rawReleasedButtons_.contains(button)) return;
  hook_->RequestStop();
}

void InputService::RefreshGeometry() {
  Point2i origin{};
  DesktopGeometry geometry = DesktopGeometry::Capture(&origin);
  bool changed = desktop_.left != origin.x || desktop_.top != origin.y || !geometry.SameAs(geometry_);
  // 回切必须使用新桌面的可见区域，不能先按已移除的显示器排入旧坐标
  Size2i size = geometry.Size();
  geometry_ = std::move(geometry);
  desktop_ = RectI{origin.x, origin.y, origin.x + size.width, origin.y + size.height};
  anchor_ = ClampToDesktop(anchor_);
  if (changed) {
    ExitRemote();
    localInputSince_ = TickMs();
  }
  if (desktopGeometryChanged) desktopGeometryChanged(geometry_);
}

void InputService::CreateCursorHider() {
  try {
    cursorHider_ = std::make_unique<CursorHider>();
  } catch (const std::exception& error) {
    // 隐藏光标只是显示效果，失败就不藏，不能连带影响输入管线
    LogWarn(std::string("无法创建隐藏光标窗口：") + error.what());
  }
}

void InputService::RestoreSourceCursor() {
  // 安全桌面不回定位；回到普通桌面后，在下一次输入放行前同步恢复
  if (!restorePoint_ || !win::IsInputDesktopCurrent()) return;
  PositionSourceCursor(ClampToDesktop(*restorePoint_));
  restorePoint_.reset();
}

void InputService::PositionSourceCursor(Point2i point) {
  // 与 MWB 一样直接调整源光标并读回实际位置；不排异步 SendInput，
  // 保证回切后紧接的本机点击不会落在远控期间的停放点
  if (!SetCursorPos(point.x, point.y)) win::ThrowLastError("SetCursorPos");
  sourcePositionRevision_++;
  POINT actual{};
  if (!GetCursorPos(&actual)) win::ThrowLastError("GetCursorPos");
  lastCursor_ = Point2i{actual.x, actual.y};
  if (target_.load()) return;
  RectI monitor = MonitorAt(geometry_, Point2i{lastCursor_.x - desktop_.left, lastCursor_.y - desktop_.top});
  switchGate_.Enter(lastCursor_.x, lastCursor_.y, monitor.Width(), monitor.Height());
  localInputSince_ = TickMs();
}

Point2i InputService::SourceParkingPoint() const {
  return geometry_.ParkingPoint(anchor_, Point2i{desktop_.left, desktop_.top});
}

Point2i InputService::ClampToDesktop(Point2i point) const {
  return geometry_.ClampToDesktop(point, Point2i{desktop_.left, desktop_.top});
}

std::string InputService::CurrentDeviceName() const {
  std::lock_guard<std::mutex> lock(viewMutex_);
  return currentDevice_;
}

void InputService::SetCurrentDevice(std::string name) {
  std::lock_guard<std::mutex> lock(viewMutex_);
  currentDevice_ = std::move(name);
}

}  // namespace bm
