#pragma once

#include "core/platform/Win32Input.h"

#include <string>

namespace bm {

// 对应 Input/CursorHider.cs：远控期间把停在显示器中央的本机光标藏起来。
// 一个 3×3 置顶窗口加一个全透明光标，光标停在这几个像素上时不显示任何形状，离开就恢复原样。
// 窗口必须自己接住命中测试（不能用 HTTRANSPARENT），否则系统把光标判给下面的窗口、
// 不发 WM_SETCURSOR，透明光标没有机会生效；命中范围只有 3×3，且不抢焦点。
// 窗口建在钩子线程上（那里已有消息泵），因此只能在钩子线程创建、移动与销毁。
class CursorHider {
 public:
  CursorHider();
  ~CursorHider();
  CursorHider(const CursorHider&) = delete;
  CursorHider& operator=(const CursorHider&) = delete;

  // 把窗口罩在光标停放点上并置顶显示，同时立刻换成本线程的透明光标
  void Show(int x, int y);
  void Hide();

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

  std::wstring className_;
  HWND window_ = nullptr;
  HCURSOR transparent_ = nullptr;
  bool shown_ = false;
};

}  // namespace bm
