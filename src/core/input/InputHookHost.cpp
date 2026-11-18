#include "core/input/InputHookHost.h"

#include "core/platform/Common.h"
#include "core/platform/EventLog.h"
#include "core/input/Injector.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace bm {

InputHookHost* InputHookHost::current_ = nullptr;

namespace {

// LL 新 Down 可抢在旧 WM_INPUT 前执行；同一毫秒也按新周期处理，不能由旧 Up 清除
bool DownBeforeMessage(uint32_t downTime, uint32_t messageTime) {
  return static_cast<int32_t>(downTime - messageTime) < 0;
}

}  // namespace

size_t InputHookHost::KeyIdHash::operator()(const KeyId& id) const noexcept {
  return (static_cast<size_t>(id.key) * 131) ^ (static_cast<size_t>(id.scanCode) * 7) ^ (id.extended ? 1u : 0u);
}

InputHookHost::InputHookHost(RouteFn route, std::function<void()> initialize, std::function<void()> cleanup,
                             std::function<void(const std::string&)> failed, std::function<void(bool)> maintain,
                             RawReleaseFn rawRelease)
    : route_(std::move(route)),
      initialize_(std::move(initialize)),
      cleanup_(std::move(cleanup)),
      failed_(std::move(failed)),
      maintain_(std::move(maintain)),
      rawRelease_(std::move(rawRelease)) {}

InputHookHost::~InputHookHost() {
  RequestStop();
  WaitCompletion();
}

void InputHookHost::Start() {
  {
    std::lock_guard<std::mutex> lock(readyMutex_);
    if (started_) return;
    started_ = true;
  }
  thread_ = std::thread([this] { Run(); });
  std::unique_lock<std::mutex> lock(readyMutex_);
  readyCv_.wait(lock, [this] { return ready_; });
  if (!startupError_.empty()) throw std::runtime_error("输入钩子初始化失败：" + startupError_);
}

void InputHookHost::Post(std::function<void()> command) {
  {
    std::lock_guard<std::mutex> lock(commandMutex_);
    commands_.push_back(std::move(command));
  }
  uint32_t id = threadId_.load();
  if (id != 0) PostThreadMessageW(id, kWmCommand, 0, 0);
}

void InputHookHost::RequestStop() {
  uint32_t id = threadId_.load();
  if (id != 0) PostThreadMessageW(id, WM_QUIT, 0, 0);
}

void InputHookHost::WaitCompletion() {
  if (!thread_.joinable() || thread_.get_id() == std::this_thread::get_id()) return;
  thread_.join();
}

bool InputHookHost::IsCurrentThread() const { return GetCurrentThreadId() == threadId_.load(); }

void InputHookHost::SetReady() {
  {
    std::lock_guard<std::mutex> lock(readyMutex_);
    ready_ = true;
  }
  readyCv_.notify_all();
}

bool InputHookHost::IsReady() {
  std::lock_guard<std::mutex> lock(readyMutex_);
  return ready_;
}

void InputHookHost::Run() {
  HMODULE module = GetModuleHandleW(nullptr);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  try {
    rawBuffer_.assign(128, 0);
    CreateRawInputWindow(module);
    current_ = this;
    initialize_();
    mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, &InputHookHost::MouseProc, module, 0);
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &InputHookHost::KeyboardProc, module, 0);
    if (!mouseHook_ || !keyboardHook_) win::ThrowLastError("SetWindowsHookExW");
    // 锁屏与桌面切换不会给 HWND_MESSAGE 广播，周期维护和应用投递共同覆盖桌面恢复
    if (SetTimer(window_, kMaintenanceTimer, kMaintenanceIntervalMs, nullptr) == 0) win::ThrowLastError("SetTimer");
    threadId_.store(GetCurrentThreadId());
    SetReady();
    DrainCommands();
    MSG message{};
    while (true) {
      int result = GetMessageW(&message, nullptr, 0, 0);
      if (result == 0) break;
      if (result < 0) win::ThrowLastError("GetMessageW");
      if (message.message == kWmCommand) {
        DrainCommands();
        continue;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  } catch (const std::exception& error) {
    startupError_ = error.what();
    if (IsReady()) ReportFailure(startupError_);
  } catch (...) {
    startupError_ = "未知异常";
    if (IsReady()) ReportFailure(startupError_);
  }

  if (mouseHook_) UnhookWindowsHookEx(mouseHook_);
  if (keyboardHook_) UnhookWindowsHookEx(keyboardHook_);
  if (window_) {
    KillTimer(window_, kMaintenanceTimer);
    DestroyWindow(window_);
    window_ = nullptr;
  }
  if (!className_.empty()) {
    UnregisterClassW(className_.c_str(), module);
    className_.clear();
  }
  rawBuffer_.clear();
  current_ = nullptr;
  threadId_.store(0);
  try {
    cleanup_();
  } catch (const std::exception& error) {
    ReportFailure(error.what());
  }
  SetReady();
}

void InputHookHost::DrainCommands() {
  Maintain();
  while (true) {
    std::function<void()> command;
    {
      std::lock_guard<std::mutex> lock(commandMutex_);
      if (commands_.empty()) return;
      command = std::move(commands_.front());
      commands_.pop_front();
    }
    try {
      command();
    } catch (const std::exception& error) {
      ReportFailure(error.what());
    } catch (...) {
      ReportFailure("输入命令未知异常");
    }
  }
}

void InputHookHost::Maintain() {
  try {
    maintain_(win::IsInputDesktopCurrent());
  } catch (const std::exception& error) {
    ReportFailure(error.what());
  } catch (...) {
    ReportFailure("输入桌面维护未知异常");
  }
}

void InputHookHost::ReportFailure(const std::string& message) {
  try {
    failed_(message);
  } catch (const std::exception& observer) {
    // 故障订阅者本身失败时，也不能让异常越过原生回调边界
    LogError("输入故障通知失败：" + message + "；" + observer.what());
  } catch (...) {
    LogError("输入故障通知失败：" + message);
  }
}

void InputHookHost::CreateRawInputWindow(HMODULE module) {
  className_ = L"BorderlessMouseRawInput_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(TickMs());
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = &InputHookHost::WindowProc;
  windowClass.hInstance = module;
  windowClass.lpszClassName = className_.c_str();
  if (RegisterClassExW(&windowClass) == 0) win::ThrowLastError("RegisterClassExW");
  window_ = CreateWindowExW(0, className_.c_str(), className_.c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, nullptr);
  if (!window_) win::ThrowLastError("CreateWindowExW");
  SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  // 鼠标与键盘都以 INPUTSINK 注册：窗口不在前台也要收到 RawInput
  RAWINPUTDEVICE devices[2] = {{0x01, 0x02, RIDEV_INPUTSINK, window_}, {0x01, 0x06, RIDEV_INPUTSINK, window_}};
  if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) win::ThrowLastError("RegisterRawInputDevices");
}

LRESULT CALLBACK InputHookHost::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  InputHookHost* host = reinterpret_cast<InputHookHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!host) return DefWindowProcW(window, message, wParam, lParam);
  try {
    if (message == WM_INPUT) host->ReadRawInput(reinterpret_cast<HRAWINPUT>(lParam));
    else if (message == WM_TIMER && wParam == kMaintenanceTimer) host->Maintain();
    // 前台 WM_INPUT 需要 DefWindowProc 完成系统缓冲清理
    return DefWindowProcW(window, message, wParam, lParam);
  } catch (const std::exception& error) {
    host->ReportFailure(error.what());
    return 0;
  } catch (...) {
    host->ReportFailure("RawInput 未知异常");
    return 0;
  }
}

void InputHookHost::ReadRawInput(HRAWINPUT rawInput) {
  const UINT headerSize = sizeof(RAWINPUTHEADER);
  UINT size = static_cast<UINT>(rawBuffer_.size());
  UINT written = GetRawInputData(rawInput, RID_INPUT, rawBuffer_.data(), &size, headerSize);
  if (written == static_cast<UINT>(-1) || written < headerSize) return;
  RAWINPUTHEADER header{};
  std::memcpy(&header, rawBuffer_.data(), headerSize);
  uint32_t messageTime = static_cast<uint32_t>(GetMessageTime());
  if (header.dwType == RIM_TYPEKEYBOARD) {
    if (written < headerSize + sizeof(RAWKEYBOARD)) return;
    RAWKEYBOARD key{};
    std::memcpy(&key, rawBuffer_.data() + headerSize, sizeof(key));
    if (key.ExtraInformation == Injector::kMagic || key.VKey >= 255 || key.MakeCode == 0xFF) return;
    // RI_KEY_BREAK 位表示抬起；LL 若已看到更新的 Down，这条旧 Up 属于上一周期
    if ((key.Flags & RI_KEY_BREAK) == 0) return;
    bool extended = (key.Flags & RI_KEY_E0) != 0;
    KeyId id{win::PhysicalKeyId(key.VKey, key.MakeCode, extended), key.MakeCode, extended};
    auto found = keyDownTimes_.find(id);
    if (found == keyDownTimes_.end() || DownBeforeMessage(found->second, messageTime))
      rawRelease_(true, key.VKey, key.MakeCode, extended, MouseButton::Left);
    return;
  }
  if (header.dwType != RIM_TYPEMOUSE || written < headerSize + sizeof(RAWMOUSE)) return;
  RAWMOUSE mouse{};
  std::memcpy(&mouse, rawBuffer_.data() + headerSize, sizeof(mouse));
  if (mouse.ulExtraInformation == Injector::kMagic) return;
  bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
  struct ReleaseFlag {
    USHORT flag;
    MouseButton button;
  };
  const ReleaseFlag releases[5] = {{RI_MOUSE_BUTTON_1_UP, swapped ? MouseButton::Right : MouseButton::Left},
                                   {RI_MOUSE_BUTTON_2_UP, swapped ? MouseButton::Left : MouseButton::Right},
                                   {RI_MOUSE_BUTTON_3_UP, MouseButton::Middle},
                                   {RI_MOUSE_BUTTON_4_UP, MouseButton::X1},
                                   {RI_MOUSE_BUTTON_5_UP, MouseButton::X2}};
  for (const ReleaseFlag& release : releases) {
    if ((mouse.usButtonFlags & release.flag) == 0) continue;
    auto found = buttonDownTimes_.find(release.button);
    if (found == buttonDownTimes_.end() || DownBeforeMessage(found->second, messageTime))
      rawRelease_(false, 0, 0, false, release.button);
  }
  // 位置只由 LL 系统屏幕坐标产生；Raw 相对报告仅见证本机边缘的向外方向
  if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0 || (mouse.lLastX == 0 && mouse.lLastY == 0)) return;
  InputEvent input;
  input.kind = InputEventKind::RelativeActivity;
  input.deltaX = mouse.lLastX;
  input.deltaY = mouse.lLastY;
  input.time = messageTime;
  route_(input);
}

LRESULT CALLBACK InputHookHost::MouseProc(int code, WPARAM wParam, LPARAM lParam) {
  InputHookHost* host = current_;
  if (!host) return CallNextHookEx(nullptr, code, wParam, lParam);
  // 反向 P/Invoke 是异常边界；状态撤销与故障通知完成后才把未提交事件放行
  try {
    return host->ProcessMouseHook(code, wParam, lParam);
  } catch (const std::exception& error) {
    host->ReportFailure(error.what());
    return 0;
  } catch (...) {
    host->ReportFailure("鼠标钩子未知异常");
    return 0;
  }
}

LRESULT CALLBACK InputHookHost::KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
  InputHookHost* host = current_;
  if (!host) return CallNextHookEx(nullptr, code, wParam, lParam);
  try {
    return host->ProcessKeyboardHook(code, wParam, lParam);
  } catch (const std::exception& error) {
    host->ReportFailure(error.what());
    return 0;
  } catch (...) {
    host->ReportFailure("键盘钩子未知异常");
    return 0;
  }
}

LRESULT InputHookHost::ProcessMouseHook(int code, WPARAM wParam, LPARAM lParam) {
  if (code < 0) return CallNextHookEx(mouseHook_, code, wParam, lParam);
  const MSLLHOOKSTRUCT& data = *reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
  UINT message = static_cast<UINT>(wParam);
  // Magic 落点和后续物理输入共用钩子串行域；SendInput 返回后的异步通知无法保证这个顺序
  if (data.dwExtraInfo == Injector::kMagic) {
    if (message == WM_MOUSEMOVE) {
      InputEvent injected;
      injected.kind = InputEventKind::InjectedCursor;
      injected.deltaX = data.pt.x;
      injected.deltaY = data.pt.y;
      injected.physical = false;
      route_(injected);
    }
    return CallNextHookEx(mouseHook_, code, wParam, lParam);
  }
  InputEvent input;
  input.physical = (data.flags & LLMHF_INJECTED) == 0;
  if (message == WM_MOUSEMOVE) {
    input.kind = InputEventKind::Cursor;
    input.deltaX = data.pt.x;
    input.deltaY = data.pt.y;
  } else if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
    input.kind = InputEventKind::Wheel;
    input.data = static_cast<short>(data.mouseData >> 16);
    input.horizontal = message == WM_MOUSEHWHEEL;
  } else {
    MouseButton button;
    switch (message) {
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP: button = MouseButton::Left; break;
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP: button = MouseButton::Right; break;
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP: button = MouseButton::Middle; break;
      case WM_XBUTTONDOWN:
      case WM_XBUTTONUP: button = (data.mouseData >> 16) == 2 ? MouseButton::X2 : MouseButton::X1; break;
      default: return CallNextHookEx(mouseHook_, code, wParam, lParam);
    }
    bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
    if (down) buttonDownTimes_[button] = data.time;
    input.kind = InputEventKind::Button;
    input.data = static_cast<int>(button);
    input.down = down;
  }
  return route_(input) ? 1 : CallNextHookEx(mouseHook_, code, wParam, lParam);
}

LRESULT InputHookHost::ProcessKeyboardHook(int code, WPARAM wParam, LPARAM lParam) {
  if (code < 0) return CallNextHookEx(keyboardHook_, code, wParam, lParam);
  const KBDLLHOOKSTRUCT& data = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
  if (data.dwExtraInfo == Injector::kMagic) return CallNextHookEx(keyboardHook_, code, wParam, lParam);
  bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
  bool extended = (data.flags & LLKHF_EXTENDED) != 0;
  int virtualKey = static_cast<int>(data.vkCode);
  int scanCode = static_cast<int>(data.scanCode);
  if (down) keyDownTimes_[KeyId{win::PhysicalKeyId(virtualKey, scanCode, extended), scanCode, extended}] = static_cast<uint32_t>(data.time);
  InputEvent input;
  input.kind = InputEventKind::Key;
  input.down = down;
  input.virtualKey = virtualKey;
  input.scanCode = scanCode;
  input.extended = extended;
  input.physical = (data.flags & LLKHF_INJECTED) == 0;
  return route_(input) ? 1 : CallNextHookEx(keyboardHook_, code, wParam, lParam);
}

}  // namespace bm
