#pragma once

#include "core/platform/Common.h"

namespace bm {

enum class LogLevel { Info, Warning, Error };

struct LogEntry {
  uint64_t timeMs;  // 墙上时间
  LogLevel level;
  std::string text;
};

void LogText(LogLevel level, std::string text);
inline void LogInfo(std::string text) { LogText(LogLevel::Info, std::move(text)); }
inline void LogWarn(std::string text) { LogText(LogLevel::Warning, std::move(text)); }
inline void LogError(std::string text) { LogText(LogLevel::Error, std::move(text)); }

// 最近 500 条，供界面读取
std::vector<LogEntry> LogSnapshot(size_t limit = 500);
void LogClear();

}  // namespace bm
