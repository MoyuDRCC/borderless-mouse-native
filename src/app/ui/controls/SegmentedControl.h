#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app/ui/Control.h"

namespace bm {

// 胶囊分段控件：日志级别筛选等少量互斥选项，避免多个平铺幽灵按钮散落
class SegmentedControl : public Control {
 public:
  explicit SegmentedControl(std::vector<std::wstring> labels);

  int SelectedIndex() const { return selected_; }
  void SetSelectedIndex(int index);

  std::function<void(int)> onSelected;

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseUp(Point2i local, MouseButton button) override;
  void OnMouseMove(Point2i local) override;
  void OnMouseLeave() override;

 private:
  int ControlDpi() const;
  RectI ItemBounds(int index, int dpi) const;
  int HitItem(Point2i local, int dpi) const;

  std::vector<std::wstring> labels_;
  int selected_ = 0;
  int hover_ = -1;
  int pressed_ = -1;
};

}  // namespace bm
