#include "core/platform/Win32Input.h"

#include <iterator>
#include <stdexcept>
#include <string>

namespace bm::win {

namespace {

// 输入桌面与本线程桌面同名才算同一个普通桌面
bool SameAsThreadDesktop(HDESK input) {
  std::wstring inputName = DesktopName(input);
  return !inputName.empty() && inputName == DesktopName(GetThreadDesktop(GetCurrentThreadId()));
}

}  // namespace

std::wstring DesktopName(HDESK desktop) {
  wchar_t name[256]{};
  if (!GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), nullptr)) return {};
  size_t length = 0;
  while (length < std::size(name) && name[length] != L'\0') length++;
  return std::wstring(name, length);
}

bool IsOwnInputDesktop() {
  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input) return false;
  bool own = SameAsThreadDesktop(input);
  CloseDesktop(input);
  return own;
}

bool IsInputDesktopCurrent() {
  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input) return false;
  bool current = SameAsThreadDesktop(input);
  CloseDesktop(input);
  if (!current) return false;
  POINT cursor{};
  return GetCursorPos(&cursor) != 0;
}

HCURSOR CreateTransparentCursor() {
  BYTE andPlane[1] = {0xFF};
  BYTE xorPlane[1] = {0x00};
  HCURSOR cursor = CreateCursor(nullptr, 0, 0, 1, 1, andPlane, xorPlane);
  if (!cursor) ThrowLastError("CreateCursor");
  return cursor;
}

int PhysicalKeyId(int virtualKey, int scanCode, bool extended) {
  switch (virtualKey) {
    case VK_SHIFT: return scanCode == 0x36 ? 0xA1 : 0xA0;  // 右 Shift 扫描码 0x36
    case VK_CONTROL: return extended ? 0xA3 : 0xA2;
    case VK_MENU: return extended ? 0xA5 : 0xA4;
    default: return virtualKey;
  }
}

void ThrowLastError(const char* call) {
  DWORD error = GetLastError();
  throw std::runtime_error(std::string(call) + " 失败，Win32 " + std::to_string(error));
}

}  // namespace bm::win
