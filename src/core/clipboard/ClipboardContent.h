#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/platform/Common.h"
#include "core/protocol/TransferCodec.h"

namespace bm {

// 本机内容的来源：本地复制、对端写入，或同步关闭时的空状态
enum class ClipboardOrigin { Inactive, Local, Remote };

// 一条剪贴板内容；Kind 决定哪个字段有效
struct ClipboardContent {
  net::ClipboardContentKind kind = net::ClipboardContentKind::Text;
  std::string text;                 // UTF-8 文本
  Bytes image;                      // PNG 文档
  std::vector<std::wstring> paths;  // 普通文件的完整路径
};

// 进程当前内容的不可变快照；撤销标记同时中断所有会话的旧版本生产者
class ClipboardSnapshot {
 public:
  ClipboardSnapshot(int64_t version, ClipboardOrigin origin, std::shared_ptr<const ClipboardContent> content);

  int64_t Version() const { return version_; }
  ClipboardOrigin Origin() const { return origin_; }
  const std::shared_ptr<const ClipboardContent>& Content() const { return content_; }
  bool IsRevoked() const { return revoked_->load(); }
  bool CanPublish() const {
    return origin_ == ClipboardOrigin::Local && content_ != nullptr && !IsRevoked();
  }
  // 只置标记：旧生产者自己退出，撤销不等待正在进行的传输
  void Revoke() { revoked_->store(true); }

 private:
  int64_t version_;
  ClipboardOrigin origin_;
  std::shared_ptr<const ClipboardContent> content_;
  std::shared_ptr<std::atomic<bool>> revoked_;
};

// 画面尺寸与 PNG 头的准入检查：在任何解码或位图分配之前完成
struct ClipboardImageBudget {
  static constexpr uint32_t kMaxDimension = 16384;
  static constexpr uint64_t kMaxPixels = 32ull * 1024 * 1024;

  static void ValidateDimensions(uint32_t width, uint32_t height);
  static void ValidatePng(std::span<const uint8_t> png);
};

// 本机剪贴板位图转为传输 PNG，尺寸先过预算检查。
Bytes PngFromDib(std::span<const uint8_t> dib);

enum class ClipboardWriteResult { Written, Busy, Changed };

// 远端内容统一带来源标记，提交前在系统剪贴板锁内核对序列号。
class ClipboardWriter {
 public:
  class Prepared {
   public:
    Prepared(Prepared&& other) noexcept;
    Prepared& operator=(Prepared&& other) noexcept;
    ~Prepared();

    // 剪贴板忙碌时保留已准备的内存，成功时由系统接管。
    ClipboardWriteResult Commit(HWND owner, uint32_t& sequence);

   private:
    friend class ClipboardWriter;
    struct Formats;
    explicit Prepared(std::unique_ptr<Formats> formats);
    std::unique_ptr<Formats> formats_;
  };

  // 与 .NET 版相同的注册格式名；同机多个实例都靠它识别远端写入
  static constexpr wchar_t kRemoteMarker[] = L"BorderlessMouse/FromPeer";
  static constexpr wchar_t kPngFormat[] = L"PNG";

  static bool WriteLocalText(HWND owner, std::wstring_view text);
  // 在后台准备全部格式，转换大图期间可通过回调取消。
  static Prepared Prepare(const ClipboardContent& content, const std::function<void()>& checkCanceled);
  static bool HasRemoteMarker();
};

}  // namespace bm
