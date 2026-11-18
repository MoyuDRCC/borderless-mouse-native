#pragma once

#include "core/input/InputEvent.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bm {

// 设备格子与当前连接出口；本机的出口为空指针
struct InputDevice {
  std::string name;
  int row = 0;
  int column = 0;
  std::shared_ptr<IInputSink> sink;  // 已发布的拓扑独立持有其会话出口。
};

// 对应 Input/InputTopology.cs：由主控生成后发布的不可变拓扑，
// 路由线程只查这张表，不读取界面或连接字典。
class InputTopology {
 public:
  InputTopology(std::string localDeviceName, std::vector<InputDevice> devices);

  const std::string& LocalDeviceName() const { return localDeviceName_; }
  // 名字与出口都一致才算同一个连接
  bool Contains(const std::string& name, const std::shared_ptr<IInputSink>& sink) const;
  // side 0 上 1 下 2 左 3 右；该方向没有格子时返回空
  const InputDevice* Neighbor(const std::string& current, int side) const;

 private:
  static int64_t CellKey(int row, int column);

  std::string localDeviceName_;
  std::unordered_map<std::string, InputDevice> devices_;
  std::unordered_map<int64_t, InputDevice> cells_;
};

}  // namespace bm
