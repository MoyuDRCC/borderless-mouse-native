#include <windows.h>

#include <algorithm>
#include <stdexcept>

#include "core/clipboard/ClipboardTransfer.h"

namespace bm {
namespace {

// 发送侧只读句柄：按块读取源文件，长度变化即视为协议错误
class SourceFile {
 public:
  explicit SourceFile(const std::wstring& path) {
    handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
      throw std::runtime_error("无法打开源文件：" + ToUtf8(path) + "（错误码 " + std::to_string(GetLastError()) + "）");
  }
  ~SourceFile() { CloseHandle(handle_); }
  SourceFile(const SourceFile&) = delete;
  SourceFile& operator=(const SourceFile&) = delete;

  int64_t Length() const {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle_, &size)) throw std::runtime_error("无法读取源文件长度");
    return size.QuadPart;
  }

  size_t Read(std::span<uint8_t> buffer) const {
    DWORD read = 0;
    if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
      throw std::runtime_error("读取源文件失败");
    return read;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

}  // namespace

void ClipboardTransfer::SendLoop() {
  try {
    for (;;) {
      bool dirty = false;
      if (wake_.Pop(dirty, stop_) != WaitResult::Signalled) break;
      for (;;) {
        std::shared_ptr<SourceTransfer> source;
        {
          std::lock_guard<std::mutex> lock(mu_);
          if (disposed_ || !enabled_ || !peerEnabled_ || !latest_ || !latest_->CanPublish() || peerEpoch_ == 0 ||
              (sentRevision_ == revision_ && sentEpoch_ == peerEpoch_ && sentSenderEpoch_ == epoch_))
            break;
          source = std::make_shared<SourceTransfer>(latest_, peerEpoch_, epoch_);
          source_ = source;
          sentRevision_ = revision_;
          sentEpoch_ = peerEpoch_;
          sentSenderEpoch_ = epoch_;
        }
        bool completed = false;
        std::string failure;
        try {
          SendSource(*source);
          completed = true;
        } catch (const Cancelled&) {
        } catch (const std::exception& error) {
          std::lock_guard<std::mutex> lock(mu_);
          source->Cancel(std::string("发送失败：") + error.what(), true);
          if (latest_.get() == source->snapshot.get()) latest_.reset();
          failure = error.what();
        }
        if (!failure.empty() && !stop_.load()) Report("剪贴板发送失败：" + failure);
        bool notify = false;
        std::string reason;
        {
          std::lock_guard<std::mutex> lock(mu_);
          reason = source->reason;
          notify = !completed && source->started && source->notifyCancel && !stop_.load();
        }
        // 旧生产者完全退出后才发取消和下一批 Start；它们与已排队的旧块共享 FIFO
        if (notify) SendCancellation(source->id, reason);
        {
          std::lock_guard<std::mutex> lock(mu_);
          if (source_ == source) source_.reset();
        }
      }
    }
  } catch (const std::exception& error) {
    Report(std::string("剪贴板发送泵已停止：") + error.what());
    Stop();
  }
}

void ClipboardTransfer::SendSource(SourceTransfer& source) {
  if (source.stopped.load()) throw Cancelled{};
  const ClipboardContent& offer = *source.snapshot->Content();
  Bytes manifest;
  std::span<const uint8_t> document;
  switch (offer.kind) {
    case net::ClipboardContentKind::Text:
      if (static_cast<int64_t>(offer.text.size()) > net::kMaxContentLength)
        throw std::runtime_error("文本超过64MiB资源上限");
      document = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(offer.text.data()), offer.text.size());
      break;
    case net::ClipboardContentKind::Image:
      document = offer.image;
      break;
    case net::ClipboardContentKind::Files: {
      std::shared_ptr<FileOffer> files;
      try {
        files = std::make_shared<FileOffer>(FileTransfer::PrepareOffer(offer.paths, source.stopped));
      } catch (const TransferCancelled&) {
        throw Cancelled{};
      }
      manifest = files->manifest;
      document = manifest;
      std::lock_guard<std::mutex> lock(mu_);
      source.files = files;
      break;
    }
    default:
      throw std::runtime_error("不支持的剪贴板类型");
  }
  if (source.stopped.load()) throw Cancelled{};
  const net::TransferStart start{source.id, source.epoch, source.senderEpoch, offer.kind,
                                 static_cast<int64_t>(document.size())};
  {
    // 先标记已公告：即使随后被新内容取消，也必须发送同 ID 的取消消息
    std::lock_guard<std::mutex> lock(mu_);
    if (source.stopped.load()) throw Cancelled{};
    source.started = true;
  }
  if (!link_.SendBulk(MessageType::ClipboardStart, net::EncodeStart(start), source.stopped)) throw Cancelled{};
  if (document.empty()) {
    SendBlock(MessageType::ClipboardChunk, net::EncodeChunk(source.id, 0, true, {}), source.stopped);
  } else {
    for (size_t offset = 0; offset < document.size(); offset += net::kChunkSize) {
      const size_t count = std::min(net::kChunkSize, document.size() - offset);
      const bool last = offset + count == document.size();
      SendBlock(MessageType::ClipboardChunk,
                net::EncodeChunk(source.id, static_cast<int64_t>(offset), last, document.subspan(offset, count)),
                source.stopped);
    }
  }

  std::shared_ptr<const FileOffer> files;
  {
    std::lock_guard<std::mutex> lock(mu_);
    files = source.files;
  }
  if (!files) return;
  // 清单之后逐个回应文件请求，每个文件一条连续数据流
  for (size_t sent = 0; sent < files->paths.size(); sent++) {
    int index = 0;
    const WaitResult result = source.requests.Pop(index, source.stopped, kProgressTimeoutMs);
    if (result == WaitResult::TimedOut) throw std::runtime_error("60秒未收到有效文件请求");
    if (result != WaitResult::Signalled) throw Cancelled{};
    SendFile(source, index);
  }
}

void ClipboardTransfer::SendFile(SourceTransfer& source, int index) {
  std::shared_ptr<const FileOffer> files;
  {
    std::lock_guard<std::mutex> lock(mu_);
    files = source.files;
  }
  const int64_t length = files->files[static_cast<size_t>(index)].length;
  const SourceFile file(files->paths[static_cast<size_t>(index)]);
  if (file.Length() != length) throw std::runtime_error("源文件在清单建立后长度改变");
  if (length == 0) {
    SendBlock(MessageType::FileData, net::EncodeChunk(source.id, index, 0, true, {}), source.stopped);
    return;
  }
  Bytes buffer(net::kFileChunkSize);
  int64_t offset = 0;
  while (offset < length) {
    const size_t requested = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(buffer.size()), length - offset));
    const size_t read = file.Read(std::span<uint8_t>(buffer.data(), requested));
    if (read == 0) throw std::runtime_error("源文件在读取完成前结束");
    SendBlock(MessageType::FileData,
              net::EncodeChunk(source.id, index, offset, offset + static_cast<int64_t>(read) == length,
                               std::span<const uint8_t>(buffer.data(), read)),
              source.stopped);
    offset += static_cast<int64_t>(read);
  }
}

void ClipboardTransfer::SendBlock(MessageType type, std::span<const uint8_t> payload,
                                 const std::atomic<bool>& abort) {
  const WaitResult result = credits_.Acquire(abort, kProgressTimeoutMs);
  if (result == WaitResult::TimedOut) throw std::runtime_error("60秒未收到数据处理确认");
  if (result != WaitResult::Signalled) throw Cancelled{};
  {
    std::lock_guard<std::mutex> lock(mu_);
    sentBlocks_ += 1;
  }
  // 已占信用的块必须入队或关闭会话，不因换批次丢失额度；旧块在接收端仍会归还信用
  if (!link_.SendBulk(type, payload, abort)) throw Cancelled{};
}

void ClipboardTransfer::NotificationLoop() {
  try {
    for (;;) {
      net::TransferCancellation notice;
      if (notifications_.Pop(notice, stop_) != WaitResult::Signalled) break;
      SendCancellation(notice.id, notice.reason);
    }
  } catch (const std::exception& error) {
    Report(std::string("传输取消通知已停止：") + error.what());
    Stop();
  }
}

void ClipboardTransfer::SendCancellation(const net::TransferId& id, std::string_view reason) {
  if (stop_.load()) return;
  if (link_.Send(MessageType::TransferCancel, net::EncodeCancel(id, reason))) return;
  if (!link_.IsClosed()) Report("取消通知发送失败");
}

}  // namespace bm
