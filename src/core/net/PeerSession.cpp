#include "core/net/PeerSession.h"

#include "core/platform/EventLog.h"
#include "core/crypto/SessionCipher.h"

#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace bm {

namespace {

constexpr int kHeartbeatIntervalMs = 500;
constexpr int kPingIntervalMs = 2000;
constexpr size_t kMaxVersionLength = 32;
constexpr int kReliableBurst = 8;
constexpr size_t kReliableCapacity = 1024;
constexpr size_t kBulkCapacity = 32;

std::string g_appVersion = "1.0.0";

// Ping 的载荷就是发出时刻的毫秒计数，对端原样回抄
Bytes TickPayload() {
  Bytes payload(8, 0);
  const uint64_t now = TickMs();
  for (int i = 0; i < 8; i++) payload[static_cast<size_t>(i)] = static_cast<uint8_t>(now >> (8 * i));
  return payload;
}

int64_t ReadInt64(std::span<const uint8_t> payload) {
  uint64_t value = 0;
  for (int i = 0; i < 8; i++) value |= static_cast<uint64_t>(payload[static_cast<size_t>(i)]) << (8 * i);
  return static_cast<int64_t>(value);
}

}  // namespace

void InitializeNetworking() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("Winsock 初始化失败");
  });
}

Bytes ControlPayload(uint64_t controlId) {
  if (controlId == 0) throw std::out_of_range("控制租约代次不能为零");
  Bytes payload(8, 0);
  for (int i = 0; i < 8; i++) payload[static_cast<size_t>(i)] = static_cast<uint8_t>(controlId >> (8 * i));
  return payload;
}

uint64_t ReadControlId(std::span<const uint8_t> payload) {
  if (payload.size() != 8) throw std::runtime_error("控制租约载荷长度非法");
  uint64_t id = 0;
  for (int i = 0; i < 8; i++) id |= static_cast<uint64_t>(payload[static_cast<size_t>(i)]) << (8 * i);
  if (id == 0) throw std::runtime_error("控制租约代次不能为零");
  return id;
}

Bytes PeerSession::WireFrame::All() const {
  Bytes bytes = header;
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  return bytes;
}

PeerSession::PeerSession(uintptr_t socket, const DesktopGeometry& localGeometry, bool initiator)
    : socket_(socket), localGeometry_(localGeometry), initiator_(initiator) {
  const SOCKET handle = static_cast<SOCKET>(socket_);
  sockaddr_in address{};
  int length = sizeof(address);
  if (getpeername(handle, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
    char text[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) != nullptr) peerAddress_ = text;
  }
  // 输入帧很小，禁用 Nagle 才能保证移动延迟不随打包抖动
  BOOL noDelay = TRUE;
  setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
  const DWORD sendTimeout = static_cast<DWORD>(kIoTimeoutMs);
  setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
  // 认证阶段只给 8 秒；确认私钥身份后才放宽到配对码核对期限
  SetReceiveTimeout(kAuthenticationTimeoutMs);
}

PeerSession::~PeerSession() {
  // 收发线程都持有 shared_ptr，能走到析构说明它们已经退出，只需解开线程对象
  if (sendThread_ && sendThread_->joinable()) sendThread_->detach();
  if (receiveThread_ && receiveThread_->joinable()) receiveThread_->detach();
  if (watchdogThread_ && watchdogThread_->joinable()) watchdogThread_->detach();
  cipher_.reset();
}

void PeerSession::SetReceiveTimeout(int milliseconds) {
  const DWORD timeout = static_cast<DWORD>(milliseconds);
  setsockopt(static_cast<SOCKET>(socket_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
}

void PeerSession::ReadExact(std::span<uint8_t> buffer) {
  size_t offset = 0;
  while (offset < buffer.size()) {
    const int received = recv(static_cast<SOCKET>(socket_), reinterpret_cast<char*>(buffer.data() + offset),
                              static_cast<int>(buffer.size() - offset), 0);
    if (received <= 0) {
      if (received == 0) throw std::runtime_error("对端已关闭连接");
      const int error = WSAGetLastError();
      if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) throw std::runtime_error("等待对端数据超时");
      throw std::runtime_error("对端接收失败，错误码 " + std::to_string(error));
    }
    offset += static_cast<size_t>(received);
  }
}

void PeerSession::WriteAll(std::span<const uint8_t> buffer) {
  size_t offset = 0;
  while (offset < buffer.size()) {
    const int sent = send(static_cast<SOCKET>(socket_), reinterpret_cast<const char*>(buffer.data() + offset),
                          static_cast<int>(buffer.size() - offset), 0);
    if (sent <= 0) throw std::runtime_error("对端发送失败");
    offset += static_cast<size_t>(sent);
  }
}

void PeerSession::WriteFrame(const Bytes& frame) {
  std::lock_guard lock(writeMutex_);
  WriteAll(frame);
}

void PeerSession::WriteEncrypted(MessageType type, std::span<const uint8_t> payload) {
  // 序号分配与整帧写出必须在同一把锁内，握手确认帧不能和发送线程交错
  std::lock_guard lock(writeMutex_);
  const Bytes frame = cipher_->Seal(type, payload);
  WriteAll(frame);
}

void PeerSession::ValidateOutgoing(MessageType type, std::span<const uint8_t> payload) const {
  if (payload.size() > kMaxPlainPayload) throw std::out_of_range("明文载荷超过可用长度");
  switch (type) {
    case MessageType::ControlStart:
    case MessageType::ControlEnd:
    case MessageType::YieldControl:
      ReadControlId(payload);  // 长度必须为 8 且代次非零
      break;
    case MessageType::Input: {
      NetInput input;
      if (!InputCodec::TryDecode(payload, input)) throw std::invalid_argument("输入载荷格式非法");
      break;
    }
    case MessageType::Heartbeat:
      if (!payload.empty()) throw std::invalid_argument("心跳必须是空载荷");
      break;
    default:
      if (!IsDataMessage(type)) throw std::invalid_argument("不能在业务队列发送握手类型");
      break;
  }
}

void PeerSession::Send(MessageType type, std::span<const uint8_t> payload) {
  if (!TrySend(type, payload) && IsBulkMessage(type) && !IsClosed())
    throw std::runtime_error("大数据队列已满，请使用阻塞发送等待背压");
}

bool PeerSession::TrySend(MessageType type, std::span<const uint8_t> payload) {
  ValidateOutgoing(type, payload);
  NetInput input;
  // 位置帧只保留最新一条，其余帧全部走可靠队列
  if (type == MessageType::Input && InputCodec::TryDecode(payload, input) && input.kind == NetInputKind::Position)
    return TrySendPosition(payload);
  bool overloaded = false;
  {
    std::lock_guard lock(queueMutex_);
    if (IsClosed()) return false;
    if (IsBulkMessage(type)) {
      if (bulk_.size() >= kBulkCapacity) return false;
      bulk_.push_back(Outgoing{type, Bytes(payload.begin(), payload.end())});
      sendSignal_.notify_one();
      return true;
    }
    const uint64_t endId = type == MessageType::ControlEnd ? ReadControlId(payload) : 0;
    if (endId != 0 && endId <= lastControlEnd_) return false;
    const size_t required = position_ ? 2 : 1;
    overloaded = reliable_.size() > kReliableCapacity - required;
    if (!overloaded) {
      // 所有可靠控制保持提交顺序：先固定位置，避免位置越过按键和租约边界
      CommitPosition();
      reliable_.push_back(Outgoing{type, Bytes(payload.begin(), payload.end())});
      if (type == MessageType::ClipboardState) pendingBulkBarriers_++;
      if (endId != 0) lastControlEnd_ = endId;
      sendSignal_.notify_one();
      return true;
    }
  }
  LogError("可靠输入队列超过 " + std::to_string(kReliableCapacity) + " 帧，关闭会话并释放控制");
  Close();
  return false;
}

bool PeerSession::TrySendPosition(std::span<const uint8_t> payload) {
  NetInput input;
  if (!InputCodec::TryDecode(payload, input) || input.kind != NetInputKind::Position)
    throw std::invalid_argument("位置载荷格式非法");
  std::lock_guard lock(queueMutex_);
  if (IsClosed()) return false;
  position_ = Bytes(payload.begin(), payload.end());
  sendSignal_.notify_one();
  return true;
}

bool PeerSession::SendBulk(MessageType type, std::span<const uint8_t> payload, const AbortCheck& aborted) {
  ValidateOutgoing(type, payload);
  if (!IsBulkMessage(type)) throw std::invalid_argument("只有大数据帧使用阻塞发送");
  Bytes copy(payload.begin(), payload.end());
  std::unique_lock lock(queueMutex_);
  bulkSpace_.wait(lock, [&] {
    return IsClosed() || bulk_.size() < kBulkCapacity || (aborted && aborted());
  });
  if (IsClosed() || (aborted && aborted())) return false;
  bulk_.push_back(Outgoing{type, std::move(copy)});
  sendSignal_.notify_one();
  return true;
}

void PeerSession::CommitPosition() {
  if (!position_) return;
  reliable_.push_back(Outgoing{MessageType::Input, std::move(*position_)});
  position_.reset();
}

bool PeerSession::TryTakeOutgoing(int& reliableRun, Outgoing& item) {
  std::lock_guard lock(queueMutex_);
  const bool hasReliable = !reliable_.empty() || position_.has_value();
  // 状态消息是批量帧的屏障：保持可靠 FIFO，同时保证后续数据不抢在状态之前
  if (hasReliable && (pendingBulkBarriers_ != 0 || reliableRun < kReliableBurst || bulk_.empty())) {
    if (!reliable_.empty()) {
      item = std::move(reliable_.front());
      reliable_.pop_front();
      if (item.type == MessageType::ClipboardState) pendingBulkBarriers_--;
    } else {
      item = Outgoing{MessageType::Input, std::move(*position_)};
      position_.reset();
    }
    reliableRun = std::min(kReliableBurst, reliableRun + 1);
    return true;
  }
  if (!bulk_.empty()) {
    item = std::move(bulk_.front());
    bulk_.pop_front();
    reliableRun = 0;
    bulkSpace_.notify_all();  // 队列腾出位置，唤醒等待背压的发送方
    return true;
  }
  return false;
}

void PeerSession::Start() {
  std::lock_guard lock(startMutex_);
  if (IsClosed()) throw std::runtime_error("会话已关闭");
  if (started_) return;
  if (!cipher_) throw std::runtime_error("安全握手尚未完成");
  // 业务阶段读写都受 4 秒期限约束，看门狗按认证帧续期
  SetReceiveTimeout(kIoTimeoutMs);
  lastAuthenticatedTick_.store(TickMs());
  sendThread_.emplace([self = shared_from_this()] { self->SendLoop(); });
  receiveThread_.emplace([self = shared_from_this()] { self->ReceiveLoop(); });
  watchdogThread_.emplace([self = shared_from_this()] { self->WatchdogLoop(); });
  started_ = true;
  // 版本在业务队列里发送：不参与签名转录，旧对端收到会当作非法消息断开
  const std::string& version = AppVersion();
  TrySend(MessageType::AppInfo, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(version.data()), version.size()));
}

void PeerSession::SendLoop() {
  const uint64_t clockStart = TickMs();
  uint64_t nextHeartbeat = kHeartbeatIntervalMs;
  uint64_t nextPing = kPingIntervalMs;
  int reliableRun = 0;
  try {
    while (!IsClosed()) {
      Outgoing item;
      const uint64_t now = TickMs() - clockStart;
      if (now >= nextHeartbeat) {
        item = Outgoing{MessageType::Heartbeat, Bytes{}};
        nextHeartbeat = now + kHeartbeatIntervalMs;
      } else if (now >= nextPing) {
        item = Outgoing{MessageType::Ping, TickPayload()};
        nextPing = now + kPingIntervalMs;
      } else if (!TryTakeOutgoing(reliableRun, item)) {
        std::unique_lock lock(queueMutex_);
        const uint64_t wait = std::max<uint64_t>(1, nextHeartbeat - now);
        sendSignal_.wait_for(lock, std::chrono::milliseconds(wait), [this] {
          return IsClosed() || !reliable_.empty() || position_.has_value() || !bulk_.empty();
        });
        continue;
      }
      WriteEncrypted(item.type, item.payload);
    }
  } catch (const std::exception& error) {
    if (!IsClosed()) LogError(std::string("对端发送终止：") + error.what());
  }
  Close();
}

void PeerSession::ReceiveLoop() {
  try {
    while (!IsClosed()) {
      WireFrame frame = ReadWireFrame();
      // 所有业务帧必须先通过帧头与密文认证，再解释或派发
      const Bytes plain = cipher_->Open(frame.header, frame.payload);
      lastAuthenticatedTick_.store(TickMs());
      if (IsClosed()) break;
      Dispatch(frame.info.type, plain);
    }
  } catch (const std::exception& error) {
    if (!IsClosed()) LogError(std::string("对端接收终止：") + error.what());
  }
  Close();
}

void PeerSession::Dispatch(MessageType type, std::span<const uint8_t> plain) {
  switch (type) {
    case MessageType::Heartbeat:
      if (!plain.empty()) throw std::runtime_error("心跳载荷非法");
      break;
    case MessageType::ControlStart:
      if (controlStartHandler_) controlStartHandler_(ReadControlId(plain));
      break;
    case MessageType::ControlEnd:
      if (controlEndHandler_) controlEndHandler_(ReadControlId(plain));
      break;
    case MessageType::YieldControl:
      if (yieldHandler_) yieldHandler_(ReadControlId(plain));
      break;
    case MessageType::AppInfo:
      if (plain.size() > kMaxVersionLength) throw std::runtime_error("对端版本载荷非法");
      {
        std::lock_guard lock(versionMutex_);
        peerVersion_.assign(plain.empty() ? "" : reinterpret_cast<const char*>(plain.data()), plain.size());
      }
      break;
    case MessageType::InputBlocked:
      if (plain.size() != 1) throw std::runtime_error("输入受阻载荷非法");
      if (inputBlockedHandler_) inputBlockedHandler_(plain[0]);
      break;
    case MessageType::Ping:
      // 原样回抄时间戳，对端据此算往返；回包走可靠队列，不插队
      if (plain.size() != 8) throw std::runtime_error("Ping 载荷非法");
      TrySend(MessageType::Pong, plain);
      break;
    case MessageType::Pong: {
      if (plain.size() != 8) throw std::runtime_error("Pong 载荷非法");
      const int64_t sent = ReadInt64(plain);
      const int64_t elapsed = static_cast<int64_t>(TickMs()) - sent;
      const int rtt = static_cast<int>(std::clamp<int64_t>(elapsed, 0, INT32_MAX));
      rttMs_.store(rtt, std::memory_order_relaxed);
      if (rttHandler_) rttHandler_(rtt);
      break;
    }
    case MessageType::Input: {
      NetInput input;
      if (!InputCodec::TryDecode(plain, input)) throw std::runtime_error("输入载荷非法");
      if (inputHandler_) inputHandler_(input);
      break;
    }
    default: {
      if (!IsDataMessage(type)) throw std::runtime_error("会话内出现非法消息类型");
      std::vector<DataHandler> handlers;
      {
        std::lock_guard lock(handlerMutex_);
        for (const auto& entry : dataHandlers_) handlers.push_back(entry.second);
      }
      for (const DataHandler& handler : handlers) handler(type, plain);
      break;
    }
  }
}

void PeerSession::WatchdogLoop() {
  while (!IsClosed()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kHeartbeatIntervalMs));
    if (IsClosed()) return;
    // 只有完整通过认证的帧才续期，慢速滴流不能延长控制租约
    if (TickMs() - lastAuthenticatedTick_.load() < static_cast<uint64_t>(kIoTimeoutMs)) continue;
    LogError("对端未在期限内完成有效认证帧，关闭会话并释放控制");
    Close();
    return;
  }
}

void PeerSession::Close() {
  if (closed_.exchange(true)) return;
  // 先中断 I/O，等握手和收发退出后再归还句柄，避免系统复用仍在使用的值。
  shutdown(static_cast<SOCKET>(socket_), SD_BOTH);
  {
    std::lock_guard lock(queueMutex_);
    reliable_.clear();
    bulk_.clear();
    position_.reset();
    pendingBulkBarriers_ = 0;
  }
  sendSignal_.notify_all();
  bulkSpace_.notify_all();
  // 关闭可能由收发线程自己触发，收尾必须放到独立线程，且不能在这里 join
  std::thread([self = shared_from_this()] { self->ReapClose(); }).detach();
}

void PeerSession::ReapClose() {
  {
    // 握手还在别的线程进行时不能销毁它正在使用的密码对象
    std::unique_lock lock(closeMutex_);
    closeSignal_.wait(lock, [this] { return handshakeDone_; });
  }
  {
    std::lock_guard lock(startMutex_);
    if (sendThread_ && sendThread_->joinable()) sendThread_->join();
    if (receiveThread_ && receiveThread_->joinable()) receiveThread_->join();
    if (watchdogThread_ && watchdogThread_->joinable()) watchdogThread_->join();
  }
  closesocket(static_cast<SOCKET>(socket_));
  cipher_.reset();
  std::vector<std::function<void()>> handlers;
  {
    std::lock_guard lock(handlerMutex_);
    closedHandlersTaken_ = true;
    for (auto& entry : closedHandlers_) handlers.push_back(std::move(entry.second));
    closedHandlers_.clear();
    dataHandlers_.clear();
  }
  for (const std::function<void()>& handler : handlers) {
    try {
      handler();
    } catch (const std::exception& error) {
      LogError(std::string("会话关闭回调失败：") + error.what());
    }
  }
  {
    std::lock_guard lock(closeMutex_);
    closeFinished_.store(true);
  }
  closeSignal_.notify_all();
}

bool PeerSession::WaitCloseFinished(uint32_t timeoutMs) const {
  std::unique_lock lock(closeMutex_);
  return closeSignal_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                               [this] { return closeFinished_.load(); });
}

size_t PeerSession::PendingFrames() const {
  std::lock_guard lock(queueMutex_);
  if (IsClosed()) return 0;
  return reliable_.size() + bulk_.size() + (position_ ? 1 : 0);
}

std::string PeerSession::PeerVersion() const {
  std::lock_guard lock(versionMutex_);
  return peerVersion_;
}

void PeerSession::SetInputHandler(std::function<void(const NetInput&)> handler) {
  inputHandler_ = std::move(handler);
}

void PeerSession::SetControlStartHandler(std::function<void(uint64_t)> handler) {
  controlStartHandler_ = std::move(handler);
}

void PeerSession::SetControlEndHandler(std::function<void(uint64_t)> handler) {
  controlEndHandler_ = std::move(handler);
}

void PeerSession::SetYieldHandler(std::function<void(uint64_t)> handler) { yieldHandler_ = std::move(handler); }

void PeerSession::SetInputBlockedHandler(std::function<void(int)> handler) {
  inputBlockedHandler_ = std::move(handler);
}

void PeerSession::SetRttHandler(std::function<void(int)> handler) { rttHandler_ = std::move(handler); }

int PeerSession::SubscribeData(DataHandler handler) {
  std::lock_guard lock(handlerMutex_);
  const int id = nextHandlerId_++;
  dataHandlers_.emplace_back(id, std::move(handler));
  return id;
}

void PeerSession::UnsubscribeData(int id) {
  std::lock_guard lock(handlerMutex_);
  dataHandlers_.erase(std::remove_if(dataHandlers_.begin(), dataHandlers_.end(),
                                    [id](const auto& entry) { return entry.first == id; }),
                      dataHandlers_.end());
}

int PeerSession::SubscribeClosed(std::function<void()> handler) {
  {
    std::lock_guard lock(handlerMutex_);
    if (!closedHandlersTaken_) {
      const int id = nextHandlerId_++;
      closedHandlers_.emplace_back(id, std::move(handler));
      return id;
    }
  }
  // 已经收尾的会话立即回调，登记方不会漏掉关闭通知；返回 0 表示没有建立订阅
  if (handler) handler();
  return 0;
}

void PeerSession::UnsubscribeClosed(int id) {
  std::lock_guard lock(handlerMutex_);
  closedHandlers_.erase(std::remove_if(closedHandlers_.begin(), closedHandlers_.end(),
                                       [id](const auto& entry) { return entry.first == id; }),
                        closedHandlers_.end());
}

void PeerSession::SetAppVersion(std::string version) { g_appVersion = std::move(version); }

const std::string& PeerSession::AppVersion() { return g_appVersion; }

}  // namespace bm
