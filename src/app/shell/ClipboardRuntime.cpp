#include "app/shell/ClipboardRuntime.h"

#include <algorithm>
#include <utility>

#include "core/clipboard/ClipboardReceipt.h"
#include "app/ui/Window.h"
#include "core/config/AppConfig.h"
#include "core/net/PeerSession.h"

namespace bm {

namespace {

constexpr UINT_PTR kClipboardRetryTimer = 2;
constexpr UINT kClipboardRetryDelayMs = 60;
constexpr int kClipboardWriteAttempts = 5;

// 会话的大数据出口：可靠帧走非阻塞队列，数据块按会话背压阻塞
class PeerClipLink final : public ClipLink {
 public:
  explicit PeerClipLink(std::shared_ptr<PeerSession> session) : session_(std::move(session)) {}

  bool Send(MessageType type, std::span<const uint8_t> payload) override { return session_->TrySend(type, payload); }
  bool SendBulk(MessageType type, std::span<const uint8_t> payload, const std::atomic<bool>& aborted) override {
    return session_->SendBulk(type, payload, [&aborted] { return aborted.load(); });
  }
  bool IsClosed() const override { return session_->IsClosed(); }
  void Close() override { session_->Close(); }

 private:
  std::shared_ptr<PeerSession> session_;
};

}  // namespace

ClipboardRuntime::ClipboardRuntime(AppConfig& config, Window& window, Callbacks callbacks)
    : config_(config), window_(window), callbacks_(std::move(callbacks)) {}

ClipboardRuntime::~ClipboardRuntime() = default;

void ClipboardRuntime::Report(const std::string& text) { callbacks_.report(text, LogLevel::Warning); }

void ClipboardRuntime::Start() {
  ClipboardSync::Callbacks callbacks;
  callbacks.publish = callbacks_.publish;
  callbacks.report = [this](std::string message) { Report(message); };
  sync_ = std::make_unique<ClipboardSync>(window_.Handle(), std::move(callbacks));
  sync_->SetMaxContentBytes(static_cast<size_t>(MaxContentBytes()));
  window_.AddMessageHook([this](UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_TIMER && wParam == kClipboardRetryTimer) {
      ApplyPending();
      return true;
    }
    sync_->HandleMessage(message, wParam, lParam);
    return false;
  });
  sync_->SetEnabled(config_.clipboardSync);
}

void ClipboardRuntime::Stop() {
  if (window_.Handle()) KillTimer(window_.Handle(), kClipboardRetryTimer);
  if (sync_) sync_->SetEnabled(false);
  std::lock_guard lock(pendingMutex_);
  pending_.reset();
}

void ClipboardRuntime::ApplyPending() {
  const HWND window = window_.Handle();
  if (window) KillTimer(window, kClipboardRetryTimer);
  std::optional<PendingClipboard> pending;
  {
    std::lock_guard lock(pendingMutex_);
    pending.swap(pending_);
  }
  if (!pending) return;
  const auto session = pending->session.lock();
  const auto& receipt = pending->content.receipt;
  if (!session || session->IsClosed()) {
    receipt->Cancel();
    return;
  }
  if (!receipt->IsValid() || sync_->TryApplyRemote(pending->content)) return;
  if (++pending->attempts == kClipboardWriteAttempts) {
    receipt->Cancel();
    Report("系统剪贴板持续忙碌，接收内容未交付");
    return;
  }
  // 未完成的写入交回待处理位置，重试间隔内继续处理窗口消息。
  {
    std::lock_guard lock(pendingMutex_);
    if (!pending_) pending_ = std::move(pending);
  }
  if (window) ::SetTimer(window, kClipboardRetryTimer, kClipboardRetryDelayMs, nullptr);
}

bool ClipboardRuntime::Enabled() const { return sync_ && sync_->Enabled(); }

int64_t ClipboardRuntime::MaxContentBytes() const { return static_cast<int64_t>(config_.clipboardLimitMb) * 1024 * 1024; }

std::unique_ptr<ClipLink> ClipboardRuntime::CreateLink(std::shared_ptr<PeerSession> session) {
  return std::make_unique<PeerClipLink>(std::move(session));
}

std::unique_ptr<ClipboardTransfer> ClipboardRuntime::CreateTransfer(ClipLink& link, std::weak_ptr<PeerSession> session) {
  auto transfer = std::make_unique<ClipboardTransfer>(
      link, [this](const std::shared_ptr<ClipboardReceipt>& receipt) { return sync_->BeginReceive(receipt); },
      [this, session = std::move(session)](ClipboardTransfer::Received received) {
        const auto receipt = received.receipt;
        const auto source = session.lock();
        const auto checkCanceled = [&] {
          if (!source || source->IsClosed() || !receipt->IsValid()) throw TransferCancelled{};
        };
        try {
          // 接收线程准备图像与系统格式。仅发布结果时持待处理锁。
          auto prepared = ClipboardSync::PrepareRemote(std::move(received), checkCanceled);
          std::lock_guard lock(pendingMutex_);
          checkCanceled();
          if (!pending_ || pending_->content.receipt->generation < receipt->generation)
            pending_ = PendingClipboard{session, std::move(prepared)};
        } catch (const TransferCancelled&) {
          receipt->Cancel();
        } catch (const std::exception& error) {
          receipt->Cancel();
          Report(std::string("无法准备收到的剪贴板内容：") + error.what());
        }
      },
      [this](std::string message) { Report(message); });
  transfer->SetMaxContentBytes(MaxContentBytes());
  transfer->SetEnabled(Enabled());
  return transfer;
}

void ClipboardRuntime::ApplyCurrent(ClipboardTransfer& transfer) {
  if (std::shared_ptr<const ClipboardSnapshot> snapshot = sync_->Current()) transfer.SetCurrent(std::move(snapshot));
}

void ClipboardRuntime::SetEnabled(bool enabled) {
  config_.clipboardSync = enabled;
  sync_->SetEnabled(enabled);
  callbacks_.saveConfig();
  Report(enabled ? "剪贴板同步已开启" : "剪贴板同步已关闭");
}

int64_t ClipboardRuntime::SetLimitMb(int megabytes) {
  config_.clipboardLimitMb = std::clamp(megabytes, 1, 64);
  const int64_t bytes = MaxContentBytes();
  sync_->SetMaxContentBytes(static_cast<size_t>(bytes));
  callbacks_.saveConfig();
  return bytes;
}

}  // namespace bm
