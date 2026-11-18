#include "app/ui/controls/ToggleSwitch.h"

#include <algorithm>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"

namespace bm {

ToggleSwitch::ToggleSwitch() { SetCursor(LoadCursorW(nullptr, IDC_HAND)); }

void ToggleSwitch::SetChecked(bool checked) {
  if (checked_ == checked) return;
  checked_ = checked;
  Invalidate();
  if (onChanged) onChanged(checked_);
}

void ToggleSwitch::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int trackWidth = Bounds().Width();
  const int trackHeight = Bounds().Height();
  if (trackWidth <= 0 || trackHeight <= 0) return;

  const bool hover = Hover() && Enabled();
  const Gdiplus::Color offColor = hover ? Theme::ToggleTrackHover : Theme::ToggleTrackOff;
  const Gdiplus::Color onColor = hover ? Theme::AccentHover : Theme::Accent;
  const Gdiplus::Color trackColor = checked_ ? onColor : offColor;

  draw::FillRound(ctx.graphics, RectI{0, 0, trackWidth - 1, trackHeight - 1}, trackHeight / 2, trackColor);

  // 滑块：位置由开关状态决定，按下时向右拉长 3 逻辑像素
  const int pad = Dpi::Scale(2, dpi);
  const int knobSize = trackHeight - (pad * 2);
  const int stretch = pressed_ ? Dpi::Scale(3, dpi) : 0;
  const int knobWidth = knobSize + stretch;
  const int knobLeft = checked_ ? trackWidth - pad - knobWidth : pad;

  draw::FillRound(ctx.graphics, RectI{knobLeft, pad, knobLeft + knobWidth, pad + knobSize}, knobSize / 2,
                  Gdiplus::Color(255, 255, 255, 255));
}

bool ToggleSwitch::OnMouseDown(Point2i, MouseButton button) {
  if (button != MouseButton::Left || !Enabled()) return false;
  pressed_ = true;
  Invalidate();
  return true;
}

void ToggleSwitch::OnMouseUp(Point2i local, MouseButton button) {
  const bool wasPressed = pressed_;
  pressed_ = false;
  Invalidate();
  if (button != MouseButton::Left || !wasPressed || !Enabled()) return;
  if (!LocalBounds().Contains(local.x, local.y)) return;
  SetChecked(!checked_);
}

void ToggleSwitch::OnMouseLeave() {
  if (!pressed_) return;
  pressed_ = false;
  Invalidate();
}

}  // namespace bm
