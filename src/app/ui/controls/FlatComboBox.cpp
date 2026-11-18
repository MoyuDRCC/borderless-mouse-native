#include "app/ui/controls/FlatComboBox.h"

#include <algorithm>
#include <utility>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"
#include "app/ui/controls/ThemedPopupMenu.h"
#include "app/ui/controls/VectorIcons.h"

namespace bm {

FlatComboBox::FlatComboBox() { SetCursor(LoadCursorW(nullptr, IDC_HAND)); }

void FlatComboBox::AddItems(std::vector<std::wstring> items) {
  items_ = std::move(items);
  if (selected_ < 0 && !items_.empty()) selected_ = 0;
  Invalidate();
}

void FlatComboBox::SetSelectedIndex(int index) {
  const int clamped = items_.empty() ? -1 : std::clamp(index, 0, static_cast<int>(items_.size()) - 1);
  if (selected_ == clamped) return;
  selected_ = clamped;
  Invalidate();
  if (onSelectedIndexChanged) onSelectedIndexChanged(selected_);
}

const std::wstring& FlatComboBox::SelectedText() const {
  static const std::wstring empty;
  if (selected_ < 0 || selected_ >= static_cast<int>(items_.size())) return empty;
  return items_[selected_];
}

void FlatComboBox::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int width = Bounds().Width();
  const int height = Bounds().Height();
  const RectI outline{0, 0, width - 1, height - 1};
  const bool hover = Hover() && Enabled();

  const Gdiplus::Color fill = hover ? Theme::SurfaceSubtle : Theme::Window;
  const Gdiplus::Color border = hover ? Theme::BorderStrong : Theme::Border;
  draw::RoundRect(ctx.graphics, outline, Dpi::Scale(Theme::RadiusControl, dpi), fill, border,
                  static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));

  const int padLeft = Dpi::Scale(10, dpi);
  const int arrowSize = Dpi::Scale(14, dpi);
  const int arrowMargin = Dpi::Scale(8, dpi);
  const int arrowLeft = width - arrowMargin - arrowSize;
  const RectI textRect{padLeft, 0, std::max(padLeft, arrowLeft - Dpi::Scale(4, dpi)), height};

  const HFONT font = Dpi::Font(Theme::BodyPx, false, dpi);
  draw::TextWithin(ctx, SelectedText(), font, textRect, Enabled() ? Theme::Text : Theme::TextTertiary);

  const int arrowTop = (height - arrowSize) / 2;
  VectorIcons::DrawChevronDown(ctx.graphics,
                               RectI{arrowLeft, arrowTop, arrowLeft + arrowSize, arrowTop + arrowSize},
                               hover ? Theme::Accent : Theme::TextSecondary, dpi);
}

bool FlatComboBox::OnMouseDown(Point2i, MouseButton button) {
  if (button != MouseButton::Left || !Enabled()) return false;
  OpenDropDown();
  return true;
}

void FlatComboBox::OnMouseMove(Point2i) {}

void FlatComboBox::OnMouseLeave() { Invalidate(); }  // 悬停态由基类维护，重绘一次即可

void FlatComboBox::OpenDropDown() {
  Window* host = Host();
  if (!host || items_.empty()) return;

  std::vector<ThemedPopupMenu::Entry> entries;
  entries.reserve(items_.size());
  for (size_t index = 0; index < items_.size(); ++index) {
    ThemedPopupMenu::Entry entry;
    entry.text = items_[index];
    entry.isChecked = static_cast<int>(index) == selected_;
    const int target = static_cast<int>(index);
    entry.onClick = [this, target] { SetSelectedIndex(target); };
    entries.push_back(std::move(entry));
  }

  const int dpi = host->Dpi();
  const int minWidth = std::max(110, MulDiv(Bounds().Width(), 96, dpi));
  const Point2i origin = AbsoluteOrigin();
  ThemedPopupMenu::Show(host, Point2i{origin.x, origin.y + Bounds().Height() + Dpi::Scale(4, dpi)},
                        std::move(entries), minWidth, true);
}

}  // namespace bm
