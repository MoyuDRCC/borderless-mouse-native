#pragma once

#include "core/platform/Common.h"
#include "core/platform/EventLog.h"
#include "core/layout/Topology.h"

#include <functional>
#include <string>
#include <vector>

namespace bm {

// 设备行展示所需的全部状态
struct DeviceRow {
  bool local = false;  // 本机行：不显示操作按钮
  std::string id;
  std::string name;
  std::string address;
  int port = 35100;
  std::string version;
  bool paired = false;
  bool connected = false;
  bool connecting = false;  // 已发起连接、会话尚未建立
  bool discovered = false;
  bool controlling = false;
  bool controlled = false;
  std::string alias;
  int pointerDpi = 0;
  int wheelScalePercent = 100;
  bool wheelReverse = false;
  bool clipboardEnabled = true;
};

// 待确认的布局合并：预览九格与来源
struct MergeCandidate {
  uint64_t revision = 0;
  std::string peerName;
  std::vector<TopologyDevice> preview;  // Resolve 结果
  TopologySnapshot snapshot;
};

struct UiSettings {
  bool sharingEnabled = true;
  bool clipboardEnabled = false;
  bool hideCursorWhileRemote = true;
  bool autoStart = false;
  int mouseDpi = 1600;
  int clipboardLimitMb = 64;
};

struct UiStats {
  int roundTripMs = 0;
  int lastInjectMs = 0;
  int worstInjectMs = 0;
  int pendingFrames = 0;
};

// 界面每 500ms 读一次的快照；控制器负责填充
struct UiModel {
  std::string localId;
  std::string localName;
  std::string localAddress;
  int port = 35100;
  std::string version;
  int protocolVersion = 3;
  std::string statusText;
  std::vector<DeviceRow> devices;
  std::vector<LogEntry> events;   // 概览页最近事件
  std::vector<LogEntry> logs;     // 日志页全部（最多 500）
  TopologySnapshot topology;
  bool canUndoLayout = false;  // 有可撤销的上一版布局
  std::vector<std::pair<std::string, std::string>> pooled;  // 标识、显示名
  std::vector<MergeCandidate> merges;
  UiSettings settings;
  UiStats stats;
};

// 连接与设备：连接、断开、控制、镜像、配对与设备设置
struct ConnectionActions {
  std::function<void(std::string name, std::string address, int port)> addDevice;
  std::function<void(std::string id)> connect;
  std::function<void(std::string id)> disconnect;
  std::function<void(std::string id)> startControl;
  std::function<void(std::string id)> startMirror;
  std::function<void(std::string id)> unpair;
  std::function<void(std::string id)> removeDevice;
  std::function<void(std::string id, DeviceRow updated)> saveDeviceSettings;
};

// 布局：拖拽、移池、撤销与合并确认
struct LayoutActions {
  std::function<void(int fromRow, int fromColumn, int toRow, int toColumn)> moveLayoutCell;
  // 从未摆放池拖进网格：按设备标识落到指定格
  std::function<void(std::string id, int row, int column)> placeDevice;
  std::function<void(std::string id, bool toPool)> poolDevice;
  std::function<void()> undoLayout;
  std::function<void(uint64_t revision, bool accepted)> resolveMerge;
  // 布局页需要让用户知道的提示，交给控制器的日志
  std::function<void(std::string message)> report;
};

// 应用级开关与生命周期
struct AppActions {
  std::function<void(bool)> setSharing;
  std::function<void(bool)> setClipboardSync;
  std::function<void(bool)> setHideCursor;
  std::function<void(bool)> setAutoStart;
  std::function<void(int)> setMouseDpi;
  std::function<void(int)> setClipboardLimit;
  std::function<void()> exportDiagnostics;
  std::function<void()> clearLogs;
  std::function<void()> quit;
};

// 界面能触发的全部动作；由控制器实现
struct UiActions {
  ConnectionActions connection;
  LayoutActions layout;
  AppActions app;
};

}  // namespace bm
