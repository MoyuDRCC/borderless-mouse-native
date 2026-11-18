# 无界键鼠 · BorderlessMouse

[![Release](https://img.shields.io/github/v/release/MoyuDRCC/borderless-mouse-native)](https://github.com/MoyuDRCC/borderless-mouse-native/releases)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D4)](#系统要求)
[![C++](https://img.shields.io/badge/C%2B%2B-20-00599C)](#构建)

一套键盘鼠标，多台 Windows 电脑。鼠标推到屏幕边缘就滑到相邻电脑，键盘随之切换；剪贴板里的文本、图片与文件也能跨设备同步。

原生 C++20 + Win32 实现，静态 CRT，发布物只有一个 `BorderlessMouse.exe`，不需要 .NET、VC 运行库或任何第三方库。

## 功能

### 键鼠共享

- 同一局域网内自动发现设备，TCP 长连接承载全部输入与数据
- 3×3 布局网格：拖动设备块摆放相对位置，最多九台设备同屏，未摆放的进入待摆放池
- 布局支持撤销；多台设备同时改动时弹出合并预览，确认后才生效
- 鼠标越过屏幕边缘即切换控制，键盘归属跟随指针；被控端检测到本地真实输入时立刻收回控制
- 两种指针模式：「控制」按相对位移驱动对端系统鼠标，「镜像」按源屏幕绝对位置映射到对端桌面
- 远控中按 `Ctrl+Alt+Esc` 或 `Win` 键随时回切本机
- 锁屏、用户切换、休眠时自动暂停共享，恢复后按用户原开关还原；远控期间可隐藏本机光标

### 设备与配对

- 每台设备持有 P-256 身份密钥，首次配对需两端核对同一个 12 位十六进制校验码
- 公钥固定：对端改名或换 IP 不影响认证身份，随时可撤销配对
- 断线自动重连并按主机退避；用户主动断开后不再自动连接
- 支持手动填写 `IP:端口`、开机自启、托盘常驻与单实例（重复启动只唤回已有窗口）
- 按设备设置别名、对端鼠标 DPI 换算、滚轮方向与倍率、是否参与剪贴板同步

### 剪贴板与文件

- 文本、PNG 图片与文件清单同步，单条上限 1–64 MB 可调
- 全局开关与按设备开关并存；关闭后本机与对端都不再收发
- 收到的文件存入 `%USERPROFILE%\Downloads\无界键鼠接收文件`，整库 8 GB 预算，只回收未交付的残片
- 字节流按会话信用窗口限流；取消只作用于未发完的批次，已交付内容不受影响

### 界面与诊断

- 自绘 Win32 界面：概览、设备、布局、日志、设置五个页面，支持 Per-Monitor V2 DPI
- 概览页展示设备名称、局域网地址、监听端口与最近事件，地址可一键复制
- 日志最多保留 500 条；可导出脱敏诊断包（版本、设备信息与最近 200 行日志，设备名与用户目录自动打码）
- 崩溃信息追加到 `%TEMP%\borderless-crash.log`

## 系统要求

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Windows 10（1809 及以上）/ Windows 11，x64 |
| 运行时 | 无，单文件可执行 |
| 网络 | 设备处于同一局域网，IPv4 广播可互通 |
| 构建 | Visual Studio 2022（MSVC C++20）、CMake 3.25+ |

## 构建

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物为 `build/Release/BorderlessMouse.exe`，可直接运行或复制到其他机器。

需要定位内存问题时改用 AddressSanitizer 构建（会切换为动态 CRT，仅用于调试）：

```powershell
cmake -S . -B build-asan -G "Visual Studio 17 2022" -A x64 -DBM_ASAN=ON
cmake --build build-asan --config Debug
```

## 使用

1. 在各台电脑上运行 `BorderlessMouse.exe`，在 Windows 防火墙提示中允许专用网络访问。
2. 打开「设备」页等待自动发现，或手动添加对端的 `IP:端口`（默认 35100）。
3. 首次连接弹出配对确认，核对两端校验码一致后接受。
4. 进入「布局」页，把设备块拖到 3×3 网格中代表相对位置的格子；本机必须留在网格内。
5. 鼠标推到相邻屏幕边缘即可控制对端；也可以直接在「设备」页点「控制」或「镜像」立即接管。

窗口关闭后程序留在托盘；托盘菜单可打开日志或退出。

## 配置与数据

| 内容 | 位置 |
| --- | --- |
| 配置（设备名、端口、配对记录、布局、开关） | `%APPDATA%\BorderlessMouse\config.json` |
| 接收文件 | `%USERPROFILE%\Downloads\无界键鼠接收文件` |
| 崩溃日志 | `%TEMP%\borderless-crash.log` |
| 监听端口 | TCP 35100（可用环境变量 `BORDERLESS_PORT` 覆盖） |
| 设备发现 | UDP 35101，向每块在用网卡发定向广播 |

`config.json` 与原 .NET 版结构一致，可直接沿用。

## 项目结构

| 路径 | 职责 |
| --- | --- |
| `src/app/main.cpp`、`src/app/shell/` | 入口、托盘与主窗口，`AppController` 协调应用状态与核心服务 |
| `src/app/pages/`、`src/app/dialogs/` | 五个设置页面与配对、设备设置、布局合并等对话框 |
| `src/app/ui/` | 自绘 Win32 控件、绘制、主题与 DPI 处理 |
| `src/core/input/`、`src/core/layout/` | 键鼠钩子、输入路由与注入，设备拓扑与屏幕布局 |
| `src/core/net/`、`src/core/protocol/`、`src/core/crypto/` | 设备发现、对等会话、消息编解码、设备身份与会话加密 |
| `src/core/clipboard/` | 剪贴板内容处理与文件收发 |
| `src/core/config/`、`src/core/platform/` | 配置读写、Win32 公共能力与事件日志 |

界面只读 `UiModel`、只通过 `UiActions` 触发动作，`AppController` 不持有任何控件。

## 协议与安全

- **传输**：每条消息是 20 字节定长头（magic、协议版本、消息类型、负载长度、序号，小端序）加 AES-256-GCM 密文与认证标签，负载上限 64 KB，帧头同时作为附加认证数据。
- **会话密钥**：P-256 临时 ECDH → HKDF-SHA256 派生 → 收发两个方向各用独立密钥与 nonce 前缀，接收端只接受严格递增的序号；双方再用 HMAC 确认持有同一会话密钥。
- **身份**：设备长期 P-256 密钥签名握手；私钥经 DPAPI 加密后写入配置，离开当前 Windows 账户无法使用。
- **信任**：广播只提供地址线索，信任始终由带签名的 TCP 握手与人工配对确认建立；首次配对生成的 12 位校验码在两端比对，防止中间人。
- **协议版本**：3，消息布局与原 .NET 版保持一致。

## 开发约定

- 源码为 UTF-8、LF 换行；编译选项为 `/utf-8 /W4 /permissive- /EHsc`。
- 核心逻辑用中文注释；只在确有必要的边界处做校验，不用防御性代码掩盖设计问题。
- 版本号在三处同步：`src/app/resources/app.rc`、`src/app/resources/app.manifest` 与 `AppController::Start` 中的 `SetAppVersion`。
- 更完整的目录职责与构建说明见 [AGENTS.md](AGENTS.md)。

## 版本

当前版本 **1.1.0**，变更记录见 [Releases](https://github.com/MoyuDRCC/borderless-mouse-native/releases)。
