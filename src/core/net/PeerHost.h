#pragma once

#include "core/platform/Common.h"
#include "core/input/DesktopGeometry.h"
#include "core/net/DiscoveryService.h"
#include "core/protocol/FrameCodec.h"
#include "core/protocol/InputCodec.h"
#include "core/net/PeerSession.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace bm {

class DeviceIdentity;

// 注入出口：主机只决定谁能注入、何时撤销，真正的 SendInput 在输入模块
struct InjectionPort {
  std::function<bool()> available;              // 注入器此刻能否接收（队列未满且未故障）
  std::function<bool()> begin;                  // 申请进入控制；false 表示上一段尚未释放
  std::function<void()> releaseAll;             // 撤销本机全部注入
  std::function<void(const NetInput&)> submit;  // 提交一条已授权的远端输入
};

// 登记全部握手与会话：连接选举、输入租约、发现候选、自动重连都由主机统一管理
class PeerHost {
 public:
  struct Options {
    std::string deviceName;
    int port = 0;
    DesktopGeometry localGeometry;
    const DeviceIdentity* identity = nullptr;  // 生命周期由应用保证
  };

  struct Callbacks {
    std::function<void(PeerSession&)> peerJoined;            // 认证完成、已登记
    std::function<void(PeerSession&)> sessionClosed;
    std::function<void(PeerSession&, int)> pingUpdated;      // Ping/Pong 往返毫秒
    // 对端撤回了它给我们的控制租约，应用据此让对应出口回本机
    std::function<void(PeerSession&, uint64_t)> yieldRequested;
    // 对端报告暂时无法注入，参数是原因码
    std::function<void(PeerSession&, int)> inputBlocked;
    std::function<void(int)> injectionFailed;                // 本机注入故障原因码
    std::function<void()> injectionRecovered;
    std::function<void()> controlEnded;                      // 对端主动结束了对本机的控制
    std::function<void(const std::string&)> listeningFailed;
    std::function<void(const std::string&)> handshakeOverloaded;
  };

  PeerHost(Options options, InjectionPort injection);
  ~PeerHost();
  PeerHost(const PeerHost&) = delete;
  PeerHost& operator=(const PeerHost&) = delete;

  void SetCallbacks(Callbacks callbacks);
  // 已配对判定与配对确认必须在 Start 之前设置
  void SetTrust(TrustedPeerCheck isTrusted, PairingConfirm confirmPairing);

  // 开始监听与局域网发现；重复调用无副作用
  void Start();
  // 停止监听、发现与全部会话，等待线程收尾；不在调用线程做网络收发之外的等待以外的事情
  void Stop();
  bool IsListening() const;
  int ListeningPort() const { return options_.port; }
  const std::string& DeviceName() const { return options_.deviceName; }

  // 发起连接：认证与配对完成后返回已登记会话，失败抛异常
  PeerSessionPtr Connect(const std::string& host, int port, const std::string* expectedPeerId = nullptr,
                         const std::atomic<bool>* canceled = nullptr);
  // 主控先接好业务回调再激活统一接收，最后由应用调用 session.Start
  void ActivateSession(PeerSession& session);

  std::vector<PeerSessionPtr> Sessions() const;
  PeerSessionPtr FindSession(const std::string& peerId) const;
  void DisconnectPeer(const std::string& peerId);
  size_t SessionCount() const;
  // 布局快照等广播：只发给已登记会话
  void Broadcast(MessageType type, std::span<const uint8_t> payload);

  void UpdateLocalGeometry(const DesktopGeometry& geometry);
  void SetSharingEnabled(bool enabled);
  // 本机产生了真实输入：立即撤销远端控制
  void NotifyLocalInput();
  // 本机主动结束对某会话的控制（切回本机屏幕）
  void ReleaseControl(PeerSession& session);
  void SuspendInputForFault();
  void ResumeInputAfterRecovery();
  std::string ControlledBy() const;
  bool IsSharingEnabled() const;

  // 注入器回报：故障、恢复、上一段控制已释放、注入队列过载
  void NotifyInjectionFailed(int code);
  void NotifyInjectionRecovered();
  void NotifyControlReleased();
  void NotifyControlOverloaded();

  // 局域网发现
  DiscoveryService& Discovery() { return discovery_; }
  std::vector<DiscoveredPeer> Peers() { return discovery_.Peers(); }
  std::vector<DiscoveredPeer> Candidates(const std::string& deviceId) { return discovery_.Candidates(deviceId); }
  void RemoveCandidates(const std::string& deviceId, const std::string& address, int port) {
    discovery_.RemoveCandidates(deviceId, address, port);
  }

  // 已配对设备自动重连：1 秒起，每轮翻倍，最长 15 秒；attempt 在主机的重连线程上执行
  void ScheduleReconnect(const std::string& deviceId, std::function<void()> attempt);
  void CancelReconnect(const std::string& deviceId);
  // 该设备是否还有一趟重连在排队：发现即连靠它避免绕开退避定时器
  bool ReconnectPending(const std::string& deviceId) const;
  int RetryCount(const std::string& deviceId) const;
  void ResetRetryCount(const std::string& deviceId);

 private:
  // 会话登记：最后一帧控制代次用于拒绝重放的 ControlStart
  struct Registration {
    PeerSessionPtr session;
    bool active = false;
    uint64_t lastControlStart = 0;
  };

  // 同一设备的候选连接集合，只有一个能成为选定连接
  struct CandidateSelection {
    std::set<PeerSession*> members;
    PeerSession* winner = nullptr;
  };

  // 等待上一段注入释放期间暂存的输入，位置只保留最新一条
  struct PendingControl {
    PeerSession* session = nullptr;
    uint64_t controlId = 0;
    std::vector<NetInput> inputs;
    bool TryAppend(const NetInput& input);
  };

  struct ReconnectEntry {
    uint64_t deadline = 0;
    std::function<void()> attempt;
  };

  PeerSession::HandshakeOptions MakeHandshakeOptions(const DesktopGeometry& localGeometry,
                                                     const std::string& deviceName, int localPort,
                                                     const std::string* expectedPeerId);
  // 打开监听套接字；失败时返回 -1 并给出错误码
  uintptr_t OpenListener(int& error);
  // 接入循环：返回时套接字已关闭，参数带出失效原因
  void AcceptLoop(uintptr_t listener, std::string& failure);
  // 监听监督：端口被占用或套接字失效时每 5 秒重建一次
  void ListenLoop();
  void AcceptClient(uintptr_t socket, const std::string& source);
  void ReconnectLoop();

  bool ReserveHandshake(bool inbound, const std::string& source);
  void ReleaseHandshake(bool inbound, const std::string& source);
  bool InjectionAvailable() const;
  void TrackSession(const PeerSessionPtr& session);
  void RegisterSession(const PeerSessionPtr& session);
  void OnSessionClosed(PeerSession& session);
  bool SelectCandidate(PeerSession& session);

  void OnControlStart(PeerSession& session, uint64_t controlId);
  void OnControlEnd(PeerSession& session, uint64_t controlId);
  void OnInput(PeerSession& session, const NetInput& input);
  void SubmitInput(const NetInput& input);
  void TryActivatePendingControl();
  void RevokeControl(PeerSession* expected, std::optional<uint64_t> expectedId, bool sendYield);
  static bool MatchesControl(PeerSession* current, uint64_t currentId, PeerSession* expected,
                             std::optional<uint64_t> expectedId);
  void ReportOverload(const std::string& source);

  // 握手额度：离开作用域必定归还
  class HandshakeReservation {
   public:
    HandshakeReservation(PeerHost& host, bool inbound, std::string source)
        : host_(host), inbound_(inbound), source_(std::move(source)) {}
    ~HandshakeReservation() { host_.ReleaseHandshake(inbound_, source_); }
    HandshakeReservation(const HandshakeReservation&) = delete;
    HandshakeReservation& operator=(const HandshakeReservation&) = delete;

   private:
    PeerHost& host_;
    bool inbound_;
    std::string source_;
  };

  Options options_;
  InjectionPort injection_;
  Callbacks callbacks_;
  TrustedPeerCheck isTrusted_;
  PairingConfirm confirmPairing_;
  DiscoveryService discovery_;

  // 主机锁可重入：撤销控制时会在锁内回调应用，应用可能立刻再调回主机
  mutable std::recursive_mutex gate_;
  std::atomic<bool> stopped_{false};
  bool sharingEnabled_ = false;
  bool inputFaulted_ = false;
  bool injectionFaulted_ = false;

  uintptr_t listener_ = static_cast<uintptr_t>(-1);
  std::thread listenThread_;  // 建立监听、接入与失败重建都在这个线程上
  std::condition_variable_any listenSignal_;  // 重试间隔的等待，Stop 时唤醒
  int inboundHandshakes_ = 0;
  int outboundHandshakes_ = 0;
  std::map<std::string, int> inboundSources_;
  int activeHandshakes_ = 0;
  // 计数用条件变量与可重入的 gate_ 搭配，必须用 _any 版本
  mutable std::condition_variable_any handshakeSignal_;

  std::vector<PeerSessionPtr> allSessions_;  // 关闭收尾期间保持会话存活
  std::map<PeerSession*, Registration> registrations_;
  std::map<std::string, CandidateSelection> candidates_;

  PeerSession* controller_ = nullptr;
  uint64_t controlId_ = 0;
  std::optional<PendingControl> pendingControl_;
  uint64_t lastOverloadReport_ = 0;

  std::thread reconnectThread_;
  mutable std::mutex reconnectMutex_;
  std::condition_variable reconnectSignal_;
  std::map<std::string, ReconnectEntry> reconnects_;
  std::map<std::string, int> retries_;
};

}  // namespace bm
