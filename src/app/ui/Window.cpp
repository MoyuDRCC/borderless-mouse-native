#include "app/ui/Window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"

namespace bm {

namespace {

constexpr int kTickTimerId = 1;
constexpr wchar_t kWindowClass[] = L"BorderlessMouseFrame";


// DWM 属性：Win11 圆角、边框色、标题区配色
constexpr DWORD kDwmwaWindowCorner = 33;
constexpr DWORD kDwmwaBorderColor = 34;
constexpr DWORD kDwmwaCaptionColor = 35;
constexpr DWORD kDwmwaTextColor = 36;
constexpr int kDwmwcpRound = 2;

}  // namespace

Window::~Window() {
  ReleaseBackBuffer();
  if (hwnd_) DestroyWindow(hwnd_);
  if (gdiplusStarted_) Gdiplus::GdiplusShutdown(gdiplusToken_);
  Dpi::ReleaseFontCache();
}

bool Window::Init() {
  // 窗口自己处理缩放，必须比任何窗口更早声明 Per-Monitor V2
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  Gdiplus::GdiplusStartupInput input;
  if (Gdiplus::GdiplusStartup(&gdiplusToken_, &input, nullptr) != Gdiplus::Ok) return false;
  gdiplusStarted_ = true;

  INITCOMMONCONTROLSEX commonControls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&commonControls);
  return true;
}

bool Window::Create(const std::wstring& title, Size2i logicalSize, Control* root) {
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(WNDCLASSEXW);
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.lpszClassName = kWindowClass;
  RegisterClassExW(&windowClass);

  dpi_ = GetDpiForSystem();
  const int width = Dpi::Scale(logicalSize.width, dpi_);
  const int height = Dpi::Scale(logicalSize.height, dpi_);
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;

  hwnd_ = CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW, x, y, width, height,
                          nullptr, nullptr, windowClass.hInstance, this);
  if (!hwnd_) return false;

  dpi_ = GetDpiForWindow(hwnd_);
  ApplyFrame(hwnd_);

  root_ = root;
  if (root_) {
    root_->AttachHost(this);
    RECT client{};
    GetClientRect(hwnd_, &client);
    root_->SetBounds(RectI{0, 0, client.right, client.bottom});
  }
  return true;
}

void Window::Show() {
  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
}

void Window::Hide() { ShowWindow(hwnd_, SW_HIDE); }

void Window::Quit() { PostQuitMessage(0); }

void Window::Close() {
  if (hwnd_) DestroyWindow(hwnd_);
}

void Window::SetTimer(std::function<void()> callback) {
  tick_ = std::move(callback);
  ::SetTimer(hwnd_, kTickTimerId, 500, nullptr);
}

void Window::PostRedraw() {
  if (hwnd_) ::InvalidateRect(hwnd_, nullptr, FALSE);  // 成员函数同名，必须限定全局
}

void Window::InvalidateRect(const RectI& clientRect) {
  if (!hwnd_ || clientRect.IsEmpty()) return;
  RECT rect{clientRect.left, clientRect.top, clientRect.right, clientRect.bottom};
  ::InvalidateRect(hwnd_, &rect, FALSE);
}

void Window::ShowPopup(std::unique_ptr<Control> popup) {
  if (!popup || !hwnd_) return;
  ClosePopup();

  RECT client{};
  GetClientRect(hwnd_, &client);
  RectI bounds = popup->Bounds();
  const int margin = Dpi::Scale(8, dpi_);

  // 贴边时整体向内平移，保证菜单完整可见
  if (bounds.right > client.right - margin) {
    const int shift = bounds.right - (client.right - margin);
    bounds.left -= shift;
    bounds.right -= shift;
  }
  if (bounds.bottom > client.bottom - margin) {
    const int shift = bounds.bottom - (client.bottom - margin);
    bounds.top -= shift;
    bounds.bottom -= shift;
  }
  if (bounds.left < margin) {
    bounds.right += margin - bounds.left;
    bounds.left = margin;
  }
  if (bounds.top < margin) {
    bounds.bottom += margin - bounds.top;
    bounds.top = margin;
  }

  popup->SetBounds(bounds);
  popup->AttachHost(this);
  popup_ = std::move(popup);
  PostRedraw();
}

void Window::ClosePopup() {
  if (!popup_) return;
  const RectI bounds = popup_->Bounds();
  if (hovered_ == popup_.get()) hovered_ = nullptr;
  if (pressed_ == popup_.get()) pressed_ = nullptr;
  popup_.reset();
  InvalidateRect(bounds);
}

void Window::FocusControl(Control* control) {
  if (focused_ == control) return;
  focused_ = control;
  PostRedraw();
}

bool Window::Maximized() const { return hwnd_ && IsZoomed(hwnd_); }

void Window::Minimize() { ShowWindow(hwnd_, SW_MINIMIZE); }

void Window::ToggleMaximize() {
  ShowWindow(hwnd_, Maximized() ? SW_RESTORE : SW_MAXIMIZE);
}

int Window::RunMessageLoop() {
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}

LRESULT CALLBACK Window::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  Window* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* params = reinterpret_cast<CREATESTRUCTW*>(lParam);
    window = static_cast<Window*>(params->lpCreateParams);
    window->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
  }
  if (!window) return DefWindowProcW(hwnd, message, wParam, lParam);
  return window->HandleMessage(hwnd, message, wParam, lParam);
}

LRESULT Window::HandleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  for (const auto& hook : messageHooks_)
    if (hook(message, wParam, lParam)) return 0;
  switch (message) {
    case WM_NCCALCSIZE:
      if (wParam) {
        // 去掉系统标题栏；最大化时贴合显示器工作区
        if (Maximized()) {
          auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
          MONITORINFO info{sizeof(MONITORINFO)};
          if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info))
            params->rgrc[0] = info.rcWork;
        }
        return 0;
      }
      break;

    case WM_NCACTIVATE:
      return TRUE;  // 自绘窗口不需要在激活状态变化时重绘非客户区

    case WM_NCHITTEST:
      return HitTestMessage(hwnd, lParam);

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
      OnPaint(hwnd);
      return 0;

    case WM_SIZE:
      OnSize(hwnd);
      return 0;

    case WM_DPICHANGED: {
      const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
      ApplyDpi(hwnd, HIWORD(wParam), *suggested);
      return 0;
    }

    case WM_ACTIVATE:
      if (LOWORD(wParam) == WA_INACTIVE) ClosePopup();
      break;

    case WM_MOUSEMOVE: {
      if (!trackingLeave_) {
        TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        trackingLeave_ = true;
      }
      DispatchMouseMove(Point2i{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
      return 0;
    }

    case WM_MOUSELEAVE:
      trackingLeave_ = false;
      if (hovered_) {
        hovered_->SetHoverState(false);
        hovered_->OnMouseLeave();
        hovered_ = nullptr;
      }
      return 0;

    case WM_LBUTTONDOWN:
      DispatchMouseDown(Point2i{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, MouseButton::Left);
      return 0;
    case WM_RBUTTONDOWN:
      DispatchMouseDown(Point2i{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, MouseButton::Right);
      return 0;
    case WM_MBUTTONDOWN:
      DispatchMouseDown(Point2i{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, MouseButton::Middle);
      return 0;
    case WM_LBUTTONUP:
      DispatchMouseUp(Point2i{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, MouseButton::Left);
      return 0;
    case WM_RBUTTONUP:
      DispatchMouseUp(Point2i{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, MouseButton::Right);
      return 0;
    case WM_MBUTTONUP:
      DispatchMouseUp(Point2i{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}, MouseButton::Middle);
      return 0;

    case WM_MOUSEWHEEL: {
      POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ScreenToClient(hwnd, &point);
      DispatchWheel(GET_WHEEL_DELTA_WPARAM(wParam), Point2i{point.x, point.y});
      return 0;
    }

    case WM_KEYDOWN:
      DispatchKey(static_cast<int>(wParam), true);
      break;
    case WM_KEYUP:
      DispatchKey(static_cast<int>(wParam), false);
      break;
    case WM_CHAR:
      if (focused_) focused_->OnChar(static_cast<wchar_t>(wParam));
      break;

    case WM_SETCURSOR:
      if (LOWORD(lParam) == HTCLIENT && hovered_ && hovered_->Cursor()) {
        ::SetCursor(hovered_->Cursor());
        return TRUE;
      }
      break;

    case WM_TIMER:
      if (wParam == kTickTimerId) {
        if (tick_) tick_();
        if (root_) root_->TickTree();
      }
      return 0;

    case WM_CLOSE:
      if (closeHandler_) {
        closeHandler_();
      } else {
        DestroyWindow(hwnd);
      }
      return 0;

    case WM_DESTROY:
      ReleaseBackBuffer();
      hwnd_ = nullptr;
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

void Window::OnPaint(HWND hwnd) {
  PAINTSTRUCT paint{};
  HDC dc = BeginPaint(hwnd, &paint);

  RECT client{};
  GetClientRect(hwnd, &client);
  ResizeBackBuffer(client.right, client.bottom);
  if (backDc_) {
    Gdiplus::Graphics graphics(backDc_);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    Gdiplus::SolidBrush background(Theme::Window);
    graphics.FillRectangle(&background, 0, 0, client.right, client.bottom);

    const RectI full{0, 0, client.right, client.bottom};
    if (root_) {
      PaintContext ctx{graphics, backDc_, dpi_, full, Point2i{0, 0}};
      root_->PaintTree(ctx);
    }
    if (popup_) {
      // 弹层覆盖在整棵控件树之上，坐标系与根一致
      PaintContext ctx{graphics, backDc_, dpi_, full, Point2i{0, 0}};
      popup_->PaintTree(ctx);
    }

    BitBlt(dc, 0, 0, client.right, client.bottom, backDc_, 0, 0, SRCCOPY);
  }

  EndPaint(hwnd, &paint);
}

void Window::OnSize(HWND hwnd) {
  if (!root_) return;
  RECT client{};
  GetClientRect(hwnd, &client);
  root_->SetBounds(RectI{0, 0, client.right, client.bottom});
}

void Window::ApplyDpi(HWND hwnd, int dpi, const RECT& suggested) {
  dpi_ = dpi;
  SetWindowPos(hwnd, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
               suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
  ReleaseBackBuffer();  // 尺寸与 DPI 同时变化，后台缓冲按新尺寸重建
  PostRedraw();
}

void Window::ApplyFrame(HWND hwnd) {
  const int corner = kDwmwcpRound;
  DwmSetWindowAttribute(hwnd, kDwmwaWindowCorner, &corner, sizeof(corner));

  COLORREF border = draw::Ref(Theme::Border);
  DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border, sizeof(border));
  COLORREF caption = draw::Ref(Theme::Window);
  DwmSetWindowAttribute(hwnd, kDwmwaCaptionColor, &caption, sizeof(caption));
  COLORREF text = draw::Ref(Theme::Text);
  DwmSetWindowAttribute(hwnd, kDwmwaTextColor, &text, sizeof(text));

  // 无缝边框：1px 底边延伸进客户区，其余边缘靠圆角偏好，保留系统阴影
  const MARGINS margins{0, 0, 0, 1};
  DwmExtendFrameIntoClientArea(hwnd, &margins);
  SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
               SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void Window::ResizeBackBuffer(int width, int height) {
  if (width == backWidth_ && height == backHeight_ && backDc_) return;
  ReleaseBackBuffer();
  if (width <= 0 || height <= 0) return;

  HDC screen = GetDC(hwnd_);
  backDc_ = CreateCompatibleDC(screen);
  ReleaseDC(hwnd_, screen);

  // 32 位 DIB：内存 DC 上才能拿到 ClearType，兼容位图会退化成灰度抗锯齿
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  backBitmap_ = CreateDIBSection(backDc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  backPrevious_ = SelectObject(backDc_, backBitmap_);
  backWidth_ = width;
  backHeight_ = height;
}

void Window::ReleaseBackBuffer() {
  if (backDc_) {
    if (backPrevious_) SelectObject(backDc_, backPrevious_);
    DeleteDC(backDc_);
  }
  if (backBitmap_) DeleteObject(backBitmap_);
  backDc_ = nullptr;
  backBitmap_ = nullptr;
  backPrevious_ = nullptr;
  backWidth_ = 0;
  backHeight_ = 0;
}

LRESULT Window::HitTestMessage(HWND hwnd, LPARAM lParam) {
  POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
  ScreenToClient(hwnd, &point);

  // 八方向缩放：只在非最大化时留边，逻辑 5px
  if (!Maximized()) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int edge = Dpi::Scale(5, dpi_);
    const bool left = point.x < edge;
    const bool right = point.x >= client.right - edge;
    const bool top = point.y < edge;
    const bool bottom = point.y >= client.bottom - edge;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
  }

  Control* hit = ControlAt(Point2i{point.x, point.y});
  int strip = Dpi::Scale(Theme::CaptionHeight, dpi_);
  if (hit) {
    if (hit->CapturesCaption()) return HTCLIENT;  // 窗口控制按钮自己吃点击
    const int own = Dpi::Scale(hit->CaptionStripHeight(), dpi_);
    if (own > strip) strip = own;  // 侧栏品牌区比默认标题条更高
  }
  return point.y < strip ? HTCAPTION : HTCLIENT;
}

Control* Window::ControlAt(Point2i point) { return root_ ? root_->HitTest(point) : nullptr; }

Point2i Window::LocalPoint(const Control* control, Point2i point) {
  const Point2i origin = control->AbsoluteOrigin();
  return Point2i{point.x - origin.x, point.y - origin.y};
}

void Window::DispatchMouseMove(Point2i point) {
  if (popup_) {
    if (popup_->Bounds().Contains(point.x, point.y)) {
      if (hovered_ != popup_.get()) {
        if (hovered_) {
          hovered_->SetHoverState(false);
          hovered_->OnMouseLeave();
        }
        hovered_ = popup_.get();
        hovered_->SetHoverState(true);
      }
      popup_->OnMouseMove(LocalPoint(popup_.get(), point));
    }
    return;  // 弹层打开时主界面不跟随悬停
  }

  // 按住拖拽期间事件始终送给按下的控件
  if (pressed_) {
    pressed_->OnMouseMove(LocalPoint(pressed_, point));
    return;
  }

  Control* hit = ControlAt(point);
  if (hit != hovered_) {
    if (hovered_) {
      hovered_->SetHoverState(false);
      hovered_->OnMouseLeave();
    }
    hovered_ = hit;
    if (hovered_) hovered_->SetHoverState(true);
  }
  if (hit) hit->OnMouseMove(LocalPoint(hit, point));
}

void Window::DispatchMouseDown(Point2i point, MouseButton button) {
  if (popup_) {
    if (popup_->Bounds().Contains(point.x, point.y)) {
      pressed_ = popup_.get();
      popup_->OnMouseDown(LocalPoint(popup_.get(), point), button);
    } else {
      ClosePopup();  // 点空白处收起弹层，并吞掉这次点击
    }
    return;
  }

  Control* hit = ControlAt(point);
  if (focused_ != hit) focused_ = nullptr;  // 点走焦点，输入框在按下时自行申请
  if (!hit) return;

  pressed_ = hit;
  SetCapture(hwnd_);
  hit->SetHoverState(true);

  // 未处理时沿父链上浮，组合控件可以整体接管
  for (Control* target = hit; target; target = target->Parent()) {
    if (target->OnMouseDown(LocalPoint(target, point), button)) break;
  }
}

void Window::DispatchMouseUp(Point2i point, MouseButton button) {
  Control* target = pressed_;
  pressed_ = nullptr;

  if (popup_) {
    if (popup_->Bounds().Contains(point.x, point.y)) popup_->OnMouseUp(LocalPoint(popup_.get(), point), button);
  } else if (target) {
    target->OnMouseUp(LocalPoint(target, point), button);
  }

  if (GetCapture() == hwnd_) ReleaseCapture();
}

void Window::DispatchWheel(int delta, Point2i point) {
  Control* target = popup_ ? popup_.get() : ControlAt(point);
  // 与 WinForms 一致：命中的控件没滚动时交给父级，悬停在开关/下拉框上也能滚页面
  for (Control* control = target; control; control = control->Parent()) control->OnWheel(delta, LocalPoint(control, point));
}

void Window::DispatchKey(int vk, bool down) {
  if (popup_) {
    popup_->OnKey(vk, down);
    return;
  }
  if (focused_) focused_->OnKey(vk, down);
}

}  // namespace bm