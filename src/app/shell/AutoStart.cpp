#include "app/shell/AutoStart.h"

#include <windows.h>

#include "core/platform/Common.h"

namespace bm {

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"BorderlessMouse";

std::wstring ExecutablePath() {
  wchar_t path[MAX_PATH * 2];
  DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  return std::wstring(path, length);
}

std::wstring QuotedExecutablePath() { return L"\"" + ExecutablePath() + L"\""; }
}  // namespace

bool IsAutoStartEnabled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
  wchar_t value[MAX_PATH * 2];
  DWORD size = sizeof(value);
  DWORD type = 0;
  LONG status = RegQueryValueExW(key, kValueName, nullptr, &type, reinterpret_cast<LPBYTE>(value), &size);
  RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_SZ) return false;
  return QuotedExecutablePath() == std::wstring(value, size / sizeof(wchar_t) - 1);
}

void SetAutoStart(bool enabled) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return;
  if (enabled) {
    std::wstring command = QuotedExecutablePath();
    RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                   static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(key, kValueName);
  }
  RegCloseKey(key);
}

}  // namespace bm
