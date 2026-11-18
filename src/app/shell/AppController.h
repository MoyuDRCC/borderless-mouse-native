#pragma once

#include <windows.h>

#include "core/config/AppConfig.h"
#include "core/input/DesktopGeometry.h"
#include "core/protocol/FrameCodec.h"
#include "app/UiModel.h"

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bm {

class Window;
class MainWindow;
class Tray;
class PeerHost;
class PeerSession;
class InputService;
class Injector;
class DeviceIdentity;
class ClipboardRuntime;
class ClipboardTransfer;
class ClipboardSnapshot;
class IInputSink;
class LayoutRuntime;

// 应用控制器：把网络、输入、剪贴板与布局接到界面模型上。
// 界面只读 UiModel、只通过 UiActions 触发动作；控制器不持有任何控件。
class AppController {
 public:
  explicit AppController(Window& window);
  ~AppController();
  AppController(const AppController&) = delete;
  AppController& operator=(const AppController&) = delete;

  // 界面建好后再挂上来：配对与布局合并需要弹窗
  void SetUi(MainWindow* mainWindow, Tray* tray);

  // 载入配置与身份、启动注入/钩子/网络；失败抛异常
  void Start();
  // 500ms 定时：刷新模型、处理待确认布局、更新托盘
  void Tick();
  // 用户点关闭：隐藏到托盘
  void OnCloseRequested();
  // 真正退出：先停网络与注入，再收尾文件
  void Shutdown();

  UiModel& Model() { return model_; }
  UiActions& Actions() { return actions_; }

 private:
  struct SessionEntry;

  struct Endpoint {
    std::string address;
    int port = 0;
  };
  struct ConnectionTask {
    std::string key;
    std::atomic<bool> canceled{false};
    std::jthread worker;
  };

  void BuildActions();
  void StartHost();
  void OnPeerJoined(const std::shared_ptr<PeerSession>& session, const Endpoint* endpoint = nullptr);
  // 配对完成即落盘：身份、公钥、名字与地址，界面与重连都读这份记录
  void RememberPeer(const PeerSession& session, const Endpoint* endpoint);
  void PublishTrust();
  void OnSessionClosed(PeerSession& session);
  void RefreshModel();
  void RefreshDevices();
  void ApplyDeviceSettings(SessionEntry& entry);
  SessionEntry* FindEntry(const std::string& peerId);
  // 布局发布时按设备标识找会话的输入出口
  std::shared_ptr<IInputSink> SessionSink(const std::string& peerId);
  void PublishClipboard(const std::shared_ptr<const ClipboardSnapshot>& snapshot);
  std::string DisplayName(const std::string& peerId) const;
  std::string StatusText();
  // 连接中的设备：已配对按标识记，未配对按 地址:端口 记
  void StartConnection(std::string key, std::string peerId, std::string name, std::vector<Endpoint> endpoints,
                       bool automatic, bool keepManualAddress);
  void CancelConnection(const std::string& key);
  void PostUi(std::function<void()> job);
  void DrainUiJobs();
  bool IsConnecting(const std::string& id, const std::string& address, int port);
  std::string ConnectingName();
  void ConnectAsync(const std::string& address, int port, const std::string& expectedPeerId);
  void ScheduleReconnect(const PairedPeer& peer);
  // 排一次自动重连；initiatorOnly 为 false 时用于发现即连，不受单端发起约束
  void QueueReconnect(const PairedPeer& peer, bool initiatorOnly);
  // 单次连接尝试；失败后按主机退避重新排队，成功后不再重试
  void AttemptConnect(const PairedPeer& peer);
  // 发现即连：广播里出现已配对设备且当前没有会话时立刻静默握手
  void ConnectDiscovered();
  // 连接端点：先上次成功地址，再按重试次数轮转一个发现到的候选
  std::vector<Endpoint> ConnectionEndpoints(const PairedPeer& peer);
  bool ManuallyStopped(const std::string& peerId);
  void MarkManualStop(const std::string& peerId);
  void SaveConfig();
  void Report(const std::string& text, LogLevel level = LogLevel::Info);
  void SetSharing(bool enabled);
  void SetClipboardEnabled(bool enabled);
  // 锁屏、会话断开、休眠都会暂停共享，恢复时按用户原有开关还原
  void OnSessionMessage(UINT message, WPARAM wParam);
  void UpdateSuspension();

  Window& window_;
  MainWindow* mainWindow_ = nullptr;
  Tray* tray_ = nullptr;

  AppConfig config_;
  bool configLoaded_ = false;
  std::atomic<std::shared_ptr<const std::vector<PairedPeer>>> trustedPeers_;
  std::unique_ptr<LayoutRuntime> layout_;
  std::unique_ptr<DeviceIdentity> identity_;
  std::unique_ptr<Injector> injector_;
  std::unique_ptr<InputService> input_;
  std::unique_ptr<PeerHost> host_;
  std::unique_ptr<ClipboardRuntime> clipboard_;

  std::mutex sessionsMutex_;
  std::vector<std::unique_ptr<SessionEntry>> sessions_;

  std::vector<std::string> manualStop_;  // 用户主动断开的设备不再自动重连

  std::list<ConnectionTask> connections_;  // 任务由界面线程创建和回收。
  std::mutex uiJobsMutex_;
  std::vector<std::function<void()>> uiJobs_;

  DesktopGeometry geometry_;
  Point2i desktopOrigin_;
  bool sharingEnabled_ = true;
  bool sharingDisabledByUser_ = false;
  bool sessionLocked_ = false;
  bool sessionDisconnected_ = false;
  bool sessionLoggedOff_ = false;
  bool powerSuspended_ = false;
  bool suspended_ = false;

  UiModel model_;
  UiActions actions_;
  std::atomic<bool> shutdown_{false};
};

}  // namespace bm
