#include "core/protocol/FileCodec.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bm::net {
namespace {

// 与 Path.GetInvalidFileNameChars 一致的集合：控制字符与 Windows 保留符号
bool HasInvalidCharacter(std::wstring_view name) {
  for (const wchar_t character : name) {
    if (character < 0x20) return true;
    switch (character) {
      case L'<': case L'>': case L':': case L'"': case L'/': case L'\\': case L'|': case L'?': case L'*':
        return true;
      default:
        break;
    }
  }
  return false;
}

// 与 .NET StringComparer.OrdinalIgnoreCase 相同的序号无关大小写比较
bool SameText(std::wstring_view left, std::wstring_view right) {
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(), static_cast<int>(right.size()),
                              TRUE) == CSTR_EQUAL;
}

// 保留设备名判定：主干取首个点之前的部分，去掉尾部空格后比较
bool IsReservedStem(std::wstring_view name) {
  static constexpr const wchar_t* kReservedNames[] = {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
  std::wstring stem(name.substr(0, name.find(L'.')));
  while (!stem.empty() && stem.back() == L' ') stem.pop_back();
  for (const wchar_t* reserved : kReservedNames) {
    if (SameText(stem, reserved)) return true;
  }
  if (stem.size() != 4) return false;
  const bool com = SameText(stem.substr(0, 3), L"COM");
  const bool lpt = SameText(stem.substr(0, 3), L"LPT");
  if (!com && !lpt) return false;
  const wchar_t suffix = stem[3];
  return (suffix >= L'1' && suffix <= L'9') || suffix == 0x00B9 || suffix == 0x00B2 || suffix == 0x00B3;
}

}  // namespace

bool IsValidFileName(std::wstring_view name) {
  if (name.empty() || name.size() > 255) return false;
  if (name.back() == L'.' || name.back() == L' ') return false;
  return !HasInvalidCharacter(name) && !IsReservedStem(name);
}

Bytes EncodeManifest(std::span<const FileEntry> files) {
  if (files.empty() || files.size() > static_cast<size_t>(kMaxFiles))
    throw std::invalid_argument("普通文件数量须在1到10000之间");
  size_t size = 4;
  int64_t total = 0;
  std::vector<std::string> names(files.size());
  for (size_t index = 0; index < files.size(); index++) {
    const FileEntry& file = files[index];
    if (file.length < 0 || file.length > kMaxBatchBytes - total || !IsValidFileName(file.name))
      throw std::invalid_argument("文件清单包含非法名称或负长度");
    total += file.length;
    names[index] = bm::ToUtf8(file.name);
    size += 2 + names[index].size() + 8;
    if (static_cast<int64_t>(size) > kMaxManifestSize) throw std::invalid_argument("文件清单超过8MiB资源上限");
  }
  Bytes payload(size);
  WriteInt32LE(payload, 0, static_cast<int32_t>(files.size()));
  size_t offset = 4;
  for (size_t index = 0; index < files.size(); index++) {
    WriteUInt16LE(payload, offset, static_cast<uint16_t>(names[index].size()));
    offset += 2;
    std::memcpy(payload.data() + offset, names[index].data(), names[index].size());
    offset += names[index].size();
    WriteInt64LE(payload, offset, files[index].length);
    offset += 8;
  }
  return payload;
}

bool TryParseManifest(std::span<const uint8_t> payload, std::vector<FileEntry>& files) {
  if (payload.size() < 4 || static_cast<int64_t>(payload.size()) > kMaxManifestSize) return false;
  const int32_t count = ReadInt32LE(payload, 0);
  // 每条至少占 2+1+8 字节，据此先排除数量与长度矛盾
  if (count < 1 || count > kMaxFiles || static_cast<size_t>(count) > (payload.size() - 4) / 11) return false;
  std::vector<FileEntry> parsed;
  parsed.reserve(static_cast<size_t>(count));
  size_t offset = 4;
  int64_t total = 0;
  for (int32_t index = 0; index < count; index++) {
    if (payload.size() - offset < 2) return false;
    const size_t nameLength = ReadUInt16LE(payload, offset);
    offset += 2;
    if (nameLength == 0 || payload.size() - offset < nameLength + 8) return false;
    const std::string_view raw(reinterpret_cast<const char*>(payload.data() + offset), nameLength);
    if (!IsValidUtf8(raw)) return false;
    offset += nameLength;
    const int64_t length = ReadInt64LE(payload, offset);
    offset += 8;
    FileEntry entry{bm::ToWide(raw), length};
    if (length < 0 || length > kMaxBatchBytes - total || !IsValidFileName(entry.name)) return false;
    total += length;
    parsed.push_back(std::move(entry));
  }
  if (offset != payload.size()) return false;
  files = std::move(parsed);
  return true;
}

Bytes EncodeRequest(const TransferId& id, int32_t index) {
  ValidateIndex(id, index);
  Bytes payload(20);
  std::memcpy(payload.data(), id.bytes.data(), id.bytes.size());
  WriteInt32LE(payload, 16, index);
  return payload;
}

bool TryParseRequest(std::span<const uint8_t> payload, FileRequest& request) {
  if (payload.size() != 20) return false;
  FileRequest parsed;
  std::memcpy(parsed.id.bytes.data(), payload.data(), parsed.id.bytes.size());
  parsed.index = ReadInt32LE(payload, 16);
  if (parsed.id.IsEmpty() || parsed.index < 0 || parsed.index >= kMaxFiles) return false;
  request = parsed;
  return true;
}

Bytes EncodeChunk(const TransferId& id, int32_t index, int64_t offset, bool last, std::span<const uint8_t> data) {
  ValidateIndex(id, index);
  ValidateChunk(id, offset, last, data.size());
  Bytes payload(29 + data.size());
  std::memcpy(payload.data(), id.bytes.data(), id.bytes.size());
  WriteInt32LE(payload, 16, index);
  WriteInt64LE(payload, 20, offset);
  payload[28] = last ? 1 : 0;
  if (!data.empty()) std::memcpy(payload.data() + 29, data.data(), data.size());
  return payload;
}

bool TryParseChunk(std::span<const uint8_t> payload, FileChunk& chunk) {
  if (payload.size() < 29 || payload.size() > kChunkSize + 29 || payload[28] > 1) return false;
  FileChunk parsed;
  std::memcpy(parsed.id.bytes.data(), payload.data(), parsed.id.bytes.size());
  parsed.index = ReadInt32LE(payload, 16);
  parsed.offset = ReadInt64LE(payload, 20);
  parsed.last = payload[28] == 1;
  if (parsed.id.IsEmpty() || parsed.index < 0 || parsed.index >= kMaxFiles || parsed.offset < 0 ||
      (payload.size() == 29 && !parsed.last))
    return false;
  parsed.data.assign(payload.begin() + 29, payload.end());
  chunk = std::move(parsed);
  return true;
}

void ValidateIndex(const TransferId& id, int32_t index) {
  if (id.IsEmpty() || index < 0 || index >= kMaxFiles) throw std::invalid_argument("文件批次或索引非法");
}

}  // namespace bm::net
