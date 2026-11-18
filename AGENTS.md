# 无界键鼠开发指南

无界键鼠（BorderlessMouse）是原生 Win32 桌面应用，用于在局域网设备之间共享键鼠，并同步剪贴板和传输文件。本指南适用于整个项目。

## 构建

使用 Windows、CMake 3.25 或更高版本以及 MSVC 的 C++20 工具链。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Release 产物位于 `build/Release/BorderlessMouse.exe`。构建设置维护在根目录的 [CMakeLists.txt](CMakeLists.txt)，`src/app/` 和 `src/core/` 下的 `.cpp` 文件由 CMake 自动收集。

## 代码入口

| 路径 | 职责 |
| --- | --- |
| `src/app/main.cpp`、`src/app/shell/` | `wWinMain` 负责初始化窗口和托盘，`AppController` 协调应用状态与核心服务。 |
| `src/app/pages/`、`src/app/dialogs/`、`src/app/ui/` | 设置页面和对话框，以及可复用的 Win32 控件、绘制和 DPI 处理。 |
| `src/core/input/`、`src/core/layout/` | 键鼠钩子、输入路由与注入、设备拓扑和屏幕布局。 |
| `src/core/net/`、`src/core/protocol/`、`src/core/crypto/` | 设备发现、对等会话、消息编解码、设备身份和会话加密。 |
| `src/core/clipboard/` | 剪贴板内容处理与文件收发，应用侧联动位于 `src/app/shell/`。 |
| `src/core/config/`、`src/core/platform/` | 配置读写、Win32 公共能力和事件日志。 |


