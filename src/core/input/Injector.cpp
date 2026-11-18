#include "core/input/Injector.h"

#include "core/platform/Common.h"
#include "core/platform/EventLog.h"
#include "core/platform/Win32Input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace bm {

namespace {

constexpr uint64_t kRecoveryIntervalMs = 250;
constexpr int kMaxRequestsPerRound = 64;
constexpr uint64_t kTimingWindowMs = 5000;

// 注入耗时用高精度计数器换算成毫秒，界面显示才有意义
double NowMs() {
  static const double kTicksPerMs = [] {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(frequency.QuadPart) / 1000.0;
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) / kTicksPerMs;
}

}  // namespace

size_t Injector::KeyStrokeHash::operator()(const KeyStroke& key) const noexcept {
  return (static_cast<size_t>(key.virtualKey) << 12) ^ (static_cast<size_t>(key.scanCode) << 1) ^ (key.extended ? 1u : 0u);
}

Injector::~Injector() {
  Stop();
  if (!thread_.joinable() || thread_.get_id() == std::this_thread::get_id()) return;
  // 析构不再等待释放屏障：桌面一直拒绝注入时注入线程会无限重试，进程退出前只能直接结束
  {
    std::lock_guard<std::mutex> lock(gate_);
    abandoned_ = true;
  }
  SignalWake();
  thread_.join();
}

void Injector::Start() {
  std::lock_guard<std::mutex> lock(gate_);
  if (thread_.joinable() || stopping_) return;
  thread_ = std::thread([this] { Run(); });
}

void Injector::Stop() {
  {
    std::lock_guard<std::mutex> lock(gate_);
    stopping_ = true;
    CancelControlLocked();
  }
  SignalWake();
  if (!thread_.joinable()) {
    MarkCompleted();
    return;
  }
  // 关闭调用有界；若桌面仍拒绝释放，注入线程继续持有责任并按周期重试
  if (thread_.get_id() == std::this_thread::get_id()) return;
  std::unique_lock<std::mutex> lock(doneMutex_);
  doneCv_.wait_for(lock, std::chrono::milliseconds(1000), [this] { return completed_; });
}

void Injector::WaitCompletion() {
  std::unique_lock<std::mutex> lock(doneMutex_);
  doneCv_.wait(lock, [this] { return completed_; });
}

void Injector::MarkCompleted() {
  {
    std::lock_guard<std::mutex> lock(doneMutex_);
    completed_ = true;
  }
  doneCv_.notify_all();
}

bool Injector::IsAvailable() const {
  std::lock_guard<std::mutex> lock(gate_);
  return !blocked_ && !stopping_;
}

bool Injector::TryBeginControl() {
  std::lock_guard<std::mutex> lock(gate_);
  if (stopping_ || blocked_ || releasePending_ || accepting_) return false;
  generation_++;
  accepting_ = true;
  return true;
}

std::pair<double, double> Injector::Timing() const {
  std::lock_guard<std::mutex> lock(timingMutex_);
  return {lastSendMs_, maxSendMs_};
}

void Injector::MoveDesktopOffset(int x, int y) { EnqueueControlled(Request{Op::DesktopOffset, x, y}); }

void Injector::Button(MouseButton button, bool down) {
  EnqueueControlled(Request{Op::Button, static_cast<int>(button), 0, false, down});
}

void Injector::Wheel(int delta, bool horizontal) { EnqueueControlled(Request{Op::Wheel, delta, 0, horizontal}); }

void Injector::Key(int virtualKey, int scanCode, bool extended, bool down) {
  EnqueueControlled(Request{Op::Key, virtualKey, scanCode, extended, down});
}

void Injector::Key(int virtualKey, bool down) {
  // MAPVK_VK_TO_VSC_EX 同时给出扩展位
  uint32_t scan = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC_EX);
  Key(virtualKey, static_cast<int>(scan & 0xFF), (scan & 0xFF00) == 0xE000, down);
}

void Injector::ReleaseAll() {
  {
    std::lock_guard<std::mutex> lock(gate_);
    CancelControlLocked();
  }
  SignalWake();
}

void Injector::CancelControlLocked() {
  accepting_ = false;
  generation_++;
  releasePending_ = true;
  queue_.clear();
  queue_.push_back(Request{Op::ReleaseAll, 0, 0, false, false, generation_});
}

void Injector::EnqueueControlled(const Request& request) {
  bool accepted;
  {
    std::lock_guard<std::mutex> lock(gate_);
    if (!accepting_ || blocked_ || releasePending_ || stopping_) return;
    Request stamped = request;
    stamped.generation = generation_;
    accepted = TryEnqueueLocked(stamped);
  }
  SignalWake();
  if (!accepted) NotifyOverloaded();
}

bool Injector::TryEnqueueLocked(const Request& request) {
  // 只覆盖相邻、同种坐标且同代次的位置，绝不跨过按键、按钮或释放屏障
  if (request.kind == Op::DesktopOffset && !queue_.empty() && queue_.back().kind == Op::DesktopOffset &&
      queue_.back().generation == request.generation) {
    queue_.back() = request;
    return true;
  }
  if (queue_.size() == kQueueCapacity) {
    CancelControlLocked();
    return false;
  }
  queue_.push_back(request);
  return true;
}

void Injector::SignalWake() {
  {
    std::lock_guard<std::mutex> lock(gate_);
    wakeSignaled_ = true;
  }
  wake_.notify_one();
}

void Injector::Run() {
  try {
    RunRequests();
  } catch (const std::exception& error) {
    LogError(std::string("注入线程异常：") + error.what());
  } catch (...) {
    LogError("注入线程未知异常");
  }
  MarkCompleted();
}

void Injector::RunRequests() {
  while (true) {
    {
      std::lock_guard<std::mutex> lock(gate_);
      if (abandoned_) return;
    }
    // 每轮最多处理固定数量请求，持续输入也不能挤掉恢复释放的周期
    for (int count = 0; count < kMaxRequestsPerRound; count++) {
      Request request;
      bool has = false;
      {
        std::lock_guard<std::mutex> lock(gate_);
        while (!queue_.empty()) {
          request = queue_.front();
          queue_.pop_front();
          if (request.kind == Op::ReleaseAll) {
            if (request.generation != generation_) continue;
          } else if (blocked_ || releasePending_ || stopping_ ||
                     (request.generation != 0 && (!accepting_ || request.generation != generation_))) {
            continue;
          }
          has = true;
          break;
        }
      }
      if (!has) break;
      Execute(request);
    }

    bool blocked;
    {
      std::lock_guard<std::mutex> lock(gate_);
      if (stopping_ && (abandoned_ || (keys_.empty() && buttons_.empty()))) return;
      blocked = blocked_;
      if (!blocked && !queue_.empty()) continue;
    }
    if (blocked && TickMs() >= nextRecovery_) Recover();
    {
      std::lock_guard<std::mutex> lock(gate_);
      if (stopping_ && (abandoned_ || (keys_.empty() && buttons_.empty()))) return;
      if (!queue_.empty()) continue;
      blocked = blocked_;
    }

    std::unique_lock<std::mutex> lock(gate_);
    auto ready = [this] { return wakeSignaled_ || !queue_.empty() || abandoned_; };
    if (blocked) {
      uint64_t now = TickMs();
      int64_t remaining = static_cast<int64_t>(nextRecovery_) - static_cast<int64_t>(now);
      int64_t wait = std::clamp<int64_t>(remaining, 1, static_cast<int64_t>(kRecoveryIntervalMs));
      wake_.wait_for(lock, std::chrono::milliseconds(wait), ready);
    } else {
      wake_.wait(lock, ready);
    }
    // 条件已满足（含队列非空）时清掉重听标记，下一轮靠队列长度继续推进
    wakeSignaled_ = false;
  }
}

void Injector::Execute(const Request& request) {
  switch (request.kind) {
    case Op::DesktopOffset:
      SendMove(request.x, request.y);
      break;
    case Op::Button: {
      MouseButton button = static_cast<MouseButton>(request.x);
      if (!SendButton(button, request.down)) break;
      std::lock_guard<std::mutex> lock(gate_);
      if (request.down) buttons_.insert(button); else buttons_.erase(button);
      break;
    }
    case Op::Wheel:
      SendMouse(request.extended ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL, static_cast<uint32_t>(request.x));
      break;
    case Op::Key: {
      KeyStroke key{request.x, request.y, request.extended};
      if (!SendKey(key, request.down)) break;
      std::lock_guard<std::mutex> lock(gate_);
      if (request.down) keys_.insert(key); else keys_.erase(key);
      break;
    }
    case Op::ReleaseAll: {
      ReleaseHeld();
      bool released;
      {
        std::lock_guard<std::mutex> lock(gate_);
        // 释放期间可能又被撤销；旧完成不能越过较新的释放屏障
        released = request.generation == generation_ && !blocked_ && keys_.empty() && buttons_.empty();
        if (released) releasePending_ = false;
      }
      if (released) NotifyReleased();
      break;
    }
  }
}

void Injector::Recover() {
  nextRecovery_ = TickMs() + kRecoveryIntervalMs;
  // 不切换、不附着安全桌面；只有当前输入桌面仍是本线程所在普通桌面时才尝试释放
  if (!win::IsOwnInputDesktop()) return;
  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(gate_);
    generation = generation_;
  }
  ReleaseHeld();
  {
    std::lock_guard<std::mutex> lock(gate_);
    if (!keys_.empty() || !buttons_.empty()) return;
  }
  // 零相对移动只探测当前可注入性，不重新注入旧 Down，也不改变用户位置
  if (!SendMouse(MOUSEEVENTF_MOVE, 0)) return;
  {
    std::lock_guard<std::mutex> lock(gate_);
    if (generation != generation_) return;
    queue_.clear();
    blocked_ = false;
    releasePending_ = false;
  }
  NotifyRecovered();
  NotifyReleased();
}

void Injector::SendMove(int x, int y) {
  int width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
  int height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
  // 传入的是被控端桌面零基坐标，按虚拟桌面跨度归一化到 0..65535
  int dx = static_cast<int>(std::nearbyint(std::clamp(x / static_cast<double>(std::max(1, width - 1)), 0.0, 1.0) * 65535));
  int dy = static_cast<int>(std::nearbyint(std::clamp(y / static_cast<double>(std::max(1, height - 1)), 0.0, 1.0) * 65535));
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dx = dx;
  input.mi.dy = dy;
  input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  input.mi.dwExtraInfo = kMagic;
  Send(&input);
}

bool Injector::SendButton(MouseButton button, bool down) {
  uint32_t flags;
  switch (button) {
    case MouseButton::Left: flags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    case MouseButton::Right: flags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case MouseButton::Middle: flags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    default: flags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP; break;
  }
  uint32_t data = button == MouseButton::X1 ? 1u : button == MouseButton::X2 ? 2u : 0u;
  return SendMouse(flags, data);
}

bool Injector::SendMouse(uint32_t flags, uint32_t data) {
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.mouseData = data;
  input.mi.dwFlags = flags;
  input.mi.dwExtraInfo = kMagic;
  return Send(&input);
}

bool Injector::SendKey(const KeyStroke& key, bool down) {
  uint32_t flags = (down ? 0u : KEYEVENTF_KEYUP) | (key.extended ? KEYEVENTF_EXTENDEDKEY : 0u);
  if (key.scanCode != 0) flags |= KEYEVENTF_SCANCODE;
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  // 有扫描码时只给扫描码，系统自行映射到当前键盘布局
  input.ki.wVk = static_cast<WORD>(key.scanCode == 0 ? key.virtualKey : 0);
  input.ki.wScan = static_cast<WORD>(key.scanCode);
  input.ki.dwFlags = flags;
  input.ki.dwExtraInfo = kMagic;
  return Send(&input);
}

bool Injector::Send(const void* input) {
  double started = NowMs();
  bool sent = SendInput(1, static_cast<LPINPUT>(const_cast<void*>(input)), sizeof(INPUT)) == 1;
  RecordTiming(NowMs() - started);
  if (sent) return true;
  DWORD error = GetLastError();
  bool notify;
  {
    std::lock_guard<std::mutex> lock(gate_);
    notify = !blocked_;
    blocked_ = true;
    accepting_ = false;
    releasePending_ = true;
    generation_++;
    queue_.clear();
  }
  nextRecovery_ = TickMs() + kRecoveryIntervalMs;
  if (notify) NotifyFailed(static_cast<int>(error));
  return false;
}

void Injector::ReleaseHeld() {
  std::vector<KeyStroke> keys;
  {
    std::lock_guard<std::mutex> lock(gate_);
    keys.assign(keys_.begin(), keys_.end());
  }
  for (const KeyStroke& key : keys) {
    if (!SendKey(key, false)) continue;
    std::lock_guard<std::mutex> lock(gate_);
    keys_.erase(key);
  }
  std::vector<MouseButton> buttons;
  {
    std::lock_guard<std::mutex> lock(gate_);
    buttons.assign(buttons_.begin(), buttons_.end());
  }
  for (MouseButton button : buttons) {
    if (!SendButton(button, false)) continue;
    std::lock_guard<std::mutex> lock(gate_);
    buttons_.erase(button);
  }
}

void Injector::RecordTiming(double milliseconds) {
  std::lock_guard<std::mutex> lock(timingMutex_);
  uint64_t now = TickMs();
  if (now - timingWindow_ >= kTimingWindowMs) {
    timingWindow_ = now;
    maxSendMs_ = 0;
  }
  lastSendMs_ = milliseconds;
  if (milliseconds > maxSendMs_) maxSendMs_ = milliseconds;
}

void Injector::NotifyFailed(int code) {
  try {
    if (injectionFailed) injectionFailed(code);
  } catch (const std::exception& error) {
    LogError(std::string("注入失败通知异常：") + error.what());
  }
}

void Injector::NotifyRecovered() {
  try {
    if (injectionRecovered) injectionRecovered();
  } catch (const std::exception& error) {
    LogError(std::string("注入恢复通知异常：") + error.what());
  }
}

void Injector::NotifyReleased() {
  try {
    if (controlReleased) controlReleased();
  } catch (const std::exception& error) {
    LogError(std::string("注入释放通知异常：") + error.what());
  }
}

void Injector::NotifyOverloaded() {
  LogWarn("接收注入队列超过 " + std::to_string(kQueueCapacity) + " 项，已撤销控制并排入释放");
  try {
    if (controlOverloaded) controlOverloaded();
  } catch (const std::exception& error) {
    LogError(std::string("注入过载通知异常：") + error.what());
  }
}

}  // namespace bm
