#pragma once

#include "core/platform/Common.h"
#include "core/input/DesktopGeometry.h"
#include "core/input/InputEvent.h"
#include "core/input/InputTopology.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <unordered_set>
#include <vector>

namespace bm {

class InputHookHost;
class CursorHider;

// 对应 Input/InputService.cs、InputService.Routing.cs、InputService.Cursor.cs：
// 钩子线程独占路由、物理按键与目标状态；公开命令只向该线程投递消息。
class InputService {
 public:
  explicit InputService(std::string localDeviceName);
  ~InputService();
  InputService(const InputService&) = delete;
  InputService& operator=(const InputService&) = delete;

  // 以下回调都在钩子线程上同步执行，不要长时间阻塞
  std::function<void(bool)> remoteChanged;                  // 是否正在控制远端设备
  std::function<void()> localInputDetected;                 // 本机真实输入；被控端据此撤销他人控制
  std::function<void(const DesktopGeometry&)> desktopGeometryChanged;
  std::function<void(const std::string&)> faulted;           // 输入管线故障：实例不可再用

  // 远控期间隐藏停在显示器中央的本机光标；Start 之前设置决定初始状态
  bool hideCursorWhileRemote = true;

  void Start();
  bool IsRemote() const;
  // 光标所在设备的标识：在远端时是远端设备，回到本机后是本机
  std::string CurrentDevice() const;

  // 被别人控制结束后把残留在边缘的本机光标藏起来；本机一动鼠标就恢复显示
  void HideLocalCursor();
  // 运行时切换隐藏光标；创建与销毁都在钩子线程上做
  void SetHideCursor(bool enabled);

  // 显式选择已连接设备，不要求它位于相邻格子；按键和按钮全部释放后才进入
  void ControlDevice(std::string deviceName, std::shared_ptr<IInputSink> sink, PointerMode mode = PointerMode::SystemMouse);
  void SetEnabled(bool enabled);
  void Stop() { SetEnabled(false); }
  // 系统暂停独立于用户开关：锁屏、会话断开、休眠由 app 层合并成一个标志
  void SetSuspended(bool suspended);
  // 拓扑必须用本实例的本机标识构造
  void PublishTopology(std::shared_ptr<const InputTopology> topology);
  // expected 限定会话实例，旧会话的关闭或让出不会打断另一个当前目标
  void ReturnHome(std::shared_ptr<IInputSink> expected);
  // 远端撤销按代次执行；命令排队期间重新进入同一出口不会被旧命令打断
  void ReturnHome(std::shared_ptr<IInputSink> expected, uint64_t controlId);
  void RefreshDesktopGeometry();
  // 立即关闭输出；若仍持有远端按键，钩子保留到对应物理 Up 之后
  void Dispose();

 private:
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
  using KeySet = std::unordered_set<KeyStroke, KeyStrokeHash>;
  using ButtonSet = std::unordered_set<MouseButton, ButtonHash>;

  // 对应 Input/DeviceSwitchGate.cs：所有目标共用的切换节流与入口行程滞回，仅由钩子线程使用
  class SwitchGate {
   public:
    bool CanSwitch(bool selectedDirectly = false) const {
      return TickMs() >= nextSwitch_ && (selectedDirectly || armed_);
    }
    void Enter(double x, double y, int width, int height) {
      nextSwitch_ = TickMs() + kSwitchIntervalMs;
      entryX_ = x;
      entryY_ = y;
      // 保持真实入口像素，只要求短距离有效行程；小尺寸设备按可用跨度缩小门槛
      travelX_ = width > 1 ? std::min(6.0, (width - 1) / 2.0) : kNoTravel;
      travelY_ = height > 1 ? std::min(6.0, (height - 1) / 2.0) : kNoTravel;
      armed_ = width <= 1 && height <= 1;
    }
    void Observe(double x, double y) {
      if (std::abs(x - entryX_) >= travelX_ || std::abs(y - entryY_) >= travelY_) armed_ = true;
    }

   private:
    static constexpr uint64_t kSwitchIntervalMs = 150;
    static constexpr double kNoTravel = std::numeric_limits<double>::infinity();
    uint64_t nextSwitch_ = 0;
    double entryX_ = 0;
    double entryY_ = 0;
    double travelX_ = 0;
    double travelY_ = 0;
    bool armed_ = true;
  };

  void Post(std::function<void()> action);
  void Initialize();
  void Cleanup();
  void Fail(std::string message);
  void ReportFault(const std::string& message);
  void TrackPhysical(const InputEvent& input);
  void MaintainState(bool inputDesktopAvailable);
  void ReconcilePhysical(const std::array<bool, 256>& down);
  void ObserveRawRelease(bool keyboard, int virtualKey, int scanCode, bool extended, MouseButton button);
  void FinishDisposalWhenReleased();
  void RefreshGeometry();

  // 源光标与隐藏光标（InputService.Cursor）
  void CreateCursorHider();
  void RestoreSourceCursor();
  void PositionSourceCursor(Point2i point);
  Point2i SourceParkingPoint() const;
  Point2i ClampToDesktop(Point2i point) const;

  // 路由（InputService.Routing）
  bool Route(const InputEvent& input);
  bool RouteKey(const InputEvent& input);
  bool RouteButton(const InputEvent& input);
  bool RouteWheel(const InputEvent& input);
  bool RouteCursor(const InputEvent& input);
  bool RouteRelativeActivity(const InputEvent& input);
  void PrepareButtonCycle(const InputEvent& input);
  bool IsolatePendingButton(const InputEvent& input);
  bool HasModifier(int generic, int left, int right) const;
  bool CanCross() const;
  bool CanSwitch(bool selectedDirectly = false) const;
  void CheckLocalEdge(Point2i cursor, double deltaX, double deltaY);
  bool MoveRemote(double deltaX, double deltaY);
  static double TakeWhole(double value, double& carry);
  bool EnterRemote(const std::string& name, const std::shared_ptr<IInputSink>& sink, int side, double x, double y, Size2i from);
  bool ActivateTarget(const std::string& name, const std::shared_ptr<IInputSink>& sink, double x, double y, bool selectedDirectly,
                      PointerMode mode = PointerMode::SystemMouse);
  void ExitRemote(std::optional<Point2i> destination = std::nullopt);

  static int PhysicalKey(const KeyStroke& key);
  static RectI MonitorAt(const DesktopGeometry& geometry, Point2i offset);
  std::string CurrentDeviceName() const;
  void SetCurrentDevice(std::string name);

  std::string localDevice_;
  std::unique_ptr<InputHookHost> hook_;
  std::shared_ptr<const InputTopology> topology_;
  std::unique_ptr<CursorHider> cursorHider_;

  mutable std::mutex viewMutex_;
  std::string currentDevice_;
  std::atomic<std::shared_ptr<IInputSink>> target_;

  // 物理按键与归属集合：只在钩子线程上读写
  KeySet physicalKeys_;
  std::unordered_set<int> initialKeys_;
  ButtonSet physicalButtons_;
  KeySet localKeys_;
  ButtonSet localButtons_;
  KeySet remoteKeys_;
  ButtonSet remoteButtons_;
  KeySet suppressedKeys_;
  ButtonSet suppressedButtons_;
  KeySet releasedRemoteKeys_;
  KeySet releasedLocalKeys_;
  ButtonSet releasedRemoteButtons_;
  ButtonSet releasedLocalButtons_;
  KeySet rawReleasedKeys_;
  ButtonSet rawReleasedButtons_;

  RectI desktop_{};
  DesktopGeometry geometry_;
  Point2i lastCursor_{};
  bool targetSelectedDirectly_ = false;
  Point2i anchor_{};
  double remoteX_ = 0;
  double remoteY_ = 0;
  double scaleCarryX_ = 0;
  double scaleCarryY_ = 0;
  PointerMode pointerMode_ = PointerMode::SystemMouse;
  uint64_t localInputSince_ = 0;
  long sourcePositionRevision_ = 0;
  std::optional<Point2i> restorePoint_;
  SwitchGate switchGate_;
  bool inputDesktopAvailable_ = false;
  bool suspended_ = false;
  bool reconcilePending_ = true;
  bool enabled_ = true;
  bool faulted_ = false;
  bool disposing_ = false;
  bool started_ = false;
  std::atomic<bool> disposed_{false};
};

}  // namespace bm
