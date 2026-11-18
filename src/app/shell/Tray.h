#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace bm {

class Window;

// 托盘图标：关闭窗口后驻留，菜单提供回到窗口、打开日志、退出
class Tray {
 public:
  Tray(Window& window, std::function<void()> showWindow, std::function<void()> showLogs,
       std::function<void()> quit);
  ~Tray();
  Tray(const Tray&) = delete;
  Tray& operator=(const Tray&) = delete;

  // 图标提示文字随连接状态变化
  void SetStatus(const std::wstring& text);
  void SetVisible(bool visible);
  // 托盘窗口过程由实现自管；为此需要窗口消息转发钩子
  void HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bm