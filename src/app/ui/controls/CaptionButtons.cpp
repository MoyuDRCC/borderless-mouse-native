#include "app/ui/controls/CaptionButtons.h"

#include <algorithm>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

CaptionButtons::CaptionButtons() = default;

RectI CaptionButtons::ButtonBounds(Kind kind) const {
  const int index = static_cast<int>(kind);
  const int width = Bounds().Width() / 3;
  const int left = index * width;
  const int right = index == 2 ? Bounds().Width() : left + width;
  return RectI{left, 0, right, Bounds().Height()};
}

CaptionButtons::Kind CaptionButtons::HitKind(Point2i local) const {
  if (!LocalBounds().Contains(local.x, local.y)) return Kind::None;
  for (int index = 0; index < 3; ++index) {
    const Kind kind = static_cast<Kind>(index);
    if (ButtonBounds(kind).Contains(local.x, local.y)) return kind;
  }
  return Kind::None;
}

void CaptionButtons::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  draw::FillRect(ctx.graphics, RectI{0, 0, Bounds().Width(), Bounds().Height()}, Theme::Window);

  const bool maximized = Host() && Host()->Maximized();
  const float stroke = std::max(1.0f, Dpi::ScaleF(1.1f, dpi));
  const Gdiplus::Color white(255, 255, 255, 255);

  for (int index = 0; index < 3; ++index) {
    const Kind kind = static_cast<Kind>(index);
    const RectI rect = ButtonBounds(kind);
    const bool hovered = hover_ == kind;
    const bool pressed = pressed_ == kind;

    if (hovered || pressed) {
      const Gdiplus::Color fill = kind == Kind::Close
                                      ? (pressed ? Theme::DangerHover : Theme::Danger)
                                      : (pressed ? Theme::SidebarHover : Theme::RowHover);
      draw::FillRect(ctx.graphics, rect, fill);
    }

    const Gdiplus::Color foreground = (kind == Kind::Close && (hovered || pressed)) ? white : Theme::TextSecondary;
    const int cx = rect.left + (rect.Width() / 2);
    const int cy = rect.top + (rect.Height() / 2);
    const int half = Dpi::Scale(5, dpi);

    Gdiplus::Pen pen(foreground, stroke);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);

    switch (kind) {
      case Kind::Minimize:
        ctx.graphics.DrawLine(&pen, cx - half, cy, cx + half, cy);
        break;

      case Kind::Maximize:
        // 方框类图形关掉抗锯齿，1px 线才不会糊
        ctx.graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        if (maximized) {
          const int offset = Dpi::Scale(2, dpi);
          const int size = (half * 2) - offset;
          ctx.graphics.DrawRectangle(&pen, cx - half, cy - half + offset, size, size);
          ctx.graphics.DrawLine(&pen, cx - half + offset, cy - half, cx + half, cy - half);
          ctx.graphics.DrawLine(&pen, cx + half, cy - half, cx + half, cy + half - offset);
        } else {
          ctx.graphics.DrawRectangle(&pen, cx - half, cy - half, half * 2, half * 2);
        }
        ctx.graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        break;

      case Kind::Close: {
        const int xHalf = Dpi::Scale(4, dpi);
        ctx.graphics.DrawLine(&pen, cx - xHalf, cy - xHalf, cx + xHalf, cy + xHalf);
        ctx.graphics.DrawLine(&pen, cx + xHalf, cy - xHalf, cx - xHalf, cy + xHalf);
        break;
      }

      default:
        break;
    }
  }
}

bool CaptionButtons::OnMouseDown(Point2i local, MouseButton button) {
  if (button != MouseButton::Left) return false;
  pressed_ = HitKind(local);
  Invalidate();
  return true;
}

void CaptionButtons::OnMouseUp(Point2i local, MouseButton button) {
  const Kind target = pressed_;
  pressed_ = Kind::None;
  Invalidate();

  if (button != MouseButton::Left || target == Kind::None || target != HitKind(local)) return;

  switch (target) {
    case Kind::Minimize:
      if (onMinimize) {
        onMinimize();
      } else if (Host()) {
        Host()->Minimize();
      }
      break;

    case Kind::Maximize:
      if (onMaximize) {
        onMaximize();
      } else if (Host()) {
        Host()->ToggleMaximize();
      }
      break;

    case Kind::Close:
      if (onClose) {
        onClose();
      } else if (Host()) {
        // 和系统关闭按钮同一条路：交给窗口的关闭处理决定隐藏还是退出
        PostMessageW(Host()->Handle(), WM_CLOSE, 0, 0);
      }
      break;

    default:
      break;
  }
}

void CaptionButtons::OnMouseMove(Point2i local) {
  const Kind hit = HitKind(local);
  if (hit == hover_) return;
  hover_ = hit;
  Invalidate();
}

void CaptionButtons::OnMouseLeave() {
  if (hover_ == Kind::None && pressed_ == Kind::None) return;
  hover_ = Kind::None;
  pressed_ = Kind::None;
  Invalidate();
}

}  // namespace bm
