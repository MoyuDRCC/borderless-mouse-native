#include "core/net/PeerHost.h"

#include "core/crypto/DeviceIdentity.h"
#include "core/platform/EventLog.h"

#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace bm {

namespace {

constexpr int kInboundHandshakeSlots = 4;
constexpr int kOutboundHandshakeSlots = 4;
constexpr int kSourceHandshakeSlots = 2;
constexpr size_t kPendingInputCapacity = 1024;
constexpr uint64_t kOverloadReportIntervalMs = 5000;
constexpr int kReconnectBaseMs = 1000;
constexpr int kReconnectMaximumMs = 15000;
constexpr int kListenRetryMs = 5000;

void CloseSocket(uintptr_t socket) {
  if (socket == static_cast<uintptr_t>(-1)) return;
  closesocket(static_cast<SOCKET>(socket));
}

std::string PeerAddressOf(uintptr_t socket) {
  sockaddr_in address{};
  int length = sizeof(address);
  if (getpeername(static_cast<SOCKET>(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0) return {};
  char text[INET_ADDRSTRLEN] = {};
  if (inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) == nullptr) return {};
  return text;
}

}  // namespace

bool PeerHost::PendingControl::TryAppend(const NetInput& input) {
  // 只合并本候选的连续位置，可靠事件始终保留 FIFO 顺序
  if (input.kind == NetInputKind::Position && !inputs.empty() && inputs.back().kind == NetInputKind::Position) {
    inputs.back() = input;
    return true;
  }
  if (inputs.size() == kPendingInputCapacity) return false;
  inputs.push_back(input);
  return true;
}

PeerHost::PeerHost(Options options, InjectionPort injection)
    : options_(std::move(options)),
      injection_(std::move(injection)),
      discovery_(options_.deviceName, options_.port, options_.identity != nullptr ? options_.identity->Id() : std::string()) {
  if (options_.identity == nullptr) throw std::invalid_argument("主机缺少设备身份");
  if (!options_.localGeometry.Valid()) throw std::invalid_argument("主机缺少本机显示布局");
}

PeerHost::~PeerHost() { Stop(); }

void PeerHost::SetCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

void PeerHost::SetTrust(TrustedPeerCheck isTrusted, PairingConfirm confirmPairing) {
  isTrusted_ = std::move(isTrusted);
  confirmPairing_ = std::move(confirmPairing);
}

PeerSession::HandshakeOptions PeerHost::MakeHandshakeOptions(const DesktopGeometry& localGeometry,
                                                            const std::string& deviceName, int localPort,
                                                            const std::string* expectedPeerId) {
  PeerSession::HandshakeOptions handshake;
  handshake.localName = deviceName;
  handshake.localPort = localPort;
  handshake.localGeometry = &localGeometry;
  handshake.identity = options_.identity;
  handshake.isTrusted = isTrusted_;
  handshake.confirmPairing = confirmPairing_;
  handshake.expectedPeerId = expectedPeerId;
  handshake.selectCandidate = [this](PeerSession& session) { return SelectCandidate(session); };
  handshake.constructed = [this](const PeerSessionPtr& session) { TrackSession(session); };
  return handshake;
}

void PeerHost::Start() {
  {
    std::lock_guard lock(gate_);
    if (stopped_.load()) throw std::runtime_error("主机已停止");
  }
  if (!listenThread_.joinable()) listenThread_ = std::thread([this] { ListenLoop(); });
  if (!reconnectThread_.joinable()) reconnectThread_ = std::thread([this] { ReconnectLoop(); });
  discovery_.Start();
}

uintptr_t PeerHost::OpenListener(int& error) {
  InitializeNetworking();
  const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    error = WSAGetLastError();
    return static_cast<uintptr_t>(-1);
  }
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(static_cast<u_short>(options_.port));
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(listener, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
      listen(listener, SOMAXCONN) != 0) {
    error = WSAGetLastError();
    closesocket(listener);
    return static_cast<uintptr_t>(-1);
  }
  u_long nonBlocking = 1;
  if (ioctlsocket(listener, FIONBIO, &nonBlocking) != 0) {
    error = WSAGetLastError();
    closesocket(listener);
    return static_cast<uintptr_t>(-1);
  }
  return static_cast<uintptr_t>(listener);
}

// 端口被别的进程占用、或监听套接字被系统关闭后，每 5 秒重建一次，直到成功或退出
void PeerHost::ListenLoop() {
  bool failureReported = false;
  while (!stopped_.load()) {
    int error = 0;
    const uintptr_t listener = OpenListener(error);
    std::string failure;
    if (listener != static_cast<uintptr_t>(-1)) {
      {
        std::lock_guard lock(gate_);
        // Stop 先取主机锁收回监听，这里看到停止就不再交出套接字
        if (stopped_.load()) {
          CloseSocket(listener);
          return;
        }
        listener_ = listener;
      }
      if (failureReported) {
        failureReported = false;
        LogInfo("监听端口 " + std::to_string(options_.port) + " 已恢复");
      }
      AcceptLoop(listener, failure);
    } else {
      failure = "监听端口 " + std::to_string(options_.port) + " 失败，错误码 " + std::to_string(error) +
                "，每 5 秒重试";
    }
    if (stopped_.load()) return;
    // 同一个失败状态只报一次，重试期间不再写日志
    if (!failureReported && !failure.empty()) {
      failureReported = true;
      LogError(failure);
      if (callbacks_.listeningFailed) callbacks_.listeningFailed(failure);
    }
    std::unique_lock lock(gate_);
    listenSignal_.wait_for(lock, std::chrono::milliseconds(kListenRetryMs), [this] { return stopped_.load(); });
  }
}

void PeerHost::Stop() {
  if (stopped_.exchange(true)) return;
  std::vector<PeerSessionPtr> sessions;
  {
    std::lock_guard lock(gate_);
    sharingEnabled_ = false;
    RevokeControl(nullptr, std::nullopt, false);
    sessions = allSessions_;
  }
  reconnectSignal_.notify_all();
  listenSignal_.notify_all();
  discovery_.Stop();
  for (const PeerSessionPtr& session : sessions) session->Close();
  if (listenThread_.joinable()) listenThread_.join();
  if (reconnectThread_.joinable()) reconnectThread_.join();
  {
    std::unique_lock lock(gate_);
    handshakeSignal_.wait(lock, [this] { return activeHandshakes_ == 0; });
    // Stop 发起后才构造的会话也必须收尾完毕，才能释放主机及其回调。
    sessions = allSessions_;
  }
  // 收尾线程要等握手结束才销毁密码对象；这里等到它结束，之后才允许释放主机
  for (const PeerSessionPtr& session : sessions) session->WaitCloseFinished(UINT32_MAX);
  std::lock_guard lock(gate_);
  allSessions_.clear();
  registrations_.clear();
  candidates_.clear();
  pendingControl_.reset();
  controller_ = nullptr;
  controlId_ = 0;
}

bool PeerHost::IsListening() const {
  std::lock_guard lock(gate_);
  return listener_ != static_cast<uintptr_t>(-1);
}

bool PeerHost::ReserveHandshake(bool inbound, const std::string& source) {
  if (stopped_.load()) return false;
  // 双向额度分离，每个来源至多占用两个入站位置；未认证阶段另受期限约束
  if (inbound) {
    if (inboundHandshakes_ >= kInboundHandshakeSlots) return false;
    int& used = inboundSources_[source];
    if (used >= kSourceHandshakeSlots) return false;
    used++;
    inboundHandshakes_++;
  } else {
    if (outboundHandshakes_ >= kOutboundHandshakeSlots) return false;
    outboundHandshakes_++;
  }
  activeHandshakes_++;
  return true;
}

void PeerHost::ReleaseHandshake(bool inbound, const std::string& source) {
  std::lock_guard lock(gate_);
  if (inbound) {
    auto entry = inboundSources_.find(source);
    if (entry != inboundSources_.end()) {
      if (--entry->second == 0) inboundSources_.erase(entry);
    }
    inboundHandshakes_--;
  } else {
    outboundHandshakes_--;
  }
  if (--activeHandshakes_ == 0) handshakeSignal_.notify_all();
}

void PeerHost::AcceptLoop(uintptr_t listener, std::string& failure) {
  while (!stopped_.load()) {
    // 监听线程独占监听句柄，短轮询让退出无需跨线程关闭正在使用的套接字。
    WSAPOLLFD descriptor{static_cast<SOCKET>(listener), POLLRDNORM, 0};
    const int ready = WSAPoll(&descriptor, 1, 100);
    if (ready == 0) continue;
    if (ready == SOCKET_ERROR || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      failure = "监听套接字已失效";
      break;
    }
    const SOCKET client = accept(static_cast<SOCKET>(listener), nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      const int error = WSAGetLastError();
      if (error == WSAEWOULDBLOCK) continue;
      if (!stopped_.load()) failure = "监听套接字已失效，错误码 " + std::to_string(error);
      break;
    }
    u_long blocking = 0;
    if (ioctlsocket(client, FIONBIO, &blocking) != 0) {
      closesocket(client);
      continue;
    }
    const uintptr_t socket = static_cast<uintptr_t>(client);
    const std::string source = PeerAddressOf(socket);
    bool accepted;
    {
      std::lock_guard lock(gate_);
      accepted = ReserveHandshake(true, source);
    }
    if (!accepted) {
      CloseSocket(socket);
      ReportOverload(source);
      continue;
    }
    try {
      std::thread([this, socket, source] { AcceptClient(socket, source); }).detach();
    } catch (const std::system_error& error) {
      CloseSocket(socket);
      ReleaseHandshake(true, source);
      LogError(std::string("无法启动接入线程：") + error.what());
    }
  }
  {
    std::lock_guard lock(gate_);
    if (listener_ == listener) CloseSocket(std::exchange(listener_, static_cast<uintptr_t>(-1)));
  }
}

void PeerHost::AcceptClient(uintptr_t socket, const std::string& source) {
  HandshakeReservation reservation(*this, true, source);
  PeerSessionPtr session;
  bool handedOver = false;
  try {
    DesktopGeometry localGeometry;
    std::string deviceName;
    int localPort;
    {
      std::lock_guard lock(gate_);
      if (stopped_.load()) throw std::runtime_error("主机已停止");
      localGeometry = options_.localGeometry;
      deviceName = options_.deviceName;
      localPort = options_.port;
    }
    auto options = MakeHandshakeOptions(localGeometry, deviceName, localPort, nullptr);
    handedOver = true;
    session = PeerSession::Accept(socket, options);
    {
      std::lock_guard lock(gate_);
      if (stopped_.load()) throw std::runtime_error("主机已停止");
      if (!localGeometry.SameAs(options_.localGeometry)) throw std::runtime_error("认证期间本机显示布局变化，需要重新连接");
      RegisterSession(session);
    }
    if (callbacks_.peerJoined) {
      callbacks_.peerJoined(*session);
    } else {
      session->Close();
    }
  } catch (const std::exception& error) {
    if (!stopped_.load()) LogInfo(std::string("单条接入已结束：") + error.what());
    // 交接之前由接入线程关闭，交接后由会话统一收尾。
    if (!handedOver) CloseSocket(socket);
    if (session) session->Close();
  }
}

PeerSessionPtr PeerHost::Connect(const std::string& host, int port, const std::string* expectedPeerId,
                                 const std::atomic<bool>* canceled) {
  DesktopGeometry localGeometry;
  std::string deviceName;
  int localPort;
  {
    std::lock_guard lock(gate_);
    if (stopped_.load()) throw std::runtime_error("主机已停止");
    localGeometry = options_.localGeometry;
    deviceName = options_.deviceName;
    localPort = options_.port;
    if (!ReserveHandshake(false, {})) throw std::runtime_error("本机主动认证已达到四条，请稍后重试");
  }
  HandshakeReservation reservation(*this, false, {});
  PeerSessionPtr session;
  try {
    session = PeerSession::Connect(host, port, MakeHandshakeOptions(localGeometry, deviceName, localPort, expectedPeerId),
                                   canceled);
    std::lock_guard lock(gate_);
    if (stopped_.load()) throw std::runtime_error("主机已停止");
    if (!localGeometry.SameAs(options_.localGeometry)) throw std::runtime_error("认证期间本机显示布局变化，需要重新连接");
    RegisterSession(session);
    return session;
  } catch (...) {
    if (session) session->Close();
    throw;
  }
}

void PeerHost::TrackSession(const PeerSessionPtr& session) {
  std::lock_guard lock(gate_);
  // 已收尾结束的旧会话在这里释放引用：不能在收尾线程上销毁它自己
  allSessions_.erase(std::remove_if(allSessions_.begin(), allSessions_.end(),
                                    [](const PeerSessionPtr& item) { return item->IsCloseFinished(); }),
                     allSessions_.end());
  allSessions_.push_back(session);
  // 认证失败也要清理候选，此处先于任何握手与候选登记。
  session->SubscribeClosed([this, raw = session.get()] { OnSessionClosed(*raw); });
  if (stopped_.load()) session->Close();
}

void PeerHost::RegisterSession(const PeerSessionPtr& session) {
  if (session->IsClosed()) throw std::runtime_error("会话已关闭");
  if (registrations_.find(session.get()) != registrations_.end()) return;
  registrations_[session.get()].session = session;
  PeerSession* raw = session.get();
  // 业务回调在此统一接上，应用只跟主机打交道
  session->SetInputHandler([this, raw](const NetInput& input) { OnInput(*raw, input); });
  session->SetControlStartHandler([this, raw](uint64_t controlId) { OnControlStart(*raw, controlId); });
  session->SetControlEndHandler([this, raw](uint64_t controlId) { OnControlEnd(*raw, controlId); });
  // 对端撤回的控制租约属于应用侧出口，主机只转发
  session->SetYieldHandler([this, raw](uint64_t controlId) {
    if (callbacks_.yieldRequested) callbacks_.yieldRequested(*raw, controlId);
  });
  session->SetInputBlockedHandler([this, raw](int code) {
    if (callbacks_.inputBlocked) callbacks_.inputBlocked(*raw, code);
  });
  session->SetRttHandler([this, raw](int rtt) {
    if (callbacks_.pingUpdated) callbacks_.pingUpdated(*raw, rtt);
  });
}

void PeerHost::ActivateSession(PeerSession& session) {
  std::lock_guard lock(gate_);
  if (stopped_.load()) throw std::runtime_error("主机已停止");
  auto entry = registrations_.find(&session);
  if (entry == registrations_.end()) throw std::invalid_argument("会话未在主机登记");
  // 应用已经接好业务回调，从这里开始接收输入与控制帧
  entry->second.active = true;
}

void PeerHost::OnSessionClosed(PeerSession& session) {
  bool registered;
  {
    std::lock_guard lock(gate_);
    RevokeControl(&session, std::nullopt, false);
    registered = registrations_.erase(&session) != 0;
    if (auto entry = candidates_.find(session.PeerId()); entry != candidates_.end()) {
      CandidateSelection& selection = entry->second;
      selection.members.erase(&session);
      if (selection.winner == &session) selection.winner = nullptr;
      if (selection.members.empty()) candidates_.erase(entry);
    }
  }
  if (registered && callbacks_.sessionClosed) callbacks_.sessionClosed(session);
}

std::vector<PeerSessionPtr> PeerHost::Sessions() const {
  std::lock_guard lock(gate_);
  std::vector<PeerSessionPtr> sessions;
  for (const auto& entry : registrations_) sessions.push_back(entry.second.session);
  return sessions;
}

PeerSessionPtr PeerHost::FindSession(const std::string& peerId) const {
  std::lock_guard lock(gate_);
  for (const auto& entry : registrations_)
    if (entry.second.session->PeerId() == peerId) return entry.second.session;
  return nullptr;
}

void PeerHost::DisconnectPeer(const std::string& peerId) {
  std::lock_guard lock(gate_);
  const auto selection = candidates_.find(peerId);
  if (selection == candidates_.end()) return;
  // 同一身份下已选会话和认证中的候选一起关闭。
  for (PeerSession* session : selection->second.members) session->Close();
}

size_t PeerHost::SessionCount() const {
  std::lock_guard lock(gate_);
  return registrations_.size();
}

void PeerHost::Broadcast(MessageType type, std::span<const uint8_t> payload) {
  std::vector<PeerSessionPtr> sessions;
  {
    std::lock_guard lock(gate_);
    for (const auto& entry : registrations_) sessions.push_back(entry.second.session);
  }
  for (const PeerSessionPtr& session : sessions)
    if (!session->IsClosed()) session->TrySend(type, payload);
}

std::string PeerHost::ControlledBy() const {
  std::lock_guard lock(gate_);
  return controller_ != nullptr ? controller_->PeerName() : std::string();
}

bool PeerHost::IsSharingEnabled() const {
  std::lock_guard lock(gate_);
  return sharingEnabled_;
}

bool PeerHost::InjectionAvailable() const {
  return injection_.available && injection_.available();
}

void PeerHost::SetSharingEnabled(bool enabled) {
  std::lock_guard lock(gate_);
  if (stopped_.load()) return;
  sharingEnabled_ = enabled;
  if (!enabled) RevokeControl(nullptr, std::nullopt, true);
}

void PeerHost::NotifyLocalInput() {
  std::lock_guard lock(gate_);
  if (stopped_.load()) return;
  RevokeControl(nullptr, std::nullopt, true);
}

void PeerHost::ReleaseControl(PeerSession& session) {
  std::lock_guard lock(gate_);
  RevokeControl(&session, std::nullopt, false);
}

void PeerHost::SuspendInputForFault() {
  std::lock_guard lock(gate_);
  if (stopped_.load()) return;
  inputFaulted_ = true;
  RevokeControl(nullptr, std::nullopt, true);
  if (injection_.releaseAll) injection_.releaseAll();
}

void PeerHost::ResumeInputAfterRecovery() {
  std::lock_guard lock(gate_);
  if (!stopped_.load()) inputFaulted_ = false;
}

void PeerHost::NotifyInjectionFailed(int code) {
  PeerSessionPtr controller;
  {
    std::lock_guard lock(gate_);
    if (stopped_.load()) return;
    injectionFaulted_ = true;
    const auto entry = controller_ != nullptr ? registrations_.find(controller_) : registrations_.end();
    if (entry != registrations_.end()) controller = entry->second.session;
    RevokeControl(nullptr, std::nullopt, true);
  }
  // 明确告诉对端“本机现在收不了输入”，而不是让它只看到一次 Yield
  if (controller && !controller->IsClosed()) {
    const uint8_t reason = static_cast<uint8_t>(code);
    controller->TrySend(MessageType::InputBlocked, std::span<const uint8_t>(&reason, 1));
  }
  if (callbacks_.injectionFailed) callbacks_.injectionFailed(code);
}

void PeerHost::NotifyInjectionRecovered() {
  {
    std::lock_guard lock(gate_);
    if (stopped_.load()) return;
    // 注入器只清除自己的故障，钩子侧故障仍由显式恢复入口负责
    injectionFaulted_ = false;
  }
  if (callbacks_.injectionRecovered) callbacks_.injectionRecovered();
}

void PeerHost::NotifyControlReleased() {
  std::lock_guard lock(gate_);
  TryActivatePendingControl();
}

void PeerHost::NotifyControlOverloaded() {
  std::lock_guard lock(gate_);
  RevokeControl(nullptr, std::nullopt, true);
}

void PeerHost::SubmitInput(const NetInput& input) {
  if (injection_.submit) injection_.submit(input);
}

bool PeerHost::MatchesControl(PeerSession* current, uint64_t currentId, PeerSession* expected,
                              std::optional<uint64_t> expectedId) {
  return current != nullptr && (expected == nullptr || current == expected) &&
         (!expectedId.has_value() || currentId == *expectedId);
}

void PeerHost::OnControlStart(PeerSession& session, uint64_t controlId) {
  std::lock_guard lock(gate_);
  if (stopped_.load() || session.IsClosed()) return;
  auto entry = registrations_.find(&session);
  if (entry == registrations_.end() || !entry->second.active) return;
  Registration& registration = entry->second;
  if (controlId <= registration.lastControlStart) return;
  registration.lastControlStart = controlId;
  RevokeControl(nullptr, std::nullopt, true);
  if (!sharingEnabled_ || inputFaulted_ || injectionFaulted_ || !InjectionAvailable()) {
    session.TrySend(MessageType::YieldControl, ControlPayload(controlId));
    return;
  }
  // 候选与输入绑定同一会话和代次；释放尚未完成只表示等待，不能立即 Yield 新控制
  pendingControl_ = PendingControl{&session, controlId, {}};
  TryActivatePendingControl();
}

void PeerHost::OnControlEnd(PeerSession& session, uint64_t controlId) {
  std::lock_guard lock(gate_);
  RevokeControl(&session, controlId, false);
}

void PeerHost::OnInput(PeerSession& session, const NetInput& input) {
  std::lock_guard lock(gate_);
  if (stopped_.load() || !sharingEnabled_ || inputFaulted_ || injectionFaulted_ || session.IsClosed()) return;
  const bool active = MatchesControl(controller_, controlId_, &session, input.controlId);
  if (!active && (!pendingControl_ ||
                  !MatchesControl(pendingControl_->session, pendingControl_->controlId, &session, input.controlId)))
    return;
  NetInput submitted = input;
  if (input.kind == NetInputKind::Position) {
    // 远端坐标可能落在可见区域外，注入前统一按本机几何夹取
    const Point2i visible = options_.localGeometry.ClampPixel(Point2i{input.a, input.b});
    submitted.a = visible.x;
    submitted.b = visible.y;
  }
  if (active) {
    SubmitInput(submitted);
    return;
  }
  if (!pendingControl_->TryAppend(submitted)) {
    LogWarn("控制交接缓冲超过 " + std::to_string(kPendingInputCapacity) + " 项，已撤销候选控制");
    RevokeControl(&session, input.controlId, true);
  }
}

void PeerHost::TryActivatePendingControl() {
  if (!pendingControl_) return;
  PeerSession* session = pendingControl_->session;
  const uint64_t controlId = pendingControl_->controlId;
  auto entry = registrations_.find(session);
  if (stopped_.load() || !sharingEnabled_ || inputFaulted_ || injectionFaulted_ || !InjectionAvailable() ||
      session->IsClosed() || entry == registrations_.end() || !entry->second.active ||
      entry->second.lastControlStart != controlId) {
    RevokeControl(session, controlId, true);
    return;
  }
  // 注入器尚未释放上一段控制时保持候选，等它回报后再接管
  if (!injection_.begin || !injection_.begin()) return;
  const PendingControl pending = std::move(*pendingControl_);
  pendingControl_.reset();
  controller_ = pending.session;
  controlId_ = pending.controlId;
  // 主机锁内只提交队列，SendInput 始终在注入线程；新输入不能插到交接缓冲之前
  for (const NetInput& input : pending.inputs) {
    if (!MatchesControl(controller_, controlId_, pending.session, controlId)) break;
    if (pending.session->IsClosed()) {
      RevokeControl(pending.session, controlId, false);
      return;
    }
    SubmitInput(input);
  }
  if (pending.session->IsClosed()) RevokeControl(pending.session, controlId, false);
}

void PeerHost::RevokeControl(PeerSession* expected, std::optional<uint64_t> expectedId, bool sendYield) {
  if (pendingControl_ && MatchesControl(pendingControl_->session, pendingControl_->controlId, expected, expectedId)) {
    PeerSession* pendingSession = pendingControl_->session;
    const uint64_t pendingId = pendingControl_->controlId;
    pendingControl_.reset();
    if (sendYield && !pendingSession->IsClosed())
      pendingSession->TrySend(MessageType::YieldControl, ControlPayload(pendingId));
  }
  if (!MatchesControl(controller_, controlId_, expected, expectedId)) return;
  PeerSession* current = controller_;
  const uint64_t previousId = controlId_;
  controller_ = nullptr;
  controlId_ = 0;
  if (injection_.releaseAll) injection_.releaseAll();
  if (sendYield) {
    if (!current->IsClosed()) current->TrySend(MessageType::YieldControl, ControlPayload(previousId));
    return;
  }
  if (callbacks_.controlEnded) callbacks_.controlEnded();
}

void PeerHost::UpdateLocalGeometry(const DesktopGeometry& geometry) {
  std::lock_guard lock(gate_);
  if (stopped_.load()) return;
  if (options_.localGeometry.SameAs(geometry)) return;
  // 候选位置已按旧几何夹取：先撤销所有代次和缓冲，再允许新几何接收输入
  RevokeControl(nullptr, std::nullopt, true);
  if (injection_.releaseAll) injection_.releaseAll();
  options_.localGeometry = geometry;
  const std::vector<PeerSessionPtr> sessions = allSessions_;
  for (const PeerSessionPtr& session : sessions) session->Close();
}

bool PeerHost::SelectCandidate(PeerSession& session) {
  const bool coordinator = options_.identity->Id() < session.PeerId();
  {
    std::lock_guard lock(gate_);
    if (stopped_.load()) throw std::runtime_error("主机已停止");
    CandidateSelection& selection = candidates_[session.PeerId()];
    selection.members.insert(&session);
    if (selection.winner != nullptr && selection.winner->IsClosed()) selection.winner = nullptr;
    if (selection.winner != nullptr) return selection.winner == &session;
    // 协调端优先自己的主动方向；非协调端只在收到事务选定消息后才进来
    if (!coordinator || session.IsInitiator()) {
      selection.winner = &session;
      return true;
    }
  }
  // 只有反向连接时短暂汇合候选后即可首配，不等待另一方向必须出现
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  std::lock_guard lock(gate_);
  if (stopped_.load()) throw std::runtime_error("主机已停止");
  CandidateSelection& selection = candidates_[session.PeerId()];
  if (selection.winner == nullptr) {
    PeerSession* best = nullptr;
    for (PeerSession* candidate : selection.members) {
      if (candidate->IsClosed()) continue;
      if (best == nullptr) {
        best = candidate;
        continue;
      }
      if (candidate->IsInitiator() != best->IsInitiator()) {
        if (candidate->IsInitiator()) best = candidate;
        continue;
      }
      if (candidate->HandshakeTransaction() < best->HandshakeTransaction()) best = candidate;
    }
    selection.winner = best;
  }
  return selection.winner == &session;
}

void PeerHost::ReportOverload(const std::string& source) {
  {
    std::lock_guard lock(gate_);
    const uint64_t now = TickMs();
    if (stopped_.load() || (lastOverloadReport_ != 0 && now - lastOverloadReport_ < kOverloadReportIntervalMs)) return;
    lastOverloadReport_ = now;
  }
  const std::string message = "已拒绝来自 " + source + " 的认证连接：入站最多四条、每来源最多两条，未认证八秒自动关闭。";
  LogWarn(message);
  if (callbacks_.handshakeOverloaded) callbacks_.handshakeOverloaded(message);
}

void PeerHost::ScheduleReconnect(const std::string& deviceId, std::function<void()> attempt) {
  {
    std::lock_guard lock(reconnectMutex_);
    if (stopped_.load()) return;
    const int index = retries_[deviceId]++;
    const int delay = std::min(kReconnectBaseMs << std::min(index, 4), kReconnectMaximumMs);
    reconnects_[deviceId] = ReconnectEntry{TickMs() + static_cast<uint64_t>(delay), std::move(attempt)};
  }
  reconnectSignal_.notify_all();
}

void PeerHost::CancelReconnect(const std::string& deviceId) {
  std::lock_guard lock(reconnectMutex_);
  reconnects_.erase(deviceId);
}

bool PeerHost::ReconnectPending(const std::string& deviceId) const {
  std::lock_guard lock(reconnectMutex_);
  return reconnects_.find(deviceId) != reconnects_.end();
}

int PeerHost::RetryCount(const std::string& deviceId) const {
  std::lock_guard lock(reconnectMutex_);
  auto entry = retries_.find(deviceId);
  return entry != retries_.end() ? entry->second : 0;
}

void PeerHost::ResetRetryCount(const std::string& deviceId) {
  std::lock_guard lock(reconnectMutex_);
  retries_.erase(deviceId);
}

void PeerHost::ReconnectLoop() {
  while (!stopped_.load()) {
    std::vector<std::function<void()>> due;
    {
      std::unique_lock lock(reconnectMutex_);
      const uint64_t now = TickMs();
      uint64_t nextDeadline = 0;
      for (auto entry = reconnects_.begin(); entry != reconnects_.end();) {
        if (entry->second.deadline <= now) {
          due.push_back(std::move(entry->second.attempt));
          entry = reconnects_.erase(entry);
          continue;
        }
        nextDeadline = nextDeadline == 0 ? entry->second.deadline : std::min(nextDeadline, entry->second.deadline);
        ++entry;
      }
      if (due.empty()) {
        const uint64_t wait = nextDeadline == 0 ? 1000 : std::max<uint64_t>(1, nextDeadline - now);
        reconnectSignal_.wait_for(lock, std::chrono::milliseconds(wait), [this] { return stopped_.load(); });
        continue;
      }
    }
    for (const std::function<void()>& attempt : due) {
      if (stopped_.load()) return;
      try {
        attempt();
      } catch (const std::exception& error) {
        LogError(std::string("重连回调失败：") + error.what());
      }
    }
  }
}

}  // namespace bm
