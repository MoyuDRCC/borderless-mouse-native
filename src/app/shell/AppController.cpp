#include "app/shell/AppController.h"

#include <windows.h>
#include <wtsapi32.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cstdlib>
#include <thread>

#include "app/shell/AutoStart.h"
#include "app/shell/ClipboardRuntime.h"
#include "app/shell/Diagnostics.h"
#include "app/shell/LayoutRuntime.h"
#include "app/shell/MainWindow.h"
#include "app/shell/Tray.h"
#include "app/ui/Window.h"

#include "core/clipboard/ClipboardTransfer.h"
#include "core/crypto/DeviceIdentity.h"
#include "core/clipboard/FileTransfer.h"
#include "core/input/Injector.h"
#include "core/input/InputService.h"
#include "core/net/NetworkSink.h"
#include "core/net/PeerHost.h"
#include "core/net/PeerSession.h"

namespace bm {

namespace {

std::string LocalAddress() {
  char host[256] = {};
  if (gethostname(host, sizeof(host)) != 0) return "127.0.0.1";
  addrinfo hints{};
  hints.ai_family = AF_INET;
  addrinfo* result = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &result) != 0 || !result) return "127.0.0.1";
  char text[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr, text, sizeof(text));
  freeaddrinfo(result);
  return text;
}

// 会话与电源消息：锁屏、会话断开、休眠都要暂停共享
constexpr UINT kSessionChange = 0x02B1;   // WM_WTSSESSION_CHANGE
constexpr UINT kPowerBroadcast = 0x0218;  // WM_POWERBROADCAST

// 未认证候选的标识用地址端口，已认证设备用公钥摘要
std::string CandidateKey(const std::string& address, int port) { return address + ":" + std::to_string(port); }

std::string LevelText(LogLevel level) {
  switch (level) {
    case LogLevel::Warning: return "警告";
    case LogLevel::Error: return "错误";
    default: return "信息";
  }
}

}  // namespace

struct AppController::SessionEntry {
  PeerSessionPtr session;
  std::shared_ptr<NetworkSink> sink;
  std::unique_ptr<ClipLink> link;
  std::unique_ptr<ClipboardTransfer> clipboard;
  int dataId = 0;
};

AppController::AppController(Window& window) : window_(window) {
  layout_ = std::make_unique<LayoutRuntime>(config_, LayoutRuntime::Callbacks{
      [this](const std::string& text, LogLevel level) { Report(text, level); },
      [this](const std::string& peerId) { return SessionSink(peerId); }});
  clipboard_ = std::make_unique<ClipboardRuntime>(config_, window_, ClipboardRuntime::Callbacks{
      [this](const std::string& text, LogLevel level) { Report(text, level); }, [this] { SaveConfig(); },
      [this](std::shared_ptr<const ClipboardSnapshot> snapshot) { PublishClipboard(snapshot); }});
  BuildActions();
}

AppController::~AppController() = default;

void AppController::SetUi(MainWindow* mainWindow, Tray* tray) {
  mainWindow_ = mainWindow;
  tray_ = tray;
  layout_->SetMergePrompt([this](const MergeCandidate& candidate) { mainWindow_->ShowMerge(candidate); });
}

void AppController::Report(const std::string& text, LogLevel level) { LogText(level, text); }

void AppController::SaveConfig() {
  if (!configLoaded_) return;
  try {
    config_.Save();
  } catch (const std::exception& error) {
    Report(std::string("配置保存失败：") + error.what(), LogLevel::Error);
  }
}

void AppController::Start() {
  InitializeNetworking();
  config_ = AppConfig::Load();
  configLoaded_ = true;
  PublishTrust();
  PeerSession::SetAppVersion("1.1.0");
  model_.version = PeerSession::AppVersion();
  model_.protocolVersion = kProtocolVersion;
  model_.localName = config_.deviceName;
  model_.port = config_.port;

  identity_ = DeviceIdentity::LoadOrCreate(config_.protectedIdentityKey.empty() ? nullptr : &config_.protectedIdentityKey);
  layout_->SetIdentity(identity_.get());
  if (config_.protectedIdentityKey != identity_->ProtectedPrivateKey()) {
    config_.protectedIdentityKey = identity_->ProtectedPrivateKey();
    SaveConfig();
  }

  geometry_ = DesktopGeometry::Capture(&desktopOrigin_);
  if (!config_.hasTopology) {
    layout_->EnsureLocalDevice(config_.topology);
    config_.hasTopology = true;
    SaveConfig();
  }
  sharingEnabled_ = true;

  injector_ = std::make_unique<Injector>();
  injector_->injectionFailed = [this](int code) {
    Report("注入被系统拒绝（错误码 " + std::to_string(code) + "），已撤销远端控制", LogLevel::Warning);
    if (host_) host_->NotifyInjectionFailed(code);
  };
  injector_->injectionRecovered = [this] {
    if (host_) host_->NotifyInjectionRecovered();
    Report("注入已恢复");
  };
  injector_->controlReleased = [this] {
    if (host_) host_->NotifyControlReleased();
  };
  injector_->controlOverloaded = [this] {
    if (host_) host_->NotifyControlOverloaded();
    Report("待注入队列过载，已撤销远端控制", LogLevel::Warning);
  };

  input_ = std::make_unique<InputService>(config_.deviceName);
  layout_->SetInputService(input_.get());
  input_->hideCursorWhileRemote = config_.hideCursorWhileRemote;
  input_->localInputDetected = [this] {
    if (host_) host_->NotifyLocalInput();
  };
  input_->desktopGeometryChanged = [this](const DesktopGeometry& geometry) {
    PostUi([this, geometry] {
      geometry_ = geometry;
      host_->UpdateLocalGeometry(geometry);
      layout_->ApplyTopologyToInput();
    });
  };
  input_->faulted = [this](const std::string& message) {
    Report("输入共享已停止，请重启应用后恢复：" + message, LogLevel::Error);
    host_->SetSharingEnabled(false);
    PostUi([this] { sharingEnabled_ = false; });
  };

  // 锁屏/断开/休眠时暂停共享；消息由主窗口代收
  WTSRegisterSessionNotification(window_.Handle(), 0);
  window_.AddMessageHook([this](UINT message, WPARAM wParam, LPARAM) {
    OnSessionMessage(message, wParam);
    return false;
  });

  clipboard_->Start();
  StartHost();
  layout_->ApplyTopologyToInput();

  // 已配对设备启动即静默握手一次，失败再按退避重试，不必先等一个退避周期
  for (const PairedPeer& peer : config_.pairedPeers) AttemptConnect(peer);
  Report("已启动：监听端口 " + std::to_string(config_.port));
}

void AppController::StartHost() {
  InjectionPort port;
  port.available = [this] { return injector_ && injector_->IsAvailable(); };
  port.begin = [this] { return injector_ && injector_->TryBeginControl(); };
  port.releaseAll = [this] {
    if (injector_) injector_->ReleaseAll();
  };
  port.submit = [this](const NetInput& input) {
    if (!injector_) return;
    switch (input.kind) {
      case NetInputKind::Position: injector_->MoveDesktopOffset(input.a, input.b); break;
      case NetInputKind::Button: injector_->Button(static_cast<MouseButton>(input.a), input.flag); break;
      case NetInputKind::Wheel: injector_->Wheel(input.a, input.flag); break;
      case NetInputKind::Key: injector_->Key(input.a, input.scanCode, input.extended, input.flag); break;
    }
  };

  PeerHost::Options options;
  options.deviceName = config_.deviceName;
  options.port = config_.port;
  options.localGeometry = geometry_;
  options.identity = identity_.get();
  host_ = std::make_unique<PeerHost>(std::move(options), std::move(port));
  layout_->SetPeerHost(host_.get());

  host_->SetTrust(
      [this](const std::string& peerId, const std::string& publicKey) {
        const auto trusted = trustedPeers_.load();
        for (const PairedPeer& peer : *trusted)
          if (peer.id == peerId && peer.publicKey == publicKey) return true;
        return false;
      },
      [this](const PairingRequest& request, const std::atomic<bool>& aborted) {
        return mainWindow_ ? mainWindow_->PromptPairing(request, aborted) : false;
      });

  PeerHost::Callbacks callbacks;
  callbacks.peerJoined = [this](PeerSession& session) {
    PostUi([this, peer = session.shared_from_this()] { OnPeerJoined(peer); });
  };
  callbacks.sessionClosed = [this](PeerSession& session) {
    PostUi([this, peer = session.shared_from_this()] { OnSessionClosed(*peer); });
  };
  callbacks.yieldRequested = [this](PeerSession& session, uint64_t controlId) {
    std::shared_ptr<NetworkSink> sink;
    {
      std::lock_guard lock(sessionsMutex_);
      for (const auto& entry : sessions_)
        if (entry->session.get() == &session) sink = entry->sink;
    }
    if (sink && sink->HandleYield(controlId)) input_->ReturnHome(sink, controlId);
  };
  callbacks.inputBlocked = [this](PeerSession& session, int code) {
    // 回切由带租约代次的 YieldControl 完成，InputBlocked 只补充具体故障。
    Report(session.PeerName() + " 无法注入输入（原因码 " + std::to_string(code) + "）", LogLevel::Warning);
  };
  callbacks.injectionFailed = [this](int code) {
    Report("本机注入故障，已暂停共享（错误码 " + std::to_string(code) + "）", LogLevel::Error);
  };
  callbacks.injectionRecovered = [this] { Report("本机注入已恢复"); };
  callbacks.controlEnded = [this] {
    Report("对端结束了对本机的控制");
    // 被放开后光标还停在屏幕边缘：藏起本地光标，本机一动鼠标就恢复
    if (input_) input_->HideLocalCursor();
  };
  callbacks.listeningFailed = [](const std::string&) {};  // 主机已记录同一条失败，避免重复刷屏
  callbacks.handshakeOverloaded = [this](const std::string& message) { Report(message, LogLevel::Warning); };
  host_->SetCallbacks(std::move(callbacks));

  // 两个工作线程启动前主机已经就绪，回调可安全访问它。
  injector_->Start();
  input_->Start();
  host_->SetSharingEnabled(sharingEnabled_);
  input_->SetEnabled(sharingEnabled_);
  host_->Start();
}

void AppController::OnPeerJoined(const PeerSessionPtr& session, const Endpoint* endpoint) {
  if (session->IsClosed()) return;
  try {
    RememberPeer(*session, endpoint);
    auto entry = std::make_unique<SessionEntry>();
    entry->session = session;
    entry->sink = std::make_shared<NetworkSink>(session);
    entry->link = clipboard_->CreateLink(session);
    entry->clipboard = clipboard_->CreateTransfer(*entry->link, session);
    ApplyDeviceSettings(*entry);
    auto* transfer = entry->clipboard.get();
    // 会话收发收尾以后才移除条目，数据回调始终绑定到原会话的传输对象。
    entry->dataId = session->SubscribeData(
        [this, transfer](MessageType type, std::span<const uint8_t> payload) {
          if (type == MessageType::Topology) {
            layout_->ReceiveTopology(payload);
          } else {
            transfer->OnFrame(type, payload);
          }
        });
    {
      std::lock_guard lock(sessionsMutex_);
      sessions_.push_back(std::move(entry));
    }

    host_->ActivateSession(*session);
    session->Start();
    clipboard_->ApplyCurrent(*transfer);
    layout_->AutoPlacePeer(session->PeerId(), session->PeerName());
    layout_->ApplyTopologyToInput();
    Report("已连接 " + session->PeerName() + "（" + session->PeerAddress() + "）");
  } catch (const std::exception& error) {
    session->Close();
    Report(std::string("无法启用会话：") + error.what(), LogLevel::Warning);
  }
}

void AppController::PublishTrust() {
  trustedPeers_.store(std::make_shared<const std::vector<PairedPeer>>(config_.pairedPeers));
}

// 配对完成的唯一落点：认证通过后固定公钥并记住最后地址，重连免弹窗、设备行显示已配对
void AppController::RememberPeer(const PeerSession& session, const Endpoint* endpoint) {
  PairedPeer* peer = nullptr;
  for (PairedPeer& candidate : config_.pairedPeers)
    if (candidate.id == session.PeerId()) peer = &candidate;
  const bool added = peer == nullptr;
  if (added) {
    config_.pairedPeers.push_back(PairedPeer{});
    peer = &config_.pairedPeers.back();
    peer->id = session.PeerId();
    peer->publicKey = session.PeerPublicKey();
  }
  peer->name = session.PeerName();
  peer->address = endpoint ? endpoint->address : session.PeerAddress();
  peer->port = endpoint ? endpoint->port : session.PeerPort();
  // 认到身份前设置按 地址:端口 保存，认证后要迁到固定标识上，别丢别名与滚轮设置
  const std::string candidate = CandidateKey(session.PeerAddress(), session.PeerPort());
  if (candidate != peer->id) {
    bool occupied = false;
    for (const DeviceSetting& setting : config_.deviceSettings)
      if (setting.id == peer->id) occupied = true;
    for (DeviceSetting& setting : config_.deviceSettings)
      if (setting.id == candidate && !occupied) setting.id = peer->id;
  }
  if (added) PublishTrust();
  SaveConfig();
  if (added) Report("已与 " + session.PeerName() + " 完成配对");
}

void AppController::OnSessionClosed(PeerSession& session) {
  std::unique_ptr<SessionEntry> removed;
  {
    std::lock_guard lock(sessionsMutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
      if ((*it)->session.get() == &session) {
        removed = std::move(*it);
        sessions_.erase(it);
        break;
      }
    }
  }
  if (removed) {
    input_->ReturnHome(removed->sink);
    if (removed->dataId) removed->session->UnsubscribeData(removed->dataId);
    removed->clipboard->Dispose();
    layout_->ApplyTopologyToInput();
    Report("已断开 " + session.PeerName());
  }
  // 已配对设备掉线后继续按退避重连；用户主动断开的不再打扰
  for (const PairedPeer& peer : config_.pairedPeers) {
    if (peer.id != session.PeerId()) continue;
    ScheduleReconnect(peer);
    break;
  }
}

AppController::SessionEntry* AppController::FindEntry(const std::string& peerId) {
  std::lock_guard lock(sessionsMutex_);
  for (auto& entry : sessions_)
    if (entry->session->PeerId() == peerId && !entry->session->IsClosed()) return entry.get();
  return nullptr;
}

std::shared_ptr<IInputSink> AppController::SessionSink(const std::string& peerId) {
  SessionEntry* entry = FindEntry(peerId);
  return entry ? entry->sink : nullptr;
}

void AppController::PublishClipboard(const std::shared_ptr<const ClipboardSnapshot>& snapshot) {
  std::lock_guard lock(sessionsMutex_);
  for (auto& entry : sessions_) entry->clipboard->SetCurrent(snapshot);
}

std::string AppController::DisplayName(const std::string& peerId) const { return layout_->DisplayName(peerId); }

void AppController::ApplyDeviceSettings(SessionEntry& entry) {
  const std::string peerId = entry.session->PeerId();
  DeviceSetting setting;
  for (const DeviceSetting& candidate : config_.deviceSettings)
    if (candidate.id == peerId) setting = candidate;

  const double scale = (setting.pointerDpi > 0 && config_.mouseDpi > 0)
                           ? static_cast<double>(setting.pointerDpi) / config_.mouseDpi
                           : 1.0;
  entry.sink->SetPointerScale(scale);
  entry.sink->SetWheelReverse(setting.wheelReverse);
  entry.sink->SetWheelScalePercent(setting.wheelScalePercent);
  entry.clipboard->SetEnabled(clipboard_->Enabled() && setting.clipboard);
}

void AppController::BuildActions() {
  // 应用级开关与生命周期
  actions_.app.setSharing = [this](bool enabled) { SetSharing(enabled); };
  actions_.app.setClipboardSync = [this](bool enabled) { SetClipboardEnabled(enabled); };
  actions_.app.setHideCursor = [this](bool enabled) {
    config_.hideCursorWhileRemote = enabled;
    if (input_) input_->SetHideCursor(enabled);
    SaveConfig();
  };
  actions_.app.setAutoStart = [this](bool enabled) { SetAutoStart(enabled); };
  actions_.app.setMouseDpi = [this](int dpi) {
    config_.mouseDpi = std::max(1, dpi);
    SaveConfig();
    std::lock_guard lock(sessionsMutex_);
    for (auto& entry : sessions_) ApplyDeviceSettings(*entry);
  };
  actions_.app.setClipboardLimit = [this](int megabytes) {
    const int64_t bytes = clipboard_->SetLimitMb(megabytes);
    std::lock_guard lock(sessionsMutex_);
    for (auto& entry : sessions_) entry->clipboard->SetMaxContentBytes(bytes);
  };
  actions_.app.exportDiagnostics = [this] { ExportDiagnostics(model_, config_); };
  actions_.app.clearLogs = [] { LogClear(); };
  actions_.app.quit = [this] { window_.Close(); };

  // 连接与设备
  actions_.connection.addDevice = [this](std::string name, std::string address, int port) {
    if (address.empty() || port <= 0) return;
    for (ManualPeer& peer : config_.manualPeers) {
      if (peer.address == address && peer.port == port) {
        peer.name = std::move(name);
        SaveConfig();
        ConnectAsync(address, port, {});
        return;
      }
    }
    config_.manualPeers.push_back(ManualPeer{std::move(name), address, port});
    SaveConfig();
    ConnectAsync(address, port, {});
  };
  actions_.connection.connect = [this](std::string id) {
    for (const PairedPeer& peer : config_.pairedPeers) {
      if (peer.id == id) {
        std::erase(manualStop_, id);
        ConnectAsync(peer.address, peer.port, peer.id);
        return;
      }
    }
    for (const ManualPeer& peer : config_.manualPeers) {
      if (CandidateKey(peer.address, peer.port) == id) {
        ConnectAsync(peer.address, peer.port, {});
        return;
      }
    }
    // 自动发现的候选：id 就是"地址:端口"
    const size_t colon = id.rfind(':');
    if (colon != std::string::npos) ConnectAsync(id.substr(0, colon), std::atoi(id.c_str() + colon + 1), {});
  };
  actions_.connection.disconnect = [this](std::string id) {
    if (!host_) return;
    MarkManualStop(id);
    CancelConnection(id);
    host_->CancelReconnect(id);
    host_->DisconnectPeer(id);
  };
  actions_.connection.startControl = [this](std::string id) {
    SessionEntry* entry = FindEntry(id);
    if (!entry || !input_) return;
    ApplyDeviceSettings(*entry);
    input_->ControlDevice(DisplayName(id), entry->sink, PointerMode::SystemMouse);
  };
  actions_.connection.startMirror = [this](std::string id) {
    SessionEntry* entry = FindEntry(id);
    if (!entry || !input_) return;
    ApplyDeviceSettings(*entry);
    input_->ControlDevice(DisplayName(id), entry->sink, PointerMode::AbsoluteDevice);
  };
  actions_.connection.unpair = [this](std::string id) {
    if (!host_) return;
    MarkManualStop(id);
    CancelConnection(id);
    host_->CancelReconnect(id);
    config_.pairedPeers.erase(std::remove_if(config_.pairedPeers.begin(), config_.pairedPeers.end(),
                                             [&id](const PairedPeer& peer) { return peer.id == id; }),
                              config_.pairedPeers.end());
    PublishTrust();
    host_->DisconnectPeer(id);
    config_.deviceSettings.erase(std::remove_if(config_.deviceSettings.begin(), config_.deviceSettings.end(),
                                                [&id](const DeviceSetting& setting) { return setting.id == id; }),
                                 config_.deviceSettings.end());
    layout_->RemoveDevice(id);
    Report("已撤销配对：" + DisplayName(id));
  };
  actions_.connection.removeDevice = [this](std::string id) {
    config_.manualPeers.erase(std::remove_if(config_.manualPeers.begin(), config_.manualPeers.end(),
                                             [&id](const ManualPeer& peer) { return CandidateKey(peer.address, peer.port) == id; }),
                              config_.manualPeers.end());
    SaveConfig();
  };
  actions_.connection.saveDeviceSettings = [this](std::string id, DeviceRow updated) {
    DeviceSetting setting;
    for (const DeviceSetting& candidate : config_.deviceSettings)
      if (candidate.id == id) setting = candidate;
    setting.id = id;
    setting.alias = updated.alias;
    setting.wheelReverse = updated.wheelReverse;
    setting.wheelScalePercent = updated.wheelScalePercent;
    setting.pointerDpi = updated.pointerDpi;
    setting.clipboard = updated.clipboardEnabled;
    bool replaced = false;
    for (DeviceSetting& candidate : config_.deviceSettings) {
      if (candidate.id != id) continue;
      candidate = setting;
      replaced = true;
    }
    if (!replaced) config_.deviceSettings.push_back(setting);

    // 别名同步进布局，界面上换名字后拓扑也对得上
    for (PairedPeer& peer : config_.pairedPeers)
      if (peer.id == id && !updated.alias.empty()) peer.name = updated.alias;
    SaveConfig();
    if (SessionEntry* entry = FindEntry(id)) ApplyDeviceSettings(*entry);

    layout_->RenameDevice(id);
  };

  // 布局
  actions_.layout.moveLayoutCell = [this](int fromRow, int fromColumn, int toRow, int toColumn) {
    layout_->MoveCell(fromRow, fromColumn, toRow, toColumn);
  };
  actions_.layout.placeDevice = [this](std::string id, int row, int column) {
    layout_->PlaceDevice(id, row, column);
  };
  actions_.layout.poolDevice = [this](std::string id, bool toPool) { layout_->PoolDevice(id, toPool); };
  actions_.layout.undoLayout = [this] { layout_->UndoLayout(); };
  actions_.layout.resolveMerge = [this](uint64_t revision, bool accepted) {
    // 先收取已到达的网络更新，再核对用户看到的候选。
    layout_->ApplyReceivedTopology();
    layout_->ResolveMerge(revision, accepted);
  };
  actions_.layout.report = [this](std::string message) { Report(message, LogLevel::Warning); };
}

void AppController::SetSharing(bool enabled) {
  sharingEnabled_ = enabled;
  if (!enabled) sharingDisabledByUser_ = true;
  if (host_) host_->SetSharingEnabled(enabled);
  if (input_) input_->SetEnabled(enabled);
  Report(enabled ? "键鼠共享已开启" : "键鼠共享已关闭");
}

void AppController::OnSessionMessage(UINT message, WPARAM wParam) {
  if (message == kSessionChange) {
    switch (wParam) {
      case WTS_SESSION_LOCK: sessionLocked_ = true; break;
      case WTS_SESSION_UNLOCK: sessionLocked_ = false; break;
      case WTS_CONSOLE_DISCONNECT:
      case WTS_REMOTE_DISCONNECT: sessionDisconnected_ = true; break;
      case WTS_CONSOLE_CONNECT:
      case WTS_REMOTE_CONNECT: sessionDisconnected_ = false; break;
      case WTS_SESSION_LOGOFF: sessionLoggedOff_ = true; break;
      default: return;
    }
    UpdateSuspension();
    return;
  }
  if (message == kPowerBroadcast) {
    if (wParam == PBT_APMSUSPEND) {
      powerSuspended_ = true;
    } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
      powerSuspended_ = false;
    } else {
      return;
    }
    UpdateSuspension();
  }
}

void AppController::UpdateSuspension() {
  const bool suspended = sessionLocked_ || sessionDisconnected_ || sessionLoggedOff_ || powerSuspended_;
  if (suspended == suspended_) return;
  suspended_ = suspended;
  if (input_) input_->SetSuspended(suspended);
  if (!host_) return;
  if (suspended) {
    host_->SetSharingEnabled(false);
    Report("已暂停共享：屏幕锁定、会话断开或系统休眠");
    return;
  }
  host_->SetSharingEnabled(sharingEnabled_);
  if (sharingEnabled_) Report("共享已恢复");
}

void AppController::SetClipboardEnabled(bool enabled) {
  clipboard_->SetEnabled(enabled);
  std::lock_guard lock(sessionsMutex_);
  for (auto& entry : sessions_) ApplyDeviceSettings(*entry);
}

void AppController::Tick() {
  if (shutdown_) return;
  DrainUiJobs();
  layout_->Tick();
  clipboard_->ApplyPending();
  ConnectDiscovered();
  RefreshModel();
  if (tray_) tray_->SetStatus(ToWide(model_.statusText));
}

void AppController::PostUi(std::function<void()> job) {
  std::lock_guard lock(uiJobsMutex_);
  if (!shutdown_) uiJobs_.push_back(std::move(job));
}

void AppController::DrainUiJobs() {
  std::vector<std::function<void()>> jobs;
  {
    std::lock_guard lock(uiJobsMutex_);
    jobs.swap(uiJobs_);
  }
  for (const auto& job : jobs) job();
}

std::string AppController::StatusText() {
  if (suspended_) return "已暂停（锁屏或休眠）";
  if (!sharingEnabled_) return "共享已关闭";
  if (input_ && input_->IsRemote()) return "正在控制 " + input_->CurrentDevice();
  if (host_) {
    const std::string controlledBy = host_->ControlledBy();
    if (!controlledBy.empty()) return "正在被 " + controlledBy + " 控制";
    const size_t count = host_->SessionCount();
    if (count > 0) return "已连接 " + std::to_string(count) + " 台";
  }
  // 还没有会话但在连接中：状态条给出正在连接的目标，而不是一句“未连接”
  const std::string connecting = ConnectingName();
  if (!connecting.empty()) return "正在连接 " + connecting;
  return "未连接";
}

void AppController::RefreshDevices() {
  std::vector<DeviceRow> rows;
  DeviceRow local;
  local.id = identity_->Id();
  local.name = config_.deviceName;
  local.address = model_.localAddress;
  local.port = config_.port;
  local.local = true;
  local.paired = true;
  local.connected = true;
  rows.push_back(local);

  const std::vector<PeerSessionPtr> sessions = host_ ? host_->Sessions() : std::vector<PeerSessionPtr>{};
  const std::string controlling = (input_ && input_->IsRemote()) ? input_->CurrentDevice() : std::string();
  const std::string controlledBy = host_ ? host_->ControlledBy() : std::string();

  auto fill_session = [&](DeviceRow& row) {
    for (const PeerSessionPtr& session : sessions) {
      if (session->PeerId() != row.id) continue;
      row.connected = true;
      row.version = session->PeerVersion();
      row.address = session->PeerAddress();
      row.port = session->PeerPort();
      if (!session->PeerName().empty() && row.alias.empty()) row.name = session->PeerName();
      break;
    }
    row.controlling = !controlling.empty() && controlling == row.name;
    row.controlled = !controlledBy.empty() && controlledBy == row.name;
  };

  for (const PairedPeer& peer : config_.pairedPeers) {
    DeviceRow row;
    row.id = peer.id;
    row.name = peer.name;
    row.address = peer.address;
    row.port = peer.port;
    // 已配对设备不是广播候选：只有手动地址与局域网发现才标记为 discovered
    row.paired = true;
    fill_session(row);
    for (const DeviceSetting& setting : config_.deviceSettings) {
      if (setting.id != peer.id) continue;
      row.alias = setting.alias;
      if (!setting.alias.empty()) row.name = setting.alias;
      row.pointerDpi = setting.pointerDpi;
      row.wheelScalePercent = setting.wheelScalePercent;
      row.wheelReverse = setting.wheelReverse;
      row.clipboardEnabled = setting.clipboard;
    }
    rows.push_back(std::move(row));
  }

  for (const ManualPeer& peer : config_.manualPeers) {
    const std::string key = CandidateKey(peer.address, peer.port);
    bool listed = false;
    for (const DeviceRow& row : rows)
      if (row.paired && row.address == peer.address && row.port == peer.port) listed = true;
    if (listed) continue;
    DeviceRow row;
    row.id = key;
    row.name = peer.name.empty() ? peer.address : peer.name;
    row.address = peer.address;
    row.port = peer.port;
    rows.push_back(std::move(row));
  }

  if (host_) {
    for (const DiscoveredPeer& peer : host_->Peers()) {
      if (!peer.deviceId.empty()) {
        bool known = false;
        for (const DeviceRow& row : rows)
          if (row.paired && row.id == peer.deviceId) known = true;
        if (known) continue;
      }
      const std::string key = CandidateKey(peer.address, peer.port);
      bool listed = false;
      for (const DeviceRow& row : rows)
        if (row.address == peer.address && row.port == peer.port) listed = true;
      if (listed) continue;
      DeviceRow row;
      row.id = key;
      row.name = peer.name;
      row.address = peer.address;
      row.port = peer.port;
      row.discovered = true;
      rows.push_back(std::move(row));
    }
  }

  // 连接中只补在还没连上的行上：会话一建立就由连接状态接管
  for (DeviceRow& row : rows)
    if (!row.local && !row.connected) row.connecting = IsConnecting(row.id, row.address, row.port);

  model_.devices = std::move(rows);
}

void AppController::RefreshModel() {
  model_.localId = identity_ ? identity_->Id() : std::string();
  model_.localName = config_.deviceName;
  model_.localAddress = LocalAddress();
  model_.port = config_.port;
  model_.version = PeerSession::AppVersion();
  model_.protocolVersion = kProtocolVersion;
  RefreshDevices();

  model_.logs = LogSnapshot(500);
  model_.events.clear();
  for (size_t i = model_.logs.size() > 5 ? model_.logs.size() - 5 : 0; i < model_.logs.size(); i++)
    model_.events.push_back(model_.logs[i]);

  layout_->FillModel(model_);

  model_.settings.sharingEnabled = sharingEnabled_ && !suspended_;
  model_.settings.clipboardEnabled = clipboard_->Enabled();
  model_.settings.hideCursorWhileRemote = config_.hideCursorWhileRemote;
  model_.settings.autoStart = IsAutoStartEnabled();
  model_.settings.mouseDpi = config_.mouseDpi;
  model_.settings.clipboardLimitMb = config_.clipboardLimitMb;

  UiStats stats;
  if (host_) {
    for (const PeerSessionPtr& session : host_->Sessions()) {
      stats.roundTripMs = std::max(stats.roundTripMs, session->RttMs());
      stats.pendingFrames += static_cast<int>(session->PendingFrames());
    }
  }
  if (injector_) {
    const auto [last, worst] = injector_->Timing();
    stats.lastInjectMs = static_cast<int>(last);
    stats.worstInjectMs = static_cast<int>(worst);
  }
  model_.stats = stats;
  model_.statusText = StatusText();
}

void AppController::OnCloseRequested() {
  window_.Hide();
  Report("窗口已隐藏到托盘，可从托盘菜单退出");
}
void AppController::Shutdown() {
  if (shutdown_.exchange(true)) return;
  WTSUnRegisterSessionNotification(window_.Handle());
  if (input_) input_->SetEnabled(false);
  // 中断握手后等待连接任务，确保接下来只剩界面线程负责清理。
  for (ConnectionTask& task : connections_) task.canceled.store(true);
  if (host_) host_->Stop();
  connections_.clear();
  input_.reset();
  clipboard_->Stop();

  decltype(sessions_) sessions;
  {
    std::lock_guard lock(sessionsMutex_);
    sessions.swap(sessions_);
  }
  for (auto& entry : sessions) {
    if (entry->dataId) entry->session->UnsubscribeData(entry->dataId);
    entry->clipboard->Dispose();
  }
  {
    std::lock_guard lock(uiJobsMutex_);
    uiJobs_.clear();
  }
  if (injector_) {
    injector_->Stop();
    injector_->WaitCompletion();
  }
  try {
    FileTransfer::Cleanup();
  } catch (const std::exception& error) {
    Report(std::string("接收目录清理失败：") + error.what(), LogLevel::Warning);
  }
  SaveConfig();
  Report("已退出");
}

}  // namespace bm
