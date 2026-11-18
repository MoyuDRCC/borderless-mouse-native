#include "core/clipboard/ClipboardTransfer.h"

#include <stdexcept>

namespace bm {

ClipboardTransfer::SourceTransfer::SourceTransfer(std::shared_ptr<const ClipboardSnapshot> snapshot, uint64_t epoch,
                                                 uint64_t senderEpoch)
    : id(net::NewTransferId()), snapshot(std::move(snapshot)), epoch(epoch), senderEpoch(senderEpoch) {}

void ClipboardTransfer::SourceTransfer::Cancel(std::string why, bool notify) {
  reason = std::move(why);
  notifyCancel = notify;
  stopped.store(true);
  requests.Close();  // 唤醒等待文件请求的发送泵
}

ClipboardTransfer::IncomingTransfer::IncomingTransfer(const net::TransferStart& start,
                                                      std::shared_ptr<ClipboardReceipt> receipt)
    : start(start), receipt(std::move(receipt)), lastProgress(TickMs()) {}

ClipboardTransfer::ClipboardTransfer(ClipLink& link, BeginReceive beginReceive, CompletedCallback completed,
                                     ReportCallback report)
    : link_(link),
      beginReceive_(std::move(beginReceive)),
      completed_(std::move(completed)),
      report_(std::move(report)),
      credits_(net::kCreditWindow),
      receive_(net::kCreditWindow),
      wake_(1),
      notifications_(kNotificationCapacity),
      retired_(std::make_shared<RetireQueue>(0)) {
  sendThread_ = std::thread(&ClipboardTransfer::SendLoop, this);
  receiveThread_ = std::thread(&ClipboardTransfer::ReceiveLoop, this);
  notificationThread_ = std::thread(&ClipboardTransfer::NotificationLoop, this);
  maintenanceThread_ = std::thread(&ClipboardTransfer::MaintenanceLoop, this);
}

ClipboardTransfer::~ClipboardTransfer() { Dispose(); }

void ClipboardTransfer::SetMaxContentBytes(int64_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  maxContentBytes_ = bytes;
}

void ClipboardTransfer::SetEnabled(bool enabled) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (disposed_ || stop_.load()) return;
    enabled_ = enabled;
    epoch_ += 1;
    receivedIds_.clear();
    recentIds_.clear();
    CancelSourceLocked("剪贴板同步状态已改变", true);
    CancelReceivedLocked("剪贴板同步状态已改变", true);
    if (!enabled) latest_.reset();
    link_.Send(MessageType::ClipboardState, net::EncodeState(enabled, epoch_));
  }
  Wake();
}

void ClipboardTransfer::SetCurrent(std::shared_ptr<const ClipboardSnapshot> snapshot) {
  bool oversized = false;
  int64_t limit = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (disposed_ || stop_.load() || snapshot->IsRevoked() || snapshot->Version() < revision_) return;
    if (latest_.get() == snapshot.get()) return;
    revision_ = snapshot->Version();
    // 不支持的内容也占住当前版本，旧发布不能趁此重发
    const bool publishable = snapshot->CanPublish();
    const bool fits = publishable && Fits(*snapshot->Content());
    oversized = publishable && !fits;
    limit = maxContentBytes_;
    latest_ = fits ? std::move(snapshot) : nullptr;
    CancelSourceLocked("剪贴板当前内容已改变", true);
    // 接收只由剪贴板当前 receipt 撤销；迟到的本地快照通知不能取消较新的远端接收
  }
  if (oversized) Report("剪贴板内容超过 " + std::to_string(limit / (1024 * 1024)) + " MB 上限，未发送");
  Wake();
}

bool ClipboardTransfer::Fits(const ClipboardContent& content) {
  switch (content.kind) {
    case net::ClipboardContentKind::Text:
      return static_cast<int64_t>(content.text.size()) <= maxContentBytes_;
    case net::ClipboardContentKind::Image:
      return static_cast<int64_t>(content.image.size()) <= maxContentBytes_;
    default:
      return true;  // 文件批次由清单与总量单独限制
  }
}

void ClipboardTransfer::OnFrame(MessageType type, std::span<const uint8_t> payload) {
  if (stop_.load()) return;
  try {
    switch (type) {
      case MessageType::ClipboardState:
        ReceiveState(payload);
        break;
      case MessageType::TransferCancel:
        ReceiveCancel(payload);
        break;
      case MessageType::TransferCredit:
        ReceiveCredit(payload);
        break;
      case MessageType::FileRequest:
        ReceiveRequest(payload);
        break;
      case MessageType::ClipboardStart:
        ReceiveStart(payload);
        break;
      case MessageType::ClipboardChunk:
      case MessageType::FileData:
        // 正常发送方全会话最多有 8 块在途，换批次也不能突破这个窗口
        if (!receive_.TryPush(Frame{type, Bytes(payload.begin(), payload.end())})) {
          Report("对端超过接收信用窗口，已关闭连接");
          link_.Close();
        }
        break;
      default:
        break;  // 其它业务帧不属于剪贴板协议
    }
  } catch (const std::exception& error) {
    RejectEncoding(payload, std::string("接收控制消息失败：") + error.what());
  }
}

void ClipboardTransfer::ReceiveState(std::span<const uint8_t> payload) {
  net::ClipboardState state;
  if (!net::TryParseState(payload, state)) throw std::invalid_argument("剪贴板状态编码非法");
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (disposed_ || state.epoch <= peerEpoch_) return;
    peerEpoch_ = state.epoch;
    peerEnabled_ = state.enabled;
    CancelSourceLocked("对端接收代次已改变", false);
    CancelReceivedLocked("对端同步代次已改变", false);
  }
  Wake();
}

void ClipboardTransfer::ReceiveCancel(std::span<const uint8_t> payload) {
  net::TransferCancellation cancellation;
  if (!net::TryParseCancel(payload, cancellation)) throw std::invalid_argument("取消消息编码非法");
  bool pending = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    RememberIdLocked(cancellation.id);
    if (source_ && source_->id == cancellation.id) {
      source_->Cancel("对端取消：" + cancellation.reason, false);
      pending = true;
    }
    if (incoming_ && incoming_->start.id == cancellation.id) {
      CancelReceivedLocked("对端取消：" + cancellation.reason, false);
      pending = true;
    } else if (completedIncoming_ && completedIncoming_->start.id == cancellation.id) {
      // 已交付批次只回收资源；内容已经写进剪贴板，提示用户只会制造噪音
      CancelReceivedLocked("对端取消：" + cancellation.reason, false);
    }
  }
  if (pending) Report("对端取消传输：" + cancellation.reason);
  Wake();
}

void ClipboardTransfer::ReceiveCredit(std::span<const uint8_t> payload) {
  uint64_t consumed = 0;
  int returned = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (disposed_) return;
    if (!net::TryParseCredit(payload, consumed) || consumed <= acknowledgedBlocks_ || consumed > sentBlocks_) {
      link_.Close();
      return;
    }
    returned = static_cast<int>(consumed - acknowledgedBlocks_);
    acknowledgedBlocks_ = consumed;
  }
  credits_.Release(returned);
}

void ClipboardTransfer::ReceiveRequest(std::span<const uint8_t> payload) {
  net::FileRequest request;
  if (!net::TryParseRequest(payload, request)) throw std::invalid_argument("文件请求编码非法");
  bool invalid = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    const std::shared_ptr<SourceTransfer> source = source_;
    if (!source || source->id != request.id || source->stopped.load() || !source->files) return;
    const size_t count = source->files->paths.size();
    if (static_cast<size_t>(request.index) >= count || !source->requested.insert(request.index).second ||
        !source->requests.TryPush(request.index)) {
      source->Cancel("文件请求重复、越界或请求队列超限", true);
      invalid = true;
    }
  }
  if (invalid) Report("非法文件请求，已取消对应批次");
}

void ClipboardTransfer::RejectEncoding(std::span<const uint8_t> payload, std::string reason) {
  Report(reason);
  net::TransferId id;
  if (!net::TryGetTransferId(payload, id)) return;
  {
    std::lock_guard<std::mutex> lock(mu_);
    RememberIdLocked(id);
    if (source_ && source_->id == id) source_->Cancel(reason, true);
    if ((incoming_ && incoming_->start.id == id) || (completedIncoming_ && completedIncoming_->start.id == id))
      CancelReceivedLocked(reason, true);
    else
      NotifyCancel(id, reason);
  }
  Wake();
}

void ClipboardTransfer::RememberIdLocked(const net::TransferId& id) {
  if (!receivedIds_.insert(id).second) return;
  recentIds_.push_back(id);
  while (recentIds_.size() > kReceivedIdHistory) {
    receivedIds_.erase(recentIds_.front());
    recentIds_.pop_front();
  }
}

void ClipboardTransfer::CancelSourceLocked(std::string_view reason, bool notify) {
  if (source_) source_->Cancel(std::string(reason), notify);
}

void ClipboardTransfer::CancelReceivedLocked(std::string_view reason, bool notify) {
  std::shared_ptr<IncomingTransfer> incoming = std::move(incoming_);
  std::shared_ptr<IncomingTransfer> completed = std::move(completedIncoming_);
  std::shared_ptr<IncomingTransfer>* const items[] = {&incoming, &completed};
  for (std::shared_ptr<IncomingTransfer>* item : items) {
    if (!*item) continue;
    const net::TransferId id = (*item)->start.id;
    (*item)->receipt->Cancel();
    if (notify) NotifyCancel(id, reason);
    EnqueueRetire(retired_, *item);
  }
}

void ClipboardTransfer::NotifyCancel(const net::TransferId& id, std::string_view reason) {
  if (stop_.load()) return;
  if (!notifications_.TryPush(net::TransferCancellation{id, std::string(reason)})) {
    Report("取消通知过载，已关闭连接以撤销对端任务");
    link_.Close();
  }
}

void ClipboardTransfer::Wake() { wake_.TryPush(true); }

void ClipboardTransfer::EnqueueRetire(const std::shared_ptr<RetireQueue>& queue,
                                      std::shared_ptr<IncomingTransfer> incoming) {
  if (incoming->retired.exchange(true)) return;
  // 只做标记与入队：调用方可能正持传输锁或剪贴板锁
  incoming->stopped.store(true);
  queue->TryPush(std::move(incoming));
}

void ClipboardTransfer::CheckTimeout() {
  bool timedOut = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (disposed_ || !incoming_) return;
    if (incoming_->stopped.load()) {
      CancelReceivedLocked("本机已有更新内容", true);
      return;
    }
    if (TickMs() - incoming_->lastProgress.load() < kProgressTimeoutMs) return;
    CancelReceivedLocked("接收60秒没有有效进展", true);
    timedOut = true;
  }
  if (timedOut) Report("接收60秒没有有效进展，已取消并清理残片");
}

void ClipboardTransfer::CleanupIncoming(const std::shared_ptr<IncomingTransfer>& incoming) {
  try {
    // 等当前数据块退出，再释放文件与文档缓冲
    std::lock_guard<std::mutex> lock(incoming->work);
    if (incoming->files) incoming->files->Dispose();
    incoming->document = {};
  } catch (const std::exception& error) {
    Report(std::string("批次清理失败：") + error.what());
  }
}

void ClipboardTransfer::MaintenanceLoop() {
  try {
    for (;;) {
      std::shared_ptr<IncomingTransfer> incoming;
      const WaitResult result = retired_->Pop(incoming, stop_, 1000);
      if (result == WaitResult::Signalled) {
        CleanupIncoming(incoming);
        continue;
      }
      if (stop_.load()) break;  // 收尾：剩余残片交给批次析构回收
      CheckTimeout();
    }
  } catch (const std::exception& error) {
    Report(std::string("接收超时检查已停止：") + error.what());
  }
}

void ClipboardTransfer::Report(std::string message) {
  // 报告回调在传输线程上执行，抛出会打断泵；调用方自己保证线程安全
  try {
    if (report_) report_(std::move(message));
  } catch (const std::exception&) {
  }
}

void ClipboardTransfer::Stop() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (disposed_) return;
    disposed_ = true;
    enabled_ = false;
    latest_.reset();
    CancelSourceLocked("连接传输已关闭", false);
    CancelReceivedLocked("连接传输已关闭", false);
    stop_.store(true);
  }
  receive_.Close();
  notifications_.Close();
  wake_.Close();
  credits_.Close();
  retired_->Close();
}

void ClipboardTransfer::Join() {
  const std::thread::id current = std::this_thread::get_id();
  std::thread* const threads[] = {&sendThread_, &receiveThread_, &notificationThread_, &maintenanceThread_};
  for (std::thread* thread : threads) {
    if (thread->joinable() && thread->get_id() != current) thread->join();
  }
}

void ClipboardTransfer::Dispose() {
  Stop();
  Join();
}

}  // namespace bm
