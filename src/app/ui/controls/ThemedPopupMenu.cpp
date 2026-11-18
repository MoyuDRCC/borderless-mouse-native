#include "app/ui/controls/ThemedPopupMenu.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"
#include "app/ui/controls/VectorIcons.h"

namespace bm {

ThemedPopupMenu::ThemedPopupMenu(std::vector<Entry> entries, int minLogicalWidth, bool reserveCheckColumn)
    : entries_(std::move(entries)),
      minLogicalWidth_(minLogicalWidth),
      reserveCheckColumn_(reserveCheckColumn) {}

Size2i ThemedPopupMenu::ContentSize(int dpi) {
  const int pad = Dpi::Scale(6, dpi);
  const int itemHeight = Dpi::Scale(34, dpi);
  const int separatorHeight = Dpi::Scale(9, dpi);
  const int checkWidth = reserveCheckColumn_ ? Dpi::Scale(22, dpi) : 0;

  const HFONT font = Dpi::Font(Theme::BodyPx, false, dpi);
  const HFONT boldFont = Dpi::Font(Theme::BodyPx, true, dpi);

  // 宽度取最长条目，避免弹出后文字被省略
  int maxTextWidth = 0;
  for (const Entry& entry : entries_) {
    if (entry.isSeparator) continue;
    const Size2i size = draw::Measure(entry.text, entry.isChecked ? boldFont : font);
    maxTextWidth = std::max(maxTextWidth, size.width);
  }
  const int width = std::max(Dpi::Scale(minLogicalWidth_, dpi),
                             (pad * 2) + Dpi::Scale(24, dpi) + checkWidth + maxTextWidth);

  rects_.clear();
  int top = pad;
  for (const Entry& entry : entries_) {
    const int height = entry.isSeparator ? separatorHeight : itemHeight;
    rects_.push_back(RectI{pad, top, width - pad, top + height});
    top += height;
  }
  return Size2i{width, top + pad};
}

void ThemedPopupMenu::Show(Window* host, Point2i anchor, std::vector<Entry> entries, int minLogicalWidth,
                           bool reserveCheckColumn) {
  if (!host) return;
  auto menu = std::make_unique<ThemedPopupMenu>(std::move(entries), minLogicalWidth, reserveCheckColumn);
  const Size2i size = menu->ContentSize(host->Dpi());
  menu->SetBounds(RectI{anchor.x, anchor.y, anchor.x + size.width, anchor.y + size.height});
  host->ShowPopup(std::move(menu));
}

void ThemedPopupMenu::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int width = Bounds().Width();
  const int height = Bounds().Height();

  // 覆盖层没有系统圆角，自己画一层表面与 1px 边框
  draw::RoundRect(ctx.graphics, RectI{0, 0, width, height}, Dpi::Scale(Theme::RadiusSurface, dpi),
                  Theme::Window, Theme::Border, static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));

  const HFONT font = Dpi::Font(Theme::BodyPx, false, dpi);
  const HFONT boldFont = Dpi::Font(Theme::BodyPx, true, dpi);

  for (size_t index = 0; index < entries_.size(); ++index) {
    const Entry& entry = entries_[index];
    const RectI rect = rects_[index];

    if (entry.isSeparator) {
      draw::Separator(ctx, rect.left + Dpi::Scale(6, dpi), rect.right - Dpi::Scale(6, dpi),
                      rect.top + (rect.Height() / 2));
      continue;
    }

    if (static_cast<int>(index) == hover_ && entry.isEnabled) {
      draw::FillRound(ctx.graphics, rect, Dpi::Scale(Theme::RadiusControl, dpi),
                      entry.isDanger ? Theme::DangerSoft : Theme::RowHover);
    }

    int textLeft = rect.left + Dpi::Scale(12, dpi);
    if (reserveCheckColumn_) {
      if (entry.isChecked) {
        const int iconSize = Dpi::Scale(14, dpi);
        const int iconTop = rect.top + ((rect.Height() - iconSize) / 2);
        const int iconLeft = rect.left + Dpi::Scale(10, dpi);
        VectorIcons::DrawCheck(ctx.graphics, RectI{iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize},
                               Theme::Accent, dpi);
      }
      textLeft += Dpi::Scale(22, dpi);
    }

    const Gdiplus::Color foreground = !entry.isEnabled ? Theme::TextTertiary
                                        : entry.isDanger ? Theme::Danger
                                        : entry.isChecked ? Theme::Accent
                                                          : Theme::Text;
    const HFONT entryFont = entry.isChecked ? boldFont : font;
    draw::TextWithin(ctx, entry.text, entryFont,
                     RectI{textLeft, rect.top, rect.right - Dpi::Scale(10, dpi), rect.bottom}, foreground);
  }
}

int ThemedPopupMenu::HitTest(Point2i local) const {
  for (size_t index = 0; index < entries_.size(); ++index) {
    if (entries_[index].isSeparator || !entries_[index].isEnabled) continue;
    if (rects_[index].Contains(local.x, local.y)) return static_cast<int>(index);
  }
  return -1;
}

bool ThemedPopupMenu::OnMouseDown(Point2i, MouseButton) { return true; }

void ThemedPopupMenu::OnMouseUp(Point2i local, MouseButton button) {
  if (button != MouseButton::Left) return;
  const int index = HitTest(local);
  if (index >= 0) Activate(index);
}

void ThemedPopupMenu::OnMouseMove(Point2i local) {
  const int hit = HitTest(local);
  if (hit == hover_) return;
  hover_ = hit;
  SetCursor(hit >= 0 ? LoadCursorW(nullptr, IDC_HAND) : nullptr);
  Invalidate();
}

void ThemedPopupMenu::OnMouseLeave() {
  if (hover_ < 0) return;
  hover_ = -1;
  SetCursor(nullptr);
  Invalidate();
}

void ThemedPopupMenu::OnKey(int vk, bool down) {
  if (!down) return;

  if (vk == VK_ESCAPE) {
    if (Window* host = Host()) host->ClosePopup();
    return;
  }
  if (vk == VK_RETURN) {
    Activate(hover_);
    return;
  }
  if (vk != VK_DOWN && vk != VK_UP) return;

  const int step = vk == VK_DOWN ? 1 : -1;
  const int count = static_cast<int>(entries_.size());
  for (int tries = 0; tries < count; ++tries) {
    hover_ = ((hover_ + step + count) % count);
    if (!entries_[hover_].isSeparator && entries_[hover_].isEnabled) {
      Invalidate();
      return;
    }
  }
}

void ThemedPopupMenu::Activate(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) return;

  // 先拷贝回调再收起菜单：ClosePopup 会销毁本对象
  std::function<void()> action = entries_[index].onClick;
  Window* host = Host();
  entries_.clear();
  if (host) host->ClosePopup();
  if (action) action();
}

}  // namespace bm
