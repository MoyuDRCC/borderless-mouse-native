#pragma once

#include "core/input/InputEvent.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

namespace bm {

// 对应 Input/Injector.cs：专用线程独占实际注入状态；准入锁只管理请求代次、撤销与恢复屏障，
// 绝不持锁调用 SendInput。回调都在注入线程上执行。
class Injector {
 public:
  // SendInput 的 EXTRA_INFO 标记；低级钩子据此识别自身注入，只同步落点、不触发抢占
  static constexpr uintptr_t kMagic = 0x574D424D;
  // 待注入队列上限，超过即撤销当前控制并排入释放
  static constexpr size_t kQueueCapacity = 1024;

  Injector() = default;
  ~Injector();
  Injector(const Injector&) = delete;
  Injector& operator=(const Injector&) = delete;

  // SendInput 被系统拒绝，携带 Win32 错误码；同一故障只通知一次
  std::function<void(int)> injectionFailed;
  // 普通桌面重新可注入
  std::function<void()> injectionRecovered;
  // 实际释放屏障已完成；只通知重新检查准入，不会自动恢复旧控制
  std::function<void()> controlReleased;
  // 队列过载已同步撤销代次；调用方应明确通知旧控制者退出
  std::function<void()> controlOverloaded;

  void Start();
  void Stop();
  // 等待注入线程真正结束；Stop 返回不代表尚未成功的释放已完成
  void WaitCompletion();
  // 健康且未停止；释放中的候选仍需用 TryBeginControl 检查实际屏障
  bool IsAvailable() const;
  bool TryBeginControl();
  // 最近一次与 5 秒窗口内最慢的 SendInput 耗时（毫秒），供界面显示
  std::pair<double, double> Timing() const;

  // 位置为被控端虚拟桌面的零基坐标
  void MoveDesktopOffset(int x, int y);
  void Button(MouseButton button, bool down);
  void Wheel(int delta, bool horizontal);
  void Key(int virtualKey, int scanCode, bool extended, bool down);
  void Key(int virtualKey, bool down);
  // 撤销在调用线程立即关闭准入，队列中旧 Down 直接作废；释放实际按下状态仍由注入线程负责
  void ReleaseAll();

 private:
  enum class Op { DesktopOffset, Button, Wheel, Key, ReleaseAll };

  struct KeyStroke {
    int virtualKey = 0;
    int scanCode = 0;
    bool extended = false;
    bool operator==(const KeyStroke&) const = default;
  };
  struct KeyStrokeHash {
    size_t operator()(const KeyStroke& key) const noexcept;
  };
  struct ButtonHash {
    size_t operator()(MouseButton button) const noexcept { return static_cast<size_t>(button); }
  };
  struct Request {
    Op kind = Op::DesktopOffset;
    int x = 0;
    int y = 0;
    bool extended = false;
    bool down = false;
    uint64_t generation = 0;
  };

  void Run();
  void RunRequests();
  void Execute(const Request& request);
  void Recover();
  void SendMove(int x, int y);
  bool SendButton(MouseButton button, bool down);
  bool SendMouse(uint32_t flags, uint32_t data);
  bool SendKey(const KeyStroke& key, bool down);
  // 发送一条已组好的 INPUT；失败时同步撤销控制并通知
  bool Send(const void* input);
  void ReleaseHeld();
  void RecordTiming(double milliseconds);

  void CancelControlLocked();
  bool TryEnqueueLocked(const Request& request);
  void EnqueueControlled(const Request& request);
  void SignalWake();
  void MarkCompleted();

  void NotifyFailed(int code);
  void NotifyRecovered();
  void NotifyReleased();
  void NotifyOverloaded();

  mutable std::mutex gate_;
  std::deque<Request> queue_;
  std::condition_variable wake_;
  bool wakeSignaled_ = false;
  // 只记录 SendInput 已成功的 Down；失败的 Up 必须留在集合中，直到真正释放
  std::unordered_set<KeyStroke, KeyStrokeHash> keys_;
  std::unordered_set<MouseButton, ButtonHash> buttons_;
  std::thread thread_;
  uint64_t generation_ = 0;
  // 只在注入线程上读写
  uint64_t nextRecovery_ = 0;
  bool accepting_ = false;
  bool releasePending_ = false;
  bool blocked_ = false;
  bool stopping_ = false;
  // 析构时置位：不再等待释放屏障，立即结束注入线程
  bool abandoned_ = false;

  std::mutex doneMutex_;
  std::condition_variable doneCv_;
  bool completed_ = false;

  // 耗时统计用独立的锁：既不让界面读数字挡住注入队列，也不和状态锁互相等待
  mutable std::mutex timingMutex_;
  double lastSendMs_ = 0;
  double maxSendMs_ = 0;
  uint64_t timingWindow_ = 0;
};

}  // namespace bm
