#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/platform/Common.h"
#include "core/protocol/TransferCodec.h"

// 文件清单作为可分片文档；请求与内容块始终携带同一批次和文件索引
namespace bm::net {

struct FileEntry {
  std::wstring name;  // 单个 Windows 文件名，不含路径
  int64_t length = 0;
};

struct FileRequest {
  TransferId id;
  int32_t index = 0;
};

struct FileChunk {
  TransferId id;
  int32_t index = 0;
  int64_t offset = 0;
  bool last = false;
  Bytes data;
};

constexpr size_t kFileChunkSize = kChunkSize;
constexpr int64_t kMaxManifestSize = kMaxManifestLength;
constexpr int32_t kMaxFiles = 10000;
constexpr int64_t kMaxBatchBytes = 2LL * 1024 * 1024 * 1024;

// 清单编码：文件数量、单个名称与批次总量都要在上限内，非法即抛 std::invalid_argument
Bytes EncodeManifest(std::span<const FileEntry> files);
bool TryParseManifest(std::span<const uint8_t> payload, std::vector<FileEntry>& files);

Bytes EncodeRequest(const TransferId& id, int32_t index);
bool TryParseRequest(std::span<const uint8_t> payload, FileRequest& request);

Bytes EncodeChunk(const TransferId& id, int32_t index, int64_t offset, bool last, std::span<const uint8_t> data);
bool TryParseChunk(std::span<const uint8_t> payload, FileChunk& chunk);

void ValidateIndex(const TransferId& id, int32_t index);

// 只允许单个 Windows 文件名：拒绝路径、保留设备名和系统会规范化的尾部字符
bool IsValidFileName(std::wstring_view name);

}  // namespace bm::net
