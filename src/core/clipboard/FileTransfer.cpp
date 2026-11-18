#include "core/clipboard/FileTransfer.h"

#include <windows.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace bm {
namespace {

constexpr wchar_t kOwnerName[] = L".owner";
constexpr wchar_t kReservedName[] = L".reserved";
constexpr wchar_t kKeepName[] = L".keep";
// 预留空间时给磁盘留出的余量
constexpr int64_t kFreeSpaceMargin = 64LL * 1024 * 1024;

struct FolderEntry {
  std::wstring path;
  DWORD attributes = 0;
};

std::string LastErrorText() {
  const DWORD code = GetLastError();
  wchar_t buffer[256] = {};
  const DWORD length =
      FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buffer, 256, nullptr);
  std::wstring text(buffer, length);
  while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
  return ToUtf8(text) + "（错误码 " + std::to_string(code) + "）";
}

std::wstring JoinPath(std::wstring_view folder, std::wstring_view name) {
  std::wstring path(folder);
  if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
  path.append(name);
  return path;
}

std::wstring FileNameOf(std::wstring_view path) {
  return std::wstring(path.substr(path.find_last_of(L"\\/") + 1));
}

bool Exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool IsReparsePoint(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

int64_t FileLength(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return 0;
  return (static_cast<int64_t>(info.nFileSizeHigh) << 32) | static_cast<int64_t>(info.nFileSizeLow);
}

std::vector<FolderEntry> ListFolder(const std::wstring& folder) {
  std::vector<FolderEntry> entries;
  WIN32_FIND_DATAW data{};
  HANDLE search = FindFirstFileW(JoinPath(folder, L"*").c_str(), &data);
  if (search == INVALID_HANDLE_VALUE) return entries;
  do {
    const std::wstring_view name(data.cFileName);
    if (name == L"." || name == L"..") continue;
    entries.push_back(FolderEntry{JoinPath(folder, name), data.dwFileAttributes});
  } while (FindNextFileW(search, &data));
  FindClose(search);
  return entries;
}

// 用户整理的目录只计实际占用，不沿链接遍历
int64_t DirectoryBytes(const std::wstring& folder) {
  int64_t total = 0;
  for (const FolderEntry& entry : ListFolder(folder)) {
    if ((entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      total += DirectoryBytes(entry.path);
      continue;
    }
    total += FileLength(entry.path);
  }
  return total;
}

void CreateDirectories(const std::wstring& path) {
  for (size_t index = 1; index <= path.size(); index++) {
    if (index != path.size() && path[index] != L'\\' && path[index] != L'/') continue;
    const std::wstring part = path.substr(0, index);
    if (part.size() == 2 && part[1] == L':') continue;  // 盘符本身必然存在
    if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
      throw std::runtime_error("无法创建接收目录：" + LastErrorText());
  }
}

void WriteAllText(const std::wstring& path, std::string_view text) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("无法写入批次标记：" + LastErrorText());
  DWORD written = 0;
  const BOOL ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  const std::string failure = ok ? std::string() : LastErrorText();
  CloseHandle(file);
  if (!ok || written != static_cast<DWORD>(text.size()))
    throw std::runtime_error("无法写入批次标记：" + failure);
}

bool ReadAllText(const std::wstring& path, std::string& text) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  char buffer[64] = {};
  DWORD read = 0;
  const BOOL ok = ReadFile(file, buffer, sizeof(buffer), &read, nullptr);
  CloseHandle(file);
  if (!ok) return false;
  text.assign(buffer, read);
  return true;
}

// 递归删除已确认不含链接的子树
void DeleteTree(const std::wstring& path) {
  for (const FolderEntry& entry : ListFolder(path)) {
    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) DeleteTree(entry.path);
    else DeleteFileW(entry.path.c_str());
  }
  RemoveDirectoryW(path.c_str());
}

bool IsHex32(std::wstring_view text) {
  if (text.size() != 32) return false;
  for (const wchar_t character : text) {
    const bool digit = character >= L'0' && character <= L'9';
    const bool letter = (character >= L'a' && character <= L'f') || (character >= L'A' && character <= L'F');
    if (!digit && !letter) return false;
  }
  return true;
}

// 只认程序原本写入的非负预留额；缺失、损坏或越界元数据不授予删除权
bool TryReadReservation(const std::wstring& folder, int64_t& size) {
  size = 0;
  const std::wstring name = FileNameOf(folder);
  if (name.size() != 65 || name[32] != L'-' || !IsHex32(std::wstring_view(name).substr(0, 32)) ||
      !IsHex32(std::wstring_view(name).substr(33, 32)) || IsReparsePoint(folder))
    return false;
  WIN32_FILE_ATTRIBUTE_DATA owner{};
  if (!GetFileAttributesExW(JoinPath(folder, kOwnerName).c_str(), GetFileExInfoStandard, &owner)) return false;
  if ((owner.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
  if (owner.nFileSizeHigh != 0 || owner.nFileSizeLow != 0) return false;
  const std::wstring reservation = JoinPath(folder, kReservedName);
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (!GetFileAttributesExW(reservation.c_str(), GetFileExInfoStandard, &info)) return false;
  if ((info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0) return false;
  std::string text;
  if (!ReadAllText(reservation, text) || text.empty() || text.size() > 18) return false;
  for (const char character : text) {
    if (character < '0' || character > '9') return false;  // NumberStyles.None：不允许符号与空白
  }
  size = std::stoll(text);
  return size <= net::kMaxBatchBytes;
}

int64_t ReservedBytes(const std::wstring& folder) {
  if (IsReparsePoint(folder)) return 0;
  int64_t size = 0;
  if (!Exists(JoinPath(folder, kKeepName)) && TryReadReservation(folder, size)) return size;
  return DirectoryBytes(folder);
}

// 删除前检查整棵批次树；用户加入链接时整批保留，绝不递归到链接目标
void RemoveBatch(const std::wstring& folder) {
  std::vector<std::wstring> pending{folder};
  while (!pending.empty()) {
    const std::wstring directory = pending.back();
    pending.pop_back();
    if (IsReparsePoint(directory)) throw std::runtime_error("接收批次包含目录链接，已保留文件");
    for (const FolderEntry& entry : ListFolder(directory)) {
      if ((entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        throw std::runtime_error("接收批次包含文件或目录链接，已保留文件");
      if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) pending.push_back(entry.path);
    }
  }
  // 先删接收内容，保留所有权和预留额到最后；占用失败不会让残片失去下次清理依据
  for (const FolderEntry& entry : ListFolder(folder)) {
    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) DeleteTree(entry.path);
  }
  const std::wstring owner = JoinPath(folder, kOwnerName);
  const std::wstring reservation = JoinPath(folder, kReservedName);
  for (const FolderEntry& entry : ListFolder(folder)) {
    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    if (entry.path == owner || entry.path == reservation) continue;
    DeleteFileW(entry.path.c_str());
  }
  DeleteTree(folder);
}

class FileHandle {
 public:
  explicit FileHandle(HANDLE handle) : handle_(handle) {}
  ~FileHandle() { Close(); }
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  bool Valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
  HANDLE Get() const { return handle_; }
  void Close() {
    if (Valid()) {
      CloseHandle(handle_);
      handle_ = nullptr;
    }
  }

 private:
  HANDLE handle_;
};

void WriteAll(HANDLE file, std::span<const uint8_t> data) {
  DWORD written = 0;
  if (!WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) ||
      written != static_cast<DWORD>(data.size()))
    throw std::runtime_error("文件写入失败：" + LastErrorText());
}

// 同机实例共用接收库：命名互斥锁把预算统计与删除串行化
class StoreLock {
 public:
  StoreLock() {
    mutex_ = CreateMutexW(nullptr, FALSE, L"LocalBorderlessMouse.ReceivedFiles");
    if (mutex_ == nullptr) throw std::runtime_error("无法创建接收目录锁：" + LastErrorText());
    const DWORD result = WaitForSingleObject(mutex_, 2000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
      CloseHandle(mutex_);
      mutex_ = nullptr;
      throw std::runtime_error("接收目录正在被其他实例使用，请稍后重试");
    }
  }
  ~StoreLock() {
    if (mutex_ != nullptr) {
      ReleaseMutex(mutex_);
      CloseHandle(mutex_);
    }
  }
  StoreLock(const StoreLock&) = delete;
  StoreLock& operator=(const StoreLock&) = delete;

 private:
  HANDLE mutex_ = nullptr;
};

void CleanAbandoned() {
  const std::wstring root = FileTransfer::ReceiveDirectory();
  if (!IsDirectory(root)) return;
  for (const FolderEntry& entry : ListFolder(root)) {
    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
    try {
      if (Exists(JoinPath(entry.path, kKeepName))) continue;
      int64_t reserved = 0;
      if (!TryReadReservation(entry.path, reserved)) continue;
      {
        // 只打开既有标记；活动实例持有独占锁，未知目录绝不创建标记认领
        FileHandle owner(CreateFileW(JoinPath(entry.path, kOwnerName).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!owner.Valid()) continue;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(owner.Get(), &size) || size.QuadPart != 0) continue;
        if (Exists(JoinPath(entry.path, kKeepName))) continue;
      }
      // 调用者始终持有库互斥锁，释放句柄到删除之间其他实例不能创建或接管批次
      RemoveBatch(entry.path);
    } catch (const std::exception&) {
      // 活动、已移动或不可访问的目录均保留，不阻碍其他批次接收与退出
    }
  }
}

int64_t FreeSpace(const std::wstring& path) {
  ULARGE_INTEGER available{};
  if (!GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr))
    throw std::runtime_error("无法读取接收磁盘剩余空间：" + LastErrorText());
  return static_cast<int64_t>(available.QuadPart);
}

std::wstring IndexName(size_t index) {
  std::wstring text = std::to_wstring(index);
  while (text.size() < 5) text.insert(text.begin(), L'0');
  return text;
}

std::wstring HexRandom() { return ToWide(net::ToHex(net::NewTransferId())); }

std::wstring FullPath(const std::wstring& path) {
  wchar_t buffer[1024] = {};
  const DWORD length = GetFullPathNameW(path.c_str(), 1024, buffer, nullptr);
  if (length == 0 || length >= 1024) throw std::runtime_error("无法解析文件路径：" + LastErrorText());
  return std::wstring(buffer, length);
}

}  // namespace

struct FileTransfer::Entry {
  std::wstring name;
  std::wstring folder;
  std::wstring target;
  std::wstring temporary;
  int64_t length = 0;
  int64_t received = 0;
};

std::wstring FileTransfer::ReceiveDirectory() {
  wchar_t profile[1024] = {};
  const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", profile, 1024);
  if (length == 0 || length >= 1024) throw std::runtime_error("无法定位用户目录");
  std::wstring path(profile, length);
  path += L"\\Downloads\\无界键鼠接收文件";
  return path;
}

FileTransfer::FileTransfer(const net::TransferId& id, const std::vector<net::FileEntry>& files) {
  int64_t size = 0;
  for (const net::FileEntry& file : files) size += file.length;
  if (size > net::kMaxBatchBytes) throw std::runtime_error("单次文件批次不能超过2GiB");

  const std::wstring root = ReceiveDirectory();
  folder_ = JoinPath(root, ToWide(net::ToHex(id)) + L"-" + HexRandom());
  for (size_t index = 0; index < files.size(); index++) {
    auto entry = std::make_unique<Entry>();
    entry->name = files[index].name;
    entry->length = files[index].length;
    entry->folder = JoinPath(folder_, IndexName(index));
    entry->target = JoinPath(entry->folder, entry->name);
    entry->temporary = JoinPath(entry->folder, HexRandom() + L".part");
    entries_.push_back(std::move(entry));
  }

  // 同机实例共享预算，磁盘上保存预留额，不能只统计已写入的部分
  StoreLock lock;
  CreateDirectories(root);
  CleanAbandoned();
  int64_t reserved = 0;
  for (const FolderEntry& entry : ListFolder(root)) {
    // 用户可能把已交付文件移到接收根目录，根层普通文件也计入同一份预算
    if ((entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) reserved += ReservedBytes(entry.path);
    else if ((entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) reserved += FileLength(entry.path);
  }
  if (size > kStoreBudget - reserved) throw std::runtime_error("接收目录达到8GiB预算，请整理接收文件后重试");
  if (FreeSpace(root) < size + kFreeSpaceMargin) throw std::runtime_error("接收磁盘空间不足，未开始下载");
  if (Exists(folder_)) throw std::runtime_error("接收批次目录已存在");
  CreateDirectories(folder_);
  try {
    const HANDLE owner = CreateFileW(JoinPath(folder_, kOwnerName).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (owner == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建批次所有权标记：" + LastErrorText());
    owner_ = owner;
    WriteAllText(JoinPath(folder_, kReservedName), std::to_string(size));
  } catch (...) {
    // 只有实际持有本次批次的所有权，才能回收构造失败留下的目录
    if (owner_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(owner_));
      owner_ = nullptr;
      DeleteTree(folder_);
    }
    throw;
  }
}

FileTransfer::~FileTransfer() {
  // 析构不能再抛：清理失败交给下次启动的残片回收
  try {
    Dispose();
  } catch (const std::exception&) {
  }
}

bool FileTransfer::Accept(const net::FileChunk& chunk, const std::atomic<bool>& stopped) {
  if (disposed_.load()) throw std::runtime_error("文件批次已关闭");
  if (stopped.load()) throw TransferCancelled{};
  if (next_ >= static_cast<int>(entries_.size()) || chunk.index != next_)
    throw std::invalid_argument("文件索引不属于当前请求");
  Entry& entry = *entries_[static_cast<size_t>(next_)];
  if (chunk.offset != entry.received || static_cast<int64_t>(chunk.data.size()) > entry.length - entry.received)
    throw std::invalid_argument("文件偏移不连续或超过声明长度");
  const int64_t length = entry.received + static_cast<int64_t>(chunk.data.size());
  if (chunk.last != (length == entry.length)) throw std::invalid_argument("文件尾块与声明长度不一致");
  if (stream_ == nullptr) {
    CreateDirectories(entry.folder);
    const HANDLE stream =
        CreateFileW(entry.temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (stream == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建接收文件：" + LastErrorText());
    stream_ = stream;
  }
  WriteAll(static_cast<HANDLE>(stream_), chunk.data);
  if (stopped.load()) throw TransferCancelled{};
  entry.received = length;
  if (!chunk.last) return false;
  FlushFileBuffers(static_cast<HANDLE>(stream_));
  CloseHandle(static_cast<HANDLE>(stream_));
  stream_ = nullptr;
  if (stopped.load()) throw TransferCancelled{};
  if (!MoveFileExW(entry.temporary.c_str(), entry.target.c_str(), 0))
    throw std::runtime_error("无法交付接收文件：" + LastErrorText());
  next_ += 1;
  return true;
}

std::vector<std::wstring> FileTransfer::PrepareDelivery() {
  if (disposed_.load()) throw std::runtime_error("文件批次已关闭");
  if (!Complete()) throw std::runtime_error("文件批次尚未完整");
  // 先持久记录交付意图，崩溃发生在剪贴板写入之后也不能在下次启动误删引用
  WriteAllText(JoinPath(folder_, kKeepName), "交付文件由用户管理");
  std::vector<std::wstring> paths;
  paths.reserve(entries_.size());
  for (const std::unique_ptr<Entry>& entry : entries_) paths.push_back(entry->target);
  return paths;
}

void FileTransfer::MarkDelivered() { delivered_.store(true); }

void FileTransfer::Dispose() {
  if (disposed_.exchange(true)) return;
  if (stream_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(stream_));
    stream_ = nullptr;
  }
  if (owner_ == nullptr) return;  // 未持有所有权：目录归构造失败路径或上次运行
  const HANDLE owner = static_cast<HANDLE>(owner_);
  try {
    if (delivered_.load()) {
      CloseHandle(owner);
      owner_ = nullptr;
      return;
    }
    // 取消已先撤销剪贴板提交权；撤销交付意图后，清理失败仍可下次回收
    DeleteFileW(JoinPath(folder_, kKeepName).c_str());
    StoreLock lock;
    CloseHandle(owner);
    owner_ = nullptr;
    if (Exists(folder_)) RemoveBatch(folder_);
  } catch (...) {
    // 等库锁或删除失败也必须释放实际持有的所有权
    if (owner_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(owner_));
      owner_ = nullptr;
    }
    throw;
  }
}

void FileTransfer::Cleanup() {
  StoreLock lock;
  CleanAbandoned();
}

FileOffer FileTransfer::PrepareOffer(const std::vector<std::wstring>& paths, const std::atomic<bool>& stopped) {
  if (paths.empty() || paths.size() > static_cast<size_t>(net::kMaxFiles))
    throw std::runtime_error("普通文件数量须在1到10000之间");
  FileOffer offer;
  offer.paths.resize(paths.size());
  offer.files.resize(paths.size());
  int64_t total = 0;
  for (size_t index = 0; index < paths.size(); index++) {
    if (stopped.load()) throw TransferCancelled{};
    const std::wstring full = FullPath(paths[index]);
    const DWORD attributes = GetFileAttributesW(full.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
      throw std::runtime_error("只支持存在的普通文件：" + ToUtf8(full));
    const int64_t length = FileLength(full);
    if (length > net::kMaxBatchBytes - total) throw std::runtime_error("单次文件批次不能超过2GiB");
    total += length;
    offer.paths[index] = full;
    offer.files[index] = net::FileEntry{FileNameOf(full), length};
  }
  offer.manifest = net::EncodeManifest(offer.files);
  return offer;
}

}  // namespace bm
