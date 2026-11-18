#pragma once

#include "core/platform/Common.h"
#include "core/input/DesktopGeometry.h"
#include "core/protocol/FrameCodec.h"
#include "core/protocol/InputCodec.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bm {

class DeviceIdentity;
class PeerSession;
class SessionCipher;
using PeerSessionPtr = std::shared_ptr<PeerSession>;

// 首次配对时交给界面核对的信息；核对码由两端同一份会话密钥材料算出
struct PairingRequest {
  std::string peerId;
  std::string peerName;
  std::string peerPublicKey;
  std::string code;
  std::string peerFingerprint;
  std::string transactionId;
};

// 已配对设备判定：对端身份与大写十六进制公钥，空回调视为一律不信任
using TrustedPeerCheck = std::function<bool(const std::string& peerId, const std::string& peerPublicKey)>;
// 用户核对配对：返回 true 表示接受；aborted 置位表示对端已拒绝，可提前关闭对话框
using PairingConfirm = std::function<bool(const PairingRequest& request, const std::atomic<bool>& aborted)>;

// 控制租约载荷：8 字节小端代次；代次为零非法
Bytes ControlPayload(uint64_t controlId);
uint64_t ReadControlId(std::span<const uint8_t> payload);

// 认证完成的会话：发送走可靠队列，接收与看门狗各用独立线程，关闭后线程自行收尾
class PeerSession : public std::enable_shared_from_this<PeerSession> {
 public:
  using DataHandler = std::function<void(MessageType, std::span<const uint8_t>)>;
  using AbortCheck = std::function<bool()>;

  // 握手参数由主机填好；本地几何与身份的生命周期由调用方保证
  struct HandshakeOptions {
    std::string localName;
    int localPort = 0;
    const DesktopGeometry* localGeometry = nullptr;
    const DeviceIdentity* identity = nullptr;
    TrustedPeerCheck isTrusted;
    PairingConfirm confirmPairing;
    const std::string* expectedPeerId = nullptr;
    // 同一设备的候选连接选举，由主机实现，返回本连接是否胜出
    std::function<bool(PeerSession&)> selectCandidate;
    // 会话一构造就回调，主机据此在关闭时等待握手收尾
    std::function<void(const PeerSessionPtr&)> constructed;
  };

  // 发起与接入都只在认证、密钥确认全部完成后返回；失败抛异常
  static PeerSessionPtr Connect(const std::string& host, int port, const HandshakeOptions& options,
                                const std::atomic<bool>* canceled = nullptr);
  static PeerSessionPtr Accept(uintptr_t socket, const HandshakeOptions& options,
                               const std::atomic<bool>* canceled = nullptr);

  ~PeerSession();
  PeerSession(const PeerSession&) = delete;
  PeerSession& operator=(const PeerSession&) = delete;

  // ---- 业务发送：位置帧合并，其余可靠帧保持提交顺序 ----
  void Send(MessageType type, std::span<const uint8_t> payload);
  bool TrySend(MessageType type, std::span<const uint8_t> payload);
  bool TrySendPosition(std::span<const uint8_t> payload);
  // 大数据帧：队列满时阻塞；会话关闭或 aborted() 为真时返回 false
  bool SendBulk(MessageType type, std::span<const uint8_t> payload, const AbortCheck& aborted = {});

  // 认证完成后先登记业务回调，再由主机显式启动收发
  void Start();
  // 幂等；只发起关闭，收尾在线程中进行，关闭回调由收尾线程触发
  void Close();

  // ---- 状态：对端信息在握手成功后才有值 ----
  bool IsClosed() const { return closed_.load(std::memory_order_acquire); }
  bool IsCloseFinished() const { return closeFinished_.load(std::memory_order_acquire); }
  // 等待关闭收尾，可当作取消等待用；超时返回 false
  bool WaitCloseFinished(uint32_t timeoutMs) const;
  const std::string& PeerId() const { return peerId_; }
  const std::string& PeerPublicKey() const { return peerPublicKey_; }
  const std::string& PeerName() const { return peerName_; }
  // 对端自报的程序版本，尚未收到时为空；跨线程读取，按值返回
  std::string PeerVersion() const;
  const std::string& PeerAddress() const { return peerAddress_; }
  int PeerPort() const { return peerPort_; }
  const DesktopGeometry& PeerGeometry() const { return peerGeometry_; }
  const DesktopGeometry& LocalGeometry() const { return localGeometry_; }
  bool IsInitiator() const { return initiator_; }
  // 由完整签名转录确定，同一物理连接在两端具有相同事务标识
  const std::string& HandshakeTransaction() const { return handshakeTransaction_; }
  int64_t TrustEpoch() const { return trustEpoch_; }
  void SetTrustEpoch(int64_t epoch) { trustEpoch_ = epoch; }
  // 最近一次 Ping/Pong 的往返毫秒数，还没测到时为 0
  int RttMs() const { return rttMs_.load(std::memory_order_relaxed); }
  size_t PendingFrames() const;

  // ---- 回调：输入与控制帧由主机独占，业务数据帧支持多订阅 ----
  void SetInputHandler(std::function<void(const NetInput&)> handler);
  void SetControlStartHandler(std::function<void(uint64_t)> handler);
  void SetControlEndHandler(std::function<void(uint64_t)> handler);
  void SetYieldHandler(std::function<void(uint64_t)> handler);
  void SetInputBlockedHandler(std::function<void(int)> handler);
  void SetRttHandler(std::function<void(int)> handler);
  int SubscribeData(DataHandler handler);
  void UnsubscribeData(int id);
  int SubscribeClosed(std::function<void()> handler);
  void UnsubscribeClosed(int id);

  // 对端程序版本在这里维护，重连后重新为空
  static void SetAppVersion(std::string version);
  static const std::string& AppVersion();

 private:
  struct Outgoing {
    MessageType type = MessageType::Heartbeat;
    Bytes payload;
  };

  // 超时口径照抄 .NET 版：认证 8 秒、配对确认 90 秒、读写 4 秒
  static constexpr int kAuthenticationTimeoutMs = 8000;
  static constexpr int kHandshakeTimeoutMs = 90000;
  static constexpr int kIoTimeoutMs = 4000;

  // 线上原始帧：握手阶段要用完整字节算转录，业务阶段只看解析后的头
  struct WireFrame {
    FrameHeader info;
    Bytes header;
    Bytes payload;
    Bytes All() const;
  };

  PeerSession(uintptr_t socket, const DesktopGeometry& localGeometry, bool initiator);
  // 两个方向共用的认证流程：完成后才把会话交给主机
  static PeerSessionPtr Complete(uintptr_t socket, bool initiator, const HandshakeOptions& options,
                                 const std::atomic<bool>* canceled);

  // ---- 握手（PeerSessionHandshake.cpp）----
  void Authenticate(const HandshakeOptions& options);
  void ElectPhysicalConnection(const HandshakeOptions& options);
  void ExchangePairing(bool localTrust, bool remoteTrust, const HandshakeOptions& options);
  WireFrame ReadWireFrame();
  WireFrame ReadPlain(MessageType expectedType);
  Bytes ReadEncrypted(MessageType expectedType);
  bool ReadEncryptedBoolean(MessageType expectedType);
  void ThrowIfCanceled() const;

  // ---- 传输（PeerSession.cpp）----
  void ReadExact(std::span<uint8_t> buffer);
  void WriteAll(std::span<const uint8_t> buffer);
  void WriteFrame(const Bytes& frame);
  void WriteEncrypted(MessageType type, std::span<const uint8_t> payload);
  void SetReceiveTimeout(int milliseconds);
  void ValidateOutgoing(MessageType type, std::span<const uint8_t> payload) const;
  void SendLoop();
  void ReceiveLoop();
  void WatchdogLoop();
  void Dispatch(MessageType type, std::span<const uint8_t> plain);
  bool TryTakeOutgoing(int& reliableRun, Outgoing& item);
  void CommitPosition();
  void ReapClose();
  void FinishHandshake();

  const uintptr_t socket_;
  DesktopGeometry localGeometry_;
  bool initiator_ = false;
  bool started_ = false;
  mutable std::mutex startMutex_;

  std::string peerAddress_;
  std::string peerId_;
  std::string peerPublicKey_;
  std::string peerName_;
  std::string peerVersion_;
  mutable std::mutex versionMutex_;
  DesktopGeometry peerGeometry_;
  int peerPort_ = 0;
  std::string handshakeTransaction_;
  int64_t trustEpoch_ = 0;

  std::unique_ptr<SessionCipher> cipher_;  // 握手完成后才创建，收尾线程销毁
  std::atomic<bool> closed_{false};
  std::atomic<bool> closeFinished_{false};
  std::atomic<uint64_t> lastAuthenticatedTick_{0};
  std::atomic<int> rttMs_{0};

  // 发送队列：可靠帧 FIFO，位置帧只留最新一条，大数据帧单独限容
  mutable std::mutex queueMutex_;
  mutable std::condition_variable sendSignal_;
  mutable std::condition_variable bulkSpace_;
  std::deque<Outgoing> reliable_;
  std::deque<Outgoing> bulk_;
  std::optional<Bytes> position_;
  size_t pendingBulkBarriers_ = 0;
  uint64_t lastControlEnd_ = 0;

  // 关闭收尾：握手结束后才能等待收发线程并销毁密码对象
  mutable std::mutex closeMutex_;
  mutable std::condition_variable closeSignal_;
  std::optional<std::thread> sendThread_;
  std::optional<std::thread> receiveThread_;
  std::optional<std::thread> watchdogThread_;

  mutable std::mutex writeMutex_;  // 握手阶段的确认帧与发送线程不得交错写
  mutable std::mutex handlerMutex_;
  std::function<void(const NetInput&)> inputHandler_;
  std::function<void(uint64_t)> controlStartHandler_;
  std::function<void(uint64_t)> controlEndHandler_;
  std::function<void(uint64_t)> yieldHandler_;
  std::function<void(int)> inputBlockedHandler_;
  std::function<void(int)> rttHandler_;
  std::vector<std::pair<int, DataHandler>> dataHandlers_;
  std::vector<std::pair<int, std::function<void()>>> closedHandlers_;
  bool closedHandlersTaken_ = false;
  int nextHandlerId_ = 1;
  bool handshakeDone_ = false;  // 与收尾线程共用 closeMutex_。
  const std::atomic<bool>* canceled_ = nullptr;
};

// Winsock 只需要初始化一次；主程序启动时调用，重复调用无副作用
void InitializeNetworking();

}  // namespace bm
