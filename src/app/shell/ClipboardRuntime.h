#pragma once

#include "app/shell/ClipboardSync.h"
#include "core/platform/Common.h"
#include "core/platform/EventLog.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace bm {

struct AppConfig;
class ClipLink;
class PeerSession;
class Window;

// 剪贴板协作：本机剪贴板监听的建立，以及每个会话的传输对象、开关与单条上限的落地。
// 工作线程收到完整批次后，将系统剪贴板写入交给界面线程。
class ClipboardRuntime {
 public:
  struct Callbacks {
    std::function<void(const std::string&, LogLevel)> report;
    std::function<void()> saveConfig;
    // 本机内容变化：交给每个会话的传输对象
    std::function<void(std::shared_ptr<const ClipboardSnapshot>)> publish;
  };

  ClipboardRuntime(AppConfig& config, Window& window, Callbacks callbacks);
  ~ClipboardRuntime();
  ClipboardRuntime(const ClipboardRuntime&) = delete;
  ClipboardRuntime& operator=(const ClipboardRuntime&) = delete;

  // 建立剪贴板监听、挂上窗口消息转发，并按配置启用；退出时关掉监听
  void Start();
  void Stop();
  void ApplyPending();
  bool Enabled() const;
  int64_t MaxContentBytes() const;

  // 会话侧：发送出口与传输对象
  std::unique_ptr<ClipLink> CreateLink(std::shared_ptr<PeerSession> session);
  std::unique_ptr<ClipboardTransfer> CreateTransfer(ClipLink& link, std::weak_ptr<PeerSession> session);
  // 新会话接入时补一次当前内容
  void ApplyCurrent(ClipboardTransfer& transfer);

  // 设置项落地：总开关与单条上限都写回配置；上限返回新的字节数，供会话侧同步
  void SetEnabled(bool enabled);
  int64_t SetLimitMb(int megabytes);

 private:
  void Report(const std::string& text);

  AppConfig& config_;
  Window& window_;
  Callbacks callbacks_;
  std::unique_ptr<ClipboardSync> sync_;
  std::mutex pendingMutex_;
  struct PendingClipboard {
    std::weak_ptr<PeerSession> session;
    ClipboardSync::PreparedRemote content;
    int attempts = 0;
  };
  std::optional<PendingClipboard> pending_;
};

}  // namespace bm
