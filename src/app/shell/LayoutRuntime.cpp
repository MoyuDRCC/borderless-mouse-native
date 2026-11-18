#include "app/shell/LayoutRuntime.h"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

#include "core/config/AppConfig.h"
#include "core/crypto/DeviceIdentity.h"
#include "core/input/InputService.h"
#include "core/input/InputTopology.h"
#include "core/net/PeerHost.h"

namespace bm {

LayoutRuntime::LayoutRuntime(AppConfig& config, Callbacks callbacks)
    : config_(config), callbacks_(std::move(callbacks)) {}

void LayoutRuntime::Report(const std::string& text, LogLevel level) { callbacks_.report(text, level); }

void LayoutRuntime::ReceiveTopology(std::span<const uint8_t> payload) {
  TopologySnapshot incoming;
  if (!TopologyCodec::TryDecode(payload, incoming)) {
    Report("收到无效设备布局或超过本机时间五分钟的布局，已拒收；请校准双方系统时间。", LogLevel::Warning);
    return;
  }
  std::lock_guard lock(incomingMutex_);
  auto combined = incomingTopology_ ? TopologyMerge::Merge(*incomingTopology_, incoming) : std::move(incoming);
  if (combined.devices.size() <= TopologyCodec::kMaxRecords * 2) incomingTopology_ = std::move(combined);
  else Report("收到过多不同设备的布局，已保留此前收到的更新", LogLevel::Warning);
}

void LayoutRuntime::ApplyReceivedTopology() {
  std::optional<TopologySnapshot> incoming;
  {
    std::lock_guard lock(incomingMutex_);
    incoming.swap(incomingTopology_);
  }
  if (!incoming) return;
  // 网络线程只合并收到的消息，界面线程再与当前本地配置合并。
  TopologySnapshot pending = pendingTopology_ ? TopologyMerge::Merge(*pendingTopology_, *incoming)
                                               : std::move(*incoming);
  if (TopologyMerge::Merge(config_.topology, pending).devices.size() > TopologyCodec::kMaxRecords * 2) {
    Report("待处理布局已达到 64 个设备状态，请先处理已有合并；新布局未接纳。", LogLevel::Warning);
    return;
  }
  pendingTopology_ = std::move(pending);
  RefreshMergeCandidate();
}

void LayoutRuntime::RefreshMergeCandidate() {
  if (!pendingTopology_) return;
  auto merged = TopologyMerge::Merge(config_.topology, *pendingTopology_);
  if (TopologyMerge::SameEntries(config_.topology, merged)) {
    pendingTopology_.reset();
    mergeCandidate_.reset();
    return;
  }
  if (mergeCandidate_ && TopologyMerge::SameEntries(mergeCandidate_->snapshot, merged) &&
      TopologyCodec::Compare(mergeCandidate_->snapshot, merged) == 0)
    return;

  // 确认只对当前可见的候选有效，迟到的决定不能覆盖新布局。
  MergeCandidate candidate;
  candidate.revision = ++nextMergeRevision_;
  candidate.preview = TopologyMerge::Resolve(merged);
  candidate.snapshot = std::move(merged);
  mergeCandidate_ = std::move(candidate);
}

void LayoutRuntime::EnsureLocalDevice(TopologySnapshot& snapshot) {
  for (const TopologyDevice& device : snapshot.devices)
    if (device.deviceId == identity_->Id() && device.state == TopologyDeviceState::Placed) return;
  int64_t clock = TopologyCodec::NextClock(snapshot.clock);
  snapshot.devices.erase(std::remove_if(snapshot.devices.begin(), snapshot.devices.end(),
                                        [this](const TopologyDevice& device) { return device.deviceId == identity_->Id(); }),
                         snapshot.devices.end());
  snapshot.devices.push_back(TopologyDevice{identity_->Id(), config_.deviceName, 1, 1, clock, identity_->Id(),
                                            TopologyDeviceState::Placed});
  snapshot.clock = clock;
  snapshot.authorId = identity_->Id();
}

bool LayoutRuntime::CommitTopology(TopologySnapshot snapshot, bool rememberUndo) {
  EnsureLocalDevice(snapshot);
  if (snapshot.devices.size() > TopologyCodec::kMaxRecords) {
    Report("布局最多保留 32 个设备状态（包含移池与撤销记录），未保存此次修改。", LogLevel::Warning);
    return false;
  }
  Bytes payload;
  try {
    payload = TopologyCodec::Encode(snapshot);
  } catch (const std::exception& error) {
    Report(std::string("设备布局无法保存：") + error.what(), LogLevel::Warning);
    return false;
  }
  AppConfig updated = config_;
  updated.topology = std::move(snapshot);
  updated.hasTopology = true;
  try {
    updated.Save();
  } catch (const std::exception& error) {
    Report(std::string("布局保存失败：") + error.what(), LogLevel::Error);
    return false;
  }
  if (rememberUndo) {
    undoTopology_ = std::move(config_.topology);
    hasUndo_ = true;
  }
  config_.topology = std::move(updated.topology);
  config_.hasTopology = true;
  RefreshMergeCandidate();
  ApplyTopologyToInput();
  for (const PeerSessionPtr& session : host_->Sessions()) session->Send(MessageType::Topology, payload);
  return true;
}

void LayoutRuntime::AutoPlacePeer(const std::string& peerId, const std::string& name) {
  for (const TopologyDevice& device : config_.topology.devices)
    if (device.deviceId == peerId && device.state == TopologyDeviceState::Placed) return;

  // 新连上的设备自动摆到本机旁边的空格，不打断已有布局
  auto placed = TopologyMerge::Resolve(config_.topology);
  int localRow = 1, localColumn = 1;
  bool occupied[kLayoutRows][kLayoutColumns] = {};
  for (const TopologyDevice& device : placed) {
    occupied[device.row][device.column] = true;
    if (device.deviceId == identity_->Id()) {
      localRow = device.row;
      localColumn = device.column;
    }
  }
  const int offsets[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};
  int row = -1, column = -1;
  for (const auto& offset : offsets) {
    const int r = localRow + offset[0];
    const int c = localColumn + offset[1];
    if (r >= 0 && r < kLayoutRows && c >= 0 && c < kLayoutColumns && !occupied[r][c]) {
      row = r;
      column = c;
      break;
    }
  }
  // 九格已满时把设备留在未摆放池，并在布局页显示出来等用户腾格子
  const TopologyDeviceState state = row < 0 ? TopologyDeviceState::Pooled : TopologyDeviceState::Placed;

  TopologySnapshot snapshot = config_.topology;
  const int64_t clock = TopologyCodec::NextClock(snapshot.clock);
  snapshot.clock = clock;
  snapshot.authorId = identity_->Id();
  snapshot.devices.erase(std::remove_if(snapshot.devices.begin(), snapshot.devices.end(),
                                        [&peerId](const TopologyDevice& device) { return device.deviceId == peerId; }),
                         snapshot.devices.end());
  snapshot.devices.push_back(TopologyDevice{peerId, name, row, column, clock, identity_->Id(), state});
  if (CommitTopology(std::move(snapshot), true) && row < 0)
    Report("九格布局已满（最多 9 台）。新配对设备保留在未摆放池，请先明确移出一台设备再摆放。",
           LogLevel::Warning);
}

void LayoutRuntime::ApplyTopologyToInput() {
  if (!input_) return;
  std::vector<InputDevice> devices;
  std::vector<std::string> taken;  // 名字是输入层的路由键，重名设备加序号区分
  const std::vector<TopologyDevice> placed = TopologyMerge::Resolve(config_.topology);
  for (const TopologyDevice& device : placed) {
    std::shared_ptr<IInputSink> sink;
    std::string name = device.name;
    if (device.deviceId == identity_->Id()) {
      name = config_.deviceName;
    } else {
      sink = callbacks_.sessionSink(device.deviceId);
      if (name.empty()) name = DisplayName(device.deviceId);
    }
    for (int index = 2; std::find(taken.begin(), taken.end(), name) != taken.end(); ++index) {
      name = DisplayName(device.deviceId) + " (" + std::to_string(index) + ")";
    }
    taken.push_back(name);
    devices.push_back(InputDevice{name, device.row, device.column, sink});
  }
  input_->PublishTopology(std::make_shared<const InputTopology>(config_.deviceName, std::move(devices)));
}

std::string LayoutRuntime::DisplayName(const std::string& peerId) const {
  for (const DeviceSetting& setting : config_.deviceSettings)
    if (setting.id == peerId && !setting.alias.empty()) return setting.alias;
  for (const PairedPeer& peer : config_.pairedPeers)
    if (peer.id == peerId && !peer.name.empty()) return peer.name;
  for (const TopologyDevice& device : config_.topology.devices)
    if (device.deviceId == peerId && !device.name.empty()) return device.name;
  return peerId.size() > 8 ? peerId.substr(0, 8) : peerId;
}

void LayoutRuntime::MoveCell(int fromRow, int fromColumn, int toRow, int toColumn) {
  if (fromRow < 0 || fromRow >= kLayoutRows || fromColumn < 0 || fromColumn >= kLayoutColumns) return;
  if (fromRow == toRow && fromColumn == toColumn) return;
  std::string cells[kLayoutRows][kLayoutColumns];
  for (const TopologyDevice& device : TopologyMerge::Resolve(config_.topology))
    cells[device.row][device.column] = device.deviceId;
  std::swap(cells[fromRow][fromColumn], cells[toRow][toColumn]);
  RebuildLayout(cells);
}

void LayoutRuntime::PlaceDevice(const std::string& id, int row, int column) {
  if (row < 0 || row >= kLayoutRows || column < 0 || column >= kLayoutColumns) return;
  std::string cells[kLayoutRows][kLayoutColumns];
  for (const TopologyDevice& device : TopologyMerge::Resolve(config_.topology)) {
    if (device.deviceId != id) cells[device.row][device.column] = device.deviceId;
  }
  cells[row][column] = id;
  RebuildLayout(cells);
}

void LayoutRuntime::PoolDevice(const std::string& id, bool toPool) {
  std::string cells[kLayoutRows][kLayoutColumns];
  for (const TopologyDevice& device : TopologyMerge::Resolve(config_.topology))
    cells[device.row][device.column] = device.deviceId;
  for (int row = 0; row < kLayoutRows; row++)
    for (int column = 0; column < kLayoutColumns; column++)
      if (cells[row][column] == id) cells[row][column].clear();
  if (!toPool) return;
  RebuildLayout(cells);
}

void LayoutRuntime::UndoLayout() {
  if (!hasUndo_) return;
  TopologySnapshot snapshot = undoTopology_;
  const int64_t clock = TopologyCodec::NextClock(std::max(snapshot.clock, config_.topology.clock));
  for (TopologyDevice& device : snapshot.devices) {
    if (device.state == TopologyDeviceState::Removed) continue;
    device.clock = clock;
    device.authorId = identity_->Id();
  }
  snapshot.clock = clock;
  snapshot.authorId = identity_->Id();
  EnsureLocalDevice(snapshot);
  if (CommitTopology(std::move(snapshot), false)) hasUndo_ = false;
}

void LayoutRuntime::ResolveMerge(uint64_t revision, bool accepted) {
  if (revision == visibleRevision_) visibleRevision_ = 0;
  if (!mergeCandidate_ || revision != mergeCandidate_->revision) {
    if (accepted) Report("布局预览已变化，请查看最新候选后确认", LogLevel::Warning);
    return;
  }
  promptedRevision_ = revision;
  if (accepted) {
    if (CommitTopology(mergeCandidate_->snapshot, true)) Report("已合并对端设备布局");
  } else {
    Report("已保留当前布局，待处理的合并仍留在布局页", LogLevel::Info);
  }
}

void LayoutRuntime::RemoveDevice(const std::string& id) {
  TopologySnapshot snapshot = config_.topology;
  const int64_t clock = TopologyCodec::NextClock(snapshot.clock);
  snapshot.clock = clock;
  snapshot.authorId = identity_->Id();
  for (TopologyDevice& device : snapshot.devices) {
    if (device.deviceId != id) continue;
    device.state = TopologyDeviceState::Removed;
    device.row = -1;
    device.column = -1;
    device.clock = clock;
    device.authorId = identity_->Id();
  }
  if (CommitTopology(std::move(snapshot), false)) hasUndo_ = false;
}

void LayoutRuntime::RenameDevice(const std::string& id) {
  TopologySnapshot snapshot = config_.topology;
  bool renamed = false;
  const int64_t clock = TopologyCodec::NextClock(snapshot.clock);
  for (TopologyDevice& device : snapshot.devices) {
    if (device.deviceId != id) continue;
    const std::string name = DisplayName(id);
    if (device.name == name) continue;
    device.name = name;
    device.clock = clock;
    device.authorId = identity_->Id();
    renamed = true;
  }
  if (renamed) {
    snapshot.clock = clock;
    snapshot.authorId = identity_->Id();
    CommitTopology(std::move(snapshot), false);
  }
}

// 由九格内容重建快照：格子里的设备记新时钟，其余保留原状态并移出网格
void LayoutRuntime::RebuildLayout(const std::string (&cells)[kLayoutRows][kLayoutColumns]) {
  TopologySnapshot snapshot = config_.topology;
  const int64_t clock = TopologyCodec::NextClock(snapshot.clock);
  snapshot.clock = clock;
  snapshot.authorId = identity_->Id();

  std::map<std::string, TopologyDevice> entries;
  for (const TopologyDevice& device : snapshot.devices) entries[device.deviceId] = device;
  for (int row = 0; row < kLayoutRows; row++) {
    for (int column = 0; column < kLayoutColumns; column++) {
      const std::string& id = cells[row][column];
      if (id.empty()) continue;
      TopologyDevice& device = entries[id];
      if (device.deviceId.empty()) device.deviceId = id;
      device.name = (id == identity_->Id()) ? config_.deviceName : DisplayName(id);
      device.row = row;
      device.column = column;
      device.state = TopologyDeviceState::Placed;
      device.clock = clock;
      device.authorId = identity_->Id();
    }
  }
  const std::string localId = identity_->Id();
  bool localPlaced = false;
  for (int row = 0; row < kLayoutRows; row++)
    for (int column = 0; column < kLayoutColumns; column++)
      if (cells[row][column] == localId) localPlaced = true;
  if (!localPlaced) return;  // 本机必须留在九格布局中

  snapshot.devices.clear();
  for (auto& [id, device] : entries) {
    bool placed = false;
    for (int row = 0; row < kLayoutRows && !placed; row++)
      for (int column = 0; column < kLayoutColumns && !placed; column++)
        if (cells[row][column] == id) placed = true;
    if (!placed && device.state != TopologyDeviceState::Removed) {
      device.state = TopologyDeviceState::Pooled;
      device.row = -1;
      device.column = -1;
      device.clock = clock;
      device.authorId = identity_->Id();
    }
    snapshot.devices.push_back(std::move(device));
  }
  EnsureLocalDevice(snapshot);
  CommitTopology(std::move(snapshot), true);
}

void LayoutRuntime::Tick() {
  ApplyReceivedTopology();
  if (!mergeCandidate_ || !mergePrompt_ || visibleRevision_ != 0 ||
      promptedRevision_ == mergeCandidate_->revision)
    return;
  promptedRevision_ = visibleRevision_ = mergeCandidate_->revision;
  mergePrompt_(*mergeCandidate_);
}

void LayoutRuntime::FillModel(UiModel& model) {
  model.topology = config_.topology;
  model.canUndoLayout = hasUndo_;
  model.pooled.clear();
  for (const TopologyDevice& device : config_.topology.devices) {
    if (device.state != TopologyDeviceState::Pooled) continue;
    model.pooled.emplace_back(device.deviceId, device.name.empty() ? DisplayName(device.deviceId) : device.name);
  }
  model.merges.clear();
  if (mergeCandidate_) model.merges.push_back(*mergeCandidate_);
}

}  // namespace bm
