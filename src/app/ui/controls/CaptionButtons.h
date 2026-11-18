#pragma once

#include <functional>

#include "app/ui/Control.h"

namespace bm {

// 右上角窗口控制按钮组（最小化、最大化/还原、关闭），与无缝标题栏融为一体。
// 未设置回调时直接驱动宿主窗口。
class CaptionButtons : public Control {
 public:
  CaptionButtons();

  static Size2i LogicalSize() { return Size2i{138, 34}; }

  std::function<void()> onMinimize;
  std::function<void()> onMaximize;
  std::function<void()> onClose;

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseUp(Point2i local, MouseButton button) override;
  void OnMouseMove(Point2i local) override;
  void OnMouseLeave() override;
  // 标题条区域内仍然接收鼠标
  bool CapturesCaption() const override { return true; }

 private:
  enum class Kind { None = -1, Minimize = 0, Maximize = 1, Close = 2 };

  RectI ButtonBounds(Kind kind) const;
  Kind HitKind(Point2i local) const;

  Kind hover_ = Kind::None;
  Kind pressed_ = Kind::None;
};

}  // namespace bm
