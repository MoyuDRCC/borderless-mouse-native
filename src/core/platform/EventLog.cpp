#include "core/platform/EventLog.h"

#include <windows.h>

#include <deque>
#include <mutex>

namespace bm {

namespace {
constexpr size_t kCapacity = 500;

std::mutex g_mutex;
std::deque<LogEntry> g_entries;

const wchar_t* TagOf(LogLevel level) {
  switch (level) {
    case LogLevel::Warning: return L"[警告] ";
    case LogLevel::Error: return L"[错误] ";
    default: return L"[信息] ";
  }
}
}  // namespace

void LogText(LogLevel level, std::string text) {
  SYSTEMTIME local{};
  GetLocalTime(&local);
  LogEntry entry{UnixMs(), level, std::move(text)};
  OutputDebugStringW((std::wstring(TagOf(level)) + ToWide(entry.text) + L"\n").c_str());
  std::lock_guard lock(g_mutex);
  if (g_entries.size() >= kCapacity) g_entries.pop_front();
  g_entries.push_back(std::move(entry));
}

std::vector<LogEntry> LogSnapshot(size_t limit) {
  std::lock_guard lock(g_mutex);
  size_t count = g_entries.size() < limit ? g_entries.size() : limit;
  return std::vector<LogEntry>(g_entries.end() - static_cast<ptrdiff_t>(count), g_entries.end());
}

void LogClear() {
  std::lock_guard lock(g_mutex);
  g_entries.clear();
}

}  // namespace bm
