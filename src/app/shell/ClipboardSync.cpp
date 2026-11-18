#include "app/shell/ClipboardSync.h"

#include <windows.h>
#include <shellapi.h>

#include <stdexcept>
#include <utility>

#include "core/clipboard/ClipboardContent.h"
#include "core/crypto/Crypto.h"

namespace bm {
namespace {

// 读取被系统剪贴板占用时最多尝试三次。
constexpr int kReadAttempts = 3;
constexpr DWORD kRetryDelayMs = 60;

// 系统剪贴板忙碌：稍后重试即可，不算失败
struct ClipboardBusy {};
struct ClipboardChanged {};

// 以下读取函数都要求调用方已打开剪贴板

bool ReadFormatBytes(UINT format, Bytes& out) {
  const HANDLE handle = GetClipboardData(format);
  if (handle == nullptr) return false;
  const SIZE_T size = GlobalSize(handle);
  const auto* data = static_cast<const uint8_t*>(GlobalLock(handle));
  if (data == nullptr || size == 0) {
    if (data != nullptr) GlobalUnlock(handle);
    return false;
  }
  out.assign(data, data + size);
  GlobalUnlock(handle);
  return true;
}

bool ReadText(std::string& text) {
  const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
  if (handle == nullptr) return false;
  const auto* wide = static_cast<const wchar_t*>(GlobalLock(handle));
  if (wide == nullptr) return false;
  const size_t capacity = GlobalSize(handle) / sizeof(wchar_t);
  size_t length = 0;
  while (length < capacity && wide[length] != L'\0') length++;
  text = ToUtf8(std::wstring_view(wide, length));
  GlobalUnlock(handle);
  return true;
}

// 只枚举 CF_HDROP 里的路径；目录没有实现递归，交给调用方拒绝
bool ReadFiles(std::vector<std::wstring>& paths, bool& hasDirectory) {
  const HANDLE handle = GetClipboardData(CF_HDROP);
  if (handle == nullptr) return false;
  const auto drop = static_cast<HDROP>(handle);
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT index = 0; index < count; index++) {
    const UINT length = DragQueryFileW(drop, index, nullptr, 0);
    std::wstring path(static_cast<size_t>(length) + 1, L'\0');
    const UINT written = DragQueryFileW(drop, index, path.data(), length + 1);
    path.resize(written);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) hasDirectory = true;
    paths.push_back(std::move(path));
  }
  return true;
}

// 图片：优先注册 PNG 格式，其次把 CF_DIB 编码成 PNG（.NET 走 GDI+ 位图）
bool ReadImage(Bytes& png) {
  if (ReadFormatBytes(RegisterClipboardFormatW(ClipboardWriter::kPngFormat), png)) return true;
  Bytes dib;
  if (!ReadFormatBytes(CF_DIB, dib)) return false;
  png = PngFromDib(dib);
  return true;
}

}  // namespace

ClipboardSync::ClipboardSync(HWND window, Callbacks callbacks)
    : window_(window), callbacks_(std::move(callbacks)) {}

ClipboardSync::~ClipboardSync() {
  bool registered = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    enabled_ = false;
    registered = registered_;
    registered_ = false;
    CancelReceiptLocked();
    // 撤销当前版本：接管者不应继续把旧内容发出去
    if (current_) current_->Revoke();
  }
  if (registered && window_ != nullptr) RemoveClipboardFormatListener(window_);
}

void ClipboardSync::SetMaxContentBytes(size_t bytes) {
  std::lock_guard<std::mutex> lock(mu_);
  maxContentBytes_ = bytes;
}

bool ClipboardSync::Enabled() const {
  std::lock_guard<std::mutex> lock(mu_);
  return enabled_;
}

void ClipboardSync::SetEnabled(bool enabled) {
  std::string report;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (enabled == enabled_) return;
    if (enabled) {
      // 开启先登记监听：失败就保持关闭，避免错误期间短暂共享
      if (!registered_) {
        if (window_ == nullptr || !AddClipboardFormatListener(window_)) {
          report = "无法监听系统剪贴板（错误码 " + std::to_string(GetLastError()) + "）";
        } else {
          registered_ = true;
        }
      }
      if (registered_) {
        enabled_ = true;
        generation_ += 1;
        lastSequence_ = Sequence();
        ForgetContentLocked();
        // 开启即读取当前剪贴板，作为第一个快照发布出去
        ReadCurrentLocked(report);
        changed = true;
      }
    } else {
      enabled_ = false;
      generation_ += 1;
      CancelReceiptLocked();
      if (registered_ && RemoveClipboardFormatListener(window_)) registered_ = false;
      // 关闭后不再发布：切换成空状态，所有会话随之撤销
      ReplaceCurrentLocked(ClipboardOrigin::Inactive, nullptr);
      changed = true;
    }
  }
  if (changed) FlushPublish();
  if (!report.empty()) Report(std::move(report));
}

void ClipboardSync::HandleMessage(UINT message, WPARAM, LPARAM) {
  if (message == kClipboardUpdateMessage) OnClipboardUpdate();
}

void ClipboardSync::OnClipboardUpdate() {
  std::string report;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!enabled_) return;
    const uint32_t sequence = Sequence();
    // 同一序列不重复读取：一次复制只会产生一批内容
    if (sequence == lastSequence_) return;
    lastSequence_ = sequence;
    // 本机复制取代旧接收权：正在下载的批次不得再写剪贴板
    CancelReceiptLocked();
    generation_ += 1;
    ReadCurrentLocked(report);
  }
  FlushPublish();
  if (!report.empty()) Report(std::move(report));
}

void ClipboardSync::ReadCurrentLocked(std::string& report) {
  bool remote = false;
  std::shared_ptr<const ClipboardContent> content;
  if (!ReadClipboard(content, remote, report)) {
    ReplaceCurrentLocked(ClipboardOrigin::Local, nullptr);
    return;
  }
  if (remote) {
    // 带来源标记：自己刚写回的远端内容，只更新基线，不再回传
    ForgetContentLocked();
    ReplaceCurrentLocked(ClipboardOrigin::Remote, nullptr);
    return;
  }
  // 内容为空表示不支持或超限：仍占住当前版本，旧发布不能趁此重发
  ReplaceCurrentLocked(ClipboardOrigin::Local, std::move(content));
}

bool ClipboardSync::ReadClipboard(std::shared_ptr<const ClipboardContent>& content, bool& remote, std::string& report) {
  for (int attempt = 0; attempt < kReadAttempts; attempt++) {
    if (!OpenClipboard(nullptr)) {
      Sleep(kRetryDelayMs);
      continue;
    }
    remote = ClipboardWriter::HasRemoteMarker();
    content = remote ? nullptr : ReadOpenedClipboard(report);
    CloseClipboard();
    return true;
  }
  report = "系统剪贴板持续忙碌，未读取本机内容";
  return false;
}

std::shared_ptr<const ClipboardContent> ClipboardSync::ReadOpenedClipboard(std::string& report) {
  try {
    auto content = std::make_shared<ClipboardContent>();
    std::vector<std::wstring> paths;
    bool hasDirectory = false;
    // 文件优先于路径文本，文本优先于浏览器可能附带的延迟渲染图片
    const bool fileDrop = ReadFiles(paths, hasDirectory);
    if (fileDrop && !paths.empty()) {
      if (hasDirectory) {
        report = "暂不支持文件夹复制，请选择普通文件。";
        ForgetContentLocked();
        return nullptr;
      }
      if (paths.size() > static_cast<size_t>(net::kMaxFiles)) {
        report = "普通文件数量须在1到10000之间，未发送";
        ForgetContentLocked();
        return nullptr;
      }
      content->kind = net::ClipboardContentKind::Files;
      content->paths = std::move(paths);
      RememberContentLocked(*content);
      return content;
    }
    if (ReadText(content->text)) {
      if (content->text.empty()) {
        ForgetContentLocked();
        return nullptr;
      }
      if (content->text.size() > maxContentBytes_) {
        report = "剪贴板内容超过 " + std::to_string(maxContentBytes_ / (1024 * 1024)) + " MB 上限，未发送";
        ForgetContentLocked();
        return nullptr;
      }
      content->kind = net::ClipboardContentKind::Text;
      RememberContentLocked(*content);
      return content;
    }
    if (ReadImage(content->image)) {
      // PNG 头与像素预算先卡住，超限或非法就不发布，也不做任何解码
      ClipboardImageBudget::ValidatePng(content->image);
      if (content->image.size() > maxContentBytes_) {
        report = "剪贴板内容超过 " + std::to_string(maxContentBytes_ / (1024 * 1024)) + " MB 上限，未发送";
        ForgetContentLocked();
        return nullptr;
      }
      content->kind = net::ClipboardContentKind::Image;
      RememberContentLocked(*content);
      return content;
    }
  } catch (const std::exception& error) {
    report = std::string("读取剪贴板失败：") + error.what();
  }
  ForgetContentLocked();
  return nullptr;
}

ClipboardSync::PreparedRemote ClipboardSync::PrepareRemote(ClipboardTransfer::Received received,
                                                            const std::function<void()>& checkCanceled) {
  auto content = std::make_shared<ClipboardContent>();
  content->kind = received.kind;
  content->text = std::move(received.text);
  content->image = std::move(received.image);
  content->paths = std::move(received.files);
  auto formats = ClipboardWriter::Prepare(*content, checkCanceled);
  auto key = MakeKey(content->kind, content->text, content->image, content->paths);
  return {std::move(received.receipt), std::move(received.applied), std::move(content), std::move(key),
          std::move(formats)};
}

bool ClipboardSync::TryApplyRemote(PreparedRemote& prepared) {
  const auto& receipt = prepared.receipt;
  bool committed = false;
  try {
    // 先取剪贴板状态锁，再取得批次提交权，避免取消与写入交叉。
    std::lock_guard lock(mu_);
    committed = receipt->TryCommit(
        [&] {
          if (!enabled_ || receipt != receipt_ || Sequence() != lastSequence_) throw ClipboardChanged{};
          if (prepared.content->text.size() > maxContentBytes_ || prepared.content->image.size() > maxContentBytes_)
            throw std::runtime_error("内容超过当前剪贴板大小上限");
          auto snapshot = std::make_shared<ClipboardSnapshot>(version_ + 1, ClipboardOrigin::Remote,
                                                              prepared.content);
          if (key_ != prepared.key || keySequence_ != lastSequence_) {
            const ClipboardWriteResult result = prepared.formats.Commit(window_, lastSequence_);
            if (result == ClipboardWriteResult::Busy) throw ClipboardBusy{};
            if (result == ClipboardWriteResult::Changed) throw ClipboardChanged{};
            key_ = std::move(prepared.key);
            keySequence_ = lastSequence_;
          }
          if (current_) current_->Revoke();
          current_ = std::move(snapshot);
          ++version_;
          publishPending_ = true;
        },
        prepared.applied);
  } catch (const ClipboardBusy&) {
    return false;
  } catch (const ClipboardChanged&) {
    receipt->Cancel();
  } catch (const std::exception& error) {
    receipt->Cancel();
    Report(std::string("收到的剪贴板内容无法写入：") + error.what());
  }
  if (committed) FlushPublish();
  return true;
}

bool ClipboardSync::BeginReceive(const std::shared_ptr<ClipboardReceipt>& receipt) {
  if (!receipt) return false;
  std::lock_guard<std::mutex> lock(mu_);
  // 剪贴板与接收代次都要对得上，否则这一批已经过期
  if (!enabled_ || Sequence() != lastSequence_) return false;
  CancelReceiptLocked();
  receipt_ = receipt;
  receipt_->generation = ++generation_;
  // 接收开始即废掉旧的本地发布；发布交给下一次事件，避免在会话锁内进入别的会话
  ReplaceCurrentLocked(ClipboardOrigin::Remote, nullptr);
  return true;
}

void ClipboardSync::ReplaceCurrentLocked(ClipboardOrigin origin, std::shared_ptr<const ClipboardContent> content) {
  // 撤销旧版本：会话里基于它的发送随之停止，迟到的通知也会被版本号挡住
  if (current_) current_->Revoke();
  current_ = std::make_shared<ClipboardSnapshot>(++version_, origin, std::move(content));
  publishPending_ = true;
}

void ClipboardSync::RememberContentLocked(const ClipboardContent& content) {
  key_ = MakeKey(content.kind, content.text, content.image, content.paths);
  keySequence_ = lastSequence_;
}

void ClipboardSync::ForgetContentLocked() {
  key_ = ContentKey{};
  keySequence_ = 0;
}

void ClipboardSync::FlushPublish() {
  std::shared_ptr<const ClipboardSnapshot> snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!publishPending_ || !current_) return;
    publishPending_ = false;
    snapshot = current_;
  }
  // 发布必须在释放剪贴板锁之后：回调会进入各会话的传输锁
  if (callbacks_.publish) callbacks_.publish(std::move(snapshot));
}

std::shared_ptr<const ClipboardSnapshot> ClipboardSync::Current() {
  OnClipboardUpdate();
  std::lock_guard<std::mutex> lock(mu_);
  return current_;
}

void ClipboardSync::CancelReceiptLocked() {
  if (!receipt_) return;
  const std::shared_ptr<ClipboardReceipt> previous = std::move(receipt_);
  receipt_.reset();
  previous->Cancel();
}

void ClipboardSync::Report(std::string message) {
  try {
    if (callbacks_.report) callbacks_.report(std::move(message));
  } catch (const std::exception&) {
  }
}

uint32_t ClipboardSync::Sequence() const { return GetClipboardSequenceNumber(); }

ClipboardSync::ContentKey ClipboardSync::MakeKey(net::ClipboardContentKind kind, const std::string& text,
                                                const Bytes& image, const std::vector<std::wstring>& paths) {
  ContentKey key;
  switch (kind) {
    case net::ClipboardContentKind::Text:
      key.format = ContentFormat::Text;
      key.identity = text;
      break;
    case net::ClipboardContentKind::Image:
      key.format = ContentFormat::Image;
      key.identity = ToHex(crypto::Sha256(image));
      break;
    default:
      key.format = ContentFormat::Files;
      for (const std::wstring& path : paths) {
        key.identity += ToUtf8(path);
        key.identity.push_back('\0');  // 与 .NET 的 string.Join('\0', paths) 一致
      }
      break;
  }
  return key;
}

}  // namespace bm
