#pragma once

#include "core/platform/Common.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bm {

// 局域网里被发现的设备；设备摘要只是地址线索，信任仍由 TCP 签名握手与配对确认建立
struct DiscoveredPeer {
  std::string name;
  std::string address;
  int port = 0;
  std::string deviceId;
};

// 每 2 秒向各网卡的定向广播报出自己，同时接收别人的报出
class DiscoveryService {
 public:
  static constexpr int kDiscoveryPort = 35101;

  DiscoveryService(std::string deviceName, int servicePort, std::string deviceId);
  ~DiscoveryService();
  DiscoveryService(const DiscoveryService&) = delete;
  DiscoveryService& operator=(const DiscoveryService&) = delete;

  void Start();
  void Stop();

  // 当前未认证候选快照；不能直接当作配对设备的权威地址
  std::vector<DiscoveredPeer> Peers();
  std::vector<DiscoveredPeer> Candidates(const std::string& deviceId);
  void RemoveCandidates(const std::string& deviceId, const std::string& address, int port);

 private:
  struct Candidate {
    DiscoveredPeer peer;
    uint64_t lastSeen = 0;
  };
  struct SourceWindow {
    uint64_t started = 0;
    int count = 0;
  };

  void BroadcastLoop(const Bytes& payload);
  void ReceiveLoop();
  // 通过来源限速与解析才算接纳；返回 true 表示本次应回一个单播应答
  bool RememberCandidate(std::span<const uint8_t> payload, const std::string& address);
  void ExpireCandidates(uint64_t now);
  // 首次绑定与后续重建共用同一入口，端口不可用时按 2 秒重试
  bool EnsureSocket();
  void CloseSocket();
  // target 是 sockaddr_in；头文件不引入 Winsock 类型
  bool TrySend(const Bytes& payload, const void* target, int targetLength);
  Bytes BuildPayload() const;
  // 魔数、长度、端口与名字全部校验通过才进候选缓存
  static bool TryParse(std::span<const uint8_t> data, const std::string& address, DiscoveredPeer& peer);

  std::string deviceName_;
  std::string deviceId_;
  int servicePort_ = 0;
  Bytes payload_;

  mutable std::mutex gate_;
  std::condition_variable stopSignal_;
  std::atomic<bool> running_{false};
  bool bindFailureLogged_ = false;  // 端口被占用只提示一次，避免每两秒刷屏
  uintptr_t socket_ = static_cast<uintptr_t>(-1);
  std::thread broadcastThread_;
  std::thread receiveThread_;

  std::map<std::string, Candidate> candidates_;
  std::map<std::string, SourceWindow> sources_;
};

}  // namespace bm
