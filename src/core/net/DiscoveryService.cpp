#include "core/net/DiscoveryService.h"

#include "core/platform/EventLog.h"
#include "core/net/PeerSession.h"  // 只为了 Winsock 的一次性初始化入口

#include <windows.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <stdexcept>

namespace bm {

namespace {

// 发现协议自己的一套魔数，与帧协议的 kFrameMagic 无关
constexpr int32_t kDiscoveryMagic = 0x44494D42;
constexpr size_t kMaxNameLength = 255;
constexpr size_t kHeaderLength = 40;  // 魔数 4 + 端口 2 + 身份摘要 32 + 名字长度 2
constexpr size_t kMaxCandidates = 64;
constexpr uint64_t kCandidateLifetimeMs = 30000;
constexpr int kSourceBurst = 4;
constexpr size_t kMaxSources = 128;
constexpr int kBroadcastIntervalMs = 2000;

void Put16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

void Put32(uint8_t* p, uint32_t value) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint16_t Get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

int32_t Get32(const uint8_t* p) {
  uint32_t value = 0;
  for (int i = 0; i < 4; i++) value |= static_cast<uint32_t>(p[i]) << (8 * i);
  return static_cast<int32_t>(value);
}

std::string EndpointKey(const std::string& address, int port) { return address + ":" + std::to_string(port); }

// 名字必须是严格 UTF-8、非全空白且不含控制字符，非法字节按无效包丢弃
bool DecodeName(std::span<const uint8_t> bytes, std::string& name) {
  if (bytes.empty()) return false;
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()),
                                         static_cast<int>(bytes.size()), nullptr, 0);
  if (length <= 0) return false;
  std::wstring text(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(bytes.data()),
                      static_cast<int>(bytes.size()), text.data(), length);
  bool visible = false;
  for (wchar_t ch : text) {
    if (ch <= 0x1F || (ch >= 0x7F && ch <= 0x9F)) return false;
    if (!std::iswspace(static_cast<wint_t>(ch))) visible = true;
  }
  if (!visible) return false;
  name.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return true;
}

}  // namespace

DiscoveryService::DiscoveryService(std::string deviceName, int servicePort, std::string deviceId)
    : deviceName_(std::move(deviceName)), deviceId_(std::move(deviceId)), servicePort_(servicePort) {}

DiscoveryService::~DiscoveryService() { Stop(); }

void DiscoveryService::Start() {
  if (running_.exchange(true)) return;
  InitializeNetworking();
  payload_ = BuildPayload();
  broadcastThread_ = std::thread([this] { BroadcastLoop(payload_); });
  receiveThread_ = std::thread([this] { ReceiveLoop(); });
}

void DiscoveryService::Stop() {
  if (!running_.exchange(false)) {
    // 没在运行时也可能持有绑定的端口，统一在这里收尾
    CloseSocket();
    return;
  }
  stopSignal_.notify_all();
  if (broadcastThread_.joinable()) broadcastThread_.join();
  if (receiveThread_.joinable()) receiveThread_.join();
  CloseSocket();
  std::lock_guard lock(gate_);
  candidates_.clear();
  sources_.clear();
}

bool DiscoveryService::EnsureSocket() {
  std::lock_guard lock(gate_);
  if (socket_ != static_cast<uintptr_t>(-1)) return true;
  const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (handle == INVALID_SOCKET) return false;
  BOOL reuse = TRUE;
  setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
  BOOL broadcast = TRUE;
  setsockopt(handle, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));
  const DWORD timeout = 1000;  // 让接收线程每秒回看一次停止标志
  setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(static_cast<u_short>(kDiscoveryPort));
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(handle, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
    closesocket(handle);
    if (!bindFailureLogged_) {
      bindFailureLogged_ = true;
      LogWarn("发现端点绑定失败，将每两秒重试");
    }
    return false;
  }
  bindFailureLogged_ = false;
  socket_ = static_cast<uintptr_t>(handle);
  return true;
}

void DiscoveryService::CloseSocket() {
  std::lock_guard lock(gate_);
  if (socket_ == static_cast<uintptr_t>(-1)) return;
  closesocket(static_cast<SOCKET>(socket_));
  socket_ = static_cast<uintptr_t>(-1);
}

bool DiscoveryService::TrySend(const Bytes& payload, const void* target, int targetLength) {
  SOCKET handle = INVALID_SOCKET;
  {
    std::lock_guard lock(gate_);
    if (socket_ == static_cast<uintptr_t>(-1)) return false;
    handle = static_cast<SOCKET>(socket_);
  }
  return sendto(handle, reinterpret_cast<const char*>(payload.data()), static_cast<int>(payload.size()), 0,
                reinterpret_cast<const sockaddr*>(target), targetLength) > 0;
}

void DiscoveryService::BroadcastLoop(const Bytes& payload) {
  while (running_.load()) {
    if (EnsureSocket()) {
      // 只发 255.255.255.255 会从默认路由那块网卡出去，接在第二块网卡的机器收不到，
      // 因此给每块在用网卡各算一个定向广播地址（接口列表里直接给出）
      sockaddr_in all{};
      all.sin_family = AF_INET;
      all.sin_port = htons(static_cast<u_short>(kDiscoveryPort));
      all.sin_addr.s_addr = htonl(INADDR_BROADCAST);
      TrySend(payload, &all, sizeof(all));

      INTERFACE_INFO interfaces[64] = {};
      DWORD bytes = 0;
      const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
      if (probe != INVALID_SOCKET) {
        if (WSAIoctl(probe, SIO_GET_INTERFACE_LIST, nullptr, 0, interfaces, sizeof(interfaces), &bytes, nullptr,
                     nullptr) == 0) {
          const size_t count = bytes / sizeof(INTERFACE_INFO);
          for (size_t index = 0; index < count; index++) {
            const INTERFACE_INFO& info = interfaces[index];
            if ((info.iiFlags & IFF_UP) == 0 || (info.iiFlags & IFF_LOOPBACK) != 0 ||
                (info.iiFlags & IFF_BROADCAST) == 0)
              continue;
            sockaddr_in target{};
            std::memcpy(&target, &info.iiBroadcastAddress, sizeof(target));
            target.sin_port = htons(static_cast<u_short>(kDiscoveryPort));
            TrySend(payload, &target, sizeof(target));
          }
        }
        closesocket(probe);
      }
    }
    std::unique_lock lock(gate_);
    stopSignal_.wait_for(lock, std::chrono::milliseconds(kBroadcastIntervalMs), [this] { return !running_.load(); });
  }
}

void DiscoveryService::ReceiveLoop() {
  while (running_.load()) {
    if (!EnsureSocket()) {
      std::unique_lock lock(gate_);
      stopSignal_.wait_for(lock, std::chrono::milliseconds(kBroadcastIntervalMs), [this] { return !running_.load(); });
      continue;
    }
    SOCKET handle = INVALID_SOCKET;
    {
      std::lock_guard lock(gate_);
      handle = static_cast<SOCKET>(socket_);
    }
    uint8_t buffer[512] = {};
    sockaddr_in from{};
    int fromLength = sizeof(from);
    const int received = recvfrom(handle, reinterpret_cast<char*>(buffer), sizeof(buffer), 0,
                                  reinterpret_cast<sockaddr*>(&from), &fromLength);
    if (received <= 0) {
      const int error = WSAGetLastError();
      if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) continue;  // 只是接收窗口到期
      if (!running_.load()) return;
      CloseSocket();
      std::unique_lock lock(gate_);
      stopSignal_.wait_for(lock, std::chrono::milliseconds(kBroadcastIntervalMs), [this] { return !running_.load(); });
      continue;
    }
    char text[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text)) == nullptr) continue;
    if (RememberCandidate(std::span<const uint8_t>(buffer, static_cast<size_t>(received)), text)) {
      // 广播常被网卡、交换机或防火墙丢掉，单播回一个应答：发得出广播的一边就能立刻双向可见
      TrySend(payload_, &from, fromLength);
    }
  }
}

void DiscoveryService::ExpireCandidates(uint64_t now) {
  for (auto entry = candidates_.begin(); entry != candidates_.end();) {
    if (now - entry->second.lastSeen >= kCandidateLifetimeMs)
      entry = candidates_.erase(entry);
    else
      ++entry;
  }
}

bool DiscoveryService::RememberCandidate(std::span<const uint8_t> payload, const std::string& address) {
  std::lock_guard lock(gate_);
  if (!running_.load()) return false;
  const uint64_t now = TickMs();
  auto source = sources_.find(address);
  if (source == sources_.end()) {
    for (auto entry = sources_.begin(); entry != sources_.end();) {
      if (now - entry->second.started >= 1000)
        entry = sources_.erase(entry);
      else
        ++entry;
    }
    if (sources_.size() == kMaxSources) return false;
    source = sources_.emplace(address, SourceWindow{now, 0}).first;
  }
  if (now - source->second.started >= 1000) {
    source->second.started = now;
    source->second.count = 0;
  }
  // 按真实来源地址限速，轮换声明端口或身份都不能绕过
  if (source->second.count >= kSourceBurst) return false;
  source->second.count++;
  // 来源窗口独立于候选，非法包及候选淘汰都不能重置计数
  ExpireCandidates(now);
  DiscoveredPeer peer;
  if (!TryParse(payload, address, peer) || peer.deviceId == deviceId_) return false;
  const std::string key = EndpointKey(peer.address, peer.port);
  if (candidates_.find(key) == candidates_.end() && candidates_.size() == kMaxCandidates) {
    auto oldest = std::min_element(candidates_.begin(), candidates_.end(), [](const auto& left, const auto& right) {
      return left.second.lastSeen < right.second.lastSeen;
    });
    if (oldest != candidates_.end()) candidates_.erase(oldest);
  }
  // 重复端点原位合并，容量与过期只由这一份缓存负责
  candidates_[key] = Candidate{peer, now};
  return true;
}

std::vector<DiscoveredPeer> DiscoveryService::Peers() {
  std::lock_guard lock(gate_);
  ExpireCandidates(TickMs());
  std::vector<DiscoveredPeer> peers;
  peers.reserve(candidates_.size());
  for (const auto& entry : candidates_) peers.push_back(entry.second.peer);
  return peers;
}

std::vector<DiscoveredPeer> DiscoveryService::Candidates(const std::string& deviceId) {
  std::vector<DiscoveredPeer> peers = Peers();
  std::vector<DiscoveredPeer> result;
  for (const DiscoveredPeer& peer : peers)
    if (peer.deviceId == deviceId) result.push_back(peer);
  std::sort(result.begin(), result.end(), [](const DiscoveredPeer& left, const DiscoveredPeer& right) {
    if (left.address != right.address) return left.address < right.address;
    return left.port < right.port;
  });
  return result;
}

void DiscoveryService::RemoveCandidates(const std::string& deviceId, const std::string& address, int port) {
  std::lock_guard lock(gate_);
  for (auto entry = candidates_.begin(); entry != candidates_.end();) {
    const bool match = deviceId.empty() ? entry->first == EndpointKey(address, port) : entry->second.peer.deviceId == deviceId;
    if (match)
      entry = candidates_.erase(entry);
    else
      ++entry;
  }
}

Bytes DiscoveryService::BuildPayload() const {
  if (deviceName_.empty() || deviceName_.size() > kMaxNameLength)
    throw std::invalid_argument("设备名长度必须为1至255个UTF-8字节");
  if (servicePort_ < 1 || servicePort_ > 65535) throw std::invalid_argument("服务端口非法");
  Bytes id = FromHex(deviceId_);
  if (id.size() != 32) throw std::invalid_argument("设备身份摘要非法");
  Bytes buffer(kHeaderLength + deviceName_.size(), 0);
  Put32(&buffer[0], static_cast<uint32_t>(kDiscoveryMagic));
  Put16(&buffer[4], static_cast<uint16_t>(servicePort_));
  std::memcpy(&buffer[6], id.data(), 32);
  Put16(&buffer[38], static_cast<uint16_t>(deviceName_.size()));
  std::memcpy(&buffer[kHeaderLength], deviceName_.data(), deviceName_.size());
  return buffer;
}

bool DiscoveryService::TryParse(std::span<const uint8_t> data, const std::string& address, DiscoveredPeer& peer) {
  if (data.size() < kHeaderLength || Get32(data.data()) != kDiscoveryMagic) return false;
  const size_t length = Get16(data.data() + 38);
  const int port = Get16(data.data() + 4);
  if (length < 1 || length > kMaxNameLength || data.size() != kHeaderLength + length || port == 0) return false;
  std::string name;
  if (!DecodeName(data.subspan(kHeaderLength, length), name)) return false;
  peer.name = std::move(name);
  peer.address = address;
  peer.port = port;
  peer.deviceId = ToHex(data.subspan(6, 32));
  return true;
}

}  // namespace bm
