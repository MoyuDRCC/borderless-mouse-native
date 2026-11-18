#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/clipboard/ClipboardTransfer.h"

namespace bm {

// 本机剪贴板监听与写回（窗口线程调用）：内容变化发布给所有启用同步的会话，
// 远端内容写回时带来源标记，避免再被当成新的本机内容回传
class ClipboardSync {
  enum class ContentFormat { None, Text, Image, Files };

  // 内容身份：同一内容不重复写入剪贴板，系统序列号变化才算新的一次复制。
  struct ContentKey {
    ContentFormat format = ContentFormat::None;
    std::string identity;
    bool operator==(const ContentKey&) const = default;
  };

 public:
  static constexpr UINT kClipboardUpdateMessage = 0x031D;  // WM_CLIPBOARDUPDATE

  struct Callbacks {
    // 当前内容变化：交给所有启用同步的会话
    std::function<void(std::shared_ptr<const ClipboardSnapshot>)> publish;
    // 需要用户知道的失败与提示
    std::function<void(std::string)> report;
  };

  struct PreparedRemote {
    std::shared_ptr<ClipboardReceipt> receipt;
    std::function<void()> applied;
    std::shared_ptr<const ClipboardContent> content;
    ContentKey key;
    ClipboardWriter::Prepared formats;
  };

  ClipboardSync(HWND window, Callbacks callbacks);
  ~ClipboardSync();
  ClipboardSync(const ClipboardSync&) = delete;
  ClipboardSync& operator=(const ClipboardSync&) = delete;

  // 全局开关：开启时登记监听并立即读取当前剪贴板作为新快照发布；
  // 关闭时撤销全部运行态（旧快照 Revoke），不再发布
  void SetEnabled(bool enabled);
  bool Enabled() const;
  // 文本与图片的单条字节上限；图片另受 ClipboardImageBudget 的尺寸约束
  void SetMaxContentBytes(size_t bytes);
  // 后台生成内容标识和系统格式，界面线程只核对提交权并交付。
  static PreparedRemote PrepareRemote(ClipboardTransfer::Received received, const std::function<void()>& checkCanceled);
  bool TryApplyRemote(PreparedRemote& prepared);
  // 窗口过程转发 WM_CLIPBOARDUPDATE
  void HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
  // 授予一次接收的提交权（.NET ClipboardSync.BeginReceive）；会话的 BeginReceive 回调转调这里
  bool BeginReceive(const std::shared_ptr<ClipboardReceipt>& receipt);
  // 当前内容快照（.NET GetCurrent）：新接入的会话据此补一次 SetCurrent；会先补处理一次待处理事件
  std::shared_ptr<const ClipboardSnapshot> Current();

 private:
  void OnClipboardUpdate();
  // 读取当前剪贴板并替换快照；需持 mu_
  void ReadCurrentLocked(std::string& report);
  // 解析已打开的剪贴板；需持 mu_
  std::shared_ptr<const ClipboardContent> ReadOpenedClipboard(std::string& report);
  // 打开剪贴板读取内容（系统忙碌时重试）；remote 表示带来源标记
  bool ReadClipboard(std::shared_ptr<const ClipboardContent>& content, bool& remote, std::string& report);
  void ReplaceCurrentLocked(ClipboardOrigin origin, std::shared_ptr<const ClipboardContent> content);
  void RememberContentLocked(const ClipboardContent& content);
  void ForgetContentLocked();
  void FlushPublish();
  void Report(std::string message);
  void CancelReceiptLocked();
  uint32_t Sequence() const;

  static ContentKey MakeKey(net::ClipboardContentKind kind, const std::string& text, const Bytes& image,
                            const std::vector<std::wstring>& paths);

  HWND window_ = nullptr;
  Callbacks callbacks_;

  mutable std::mutex mu_;
  bool enabled_ = false;
  bool registered_ = false;
  bool publishPending_ = false;
  uint32_t lastSequence_ = 0;
  size_t maxContentBytes_ = static_cast<size_t>(net::kMaxContentLength);
  int64_t version_ = 0;
  int64_t generation_ = 0;
  std::shared_ptr<ClipboardSnapshot> current_;
  std::shared_ptr<ClipboardReceipt> receipt_;
  ContentKey key_;
  uint32_t keySequence_ = 0;
};

}  // namespace bm
