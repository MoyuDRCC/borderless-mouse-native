#include "core/input/CursorHider.h"

namespace bm {

CursorHider::CursorHider() {
  // 全透明光标：AND 位全 1 保持屏幕不变，XOR 位全 0 不反色
  transparent_ = win::CreateTransparentCursor();
  HMODULE module = GetModuleHandleW(nullptr);
  className_ = L"BorderlessMouseCursor_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetCurrentThreadId());
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.lpfnWndProc = &CursorHider::WindowProc;
  windowClass.hInstance = module;
  windowClass.lpszClassName = className_.c_str();
  if (RegisterClassExW(&windowClass) == 0) win::ThrowLastError("RegisterClassExW");
  window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, className_.c_str(), className_.c_str(),
                            WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, module, nullptr);
  if (!window_) win::ThrowLastError("CreateWindowExW");
  SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

CursorHider::~CursorHider() {
  if (window_) {
    DestroyWindow(window_);
    window_ = nullptr;
  }
  if (!className_.empty()) {
    UnregisterClassW(className_.c_str(), GetModuleHandleW(nullptr));
    className_.clear();
  }
  if (transparent_) {
    DestroyCursor(transparent_);
    transparent_ = nullptr;
  }
}

void CursorHider::Show(int x, int y) {
  if (!window_) return;
  SetWindowPos(window_, HWND_TOPMOST, x - 1, y - 1, 3, 3, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  SetCursor(transparent_);
  shown_ = true;
}

void CursorHider::Hide() {
  if (!shown_) return;
  shown_ = false;
  if (window_) ShowWindow(window_, SW_HIDE);
}

LRESULT CALLBACK CursorHider::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  CursorHider* hider = reinterpret_cast<CursorHider*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (hider) {
    switch (message) {
      case WM_SETCURSOR:
        SetCursor(hider->transparent_);
        return 1;
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
      default:
        break;
    }
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace bm
