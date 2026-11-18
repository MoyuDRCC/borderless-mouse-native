#include "app/ui/controls/FlatButton.h"

#include <algorithm>
#include <utility>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"

namespace bm {

FlatButton::FlatButton(std::wstring text, Style style) : text_(std::move(text)), style_(style) {
  SetCursor(LoadCursorW(nullptr, IDC_HAND));
}

void FlatButton::SetText(std::wstring text) {
  text_ = std::move(text);
  Invalidate();
}

void FlatButton::SetStyle(Style style) {
  if (style_ == style) return;
  style_ = style;
  Invalidate();
}

void FlatButton::SetActive(bool active) {
  if (active_ == active) return;
  active_ = active;
  Invalidate();
}

void FlatButton::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int radius = Dpi::Scale(Theme::RadiusControl, dpi);
  const int width = Bounds().Width();
  const int height = Bounds().Height();
  const RectI outline{0, 0, width - 1, height - 1};

  const Gdiplus::Color clear(0, 0, 0, 0);
  const Gdiplus::Color white(255, 255, 255, 255);
  const bool hover = Hover() && Enabled();

  Gdiplus::Color fill = clear;
  Gdiplus::Color border = clear;
  Gdiplus::Color foreground = Theme::Text;

  if (!Enabled()) {
    fill = style_ == Style::Ghost ? clear : Theme::SurfaceSubtle;
    border = style_ == Style::Ghost ? clear : Theme::Separator;
    foreground = Theme::TextTertiary;
  } else if (active_) {
    fill = Theme::AccentSoft;
    border = Theme::AccentBorder;
    foreground = Theme::Accent;
  } else {
    switch (style_) {
      case Style::Primary:
        fill = hover ? Theme::AccentHover : Theme::Accent;
        foreground = white;
        break;
      case Style::Danger:
        fill = hover ? Theme::DangerHover : Theme::Danger;
        foreground = white;
        break;
      case Style::Ghost:
        fill = hover ? Theme::RowHover : clear;
        foreground = hover ? Theme::Text : Theme::TextSecondary;
        break;
      case Style::Secondary:
      default:
        fill = hover ? Theme::SurfaceSubtle : Theme::Window;
        border = hover ? Theme::BorderStrong : Theme::Border;
        foreground = Theme::Text;
        break;
    }
  }

  draw::RoundRect(ctx.graphics, outline, radius, fill, border,
                  static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));
  const HFONT font = Dpi::Font(Theme::CaptionPx + 0.5f, style_ == Style::Primary || style_ == Style::Danger, dpi);
  draw::TextCentered(ctx, text_, font, RectI{0, 0, width, height}, foreground);
}

bool FlatButton::OnMouseDown(Point2i, MouseButton button) {
  if (button != MouseButton::Left || !Enabled()) return false;
  pressed_ = true;
  return true;
}

void FlatButton::OnMouseUp(Point2i local, MouseButton button) {
  const bool wasPressed = pressed_;
  pressed_ = false;
  if (button != MouseButton::Left || !wasPressed || !Enabled()) return;
  if (!LocalBounds().Contains(local.x, local.y)) return;
  if (onClick) onClick();
}

void FlatButton::OnMouseLeave() { pressed_ = false; }

}  // namespace bm