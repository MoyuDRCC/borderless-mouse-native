#pragma once

#include <string>
#include <vector>

#include "app/pages/Page.h"
#include "app/UiModel.h"

namespace bm {

class AddDeviceDialog;
class DeviceListView;
class DeviceSettingsDialog;
class FlatButton;

// 设备页：通栏设备列表 + 页头右侧添加入口；行内按钮与“···”菜单的动作全部转成 UiActions。
class DevicesPage : public Page {
 public:
  DevicesPage(UiModel& model, UiActions& actions);

  void Paint(PaintContext& ctx) override;
  void OnLayout() override;
  void OnTick() override;

  void Refresh(bool force);

 private:
  void Sync(bool force);
  void OpenAddDevice();
  void OpenSettings(const std::string& id);
  const DeviceRow* FindDevice(const std::string& id) const;

  UiModel& model_;
  UiActions& actions_;

  DeviceListView* list_ = nullptr;
  FlatButton* add_ = nullptr;
  AddDeviceDialog* addDialog_ = nullptr;
  DeviceSettingsDialog* settingsDialog_ = nullptr;
  std::string settingsTarget_;

  std::vector<DeviceRow> shown_;
};

}  // namespace bm
