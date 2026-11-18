#include "app/shell/Tray.h"

#include <shellapi.h>

#include <memory>
#include <utility>

#include "app/ui/Window.h"

namespace bm {

namespace {

// 与 resources/app.rc 中的图标资源 ID 对应
constexpr int kAppIconId = 101;
// 托盘回调消息与菜单命令号；托盘窗口自管，不需要主窗口代收
constexpr UINT kTrayCallback = WM_APP + 17;
constexpr UINT kMenuShow = 1;
constexpr UINT kMenuLogs = 2;
constexpr UINT kMenuQuit = 3;
constexpr wchar_t kTrayClass[] = L"BorderlessMouseTray";

}  // namespace

// 托盘实现：隐藏窗口接收托盘回调，菜单用 TrackPopupMenu 同步取结果
class Tray::Impl {
 public:
  Impl(Window& window, std::function<void()> showWindow, std::function<void()> showLogs,
       std::function<void()> quit);
  ~Impl();
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void SetStatus(const std::wstring& text);
  void SetVisible(bool visible);
  bool WantsMessage(UINT message) const;
  void Handle(UINT message, WPARAM wParam, LPARAM lParam);

 private:
  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
  void CreateHostWindow();
  void Fill(NOTIFYICONDATAW& data) const;
  void Refresh();
  void AddIcon();
  void RemoveIcon();
  void Activate();
  void OpenMenu();

  Window& window_;
  std::function<void()> showWindow_;
  std::function<void()> showLogs_;
  std::function<void()> quit_;
  HWND hwnd_ = nullptr;
  HICON icon_ = nullptr;
  bool visible_ = false;
  std::wstring tip_ = L"无界键鼠";
  UINT taskbarCreated_ = 0;
};

Tray::Impl::Impl(Window& window, std::function<void()> showWindow, std::function<void()> showLogs,
                 std::function<void()> quit)
    : window_(window),
      showWindow_(std::move(showWindow)),
      showLogs_(std::move(showLogs)),
      quit_(std::move(quit)) {
  taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  CreateHostWindow();

  icon_ = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON,
                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                        LR_DEFAULTCOLOR | LR_SHARED));
  if (icon_ == nullptr) icon_ = LoadIconW(nullptr, IDI_APPLICATION);
  AddIcon();
}

Tray::Impl::~Impl() {
  RemoveIcon();
  if (hwnd_) DestroyWindow(hwnd_);
}

void Tray::Impl::CreateHostWindow() {
  WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
  windowClass.lpfnWndProc = &Tray::Impl::Proc;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = kTrayClass;
  // 重复注册只会在已经有实例时返回失败，创建窗口仍然可用
  RegisterClassExW(&windowClass);

  hwnd_ = CreateWindowExW(0, kTrayClass, L"无界键鼠托盘", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                          windowClass.hInstance, this);
}

LRESULT CALLBACK Tray::Impl::Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  auto* impl = reinterpret_cast<Tray::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* params = reinterpret_cast<CREATESTRUCTW*>(lParam);
    impl = static_cast<Tray::Impl*>(params->lpCreateParams);
    impl->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    return TRUE;
  }
  // 除托盘回调与资源管理器重建外一律走默认过程：右键菜单的字体系默认过程提供，
  // 全部返回 0 会让菜单只剩空白框
  if (impl != nullptr && impl->WantsMessage(message)) {
    impl->Handle(message, wParam, lParam);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool Tray::Impl::WantsMessage(UINT message) const {
  return message == kTrayCallback || (taskbarCreated_ != 0 && message == taskbarCreated_);
}

void Tray::Impl::Handle(UINT message, WPARAM wParam, LPARAM lParam) {
  // 资源管理器重启后要重新加回图标
  if (taskbarCreated_ != 0 && message == taskbarCreated_) {
    visible_ = false;
    AddIcon();
    return;
  }

  if (message != kTrayCallback) return;
  (void)wParam;

  switch (LOWORD(lParam)) {
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
      Activate();
      break;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
      OpenMenu();
      break;
    default:
      break;
  }
}

void Tray::Impl::Fill(NOTIFYICONDATAW& data) const {
  data.cbSize = sizeof(NOTIFYICONDATAW);
  data.hWnd = hwnd_;
  data.uID = 1;
  data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  data.uCallbackMessage = kTrayCallback;
  data.hIcon = icon_;
  wcsncpy_s(data.szTip, tip_.c_str(), _TRUNCATE);
}

void Tray::Impl::Refresh() {
  if (!visible_ || hwnd_ == nullptr) return;
  NOTIFYICONDATAW data{};
  Fill(data);
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::Impl::AddIcon() {
  if (visible_ || hwnd_ == nullptr) return;
  NOTIFYICONDATAW data{};
  Fill(data);
  if (!Shell_NotifyIconW(NIM_ADD, &data)) return;
  visible_ = true;
}

void Tray::Impl::RemoveIcon() {
  if (!visible_ || hwnd_ == nullptr) return;
  NOTIFYICONDATAW data{};
  Fill(data);
  Shell_NotifyIconW(NIM_DELETE, &data);
  visible_ = false;
}

void Tray::Impl::SetStatus(const std::wstring& text) {
  if (tip_ == text) return;
  tip_ = text.empty() ? L"无界键鼠" : text;
  Refresh();
}

void Tray::Impl::SetVisible(bool visible) {
  if (visible) {
    AddIcon();
  } else {
    RemoveIcon();
  }
}

void Tray::Impl::Activate() {
  if (showWindow_) showWindow_();
  HWND main = window_.Handle();
  if (main == nullptr) return;
  if (IsIconic(main)) ShowWindow(main, SW_RESTORE);
  SetForegroundWindow(main);
}

void Tray::Impl::OpenMenu() {
  if (hwnd_ == nullptr) return;

  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kMenuShow, L"打开窗口");
  AppendMenuW(menu, MF_STRING, kMenuLogs, L"打开日志");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kMenuQuit, L"退出");

  POINT point{};
  GetCursorPos(&point);
  // 菜单属于本窗口：先抢前台，收尾再补一条空消息，避免菜单残留
  SetForegroundWindow(hwnd_);
  const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, point.x, point.y,
                                      0, hwnd_, nullptr);
  DestroyMenu(menu);
  PostMessageW(hwnd_, WM_NULL, 0, 0);

  switch (command) {
    case kMenuShow:
      Activate();
      break;
    case kMenuLogs:
      if (showLogs_) showLogs_();
      break;
    case kMenuQuit:
      if (quit_) quit_();
      break;
    default:
      break;
  }
}

Tray::Tray(Window& window, std::function<void()> showWindow, std::function<void()> showLogs,
           std::function<void()> quit)
    : impl_(std::make_unique<Impl>(window, std::move(showWindow), std::move(showLogs), std::move(quit))) {}

Tray::~Tray() = default;

void Tray::SetStatus(const std::wstring& text) { impl_->SetStatus(text); }

void Tray::SetVisible(bool visible) { impl_->SetVisible(visible); }

// 宿主若用 SetMessageHook 代收消息，可以把托盘回调转给这里
void Tray::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
  impl_->Handle(message, wParam, lParam);
}

}  // namespace bm
