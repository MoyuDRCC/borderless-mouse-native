#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/clipboard/ClipboardReceipt.h"
#include "core/platform/Common.h"
#include "core/protocol/FileCodec.h"
#include "core/protocol/TransferCodec.h"

namespace bm {

// 发送侧清单：源文件完整路径、字节长度与已编码清单来自同一次读取
struct FileOffer {
  std::vector<std::wstring> paths;
  std::vector<net::FileEntry> files;
  Bytes manifest;
};

// 普通文件存入用户接收目录；已交付文件由用户管理，未交付批次由持有者清理
class FileTransfer {
 public:
  // 同机实例共享的接收预算：磁盘上的预留额与已交付文件一起计入
  static constexpr int64_t kStoreBudget = 8LL * 1024 * 1024 * 1024;

  // %USERPROFILE%\Downloads\无界键鼠接收文件
  static std::wstring ReceiveDirectory();

  // 建立批次目录并写入预留额；预算、磁盘空间或所有权冲突时抛 std::runtime_error
  FileTransfer(const net::TransferId& id, const std::vector<net::FileEntry>& files);
  ~FileTransfer();
  FileTransfer(const FileTransfer&) = delete;
  FileTransfer& operator=(const FileTransfer&) = delete;

  bool Complete() const { return next_ == static_cast<int>(entries_.size()); }
  int NextIndex() const { return next_; }

  // 写入一块数据，返回该文件是否已落盘交付；取消抛 TransferCancelled，协议或 IO 错误抛 std::exception
  bool Accept(const net::FileChunk& chunk, const std::atomic<bool>& stopped);

  // 完整批次的交付路径；先持久记录交付意图，剪贴板写入成功后再 MarkDelivered
  std::vector<std::wstring> PrepareDelivery();
  void MarkDelivered();

  // 未交付则删除整批；幂等，析构时也会调用
  void Dispose();

  // 回收上次运行遗留的未交付批次；app 在所有传输结束后调用
  static void Cleanup();

  // 发送侧：校验普通文件并编码清单
  static FileOffer PrepareOffer(const std::vector<std::wstring>& paths, const std::atomic<bool>& stopped);

 private:
  struct Entry;  // 每个文件的临时名、目标名与接收进度

  std::wstring folder_;
  std::vector<std::unique_ptr<Entry>> entries_;
  void* owner_ = nullptr;   // 批次所有权句柄：活动实例持有独占锁
  void* stream_ = nullptr;  // 当前写入的 .part 句柄
  int next_ = 0;
  std::atomic<bool> delivered_{false};
  std::atomic<bool> disposed_{false};
};

}  // namespace bm
