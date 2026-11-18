#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "core/clipboard/ClipboardTransfer.h"

namespace bm {

void ClipboardTransfer::ReceiveStart(std::span<const uint8_t> payload) {
  net::TransferStart start;
  if (!net::TryParseStart(payload, start)) throw std::invalid_argument("批次或文档长度非法");
  // 资格校验、接收权授予与登记共用门禁：旧 Start 不能跨过关闭/重开撤销新内容
  std::lock_guard<std::mutex> lock(mu_);
  if (!Accepts(start) || receivedIds_.find(start.id) != receivedIds_.end()) {
    NotifyCancel(start.id, "接收已关闭、代次过期或批次ID已使用");
    return;
  }
  if (start.totalLength > maxContentBytes_) {
    NotifyCancel(start.id, "内容超过本机 " + std::to_string(maxContentBytes_ / (1024 * 1024)) + " MB 上限");
    return;
  }
  auto receipt = std::make_shared<ClipboardReceipt>();
  if (!beginReceive_(receipt)) {
    receipt->Cancel();
    NotifyCancel(start.id, "本机已关闭接收");
    return;
  }
  CancelReceivedLocked("收到新的剪贴板批次", true);
  RememberIdLocked(start.id);
  auto incoming = std::make_shared<IncomingTransfer>(start, receipt);
  incoming_ = incoming;
  // 撤销提交权时只标记停止并入清理队列，不能在剪贴板锁上等待传输锁
  std::weak_ptr<IncomingTransfer> weak = incoming;
  // 撤销回调可能与传输对象析构并行，只引用批次和独立拥有的清理队列。
  receipt->SetCancelHandler([queue = std::weak_ptr<RetireQueue>(retired_), weak] {
    if (auto retired = queue.lock())
      if (auto item = weak.lock()) EnqueueRetire(retired, std::move(item));
  });
}

bool ClipboardTransfer::Accepts(const net::TransferStart& start) {
  return !disposed_ && enabled_ && peerEnabled_ && start.receiverEpoch == epoch_ && start.senderEpoch == peerEpoch_;
}

void ClipboardTransfer::HandleBlock(const Frame& frame) {
  net::TransferId id;
  if (!net::TryGetTransferId(frame.payload, id)) throw std::invalid_argument("数据块缺少批次ID");
  std::shared_ptr<IncomingTransfer> incoming;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!disposed_ && incoming_ && incoming_->start.id == id) incoming = incoming_;
  }
  if (!incoming) return;  // 旧批次的残留块：仍要归还信用
  std::unique_lock<std::mutex> work(incoming->work);
  // 取消立即生效：已撤销批次的数据块不再写入磁盘
  if (incoming->stopped.load() || stop_.load()) return;
  if (frame.type == MessageType::ClipboardChunk) ReceiveDocument(incoming, frame.payload);
  else ReceiveFile(incoming, frame.payload);
}

void ClipboardTransfer::ReceiveDocument(const std::shared_ptr<IncomingTransfer>& incoming,
                                        std::span<const uint8_t> payload) {
  net::TransferChunk chunk;
  if (!net::TryParseChunk(payload, chunk)) throw std::invalid_argument("文档块编码非法");
  if (incoming->documentComplete || chunk.offset != incoming->received ||
      static_cast<int64_t>(chunk.data.size()) > incoming->start.totalLength - incoming->received)
    throw std::invalid_argument("文档偏移重复、不连续或超过声明长度");
  const int64_t resulting = incoming->received + static_cast<int64_t>(chunk.data.size());
  if (chunk.last != (resulting == incoming->start.totalLength))
    throw std::invalid_argument("文档尾块与声明长度不一致");
  if (incoming->document.empty() && incoming->start.totalLength != 0)
    incoming->document.resize(static_cast<size_t>(incoming->start.totalLength));
  if (!chunk.data.empty())
    std::memcpy(incoming->document.data() + incoming->received, chunk.data.data(), chunk.data.size());
  incoming->received = resulting;
  incoming->lastProgress.store(TickMs());
  if (!chunk.last) return;
  incoming->documentComplete = true;

  if (incoming->start.kind == net::ClipboardContentKind::Files) {
    std::vector<net::FileEntry> files;
    if (!net::TryParseManifest(incoming->document, files)) throw std::invalid_argument("文件清单或批次总量非法");
    if (incoming->stopped.load()) throw TransferCancelled{};
    incoming->files = std::make_shared<FileTransfer>(incoming->start.id, files);
    incoming->document = {};  // 清单已解析，释放文档缓冲
    RequestFile(incoming);
    return;
  }
  if (incoming->start.kind == net::ClipboardContentKind::Text) {
    std::string text;
    if (!incoming->document.empty())
      text.assign(reinterpret_cast<const char*>(incoming->document.data()), incoming->document.size());
    if (!net::IsValidUtf8(text)) throw std::invalid_argument("文档不是有效UTF-8文本");
    Publish(incoming, std::move(text), {}, nullptr);
    return;
  }
  Publish(incoming, std::string(), std::move(incoming->document), nullptr);
}

void ClipboardTransfer::ReceiveFile(const std::shared_ptr<IncomingTransfer>& incoming,
                                    std::span<const uint8_t> payload) {
  net::FileChunk chunk;
  if (!net::TryParseChunk(payload, chunk)) throw std::invalid_argument("文件块编码非法");
  if (!incoming->documentComplete || !incoming->files) throw std::invalid_argument("文件清单尚未完成");
  const bool fileComplete = incoming->files->Accept(chunk, incoming->stopped);
  incoming->lastProgress.store(TickMs());
  if (!fileComplete) return;
  if (incoming->files->Complete()) Publish(incoming, std::string(), {}, incoming->files);
  else RequestFile(incoming);
}

void ClipboardTransfer::RequestFile(const std::shared_ptr<IncomingTransfer>& incoming) {
  link_.Send(MessageType::FileRequest, net::EncodeRequest(incoming->start.id, incoming->files->NextIndex()));
}

void ClipboardTransfer::Publish(const std::shared_ptr<IncomingTransfer>& incoming, std::string text, Bytes image,
                                std::shared_ptr<FileTransfer> files) {
  if (incoming->stopped.load()) throw TransferCancelled{};
  // 在后台准备持久交付意图；只有 STA 成功写入剪贴板后才把该批次标为已交付
  std::vector<std::wstring> paths;
  std::function<void()> applied;
  if (files) {
    paths = files->PrepareDelivery();
    applied = [files] { files->MarkDelivered(); };
  }
  // 提交权在锁外取值，避免持传输锁等待正在写入剪贴板的提交
  const bool receiptValid = incoming->receipt->IsValid();
  Received content;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!receiptValid || incoming_.get() != incoming.get() || !Accepts(incoming->start)) return;
    incoming_.reset();
    completedIncoming_ = incoming;
    content.id = incoming->start.id;
    content.kind = incoming->start.kind;
    content.text = std::move(text);
    content.image = std::move(image);
    content.files = std::move(paths);
    content.receipt = incoming->receipt;
    content.applied = std::move(applied);
  }
  if (completed_) completed_(std::move(content));
}

void ClipboardTransfer::ReceiveLoop() {
  try {
    for (;;) {
      Frame frame;
      if (receive_.Pop(frame, stop_) != WaitResult::Signalled) break;
      try {
        HandleBlock(frame);
      } catch (const TransferCancelled&) {
      } catch (const Cancelled&) {
      } catch (const std::exception& error) {
        RejectEncoding(frame.payload, std::string("接收失败：") + error.what());
      }
      // 被取消或替换批次的旧块也归还信用；全会话计数不随新 Start 重置
      if (!stop_.load()) {
        consumedBlocks_ += 1;
        link_.Send(MessageType::TransferCredit, net::EncodeCredit(consumedBlocks_));
      }
    }
  } catch (const std::exception& error) {
    Report(std::string("接收泵已停止：") + error.what());
    Stop();
  }
}

}  // namespace bm
