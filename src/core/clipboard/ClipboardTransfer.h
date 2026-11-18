#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "core/clipboard/ClipboardContent.h"
#include "core/clipboard/ClipboardReceipt.h"
#include "core/platform/Common.h"
#include "core/protocol/FileCodec.h"
#include "core/clipboard/FileTransfer.h"
#include "core/protocol/FrameCodec.h"
#include "core/protocol/TransferCodec.h"

namespace bm {

enum class WaitResult { Signalled, Cancelled, TimedOut };

constexpr uint64_t kNoTimeout = static_cast<uint64_t>(-1);
// 文件请求、数据块信用与接收进展共用的无进展上限
constexpr uint64_t kProgressTimeoutMs = 60000;
// 取消与状态消息的排队上限：超过即认为对端失控
constexpr size_t kNotificationCapacity = 64;
constexpr size_t kReceivedIdHistory = 2048;

// 定长队列：满时 TryPush 失败，由调用方决定是否关闭连接；容量 0 表示不限长
template <typename T>
class ClipQueue {
 public:
  explicit ClipQueue(size_t capacity) : capacity_(capacity) {}
  ClipQueue(const ClipQueue&) = delete;
  ClipQueue& operator=(const ClipQueue&) = delete;

  bool TryPush(T value) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (closed_ || (capacity_ != 0 && items_.size() >= capacity_)) return false;
      items_.push_back(std::move(value));
    }
    ready_.notify_one();
    return true;
  }

  // 阻塞取一项：先取已排队项；关闭或调用方中止返回 Cancelled，超时返回 TimedOut
  WaitResult Pop(T& out, const std::atomic<bool>& abort, uint64_t timeoutMs = kNoTimeout) {
    std::unique_lock<std::mutex> lock(mu_);
    const auto ready = [&] { return closed_ || !items_.empty() || abort.load(); };
    if (timeoutMs == kNoTimeout) {
      ready_.wait(lock, ready);
    } else if (!ready_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready)) {
      return WaitResult::TimedOut;
    }
    if (!items_.empty()) {
      out = std::move(items_.front());
      items_.pop_front();
      return WaitResult::Signalled;
    }
    return WaitResult::Cancelled;
  }

  void Close() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      closed_ = true;
    }
    ready_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable ready_;
  std::deque<T> items_;
  size_t capacity_;
  bool closed_ = false;
};

// 全会话共享的数据块信用：换批次不重置，旧块也必须归还额度
class ClipCredits {
 public:
  explicit ClipCredits(int count) : available_(count) {}
  ClipCredits(const ClipCredits&) = delete;
  ClipCredits& operator=(const ClipCredits&) = delete;

  // 取得一块发送额度；中止或关闭返回 Cancelled，超时返回 TimedOut
  WaitResult Acquire(const std::atomic<bool>& abort, uint64_t timeoutMs) {
    std::unique_lock<std::mutex> lock(mu_);
    const auto ready = [&] { return available_ > 0 || closed_ || abort.load(); };
    if (!ready_.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready)) return WaitResult::TimedOut;
    if (closed_ || abort.load()) return WaitResult::Cancelled;
    available_ -= 1;
    return WaitResult::Signalled;
  }

  void Release(int count) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      available_ += count;
    }
    ready_.notify_all();
  }

  void Close() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      closed_ = true;
    }
    ready_.notify_all();
  }

 private:
  std::mutex mu_;
  std::condition_variable ready_;
  int available_;
  bool closed_ = false;
};

// 发送出口：由 app 层用 PeerSession 适配，传输侧不直接依赖会话实现。
// 远端代次属于剪贴板协议本身（ClipboardState），由传输在线协商，链接层只负责收发。
class ClipLink {
 public:
  virtual ~ClipLink() = default;
  // 可靠帧（状态/取消/信用/文件请求）：非阻塞，会话未接受返回 false，不得抛出。
  // 载荷在返回后即失效，适配层要排队就必须自己复制
  virtual bool Send(MessageType type, std::span<const uint8_t> payload) = 0;
  // 大数据帧（Start/文档块/文件块）：队列满时阻塞到会话接受，aborted 或会话关闭返回 false
  virtual bool SendBulk(MessageType type, std::span<const uint8_t> payload, const std::atomic<bool>& aborted) = 0;
  virtual bool IsClosed() const = 0;
  // 协议违规或信用失控时撤销整条会话
  virtual void Close() = 0;
};

// 控制消息即时处理；数据块由独立泵消费，固定信用窗口限制全会话的在途数据。
// app 侧接线：
//   1. ClipLink 适配 PeerSession（Send 走 TrySend，SendBulk 走 SendBulk，Close 关闭会话）；
//   2. 会话接收线程收到业务帧后调用 OnFrame，传输不订阅会话实现；
//   3. BeginReceive 在剪贴板状态锁内授予接收权（不得回调传输对象）；
//   4. Completed 回调运行在接收线程上，写剪贴板成功后调用 content.applied；
//   5. 会话结束时 Dispose，等全部泵与残片清理退出。
class ClipboardTransfer {
 public:
  // 一次接收的成果；写剪贴板成功后由 app 调用 applied 标记文件批次已交付
  struct Received {
    net::TransferId id;
    net::ClipboardContentKind kind = net::ClipboardContentKind::Text;
    std::string text;                 // 文本内容（UTF-8）
    Bytes image;                      // PNG 文档
    std::vector<std::wstring> files;  // 落盘后的文件路径
    std::shared_ptr<ClipboardReceipt> receipt;
    std::function<void()> applied;
  };

  using BeginReceive = std::function<bool(const std::shared_ptr<ClipboardReceipt>&)>;
  using CompletedCallback = std::function<void(Received)>;
  using ReportCallback = std::function<void(std::string)>;

  ClipboardTransfer(ClipLink& link, BeginReceive beginReceive, CompletedCallback completed, ReportCallback report);
  ~ClipboardTransfer();
  ClipboardTransfer(const ClipboardTransfer&) = delete;
  ClipboardTransfer& operator=(const ClipboardTransfer&) = delete;

  // 单条内容字节上限：本地设置比协议上限更严时按本地来，两侧都校验
  void SetMaxContentBytes(int64_t bytes);
  // 同步开关：推进本端代次并撤销所有在途批次
  void SetEnabled(bool enabled);
  // 会话只订阅进程当前版本；远端来源只撤销旧发送，不取消这次接收
  void SetCurrent(std::shared_ptr<const ClipboardSnapshot> snapshot);
  // 会话线程收到业务帧后调用；只有剪贴板相关类型会被处理
  void OnFrame(MessageType type, std::span<const uint8_t> payload);
  // app 在写入剪贴板前复核：批次仍是最新、接收权有效且两侧都开启
  // 幂等：关闭全部泵并等待线程退出
  void Dispose();

 private:
  // 一次本地发送；撤销同时停止生产者并让下一批接管
  struct SourceTransfer {
    SourceTransfer(std::shared_ptr<const ClipboardSnapshot> snapshot, uint64_t epoch, uint64_t senderEpoch);
    const net::TransferId id;
    std::shared_ptr<const ClipboardSnapshot> snapshot;
    uint64_t epoch = 0;
    uint64_t senderEpoch = 0;
    std::atomic<bool> stopped{false};
    std::string reason = "传输已取消";
    bool notifyCancel = true;
    bool started = false;                    // 受传输锁保护
    std::shared_ptr<const FileOffer> files;  // 受传输锁保护
    ClipQueue<int> requests{16};
    std::unordered_set<int> requested;  // 受传输锁保护
    void Cancel(std::string why, bool notify);
  };

  // 一次接收：文档缓冲、文件批次与进展时间都随批次撤销一起回收
  struct IncomingTransfer {
    IncomingTransfer(const net::TransferStart& start, std::shared_ptr<ClipboardReceipt> receipt);
    net::TransferStart start;
    std::shared_ptr<ClipboardReceipt> receipt;
    std::atomic<bool> stopped{false};
    std::atomic<bool> retired{false};
    std::mutex work;  // 与清理线程串行，避免删除正在写入的文件
    Bytes document;
    int64_t received = 0;
    bool documentComplete = false;
    std::shared_ptr<FileTransfer> files;
    std::atomic<uint64_t> lastProgress{0};
  };

  struct Frame {
    MessageType type = MessageType::Heartbeat;
    Bytes payload;
  };

  // 传输被撤销：不是协议错误，不报告也不发取消
  struct Cancelled {};

  void SendLoop();
  void ReceiveLoop();
  void NotificationLoop();
  void MaintenanceLoop();

  void SendSource(SourceTransfer& source);
  void SendFile(SourceTransfer& source, int index);
  void SendBlock(MessageType type, std::span<const uint8_t> payload, const std::atomic<bool>& abort);
  void SendCancellation(const net::TransferId& id, std::string_view reason);

  void ReceiveState(std::span<const uint8_t> payload);
  void ReceiveCancel(std::span<const uint8_t> payload);
  void ReceiveCredit(std::span<const uint8_t> payload);
  void ReceiveRequest(std::span<const uint8_t> payload);
  void ReceiveStart(std::span<const uint8_t> payload);
  void HandleBlock(const Frame& frame);
  void ReceiveDocument(const std::shared_ptr<IncomingTransfer>& incoming, std::span<const uint8_t> payload);
  void ReceiveFile(const std::shared_ptr<IncomingTransfer>& incoming, std::span<const uint8_t> payload);
  void RequestFile(const std::shared_ptr<IncomingTransfer>& incoming);
  void Publish(const std::shared_ptr<IncomingTransfer>& incoming, std::string text, Bytes image,
               std::shared_ptr<FileTransfer> files);

  void CancelSourceLocked(std::string_view reason, bool notify);    // 需持 mu_
  void CancelReceivedLocked(std::string_view reason, bool notify);  // 需持 mu_
  void NotifyCancel(const net::TransferId& id, std::string_view reason);
  void RememberIdLocked(const net::TransferId& id);                 // 需持 mu_
  void RejectEncoding(std::span<const uint8_t> payload, std::string reason);
  bool Accepts(const net::TransferStart& start);  // 需持 mu_
  bool Fits(const ClipboardContent& content);
  void Wake();
  void CheckTimeout();
  using RetireQueue = ClipQueue<std::shared_ptr<IncomingTransfer>>;
  static void EnqueueRetire(const std::shared_ptr<RetireQueue>& queue, std::shared_ptr<IncomingTransfer> incoming);
  void CleanupIncoming(const std::shared_ptr<IncomingTransfer>& incoming);
  void Report(std::string message);
  void Stop();
  void Join();

  ClipLink& link_;
  BeginReceive beginReceive_;
  CompletedCallback completed_;
  ReportCallback report_;

  std::mutex mu_;
  std::atomic<bool> stop_{false};
  bool disposed_ = false;
  bool enabled_ = false;
  bool peerEnabled_ = false;
  uint64_t epoch_ = 0;
  uint64_t peerEpoch_ = 0;
  uint64_t sentEpoch_ = 0;
  uint64_t sentSenderEpoch_ = 0;
  uint64_t sentBlocks_ = 0;
  uint64_t acknowledgedBlocks_ = 0;
  uint64_t consumedBlocks_ = 0;  // 仅接收泵写入
  int64_t revision_ = -1;
  int64_t sentRevision_ = -1;
  int64_t maxContentBytes_ = net::kMaxContentLength;
  std::shared_ptr<const ClipboardSnapshot> latest_;
  std::shared_ptr<SourceTransfer> source_;
  std::shared_ptr<IncomingTransfer> incoming_;
  std::shared_ptr<IncomingTransfer> completedIncoming_;
  std::unordered_set<net::TransferId, net::TransferIdHash> receivedIds_;
  std::deque<net::TransferId> recentIds_;

  ClipCredits credits_;
  ClipQueue<Frame> receive_;
  ClipQueue<bool> wake_;
  ClipQueue<net::TransferCancellation> notifications_;
  std::shared_ptr<RetireQueue> retired_;

  std::thread sendThread_;
  std::thread receiveThread_;
  std::thread notificationThread_;
  std::thread maintenanceThread_;
};

}  // namespace bm
