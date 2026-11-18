#include "app/ui/controls/SegmentedControl.h"

#include <algorithm>
#include <utility>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

SegmentedControl::SegmentedControl(std::vector<std::wstring> labels) : labels_(std::move(labels)) {
  SetCursor(LoadCursorW(nullptr, IDC_HAND));
}

int SegmentedControl::ControlDpi() const { return Host() ? Host()->Dpi() : 96; }

void SegmentedControl::SetSelectedIndex(int index) {
  if (index == selected_ || index < 0 || index >= static_cast<int>(labels_.size())) return;
  selected_ = index;
  Invalidate();
  if (onSelected) onSelected(index);
}

RectI SegmentedControl::ItemBounds(int index, int dpi) const {
  if (labels_.empty()) return RectI{};
  const int pad = Dpi::Scale(3, dpi);
  const int count = static_cast<int>(labels_.size());
  const int innerWidth = std::max(1, Bounds().Width() - (pad * 2));
  const int itemWidth = innerWidth / count;
  const int left = pad + (index * itemWidth);
  const int right = index == count - 1 ? Bounds().Width() - pad : left + itemWidth;
  return RectI{left, pad, std::max(left + 1, right), std::max(pad + 1, Bounds().Height() - pad)};
}

int SegmentedControl::HitItem(Point2i local, int dpi) const {
  for (size_t index = 0; index < labels_.size(); ++index) {
    if (ItemBounds(static_cast<int>(index), dpi).Contains(local.x, local.y)) return static_cast<int>(index);
  }
  return -1;
}

void SegmentedControl::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int width = Bounds().Width();
  const int height = Bounds().Height();
  const HFONT font = Dpi::Font(Theme::CaptionPx, false, dpi);
  const float strokeWidth = static_cast<float>(std::max(1, Dpi::Scale(1, dpi)));

  // 轨道
  draw::FillRound(ctx.graphics, RectI{0, 0, width - 1, height - 1},
                  Dpi::Scale(Theme::RadiusControl + 1, dpi), Theme::SurfaceSubtle);

  for (size_t index = 0; index < labels_.size(); ++index) {
    const RectI item = ItemBounds(static_cast<int>(index), dpi);
    const bool selected = static_cast<int>(index) == selected_;
    const bool hovered = static_cast<int>(index) == hover_ && !selected;
    const int radius = Dpi::Scale(Theme::RadiusControl - 1, dpi);

    if (selected) {
      draw::RoundRect(ctx.graphics, RectI{item.left, item.top, item.right - 1, item.bottom - 1}, radius,
                      Theme::Window, Theme::Border, strokeWidth);
    } else if (hovered) {
      draw::FillRound(ctx.graphics, item, radius, Theme::RowHover);
    }

    const Gdiplus::Color color = selected || hovered ? Theme::Text : Theme::TextSecondary;
    draw::TextCentered(ctx, labels_[index], font, item, color);
  }
}

bool SegmentedControl::OnMouseDown(Point2i local, MouseButton button) {
  if (button != MouseButton::Left) return false;
  pressed_ = HitItem(local, ControlDpi());
  return true;
}

void SegmentedControl::OnMouseUp(Point2i local, MouseButton button) {
  const int pressed = pressed_;
  pressed_ = -1;
  if (button != MouseButton::Left || pressed < 0) return;
  const int hit = HitItem(local, ControlDpi());
  if (hit < 0 || hit != pressed) return;
  SetSelectedIndex(hit);
}

void SegmentedControl::OnMouseMove(Point2i local) {
  const int hover = HitItem(local, ControlDpi());
  if (hover == hover_) return;
  hover_ = hover;
  Invalidate();
}

void SegmentedControl::OnMouseLeave() {
  if (hover_ < 0) return;
  hover_ = -1;
  Invalidate();
}

}  // namespace bm
