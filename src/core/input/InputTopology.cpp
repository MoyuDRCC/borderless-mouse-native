#include "core/input/InputTopology.h"

#include <stdexcept>
#include <utility>

namespace bm {

InputTopology::InputTopology(std::string localDeviceName, std::vector<InputDevice> devices)
    : localDeviceName_(std::move(localDeviceName)) {
  devices_.reserve(devices.size());
  cells_.reserve(devices.size());
  for (InputDevice& device : devices) {
    auto name = devices_.emplace(device.name, device);
    if (!name.second) throw std::invalid_argument("拓扑中的设备名重复：" + device.name);
    auto cell = cells_.emplace(CellKey(device.row, device.column), device);
    if (!cell.second) throw std::invalid_argument("拓扑中同一格子放置了多个设备");
  }
}

bool InputTopology::Contains(const std::string& name, const std::shared_ptr<IInputSink>& sink) const {
  auto found = devices_.find(name);
  return found != devices_.end() && found->second.sink == sink;
}

const InputDevice* InputTopology::Neighbor(const std::string& current, int side) const {
  auto device = devices_.find(current);
  if (device == devices_.end()) return nullptr;
  int row = device->second.row + (side == 0 ? -1 : side == 1 ? 1 : 0);
  int column = device->second.column + (side == 2 ? -1 : side == 3 ? 1 : 0);
  auto cell = cells_.find(CellKey(row, column));
  return cell == cells_.end() ? nullptr : &cell->second;
}

int64_t InputTopology::CellKey(int row, int column) {
  return (static_cast<int64_t>(row) << 32) | static_cast<uint32_t>(column);
}

}  // namespace bm
