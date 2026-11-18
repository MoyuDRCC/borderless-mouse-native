#include "core/layout/Topology.h"

#include "core/protocol/FrameCodec.h"
#include "core/config/Json.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace bm {

namespace {

constexpr size_t kMaxPayloadBytes = kMaxPayload - kTagSize;

json::Value DeviceToJson(const TopologyDevice& device) {
  json::Value value = json::Value::MakeObject();
  value.Set("DeviceId", device.deviceId);
  value.Set("Name", device.name);
  value.Set("Row", device.row);
  value.Set("Column", device.column);
  value.Set("Clock", device.clock);
  value.Set("AuthorId", device.authorId);
  value.Set("State", static_cast<int>(device.state));
  return value;
}

TopologyDevice DeviceFromJson(const json::Value& value) {
  TopologyDevice device;
  device.deviceId = value["DeviceId"].AsString();
  device.name = value["Name"].AsString();
  device.row = static_cast<int>(value["Row"].AsInt());
  device.column = static_cast<int>(value["Column"].AsInt());
  device.clock = value["Clock"].AsInt();
  device.authorId = value["AuthorId"].AsString();
  int state = static_cast<int>(value["State"].AsInt());
  if (state < 0 || state > 2) throw std::runtime_error("布局设备状态非法");
  device.state = static_cast<TopologyDeviceState>(state);
  return device;
}

}  // namespace

int TopologyCodec::Compare(const TopologySnapshot& left, const TopologySnapshot& right) {
  if (left.clock != right.clock) return left.clock < right.clock ? -1 : 1;
  int order = left.authorId.compare(right.authorId);
  return order == 0 ? 0 : (order < 0 ? -1 : 1);
}

int64_t TopologyCodec::MaxClock() {
  return static_cast<int64_t>(UnixMs()) + kFutureAllowanceMs;
}

int64_t TopologyCodec::NextClock(int64_t previous) {
  int64_t now = static_cast<int64_t>(UnixMs());
  if (previous < 0 || previous >= now + kFutureAllowanceMs)
    throw std::runtime_error("布局时钟超出未来五分钟，请校准系统时间后重试。");
  return std::max(now, previous + 1);
}

bool TopologyCodec::IsIdentity(std::string_view id) {
  if (id.size() != 64) return false;
  for (char c : id)
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
  return true;
}

bool TopologyCodec::IsValid(const TopologySnapshot& snapshot) {
  if (snapshot.clock < 1 || snapshot.clock > MaxClock() || !IsIdentity(snapshot.authorId)) return false;
  if (snapshot.devices.size() > kMaxRecords) return false;
  size_t placed = 0;
  std::set<std::string> identities;
  for (const TopologyDevice& device : snapshot.devices) {
    if (!IsIdentity(device.deviceId) || device.name.empty() || device.name.size() > 255) return false;
    if (!identities.insert(device.deviceId).second) return false;
    if (device.clock < 1 || device.clock > snapshot.clock || !IsIdentity(device.authorId)) return false;
    if (device.state != TopologyDeviceState::Placed && device.state != TopologyDeviceState::Pooled &&
        device.state != TopologyDeviceState::Removed)
      return false;
    if (device.state == TopologyDeviceState::Placed) {
      if (device.row < 0 || device.row >= kLayoutRows || device.column < 0 || device.column >= kLayoutColumns) return false;
      placed++;
    }
  }
  return placed <= static_cast<size_t>(kLayoutCapacity);
}

Bytes TopologyCodec::Encode(const TopologySnapshot& snapshot) {
  if (!IsValid(snapshot)) throw std::invalid_argument("设备布局无效或时钟超出未来五分钟");
  json::Value value = json::Value::MakeObject();
  value.Set("Clock", snapshot.clock);
  value.Set("AuthorId", snapshot.authorId);
  json::Value devices = json::Value::MakeArray();
  for (const TopologyDevice& device : snapshot.devices) devices.Push(DeviceToJson(device));
  value.Set("Devices", std::move(devices));
  std::string text = value.Dump();
  if (text.size() > kMaxPayloadBytes) throw std::invalid_argument("设备布局超过传输容量");
  return Bytes(text.begin(), text.end());
}

bool TopologyCodec::TryDecode(std::span<const uint8_t> bytes, TopologySnapshot& snapshot) {
  if (bytes.size() > kMaxPayloadBytes) return false;
  try {
    json::Value value = json::Value::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    TopologySnapshot parsed;
    parsed.clock = value["Clock"].AsInt();
    parsed.authorId = value["AuthorId"].AsString();
    for (const json::Value& device : value["Devices"].AsArray()) parsed.devices.push_back(DeviceFromJson(device));
    if (!IsValid(parsed)) return false;
    snapshot = std::move(parsed);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

int TopologyMerge::CompareDevices(const TopologyDevice& left, const TopologyDevice& right) {
  if (left.clock != right.clock) return left.clock < right.clock ? -1 : 1;
  int order = left.authorId.compare(right.authorId);
  if (order != 0) return order < 0 ? -1 : 1;
  // 同一版本的数据不一致也采用固定顺序，避免双方因到达顺序不同而分叉
  if (left.state != right.state) return static_cast<int>(left.state) < static_cast<int>(right.state) ? -1 : 1;
  if (left.row != right.row) return left.row < right.row ? -1 : 1;
  if (left.column != right.column) return left.column < right.column ? -1 : 1;
  order = left.name.compare(right.name);
  if (order != 0) return order < 0 ? -1 : 1;
  order = left.deviceId.compare(right.deviceId);
  return order == 0 ? 0 : (order < 0 ? -1 : 1);
}

TopologySnapshot TopologyMerge::Merge(const TopologySnapshot& current, const TopologySnapshot& incoming) {
  TopologySnapshot latest = TopologyCodec::Compare(current, incoming) >= 0 ? current : incoming;
  std::map<std::string, TopologyDevice> merged;
  for (const TopologyDevice& device : current.devices) merged[device.deviceId] = device;
  for (const TopologyDevice& device : incoming.devices) {
    auto existing = merged.find(device.deviceId);
    if (existing == merged.end() || CompareDevices(device, existing->second) > 0) merged[device.deviceId] = device;
  }
  latest.devices.clear();
  for (auto& [id, device] : merged) latest.devices.push_back(std::move(device));
  return latest;
}

bool TopologyMerge::SameEntries(const TopologySnapshot& left, const TopologySnapshot& right) {
  if (left.devices.size() != right.devices.size()) return false;
  std::vector<const TopologyDevice*> leftSorted, rightSorted;
  for (const auto& device : left.devices) leftSorted.push_back(&device);
  for (const auto& device : right.devices) rightSorted.push_back(&device);
  auto byId = [](const TopologyDevice* a, const TopologyDevice* b) { return a->deviceId < b->deviceId; };
  std::sort(leftSorted.begin(), leftSorted.end(), byId);
  std::sort(rightSorted.begin(), rightSorted.end(), byId);
  for (size_t i = 0; i < leftSorted.size(); i++) {
    const TopologyDevice& a = *leftSorted[i];
    const TopologyDevice& b = *rightSorted[i];
    if (a.deviceId != b.deviceId || a.name != b.name || a.row != b.row || a.column != b.column ||
        a.clock != b.clock || a.authorId != b.authorId || a.state != b.state)
      return false;
  }
  return true;
}

std::vector<TopologyDevice> TopologyMerge::Resolve(const TopologySnapshot& snapshot) {
  std::vector<TopologyDevice> placed;
  for (const TopologyDevice& device : snapshot.devices)
    if (device.state == TopologyDeviceState::Placed) placed.push_back(device);
  if (placed.size() > static_cast<size_t>(kLayoutCapacity)) throw std::runtime_error("3×3 网格最多摆放 9 台设备。");
  std::sort(placed.begin(), placed.end(),
            [](const TopologyDevice& a, const TopologyDevice& b) { return CompareDevices(a, b) > 0; });

  std::set<std::pair<int, int>> occupied;
  std::vector<TopologyDevice> resolved;
  std::vector<TopologyDevice> collisions;
  // 先预留所有无冲突的期望格，碰撞设备不能抢走别人的原位置
  for (const TopologyDevice& device : placed) {
    if (occupied.insert({device.row, device.column}).second) {
      resolved.push_back(device);
    } else {
      collisions.push_back(device);
    }
  }
  for (TopologyDevice device : collisions) {
    int bestRow = 0, bestColumn = 0, bestDistance = INT32_MAX;
    for (int row = 0; row < kLayoutRows; row++) {
      for (int column = 0; column < kLayoutColumns; column++) {
        if (occupied.count({row, column})) continue;
        int distance = std::abs(row - device.row) + std::abs(column - device.column);
        if (distance < bestDistance || (distance == bestDistance && (row < bestRow || (row == bestRow && column < bestColumn)))) {
          bestDistance = distance;
          bestRow = row;
          bestColumn = column;
        }
      }
    }
    occupied.insert({bestRow, bestColumn});
    device.row = bestRow;
    device.column = bestColumn;
    resolved.push_back(device);
  }
  std::sort(resolved.begin(), resolved.end(), [](const TopologyDevice& a, const TopologyDevice& b) {
    if (a.row != b.row) return a.row < b.row;
    return a.column < b.column;
  });
  return resolved;
}

}  // namespace bm
