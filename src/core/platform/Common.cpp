#include "core/platform/Common.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstring>
#include <stdexcept>

namespace bm {

std::wstring ToWide(std::string_view text) {
  if (text.empty()) return {};
  int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
  return result;
}

std::string ToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
  return result;
}

std::string ToHex(std::span<const uint8_t> bytes) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string result(bytes.size() * 2, '0');
  for (size_t i = 0; i < bytes.size(); i++) {
    result[i * 2] = kDigits[bytes[i] >> 4];
    result[i * 2 + 1] = kDigits[bytes[i] & 0x0F];
  }
  return result;
}

Bytes FromHex(std::string_view hex) {
  auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::invalid_argument("十六进制字符非法");
  };
  if (hex.size() % 2 != 0) throw std::invalid_argument("十六进制长度非法");
  Bytes result(hex.size() / 2);
  for (size_t i = 0; i < result.size(); i++) result[i] = static_cast<uint8_t>((value(hex[i * 2]) << 4) | value(hex[i * 2 + 1]));
  return result;
}

std::string ToBase64(std::span<const uint8_t> bytes) {
  if (bytes.empty()) return {};
  DWORD length = 0;
  if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length))
    throw std::runtime_error("Base64 编码失败");
  std::string result(length, '\0');
  if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()),
                            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, result.data(), &length))
    throw std::runtime_error("Base64 编码失败");
  // 长度字段在不同位数下含不含结尾 NUL 不一致，按实际内容截断
  result.resize(std::strlen(result.c_str()));
  return result;
}

Bytes FromBase64(std::string_view text) {
  if (text.empty()) return {};
  DWORD length = 0;
  if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr))
    throw std::invalid_argument("Base64 解码失败");
  Bytes result(length);
  if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, result.data(), &length, nullptr, nullptr))
    throw std::invalid_argument("Base64 解码失败");
  result.resize(length);
  return result;
}

uint64_t TickMs() {
  static const uint64_t kFrequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<uint64_t>(value.QuadPart);
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<uint64_t>(counter.QuadPart) * 1000 / kFrequency;
}

uint64_t UnixMs() {
  FILETIME fileTime{};
  GetSystemTimeAsFileTime(&fileTime);
  ULARGE_INTEGER value{};
  value.LowPart = fileTime.dwLowDateTime;
  value.HighPart = fileTime.dwHighDateTime;
  return value.QuadPart / 10000 - 11644473600000ULL;
}

void SecureZero(std::span<uint8_t> bytes) {
  if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
}

bool FixedTimeEquals(std::span<const uint8_t> left, std::span<const uint8_t> right) {
  if (left.size() != right.size()) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < left.size(); i++) diff = static_cast<uint8_t>(diff | (left[i] ^ right[i]));
  return diff == 0;
}

}  // namespace bm
