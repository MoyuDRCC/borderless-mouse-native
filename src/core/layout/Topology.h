#pragma once

#include "core/platform/Common.h"

#include <string>
#include <vector>

namespace bm {

enum class TopologyDeviceState { Placed = 0, Pooled = 1, Removed = 2 };

// Row/Column 是期望位置；移池和撤销也是带版本的记录，不能用缺失条目表达
struct TopologyDevice {
  std::string deviceId;
  std::string name;
  int row = 0;
  int column = 0;
  int64_t clock = 0;
  std::string authorId;
  TopologyDeviceState state = TopologyDeviceState::Placed;
};

// 快照保持全序；每台设备保留自己的编辑版本，合并不会把缺失误当删除
struct TopologySnapshot {
  int64_t clock = 0;
  std::string authorId;
  std::vector<TopologyDevice> devices;
};

constexpr int kLayoutRows = 3;
constexpr int kLayoutColumns = 3;
constexpr int kLayoutCapacity = kLayoutRows * kLayoutColumns;

class TopologyCodec {
 public:
  static constexpr size_t kMaxRecords = 32;
  static constexpr int64_t kFutureAllowanceMs = 5 * 60 * 1000;

  static int Compare(const TopologySnapshot& left, const TopologySnapshot& right);
  static int64_t MaxClock();
  // 时钟取 UTC 毫秒与上次版本的较大值；最多借用未来五分钟
  static int64_t NextClock(int64_t previous);
  static bool IsIdentity(std::string_view id);
  static bool IsValid(const TopologySnapshot& snapshot);
  static Bytes Encode(const TopologySnapshot& snapshot);
  static bool TryDecode(std::span<const uint8_t> bytes, TopologySnapshot& snapshot);
};

class TopologyMerge {
 public:
  // 设备版本逐项取大；布局冲突只影响显示位置，不改写原始编辑意图
  static TopologySnapshot Merge(const TopologySnapshot& current, const TopologySnapshot& incoming);
  static bool SameEntries(const TopologySnapshot& left, const TopologySnapshot& right);
  // 同格先保留版本较大的设备，其余按距离、行、列找空格，双方得到相同预览
  static std::vector<TopologyDevice> Resolve(const TopologySnapshot& snapshot);
  // 设备记录的全序：时钟、作者、状态、行、列、名称、标识
  static int CompareDevices(const TopologyDevice& left, const TopologyDevice& right);
};

}  // namespace bm
