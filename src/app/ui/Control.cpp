#include "app/ui/Control.h"

#include "app/ui/Window.h"

namespace bm {

void Control::SetBounds(const RectI& bounds) {
  if (bounds.left == bounds_.left && bounds.top == bounds_.top && bounds.right == bounds_.right &&
      bounds.bottom == bounds_.bottom) {
    return;
  }
  bounds_ = bounds;
  OnLayout();
  // 布局变化可能留下旧像素，整窗重绘一次比逐个推算旧区域更可靠
  if (host_) host_->PostRedraw();
}

void Control::AddControl(std::unique_ptr<Control> control) {
  control->parent_ = this;
  control->AttachHost(host_);
  children_.push_back(std::move(control));
}

void Control::SetVisible(bool visible) {
  if (visible_ == visible) return;
  visible_ = visible;
  if (!visible_ && hover_) SetHoverState(false);
  Invalidate();
}

void Control::SetEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  Invalidate();
}

void Control::Invalidate() {
  if (!host_) return;
  host_->InvalidateRect(MakeRect(AbsoluteOrigin(), bounds_.Width(), bounds_.Height()));
}

Point2i Control::AbsoluteOrigin() const {
  Point2i origin{bounds_.left, bounds_.top};
  for (const Control* ancestor = parent_; ancestor; ancestor = ancestor->parent_) {
    origin.x += ancestor->bounds_.left;
    origin.y += ancestor->bounds_.top;
  }
  return origin;
}

void Control::AttachHost(Window* host) {
  host_ = host;
  for (const auto& child : children_) child->AttachHost(host);
}

void Control::Paint(PaintContext&) {}

bool Control::OnMouseDown(Point2i, MouseButton) { return false; }
void Control::OnMouseMove(Point2i) {}
void Control::OnMouseUp(Point2i, MouseButton) {}
void Control::OnMouseLeave() {}
void Control::OnWheel(int, Point2i) {}
void Control::OnKey(int, bool) {}
void Control::OnChar(wchar_t) {}

void Control::PaintTree(PaintContext& ctx) {
  if (!visible_) return;

  const RectI clip = Intersect(ctx.clip, LocalBounds());
  if (clip.IsEmpty()) return;

  // GDI+ 用变换定位，GDI 文字与裁剪区用设备坐标：两者都摆到同一个绝对原点上
  const Point2i origin = AbsoluteOrigin();
  const int savedDc = SaveDC(ctx.dc);
  IntersectClipRect(ctx.dc, clip.left + origin.x, clip.top + origin.y, clip.right + origin.x,
                    clip.bottom + origin.y);

  const Gdiplus::GraphicsState state = ctx.graphics.Save();
  ctx.graphics.TranslateTransform(static_cast<Gdiplus::REAL>(bounds_.left),
                                  static_cast<Gdiplus::REAL>(bounds_.top));
  ctx.graphics.SetClip(Gdiplus::Rect(clip.left, clip.top, clip.Width(), clip.Height()),
                       Gdiplus::CombineModeIntersect);

  PaintContext local{ctx.graphics, ctx.dc, ctx.dpi, clip, origin};
  Paint(local);
  for (const auto& child : children_) child->PaintTree(local);

  ctx.graphics.Restore(state);
  RestoreDC(ctx.dc, savedDc);
}

void Control::TickTree() {
  if (!visible_) return;
  OnTick();
  for (const auto& child : children_) child->TickTree();
}

Control* Control::HitTest(Point2i local) {
  if (!visible_ || !LocalBounds().Contains(local.x, local.y)) return nullptr;
  // 后加入的控件在上层，倒序命中
  for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
    const RectI& childBounds = (*it)->Bounds();
    if (Control* hit = (*it)->HitTest(Point2i{local.x - childBounds.left, local.y - childBounds.top}))
      return hit;
  }
  return this;
}

}  // namespace bm
