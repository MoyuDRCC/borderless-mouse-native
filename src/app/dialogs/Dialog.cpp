#include "app/dialogs/Dialog.h"

#include <algorithm>
#include <utility>

#include "app/ui/controls/Paint.h"
#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

namespace {

// 覆盖层遮罩：半透明深色，让下面的页面退到背景里
const Gdiplus::Color kMaskColor(120, 15, 23, 42);

// 卡片内的通用度量（逻辑像素）
constexpr int kMargin = 24;
constexpr int kTitleTop = 16;
constexpr int kDescriptionTop = 42;
constexpr int kDescriptionHeight = 22;
constexpr int kFooterHeight = 68;

}  // namespace

Dialog::Dialog(std::wstring title, std::wstring description, int logicalWidth, int logicalHeight)
    : title_(std::move(title)),
      description_(std::move(description)),
      logicalWidth_(logicalWidth),
      logicalHeight_(logicalHeight) {}

void Dialog::SetTitle(std::wstring title) { title_ = std::move(title); }

void Dialog::SetDescription(std::wstring description) { description_ = std::move(description); }

int Dialog::ViewDpi() const { return Host() ? Host()->Dpi() : 96; }

RectI Dialog::CardRect(int dpi) const {
  const int width = std::min(Dpi::Scale(logicalWidth_, dpi), std::max(Dpi::Scale(200, dpi), Bounds().Width()));
  const int height = std::min(Dpi::Scale(logicalHeight_, dpi), std::max(Dpi::Scale(160, dpi), Bounds().Height()));
  const int left = (Bounds().Width() - width) / 2;
  const int top = (Bounds().Height() - height) / 2;
  return RectI{left, top, left + width, top + height};
}

int Dialog::FooterTop(const RectI& card, int height, int dpi) const {
  return card.bottom - Dpi::Scale(20, dpi) - height;
}

RectI Dialog::FooterButton(int& right, int top, int width, int height, int dpi) const {
  const RectI rect{right - width, top, right, top + height};
  right = rect.left - Dpi::Scale(10, dpi);
  return rect;
}

void Dialog::Show() {
  if (Control* parent = Parent()) SetBounds(parent->LocalBounds());
  SetVisible(true);
  OnLayout();
  Invalidate();
}

void Dialog::Hide() { SetVisible(false); }

void Dialog::OnTick() {
  Control* parent = Parent();
  if (!parent || Bounds() == parent->LocalBounds()) return;
  SetBounds(parent->LocalBounds());
  OnLayout();
}

void Dialog::OnLayout() {
  const int dpi = ViewDpi();
  LayoutContent(CardRect(dpi), Dpi::Scale(kMargin, dpi), dpi);
}

void Dialog::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const RectI card = CardRect(dpi);
  const int margin = Dpi::Scale(kMargin, dpi);

  paint::Fill(ctx, LocalBounds(), kMaskColor);
  draw::RoundRect(ctx.graphics, card, Dpi::Scale(Theme::RadiusSurface, dpi), Theme::Window, Theme::Border,
                  static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));
  paint::Text(ctx, title_, paint::TextStyle::Section, card.left + margin, card.top + Dpi::Scale(kTitleTop, dpi),
              Theme::Text);
  if (!description_.empty()) {
    paint::TextIn(ctx, description_, paint::TextStyle::Caption,
                  RectI{card.left + margin, card.top + Dpi::Scale(kDescriptionTop, dpi), card.right - margin,
                        card.top + Dpi::Scale(kDescriptionTop + kDescriptionHeight, dpi)},
                  Theme::TextSecondary);
  }
  PaintContent(ctx, card, margin, dpi);
  paint::Separator(ctx, card.left + margin, card.right - margin, card.bottom - Dpi::Scale(kFooterHeight, dpi));
}

bool Dialog::OnMouseDown(Point2i local, MouseButton button) {
  // 卡片外的点击只吞掉，不关闭：确认或取消必须显式点按钮
  return !(button == MouseButton::Left && CardRect(ViewDpi()).Contains(local.x, local.y));
}

}  // namespace bm
