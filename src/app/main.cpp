#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "app/shell/AppController.h"
#include "core/platform/Common.h"
#include "core/platform/EventLog.h"
#include "app/shell/MainWindow.h"
#include "app/shell/Tray.h"
#include "app/ui/Window.h"

using namespace bm;

namespace {

constexpr wchar_t kInstanceMutex[] = L"BorderlessMouse.Instance.";
constexpr wchar_t kShowEvent[] = L"BorderlessMouse.Show.";

std::string InstanceKey() {
  if (const char* port = std::getenv("BORDERLESS_PORT"); port && *port) return port;
  return "default";
}

void LogCrash(const std::exception& error) {
  wchar_t temp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  std::wstring path = std::wstring(temp) + L"borderless-crash.log";
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    std::string text = ToUtf8(L"[") + "崩溃" + "] " + error.what() + "\r\n";
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
  }
  MessageBoxW(nullptr, ToWide(error.what()).c_str(), L"无界键鼠", MB_ICONERROR | MB_OK);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  (void)instance;
  // 常驻托盘的工具只允许一个实例；重复启动只把已有窗口叫回来
  const std::string key = InstanceKey();
  const std::wstring mutexName = kInstanceMutex + ToWide(key);
  HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    const std::wstring eventName = kShowEvent + ToWide(key);
    if (HANDLE signal = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str())) {
      SetEvent(signal);
      CloseHandle(signal);
    }
    return 0;
  }

  HANDLE showEvent = CreateEventW(nullptr, FALSE, FALSE, (kShowEvent + ToWide(key)).c_str());

  Window window;
  if (!window.Init()) return 1;

  Control root;
  if (!window.Create(L"无界键鼠", Size2i{980, 660}, &root)) return 1;

  AppController controller(window);
  MainWindow mainWindow(window, controller.Model(), controller.Actions());
  mainWindow.Build(root);

  Tray tray(window,
            [&window] { window.Show(); },
            [&window, &mainWindow] {
              window.Show();
              mainWindow.ShowLogs();
            },
            [&window] { window.Close(); });
  controller.SetUi(&mainWindow, &tray);

  window.SetCloseHandler([&controller] { controller.OnCloseRequested(); });
  window.SetTimer([&controller, &mainWindow] {
    controller.Tick();
    mainWindow.Refresh();
  });

  // 第二个实例通过命名事件唤回窗口
  const UINT showMessage = RegisterWindowMessageW(L"BorderlessMouse.Show");
  window.AddMessageHook([&window, showMessage](UINT message, WPARAM, LPARAM) {
    if (message != showMessage) return false;
    window.Show();
    window.PostRedraw();
    return true;
  });
  if (showEvent) {
    std::thread([showEvent, hwnd = window.Handle(), showMessage] {
      while (WaitForSingleObject(showEvent, INFINITE) == WAIT_OBJECT_0)
        PostMessageW(hwnd, showMessage, 0, 0);
      CloseHandle(showEvent);
    }).detach();
  }

  try {
    controller.Start();
  } catch (const std::exception& error) {
    LogCrash(error);
    controller.Shutdown();
    return 1;
  }

  window.Show();
  const int code = window.RunMessageLoop();
  controller.Shutdown();
  tray.SetVisible(false);
  if (mutex) CloseHandle(mutex);
  return code;
}
