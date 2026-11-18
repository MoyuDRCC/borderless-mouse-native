#pragma once

#include <functional>
#include <string>

#include "app/UiModel.h"
#include "app/dialogs/Dialog.h"

namespace bm {

class FlatButton;
class FlatComboBox;
class RoundedTextBox;
class ToggleSwitch;

// 单台设备的本地设置结果（页面据此更新 DeviceRow）
struct DeviceSettingsValues {
  std::wstring alias;
  int pointerDpi = 0;
  bool clipboard = true;
  bool wheelReverse = false;
  int wheelScalePercent = 100;
};

// 设备设置弹窗：别名、鼠标 DPI、剪贴板参与、滚轮方向与速度
class DeviceSettingsDialog : public Dialog {
 public:
  DeviceSettingsDialog();

  void SetDevice(const std::wstring& displayName, const DeviceRow& device);

  std::function<void(const DeviceSettingsValues&)> onAccepted;

 protected:
  void LayoutContent(const RectI& card, int margin, int dpi) override;
  void PaintContent(PaintContext& ctx, const RectI& card, int margin, int dpi) override;

 private:
  void Accept();

  RoundedTextBox* alias_ = nullptr;
  FlatComboBox* dpi_ = nullptr;
  FlatComboBox* speed_ = nullptr;
  ToggleSwitch* clipboard_ = nullptr;
  ToggleSwitch* reverse_ = nullptr;
  FlatButton* confirm_ = nullptr;
  FlatButton* cancel_ = nullptr;
};

}  // namespace bm
