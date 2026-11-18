#pragma once

#include "core/input/InputEvent.h"
#include "core/platform/Win32Input.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bm {

// 对应 Input/InputHookHost.cs：钩子、RawInput 和外部命令共享同一个消息泵，
// 回调返回前已确定事件归属。除命令队列与就绪状态外，所有状态只在钩子线程上读写。
class InputHookHost {
 public:
  using RouteFn = std::function<bool(const InputEvent&)>;
  using RawReleaseFn = std::function<void(bool keyboard, int virtualKey, int scanCode, bool extended, MouseButton button)>;

  InputHookHost(RouteFn route, std::function<void()> initialize, std::function<void()> cleanup,
                std::function<void(const std::string&)> failed, std::function<void(bool)> maintain, RawReleaseFn rawRelease);
  ~InputHookHost();
  InputHookHost(const InputHookHost&) = delete;
  InputHookHost& operator=(const InputHookHost&) = delete;

  // 阻塞到钩子安装完成；初始化失败时抛出
  void Start();
  // 从任意线程投递到钩子线程执行
  void Post(std::function<void()> command);
  // 只在串行域调用：持键结束后的卸载由路由决定
  void RequestStop();
  void Dispose() { RequestStop(); }
  // 等钩子线程真正卸载；不能在本线程调用
  void WaitCompletion();
  bool IsCurrentThread() const;

 private:
  struct KeyId {
    int key = 0;
    int scanCode = 0;
    bool extended = false;
    bool operator==(const KeyId&) const = default;
  };
  struct KeyIdHash {
    size_t operator()(const KeyId& id) const noexcept;
  };
  struct ButtonHash {
    size_t operator()(MouseButton button) const noexcept { return static_cast<size_t>(button); }
  };

  static constexpr UINT kWmCommand = 0x8001;  // WM_APP + 1，线程消息不能落在 WM_USER 以下
  static constexpr UINT_PTR kMaintenanceTimer = 1;
  static constexpr UINT kMaintenanceIntervalMs = 250;

  void Run();
  void DrainCommands();
  void Maintain();
  void CreateRawInputWindow(HMODULE module);
  void ReadRawInput(HRAWINPUT rawInput);
  void ReportFailure(const std::string& message);
  void SetReady();
  bool IsReady();
  LRESULT ProcessMouseHook(int code, WPARAM wParam, LPARAM lParam);
  LRESULT ProcessKeyboardHook(int code, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);

  RouteFn route_;
  std::function<void()> initialize_;
  std::function<void()> cleanup_;
  std::function<void(const std::string&)> failed_;
  std::function<void(bool)> maintain_;
  RawReleaseFn rawRelease_;

  std::mutex readyMutex_;
  std::condition_variable readyCv_;
  bool ready_ = false;
  std::string startupError_;
  bool started_ = false;

  std::thread thread_;
  std::atomic<uint32_t> threadId_{0};
  HHOOK mouseHook_ = nullptr;
  HHOOK keyboardHook_ = nullptr;
  HWND window_ = nullptr;
  std::wstring className_;
  std::vector<uint8_t> rawBuffer_;

  std::mutex commandMutex_;
  std::deque<std::function<void()>> commands_;
  // 只用于判断「LL 是否已经看到新周期的 Down」，不参与逐事件配对
  std::unordered_map<KeyId, uint32_t, KeyIdHash> keyDownTimes_;
  std::unordered_map<MouseButton, uint32_t, ButtonHash> buttonDownTimes_;

  // 低级钩子回调只有函数指针，进程内同一时刻只有一个宿主
  static InputHookHost* current_;
};

}  // namespace bm
