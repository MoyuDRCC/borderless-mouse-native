#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/ui/Control.h"
#include "app/ui/Dpi.h"

namespace bm {

// 主窗体宿主：注册类、创建无边框窗口、DWM 圆角与边框、双缓冲绘制、
// 鼠标捕获与分发、500ms 定时、覆盖层弹窗（自绘菜单）。
class Window {
 public:
  Window() = default;
  ~Window();
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  // GDI+ 与 DPI 感知初始化；进程内调用一次，必须在创建窗口之前
  bool Init();
  // 创建无边框主窗口；root 由调用方持有，尺寸按逻辑像素给出
  bool Create(const std::wstring& title, Size2i logicalSize, Control* root);

  void Show();
  void Hide();
  void Quit();
  void Close();
  // 用户点关闭按钮时先交给应用（常驻托盘的工具改为隐藏窗口）
  void SetCloseHandler(std::function<void()> handler) { closeHandler_ = std::move(handler); }
  // 主窗口代收其他模块的消息（剪贴板监听、单实例唤回等）；返回 true 表示已处理
  void AddMessageHook(std::function<bool(UINT, WPARAM, LPARAM)> hook) { messageHooks_.push_back(std::move(hook)); }

  // 500ms 定时回调；控件自身的 OnTick 由同一个定时器驱动
  void SetTimer(std::function<void()> callback);
  void PostRedraw();
  void InvalidateRect(const RectI& clientRect);

  // 覆盖层弹出菜单：popup 的 Bounds 已是客户区坐标，越界时向内收
  void ShowPopup(std::unique_ptr<Control> popup);
  void ClosePopup();
  bool PopupOpen() const { return popup_ != nullptr; }
  void FocusControl(Control* control);
  Control* FocusedControl() const { return focused_; }

  Control* Root() const { return root_; }
  int Dpi() const { return dpi_; }
  HWND Handle() const { return hwnd_; }
  bool Maximized() const;
  void Minimize();
  void ToggleMaximize();

  int RunMessageLoop();

 private:
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

  void OnPaint(HWND hwnd);
  void OnSize(HWND hwnd);
  void ApplyFrame(HWND hwnd);
  void ApplyDpi(HWND hwnd, int dpi, const RECT& suggested);
  void ResizeBackBuffer(int width, int height);
  void ReleaseBackBuffer();

  LRESULT HitTestMessage(HWND hwnd, LPARAM lParam);
  void DispatchMouseMove(Point2i point);
  void DispatchMouseDown(Point2i point, MouseButton button);
  void DispatchMouseUp(Point2i point, MouseButton button);
  void DispatchWheel(int delta, Point2i point);
  void DispatchKey(int vk, bool down);
  Control* ControlAt(Point2i point);
  static Point2i LocalPoint(const Control* control, Point2i point);

  HWND hwnd_ = nullptr;
  Control* root_ = nullptr;
  std::unique_ptr<Control> popup_;
  Control* hovered_ = nullptr;
  Control* pressed_ = nullptr;
  Control* focused_ = nullptr;
  std::function<void()> tick_;
  std::function<void()> closeHandler_;
  std::vector<std::function<bool(UINT, WPARAM, LPARAM)>> messageHooks_;

  HDC backDc_ = nullptr;
  HBITMAP backBitmap_ = nullptr;
  HGDIOBJ backPrevious_ = nullptr;
  int backWidth_ = 0;
  int backHeight_ = 0;

  int dpi_ = 96;
  bool trackingLeave_ = false;
  bool gdiplusStarted_ = false;
  ULONG_PTR gdiplusToken_ = 0;
};

}  // namespace bm
