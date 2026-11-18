#include "app/shell/AppController.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <system_error>
#include <utility>

#include "core/crypto/DeviceIdentity.h"
#include "core/net/PeerHost.h"

namespace bm {

namespace {

std::string ConnectionKey(const std::string& address, int port) { return address + ":" + std::to_string(port); }

}  // namespace

void AppController::ConnectAsync(const std::string& address, int port, const std::string& expectedPeerId) {
  const bool manualAddress = expectedPeerId.empty();
  StartConnection(manualAddress ? ConnectionKey(address, port) : expectedPeerId, expectedPeerId, {},
                  {{address, port}}, false, manualAddress);
}

bool AppController::ManuallyStopped(const std::string& peerId) {
  return std::find(manualStop_.begin(), manualStop_.end(), peerId) != manualStop_.end();
}

void AppController::MarkManualStop(const std::string& peerId) {
  if (!ManuallyStopped(peerId)) manualStop_.push_back(peerId);
}

void AppController::ScheduleReconnect(const PairedPeer& peer) { QueueReconnect(peer, true); }

void AppController::QueueReconnect(const PairedPeer& peer, bool initiatorOnly) {
  if (!host_ || shutdown_ || ManuallyStopped(peer.id)) return;
  // 退避线程只投递设备标识，真正重连时读取界面线程上的最新配置。
  if (initiatorOnly && identity_->Id() > peer.id) return;
  host_->ScheduleReconnect(peer.id, [this, id = peer.id] {
    PostUi([this, id] {
      for (const PairedPeer& current : config_.pairedPeers)
        if (current.id == id) {
          AttemptConnect(current);
          return;
        }
    });
  });
}

// 端点表与 .NET 一致：每轮先试上次成功地址，再按重试次数轮转一个发现候选，一轮只多一次握手
std::vector<AppController::Endpoint> AppController::ConnectionEndpoints(const PairedPeer& peer) {
  std::vector<Endpoint> endpoints;
  if (!peer.address.empty()) endpoints.push_back(Endpoint{peer.address, peer.port});
  const std::vector<DiscoveredPeer> candidates = host_->Candidates(peer.id);
  if (candidates.empty()) return endpoints;
  const DiscoveredPeer& candidate = candidates[static_cast<size_t>(host_->RetryCount(peer.id)) % candidates.size()];
  if (candidate.address != peer.address || candidate.port != peer.port)
    endpoints.push_back(Endpoint{candidate.address, candidate.port});
  return endpoints;
}

void AppController::AttemptConnect(const PairedPeer& peer) {
  if (shutdown_ || ManuallyStopped(peer.id) || host_->FindSession(peer.id) ||
      IsConnecting(peer.id, peer.address, peer.port))
    return;
  StartConnection(peer.id, peer.id, peer.name, ConnectionEndpoints(peer), true, false);
}

void AppController::StartConnection(std::string key, std::string peerId, std::string name,
                                    std::vector<Endpoint> endpoints, bool automatic, bool keepManualAddress) {
  if (shutdown_ || std::any_of(connections_.begin(), connections_.end(),
                               [&key](const ConnectionTask& task) { return task.key == key; }))
    return;
  auto slot = connections_.emplace(connections_.end());
  slot->key = std::move(key);
  try {
    // 任务节点保留到界面线程回收或退出汇合，取消标记在整个握手期间有效。
    slot->worker = std::jthread([this, slot, peerId = std::move(peerId), name = std::move(name),
                                 endpoints = std::move(endpoints), automatic, keepManualAddress] {
      PeerSessionPtr session;
      std::optional<Endpoint> manualEndpoint;
      std::string failure = "没有可用连接地址";
      for (const Endpoint& endpoint : endpoints) {
        if (shutdown_ || slot->canceled.load()) break;
        try {
          session = host_->Connect(endpoint.address, endpoint.port, peerId.empty() ? nullptr : &peerId, &slot->canceled);
          if (keepManualAddress) manualEndpoint = endpoint;
          break;
        } catch (const std::exception& error) {
          failure = error.what();
        }
      }
      PostUi([this, slot, peerId, name, automatic, session = std::move(session),
              manualEndpoint = std::move(manualEndpoint), failure = std::move(failure)] {
        const bool canceled = slot->canceled.load();
        connections_.erase(slot);
        if (canceled || (session && !peerId.empty() && ManuallyStopped(peerId))) {
          if (session) session->Close();
          return;
        }
        if (session && !session->IsClosed()) {
          host_->ResetRetryCount(session->PeerId());
          OnPeerJoined(session, manualEndpoint ? &*manualEndpoint : nullptr);
          return;
        }
        if (!peerId.empty() && host_->FindSession(peerId)) return;
        if (!automatic || host_->RetryCount(peerId) <= 3) {
          Report((automatic ? "连接 " + name + " 失败：" : "连接失败：") + (session ? "会话已关闭" : failure),
                 LogLevel::Warning);
        }
        for (const PairedPeer& current : config_.pairedPeers)
          if (current.id == peerId) {
            ScheduleReconnect(current);
            return;
          }
      });
    });
  } catch (const std::system_error& error) {
    connections_.erase(slot);
    Report(std::string("无法启动连接线程：") + error.what(), LogLevel::Error);
  }
}

void AppController::CancelConnection(const std::string& key) {
  for (ConnectionTask& task : connections_)
    if (task.key == key) task.canceled.store(true);
}

// 发现即连：广播证明对端在线，发现到已配对设备且没有会话时立刻静默握手，不等退避到点。
// 先排一条退避记录再握手，同一条广播不会每个 tick 都重试一次。
void AppController::ConnectDiscovered() {
  if (!host_) return;
  for (const PairedPeer& peer : config_.pairedPeers) {
    if (peer.id == identity_->Id() || ManuallyStopped(peer.id)) continue;
    if (host_->FindSession(peer.id) || IsConnecting(peer.id, peer.address, peer.port)) continue;
    if (host_->ReconnectPending(peer.id)) continue;  // 退避还在排队：交给重连定时器
    if (host_->Candidates(peer.id).empty()) continue;
    QueueReconnect(peer, false);
    AttemptConnect(peer);
  }
}

bool AppController::IsConnecting(const std::string& id, const std::string& address, int port) {
  const std::string key = ConnectionKey(address, port);
  return std::any_of(connections_.begin(), connections_.end(), [&](const ConnectionTask& task) {
    return !task.canceled.load() && (task.key == id || task.key == key);
  });
}

std::string AppController::ConnectingName() {
  for (const ConnectionTask& task : connections_) {
    if (task.canceled.load()) continue;
    for (const PairedPeer& peer : config_.pairedPeers)
      if (peer.id == task.key && !peer.name.empty()) return peer.name;
    return task.key;
  }
  return {};
}

}  // namespace bm
