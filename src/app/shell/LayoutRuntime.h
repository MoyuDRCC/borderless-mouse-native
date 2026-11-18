#pragma once

#include "app/UiModel.h"
#include "core/layout/Topology.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

namespace bm {

struct AppConfig;
class DeviceIdentity;
class IInputSink;
class InputService;
class PeerHost;

// 布局协作：接收并合并对端拓扑、推动待处理合并的确认流程、把结果交给输入层与界面模型。
// 拓扑改动统一在这里写回配置并广播给对端；界面只读填充进 UiModel 的结果。
class LayoutRuntime {
 public:
  struct Callbacks {
    // 需要用户知道的失败与提示
    std::function<void(const std::string&, LogLevel)> report;
    // 会话对应的输入出口；该设备当前没有会话时返回空指针
    std::function<std::shared_ptr<IInputSink>(const std::string& peerId)> sessionSink;
  };

  LayoutRuntime(AppConfig& config, Callbacks callbacks);

  // 启动过程中才就绪的依赖：身份、主机、输入服务、界面弹窗入口，各自就绪时挂上
  void SetIdentity(const DeviceIdentity* identity) { identity_ = identity; }
  void SetPeerHost(PeerHost* host) { host_ = host; }
  void SetInputService(InputService* input) { input_ = input; }
  void SetMergePrompt(std::function<void(const MergeCandidate&)> prompt) { mergePrompt_ = std::move(prompt); }

  // 收到对端拓扑帧；由会话数据回调转交
  void ReceiveTopology(std::span<const uint8_t> payload);
  void ApplyReceivedTopology();
  // 500ms 定时：先回收合并弹窗结果，再对尚未问过的版本发起一次询问
  void Tick();

  // 布局页的四个动作
  void MoveCell(int fromRow, int fromColumn, int toRow, int toColumn);
  void PlaceDevice(const std::string& id, int row, int column);
  void PoolDevice(const std::string& id, bool toPool);
  void UndoLayout();
  void ResolveMerge(uint64_t revision, bool accepted);

  // 取消配对后把设备标为移除；别名变化后把新名字写进布局
  void RemoveDevice(const std::string& id);
  void RenameDevice(const std::string& id);
  void ClearUndo() { hasUndo_ = false; }

  // 新连上的设备自动摆到本机旁边的空格
  void AutoPlacePeer(const std::string& peerId, const std::string& name);
  // 本机必须在布局中占一格；缺失时补回
  void EnsureLocalDevice(TopologySnapshot& snapshot);
  // 把当前布局发布给输入层
  void ApplyTopologyToInput();

  // 布局相关的界面模型字段
  void FillModel(UiModel& model);
  // 配置、配对与布局里记录的显示名
  std::string DisplayName(const std::string& peerId) const;

 private:
  void Report(const std::string& text, LogLevel level = LogLevel::Info);
  bool CommitTopology(TopologySnapshot snapshot, bool rememberUndo);
  void RefreshMergeCandidate();
  // 由九格内容重建快照：格子里的设备记新时钟，其余移入未摆放池
  void RebuildLayout(const std::string (&cells)[kLayoutRows][kLayoutColumns]);

  AppConfig& config_;
  Callbacks callbacks_;
  const DeviceIdentity* identity_ = nullptr;
  PeerHost* host_ = nullptr;
  InputService* input_ = nullptr;
  std::function<void(const MergeCandidate&)> mergePrompt_;

  std::mutex incomingMutex_;
  std::optional<TopologySnapshot> incomingTopology_;
  std::optional<TopologySnapshot> pendingTopology_;
  std::optional<MergeCandidate> mergeCandidate_;
  uint64_t nextMergeRevision_ = 0;
  uint64_t promptedRevision_ = 0;
  uint64_t visibleRevision_ = 0;
  TopologySnapshot undoTopology_;
  bool hasUndo_ = false;
};

}  // namespace bm
