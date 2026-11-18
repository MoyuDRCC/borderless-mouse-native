#pragma once

// 全项目只有这里引入 windows.h：钩子类型、鼠标/键盘消息、SendInput 标志、RawInput 结构体
// 都直接用系统定义，本文件只补 .NET 版 Input/Win32.cs 里需要多个 API 组合才能完成的部分。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace bm::win {

// 桌面对象名；取不到时返回空串
std::wstring DesktopName(HDESK desktop);
// 当前输入桌面就是本线程所在桌面：不切换、不附着安全桌面
bool IsOwnInputDesktop();
// 输入桌面可用：名称一致且能读到光标位置；锁屏或桌面不可用时为 false
bool IsInputDesktopCurrent();
// 1×1 全透明光标：AND 位全 1 保持屏幕不变，XOR 位全 0 不反色
HCURSOR CreateTransparentCursor();
// 左右修饰键的物理标识：VK_SHIFT/VK_CONTROL/VK_MENU 按扫描码与扩展位区分左右
int PhysicalKeyId(int virtualKey, int scanCode, bool extended);

// Win32 调用失败转异常，附带调用名与 GetLastError
[[noreturn]] void ThrowLastError(const char* call);

}  // namespace bm::win
