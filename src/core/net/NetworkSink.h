#pragma once

#include "core/input/InputEvent.h"
#include "core/net/PeerSession.h"

#include <atomic>
#include <mutex>

namespace bm {

// IInputSink 的网络实现：出口锁独占控制代次，租约校验与提交是同一个操作，
// 返回值只表示本帧已被会话队列接受
class NetworkSink final : public IInputSink {
 public:
  explicit NetworkSink(PeerSessionPtr session);

  // 指针速度比例：对端 DPI 除以本机 DPI，1 表示 1:1
  void SetPointerScale(double scale) { pointerScale_.store(scale, std::memory_order_relaxed); }
  // 垂直滚轮反向：本机开着自然滚动、对端是普通滚轮习惯时用
  void SetWheelReverse(bool reverse) { wheelReverse_ = reverse; }
  // 滚轮速度百分数，100 为标准
  void SetWheelScalePercent(int percent) { wheelScalePercent_ = percent; }

  const DesktopGeometry& RemoteGeometry() const override;
  bool IsClosed() const override;
  bool IsActive() const override;
  uint64_t ControlId() const override;
  void BeginControl() override;
  double PointerScale() const override;
  bool MoveTo(int x, int y) override;
  bool Button(MouseButton button, bool down) override;
  bool Wheel(int delta, bool horizontal) override;
  bool Key(int virtualKey, int scanCode, bool extended, bool down) override;
  void ReleaseControl() override;

  // 对端 Yield：只有当前代次可以被撤销，返回是否确实撤回了控制
  bool HandleYield(uint64_t controlId);

 private:
  // 按每台设备的滚轮设置换算一格滚动量，至少保留一格方向
  int ScaleWheel(int delta, bool horizontal) const;

  PeerSessionPtr session_;
  mutable std::mutex gate_;
  uint64_t lastControlId_ = 0;    // 保留最近代次，异步回切只能撤销对应的那次进入
  uint64_t activeControlId_ = 0;
  bool entryReady_ = false;
  std::atomic<double> pointerScale_{1.0};
  bool wheelReverse_ = false;
  int wheelScalePercent_ = 100;
};

}  // namespace bm
