#include "app/ui/controls/NavSidebar.h"

#include <algorithm>
#include <utility>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"
#include "app/ui/controls/VectorIcons.h"

namespace bm {

namespace {

struct NavItem {
  NavSidebar::Section section;
  const wchar_t* label;
};

constexpr NavItem kItems[] = {
    {NavSidebar::Section::Overview, L"概览"},
    {NavSidebar::Section::Devices, L"设备"},
    {NavSidebar::Section::Layout, L"布局"},
    {NavSidebar::Section::Log, L"日志"},
    {NavSidebar::Section::Settings, L"设置"},
};

constexpr int kItemCount = static_cast<int>(sizeof(kItems) / sizeof(kItems[0]));

}  // namespace

NavSidebar::NavSidebar() = default;

int NavSidebar::SidebarDpi() const { return Host() ? Host()->Dpi() : 96; }

int NavSidebar::CaptionStripHeight() const { return Theme::SidebarBrandHeight; }

RectI NavSidebar::ItemBounds(int index, int dpi) const {
  const int left = Dpi::Scale(10, dpi);
  const int right = Bounds().Width() - Dpi::Scale(10, dpi);
  const int top = Dpi::Scale(66, dpi) + (index * Dpi::Scale(40, dpi));
  return RectI{left, top, right, top + Dpi::Scale(36, dpi)};
}

int NavSidebar::HitItem(Point2i local, int dpi) const {
  for (int index = 0; index < kItemCount; ++index) {
    if (ItemBounds(index, dpi).Contains(local.x, local.y)) return index;
  }
  return -1;
}

void NavSidebar::SetSelected(Section section) {
  if (selected_ == section) return;
  selected_ = section;
  Invalidate();
}

void NavSidebar::SetStatus(std::wstring status, bool connected, std::wstring version, int connectedCount) {
  if (status_ == status && connected_ == connected && version_ == version && connectedCount_ == connectedCount)
    return;
  status_ = std::move(status);
  connected_ = connected;
  version_ = std::move(version);
  connectedCount_ = connectedCount;
  Invalidate();
}

void NavSidebar::SetPendingMerge(bool pending) {
  if (pendingMerge_ == pending) return;
  pendingMerge_ = pending;
  Invalidate();
}

void NavSidebar::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int width = Bounds().Width();
  const int height = Bounds().Height();
  draw::FillRect(ctx.graphics, RectI{0, 0, width, height}, Theme::Sidebar);

  // 右侧 1px 细分割线，代替厚重阴影
  const int edge = std::max(1, Dpi::Scale(1, dpi));
  draw::FillRect(ctx.graphics, RectI{width - edge, 0, width, height}, Theme::Separator);

  // 顶部品牌区：强调色方块 + 白色显示器图标 + 应用名
  const int left = Dpi::Scale(18, dpi);
  const int iconSize = Dpi::Scale(22, dpi);
  const int brandTop = Dpi::Scale(16, dpi);
  draw::FillRound(ctx.graphics, RectI{left, brandTop, left + iconSize, brandTop + iconSize},
                  Dpi::Scale(6, dpi), Theme::Accent);

  const int glyphSize = Dpi::Scale(15, dpi);
  const int glyphInset = (iconSize - glyphSize) / 2;
  VectorIcons::DrawMonitor(ctx.graphics,
                           RectI{left + glyphInset, brandTop + glyphInset, left + glyphInset + glyphSize,
                                 brandTop + glyphInset + glyphSize},
                           Gdiplus::Color(255, 255, 255, 255), dpi);

  const HFONT titleFont = Dpi::Font(15.0f, true, dpi);
  const Size2i titleSize = draw::Measure(L"无界键鼠", titleFont);
  draw::Text(ctx, L"无界键鼠", titleFont, left + iconSize + Dpi::Scale(10, dpi),
             brandTop + ((iconSize - titleSize.height) / 2), Theme::Text);

  const HFONT itemFont = Dpi::Font(Theme::BodyPx, false, dpi);
  const HFONT itemBoldFont = Dpi::Font(Theme::BodyPx, true, dpi);
  const HFONT badgeFont = Dpi::Font(Theme::MicroPx, true, dpi);
  const int radius = Dpi::Scale(Theme::RadiusControl, dpi);
  const float strokeWidth = static_cast<float>(std::max(1, Dpi::Scale(1, dpi)));

  for (int index = 0; index < kItemCount; ++index) {
    const RectI bounds = ItemBounds(index, dpi);
    const RectI outline{bounds.left, bounds.top, bounds.right - 1, bounds.bottom - 1};
    const bool selected = kItems[index].section == selected_;

    if (selected) {
      draw::RoundRect(ctx.graphics, outline, radius, Theme::Window, Theme::Border, strokeWidth);
    } else if (index == hover_) {
      draw::FillRound(ctx.graphics, outline, radius, Theme::SidebarHover);
    }

    if (selected) {
      // 左侧强调条
      const int barWidth = std::max(2, Dpi::Scale(3, dpi));
      const int barLeft = bounds.left + Dpi::Scale(2, dpi);
      draw::FillRound(ctx.graphics,
                      RectI{barLeft, bounds.top + Dpi::Scale(8, dpi), barLeft + barWidth,
                            bounds.bottom - Dpi::Scale(8, dpi)},
                      barWidth / 2, Theme::Accent);
    }

    const int navIconSize = Dpi::Scale(15, dpi);
    const int iconLeft = bounds.left + Dpi::Scale(13, dpi);
    const int iconTop = bounds.top + ((bounds.Height() - navIconSize) / 2);
    VectorIcons::DrawNav(ctx.graphics, kItems[index].section,
                         RectI{iconLeft, iconTop, iconLeft + navIconSize, iconTop + navIconSize},
                         selected ? Theme::Accent : Theme::TextSecondary, dpi);

    const HFONT labelFont = selected ? itemBoldFont : itemFont;
    const Size2i labelSize = draw::Measure(kItems[index].label, labelFont);
    draw::Text(ctx, kItems[index].label, labelFont, iconLeft + navIconSize + Dpi::Scale(10, dpi),
               bounds.top + ((bounds.Height() - labelSize.height) / 2),
               selected ? Theme::Text : Theme::TextSecondary);

    // 右侧状态徽章：设备已连接数、布局待合并提醒
    const int centerY = bounds.top + (bounds.Height() / 2);
    if (kItems[index].section == Section::Devices && connectedCount_ > 0) {
      const std::wstring count = std::to_wstring(connectedCount_);
      const Size2i countSize = draw::Measure(count, badgeFont);
      const int pillWidth = std::max(Dpi::Scale(18, dpi), countSize.width + Dpi::Scale(10, dpi));
      const int pillHeight = Dpi::Scale(18, dpi);
      const RectI pill{bounds.right - Dpi::Scale(8, dpi) - pillWidth, centerY - (pillHeight / 2),
                       bounds.right - Dpi::Scale(8, dpi), centerY - (pillHeight / 2) + pillHeight};
      draw::FillRound(ctx.graphics, pill, pillHeight / 2, selected ? Theme::AccentSoft : Theme::SuccessSoft);
      draw::TextCentered(ctx, count, badgeFont, pill, selected ? Theme::Accent : Theme::Success);
    } else if (kItems[index].section == Section::Layout && pendingMerge_) {
      const int dot = Dpi::Scale(7, dpi);
      draw::FillRound(ctx.graphics,
                      RectI{bounds.right - Dpi::Scale(14, dpi) - dot, centerY - (dot / 2),
                            bounds.right - Dpi::Scale(14, dpi), centerY - (dot / 2) + dot},
                      dot / 2, Theme::Warning);
    }
  }

  // 底部状态栏
  const int footerTop = height - Dpi::Scale(52, dpi);
  draw::Separator(ctx, Dpi::Scale(14, dpi), width - Dpi::Scale(14, dpi), footerTop);

  const HFONT statusFont = Dpi::Font(Theme::CaptionPx, false, dpi);
  const HFONT versionFont = Dpi::MonoFont(Theme::MicroPx, false, dpi);
  const int statusTop = height - Dpi::Scale(34, dpi);
  const Size2i statusSize = draw::Measure(status_, statusFont);

  const int dotSize = Dpi::Scale(7, dpi);
  const int dotTop = statusTop + ((statusSize.height - dotSize) / 2);
  draw::FillRound(ctx.graphics, RectI{left, dotTop, left + dotSize, dotTop + dotSize}, dotSize / 2,
                  connected_ ? Theme::Success : Theme::TextTertiary);

  const int textLeft = left + Dpi::Scale(14, dpi);
  const int versionWidth = version_.empty() ? 0 : draw::Measure(version_, versionFont).width + Dpi::Scale(8, dpi);
  draw::TextWithin(ctx, status_, statusFont,
                   RectI{textLeft, statusTop, width - Dpi::Scale(14, dpi) - versionWidth,
                         statusTop + statusSize.height},
                   connected_ ? Theme::Text : Theme::TextSecondary);

  if (!version_.empty()) {
    const Size2i versionSize = draw::Measure(version_, versionFont);
    draw::Text(ctx, version_, versionFont, width - Dpi::Scale(14, dpi) - versionSize.width,
               statusTop + ((statusSize.height - versionSize.height) / 2), Theme::TextTertiary);
  }
}

bool NavSidebar::OnMouseDown(Point2i, MouseButton button) {
  if (button != MouseButton::Left) return false;
  pressed_ = true;
  return true;
}

void NavSidebar::OnMouseUp(Point2i local, MouseButton button) {
  if (button != MouseButton::Left || !pressed_) return;
  pressed_ = false;

  const int index = HitItem(local, SidebarDpi());
  if (index < 0) return;
  const Section section = kItems[index].section;
  if (section == selected_) return;

  SetSelected(section);
  if (onSectionSelected) onSectionSelected(section);
}

void NavSidebar::OnMouseMove(Point2i local) {
  const int hover = HitItem(local, SidebarDpi());
  if (hover == hover_) return;
  hover_ = hover;
  SetCursor(hover >= 0 ? LoadCursorW(nullptr, IDC_HAND) : nullptr);
  Invalidate();
}

void NavSidebar::OnMouseLeave() {
  if (hover_ < 0) return;
  hover_ = -1;
  SetCursor(nullptr);
  Invalidate();
}

}  // namespace bm
