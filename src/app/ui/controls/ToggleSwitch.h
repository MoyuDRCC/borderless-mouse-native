#pragma once

#include <functional>

#include "app/ui/Control.h"

namespace bm {

// 自绘开关：胶囊轨道 + 白色滑块，按下时滑块轻微拉长
class ToggleSwitch : public Control {
 public:
  ToggleSwitch();

  // 逻辑尺寸（96 DPI），由宿主按 dpi 换算后 SetBounds
  static Size2i LogicalSize() { return Size2i{38, 20}; }

  bool Checked() const { return checked_; }
  void SetChecked(bool checked);

  std::function<void(bool)> onChanged;

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseUp(Point2i local, MouseButton button) override;
  void OnMouseLeave() override;

 private:
  bool checked_ = false;
  bool pressed_ = false;
};

}  // namespace bm
