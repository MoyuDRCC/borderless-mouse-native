#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bm {

using Bytes = std::vector<uint8_t>;

// UTF-8 与宽字符互转；Win32 边界统一使用宽字符
std::wstring ToWide(std::string_view text);
std::string ToUtf8(std::wstring_view text);

// 与 .NET Convert.ToHexString / ToBase64String 相同的输出形态
std::string ToHex(std::span<const uint8_t> bytes);
Bytes FromHex(std::string_view hex);
std::string ToBase64(std::span<const uint8_t> bytes);
Bytes FromBase64(std::string_view text);

// 单调毫秒，只用于间隔与超时
uint64_t TickMs();
// 墙上毫秒，用于日志时间戳与布局版本
uint64_t UnixMs();

void SecureZero(std::span<uint8_t> bytes);
bool FixedTimeEquals(std::span<const uint8_t> left, std::span<const uint8_t> right);

}  // namespace bm
