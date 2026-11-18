#include "app/ui/controls/DeviceListView.h"

#include <algorithm>
#include <utility>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"
#include "app/ui/controls/ThemedPopupMenu.h"
#include "app/ui/controls/VectorIcons.h"

namespace bm {

namespace {

struct BadgeStyle {
  std::wstring text;
  Gdiplus::Color background;
  Gdiplus::Color foreground;
};

BadgeStyle BadgeFor(const DeviceRow& device) {
  if (device.local) return {L"本机", Theme::AccentSoft, Theme::Accent};
  if (device.controlling) return {L"控制中", Theme::AccentSoft, Theme::Accent};
  if (device.controlled) return {L"被控制", Theme::AccentSoft, Theme::Accent};
  if (device.connected) return {L"已连接", Theme::SuccessSoft, Theme::Success};
  if (device.connecting) return {L"连接中", Theme::AccentSoft, Theme::Accent};
  if (device.paired) return {L"已配对 · 离线", Theme::SurfaceSubtle, Theme::TextSecondary};
  return {L"待配对", Theme::WarningSoft, Theme::Warning};
}

Gdiplus::Color StatusDot(const DeviceRow& device) {
  if (device.connected) return Theme::Success;
  if (device.connecting) return Theme::Accent;
  if (device.discovered) return Theme::Accent;
  return Theme::TextTertiary;
}

std::wstring Subtitle(const DeviceRow& device) {
  if (device.local) return L"本机屏幕 · " + ToWide(device.address);
  std::wstring text = device.address.empty() ? L"未知地址" : ToWide(device.address);
  if (!device.address.empty()) text += L":" + std::to_wstring(device.port);
  if (!device.version.empty()) text += L" · v" + ToWide(device.version);
  return text;
}

std::wstring DisplayName(const DeviceRow& device) {
  return ToWide(device.alias.empty() ? device.name : device.alias);
}

}  // namespace

DeviceListView::DeviceListView() = default;

int DeviceListView::ViewDpi() const { return Host() ? Host()->Dpi() : 96; }

int DeviceListView::RowHeight(int dpi) const { return Dpi::Scale(LogicalRowHeight, dpi); }

int DeviceListView::RowTop(int index, int dpi) const { return (index * RowHeight(dpi)) - scroll_; }

int DeviceListView::MaxScroll() const {
  return std::max(0, (static_cast<int>(devices_.size()) * RowHeight(ViewDpi())) - Bounds().Height());
}

bool DeviceListView::NeedsScroll() const {
  return (static_cast<int>(devices_.size()) * RowHeight(ViewDpi())) > Bounds().Height() &&
         Bounds().Height() > 0;
}

int DeviceListView::ContentWidth(int dpi) const {
  return Bounds().Width() - (NeedsScroll() ? Dpi::Scale(14, dpi) : 0);
}

void DeviceListView::ClampScroll() { scroll_ = std::clamp(scroll_, 0, MaxScroll()); }

void DeviceListView::SetDevices(std::vector<DeviceRow> devices) {
  devices_ = std::move(devices);
  hoverRow_ = -1;
  hoverSlot_ = -1;
  ClampScroll();
  Invalidate();
}

int DeviceListView::HitRow(Point2i local, int dpi) const {
  if (devices_.empty() || local.y < 0 || local.y >= Bounds().Height() || local.x >= ContentWidth(dpi))
    return -1;
  const int index = (local.y + scroll_) / RowHeight(dpi);
  return index >= 0 && index < static_cast<int>(devices_.size()) ? index : -1;
}

std::vector<DeviceListView::ActionSlot> DeviceListView::LayoutActions(const DeviceRow& device, int index,
                                                                     int dpi) const {
  const HFONT font = Dpi::Font(Theme::CaptionPx, true, dpi);
  const int gap = Dpi::Scale(6, dpi);
  const int height = Dpi::Scale(28, dpi);
  const int top = RowTop(index, dpi) + ((RowHeight(dpi) - height) / 2);
  int right = ContentWidth(dpi) - Dpi::Scale(12, dpi);

  // 本机行没有操作；其余行内最多两个按钮 + 一个“···”，其他动作收进菜单
  if (device.local) return {};
  const std::vector<RowAction> actions = device.connected
                                             ? std::vector<RowAction>{RowAction::Control, RowAction::Mirror, RowAction::More}
                                             : std::vector<RowAction>{RowAction::Connect, RowAction::More};

  std::vector<ActionSlot> slots;
  for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
    const RowAction action = *it;
    const wchar_t* label = action == RowAction::Connect   ? (device.connecting ? L"连接中…" : L"连接")
                           : action == RowAction::Control ? L"控制"
                           : action == RowAction::Mirror  ? L"镜像"
                                                          : L"";
    const int width = action == RowAction::More
                          ? Dpi::Scale(28, dpi)
                          : std::max(Dpi::Scale(58, dpi), draw::Measure(label, font).width + Dpi::Scale(22, dpi));
    right -= width;
    const bool disabled = action == RowAction::Connect && device.connecting;
    slots.insert(slots.begin(),
                 ActionSlot{action, label, RectI{right, top, right + width, top + height}, disabled});
    right -= gap;
  }
  return slots;
}

RectI DeviceListView::EmptyAddBounds(int dpi) const {
  const int width = Dpi::Scale(132, dpi);
  const int height = Dpi::Scale(32, dpi);
  const int cx = Bounds().Width() / 2;
  const int cy = std::max(Dpi::Scale(115, dpi), (Bounds().Height() / 2) + Dpi::Scale(20, dpi));
  return RectI{cx - (width / 2), cy, cx - (width / 2) + width, cy + height};
}

RectI DeviceListView::ScrollThumbBounds(int dpi) const {
  if (!NeedsScroll()) return RectI{};
  const int trackHeight = std::max(1, Bounds().Height() - Dpi::Scale(8, dpi));
  const int total = static_cast<int>(devices_.size()) * RowHeight(dpi);
  const int thumbHeight =
      std::max(Dpi::Scale(28, dpi), static_cast<int>(static_cast<double>(Bounds().Height()) / total * trackHeight));
  const int travel = std::max(1, trackHeight - thumbHeight);
  const int thumbTop = Dpi::Scale(4, dpi) + static_cast<int>(
                                               static_cast<double>(scroll_) / std::max(1, MaxScroll()) * travel);
  const int thumbWidth = (hoverScroll_ || draggingScroll_) ? Dpi::Scale(7, dpi) : Dpi::Scale(5, dpi);
  const int left = Bounds().Width() - thumbWidth - Dpi::Scale(3, dpi);
  return RectI{left, thumbTop, left + thumbWidth, thumbTop + thumbHeight};
}

void DeviceListView::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int width = Bounds().Width();
  const int height = Bounds().Height();
  draw::FillRect(ctx.graphics, RectI{0, 0, width, height}, Theme::Window);

  if (devices_.empty()) {
    DrawEmptyState(ctx);
    return;
  }

  const int contentWidth = ContentWidth(dpi);
  const int savedDc = SaveDC(ctx.dc);
  IntersectClipRect(ctx.dc, ctx.origin.x, ctx.origin.y, ctx.origin.x + contentWidth, ctx.origin.y + height);
  const Gdiplus::GraphicsState state = ctx.graphics.Save();
  ctx.graphics.SetClip(Gdiplus::Rect(0, 0, contentWidth, height), Gdiplus::CombineModeIntersect);

  const HFONT nameFont = Dpi::Font(Theme::BodyPx, true, dpi);
  const HFONT subFont = Dpi::MonoFont(Theme::CaptionPx, false, dpi);
  const HFONT badgeFont = Dpi::Font(Theme::MicroPx, true, dpi);
  const int rowHeight = RowHeight(dpi);
  const int padX = Dpi::Scale(10, dpi);

  const int first = std::max(0, scroll_ / std::max(1, rowHeight));
  for (int index = first; index < static_cast<int>(devices_.size()); ++index) {
    const int top = RowTop(index, dpi);
    if (top >= height) break;

    const DeviceRow& device = devices_[index];
    const bool rowHovered = index == hoverRow_;

    if (rowHovered) {
      draw::FillRound(ctx.graphics,
                      RectI{Dpi::Scale(2, dpi), top + Dpi::Scale(3, dpi), contentWidth - Dpi::Scale(2, dpi),
                            top + rowHeight - Dpi::Scale(3, dpi)},
                      Dpi::Scale(Theme::RadiusControl, dpi), Theme::RowHover);
    }
    if (index > 0 && !rowHovered && index - 1 != hoverRow_) {
      draw::Separator(ctx, padX, contentWidth - padX, top);
    }

    // 左侧设备图标与状态圆点
    const int avatarSize = Dpi::Scale(34, dpi);
    const int avatarTop = top + ((rowHeight - avatarSize) / 2);
    draw::FillRound(ctx.graphics, RectI{padX, avatarTop, padX + avatarSize, avatarTop + avatarSize},
                    Dpi::Scale(Theme::RadiusControl + 1, dpi),
                    device.connected ? Theme::AccentSoft : Theme::SurfaceSubtle);

    const int iconSize = Dpi::Scale(16, dpi);
    const int iconLeft = padX + ((avatarSize - iconSize) / 2);
    const int iconTop = avatarTop + ((avatarSize - iconSize) / 2);
    VectorIcons::DrawMonitor(ctx.graphics, RectI{iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize},
                             device.connected ? Theme::Accent : Theme::TextSecondary, dpi);

    const int dotSize = Dpi::Scale(8, dpi);
    const int dotLeft = padX + avatarSize - Dpi::Scale(5, dpi);
    const int dotTop = avatarTop + avatarSize - Dpi::Scale(5, dpi);
    draw::FillRound(ctx.graphics,
                    RectI{dotLeft - Dpi::Scale(1, dpi), dotTop - Dpi::Scale(1, dpi),
                          dotLeft + dotSize + Dpi::Scale(1, dpi), dotTop + dotSize + Dpi::Scale(1, dpi)},
                    (dotSize + Dpi::Scale(2, dpi)) / 2, rowHovered ? Theme::RowHover : Theme::Window);
    draw::FillRound(ctx.graphics, RectI{dotLeft, dotTop, dotLeft + dotSize, dotTop + dotSize}, dotSize / 2,
                    StatusDot(device));

    const std::vector<ActionSlot> slots = LayoutActions(device, index, dpi);
    const int textLeft = padX + avatarSize + Dpi::Scale(12, dpi);
    const int textRight = slots.empty() ? contentWidth - padX : slots.front().bounds.left - Dpi::Scale(12, dpi);

    // 状态胶囊
    const BadgeStyle badge = BadgeFor(device);
    const int badgeWidth = draw::Measure(badge.text, badgeFont).width + Dpi::Scale(22, dpi);
    const std::wstring name = DisplayName(device);
    const int nameWidth = std::min(draw::Measure(name, nameFont).width,
                                   std::max(Dpi::Scale(48, dpi), textRight - textLeft - badgeWidth));

    draw::TextWithin(ctx, name, nameFont, RectI{textLeft, top + Dpi::Scale(11, dpi), textLeft + nameWidth,
                                                top + Dpi::Scale(31, dpi)},
                     Theme::Text);

    if (textLeft + nameWidth + badgeWidth <= textRight + Dpi::Scale(4, dpi)) {
      draw::Pill(ctx, badge.text, badgeFont, textLeft + nameWidth + Dpi::Scale(8, dpi),
                 top + Dpi::Scale(21, dpi), badge.background, badge.foreground);
    }

    draw::TextWithin(ctx, Subtitle(device), subFont,
                     RectI{textLeft, top + Dpi::Scale(33, dpi), std::max(textLeft + Dpi::Scale(40, dpi), textRight),
                           top + Dpi::Scale(51, dpi)},
                     Theme::TextSecondary);

    DrawActions(ctx, slots, rowHovered ? hoverSlot_ : -1);
  }

  ctx.graphics.Restore(state);
  RestoreDC(ctx.dc, savedDc);

  // 滚动条
  if (NeedsScroll()) {
    const RectI thumb = ScrollThumbBounds(dpi);
    const Gdiplus::Color color = draggingScroll_ ? Theme::TextSecondary : (hoverScroll_ ? Theme::BorderStrong : Theme::ToggleTrackOff);
    draw::FillRound(ctx.graphics, thumb, thumb.Width() / 2, color);
  }
}

void DeviceListView::DrawActions(PaintContext& ctx, const std::vector<ActionSlot>& slots, int hoveredSlot) {
  const int dpi = ctx.dpi;
  const HFONT font = Dpi::Font(Theme::CaptionPx, true, dpi);
  const float strokeWidth = static_cast<float>(std::max(1, Dpi::Scale(1, dpi)));
  const int radius = Dpi::Scale(Theme::RadiusControl, dpi);

  for (size_t index = 0; index < slots.size(); ++index) {
    const ActionSlot& slot = slots[index];
    const bool hovered = static_cast<int>(index) == hoveredSlot;

    if (slot.action == RowAction::More) {
      if (hovered) draw::FillRound(ctx.graphics, slot.bounds, radius, Theme::SidebarHover);
      const int iconSize = Dpi::Scale(16, dpi);
      const int iconLeft = slot.bounds.left + ((slot.bounds.Width() - iconSize) / 2);
      const int iconTop = slot.bounds.top + ((slot.bounds.Height() - iconSize) / 2);
      VectorIcons::DrawMoreDots(ctx.graphics,
                                RectI{iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize},
                                hovered ? Theme::Text : Theme::TextSecondary, dpi);
      continue;
    }

    const bool primary = slot.action == RowAction::Connect || slot.action == RowAction::Control;
    if (primary) {
      draw::FillRound(ctx.graphics, slot.bounds, radius,
                      slot.disabled ? Theme::SurfaceSubtle : (hovered ? Theme::AccentHover : Theme::Accent));
      draw::TextCentered(ctx, slot.label, font, slot.bounds,
                         slot.disabled ? Theme::TextTertiary : Gdiplus::Color(255, 255, 255, 255));
    } else {
      draw::RoundRect(ctx.graphics, slot.bounds, radius, hovered ? Theme::SurfaceSubtle : Theme::Window,
                      hovered ? Theme::BorderStrong : Theme::Border, strokeWidth);
      draw::TextCentered(ctx, slot.label, font, slot.bounds, Theme::Text);
    }
  }
}

void DeviceListView::DrawEmptyState(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int width = Bounds().Width();
  const int height = Bounds().Height();
  const int centerX = width / 2;
  const int top = std::max(Dpi::Scale(46, dpi), (height / 2) - Dpi::Scale(46, dpi));

  const int badgeSize = Dpi::Scale(48, dpi);
  draw::FillRound(ctx.graphics, RectI{centerX - (badgeSize / 2), top, centerX + (badgeSize / 2), top + badgeSize},
                  Dpi::Scale(Theme::RadiusSurface + 4, dpi), Theme::SurfaceSubtle);

  const int iconSize = Dpi::Scale(22, dpi);
  const int iconLeft = centerX - (iconSize / 2);
  const int iconTop = top + ((badgeSize - iconSize) / 2);
  VectorIcons::DrawMonitor(ctx.graphics, RectI{iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize},
                           Theme::TextSecondary, dpi);

  const HFONT titleFont = Dpi::Font(Theme::SectionPx, true, dpi);
  const HFONT descFont = Dpi::Font(Theme::CaptionPx, false, dpi);
  const HFONT buttonFont = Dpi::Font(Theme::CaptionPx, true, dpi);

  draw::TextCentered(ctx, L"正在搜索局域网设备…", titleFont,
                     RectI{0, top + badgeSize + Dpi::Scale(14, dpi), width,
                           top + badgeSize + Dpi::Scale(36, dpi)},
                     Theme::Text);
  draw::TextCentered(ctx, L"确保对端已打开无界键鼠，或直接通过局域网 IP 添加", descFont,
                     RectI{Dpi::Scale(24, dpi), top + badgeSize + Dpi::Scale(38, dpi),
                           width - Dpi::Scale(24, dpi), top + badgeSize + Dpi::Scale(58, dpi)},
                     Theme::TextSecondary);

  const RectI button = EmptyAddBounds(dpi);
  draw::RoundRect(ctx.graphics, button, Dpi::Scale(Theme::RadiusControl, dpi),
                  hoverEmptyAdd_ ? Theme::SurfaceSubtle : Theme::Window,
                  hoverEmptyAdd_ ? Theme::BorderStrong : Theme::Border,
                  static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));
  draw::TextCentered(ctx, L"＋ 手动添加设备", buttonFont, button, Theme::Text);
}

void DeviceListView::Invoke(const std::function<void(const std::string&)>& callback, const DeviceRow& device) {
  if (!callback) return;
  const std::string id = device.id;  // 回调里可能整体替换列表，先留副本
  callback(id);
}

void DeviceListView::ShowRowMenu(const DeviceRow& device, Point2i anchor) {
  Window* host = Host();
  if (!host) return;

  const std::string id = device.id;
  std::vector<ThemedPopupMenu::Entry> entries;

  if (device.connected) {
    ThemedPopupMenu::Entry settings;
    settings.text = L"设备设置…";
    settings.onClick = [this, id] { if (onSettings) onSettings(id); };
    entries.push_back(std::move(settings));

    ThemedPopupMenu::Entry disconnect;
    disconnect.text = L"断开连接";
    disconnect.onClick = [this, id] { if (onDisconnect) onDisconnect(id); };
    entries.push_back(std::move(disconnect));

    ThemedPopupMenu::Entry separator;
    separator.isSeparator = true;
    entries.push_back(std::move(separator));
  }

  ThemedPopupMenu::Entry forget;
  forget.text = device.paired ? L"撤销配对" : L"从列表移除";
  forget.isDanger = true;
  forget.onClick = [this, id, paired = device.paired] {
    if (paired) {
      if (onUnpair) onUnpair(id);
    } else if (onRemove) {
      onRemove(id);
    }
  };
  entries.push_back(std::move(forget));

  ThemedPopupMenu::Show(host, anchor, std::move(entries), 148, false);
}

void DeviceListView::OnMouseMove(Point2i local) {
  const int dpi = ViewDpi();

  if (draggingScroll_) {
    const int trackHeight = std::max(1, Bounds().Height() - Dpi::Scale(8, dpi));
    const int total = static_cast<int>(devices_.size()) * RowHeight(dpi);
    const int thumbHeight =
        std::max(Dpi::Scale(28, dpi), static_cast<int>(static_cast<double>(Bounds().Height()) / total * trackHeight));
    const int travel = std::max(1, trackHeight - thumbHeight);
    const int delta = local.y - dragStartY_;
    scroll_ = std::clamp(dragStartScroll_ + static_cast<int>(static_cast<double>(delta) / travel * MaxScroll()), 0,
                         MaxScroll());
    Invalidate();
    return;
  }

  const int contentWidth = ContentWidth(dpi);
  const bool overScroll = NeedsScroll() && local.x >= contentWidth;
  const int row = overScroll ? -1 : HitRow(local, dpi);

  int slot = -1;
  if (row >= 0) {
    const std::vector<ActionSlot> slots = LayoutActions(devices_[row], row, dpi);
    for (size_t index = 0; index < slots.size(); ++index) {
      if (!slots[index].bounds.Contains(local.x, local.y)) continue;
      slot = slots[index].disabled ? -1 : static_cast<int>(index);
      break;
    }
  }

  const bool overEmpty = devices_.empty() && EmptyAddBounds(dpi).Contains(local.x, local.y);

  if (row != hoverRow_ || slot != hoverSlot_ || overScroll != hoverScroll_ || overEmpty != hoverEmptyAdd_) {
    hoverRow_ = row;
    hoverSlot_ = slot;
    hoverScroll_ = overScroll;
    hoverEmptyAdd_ = overEmpty;
    SetCursor((slot >= 0 || overEmpty) ? LoadCursorW(nullptr, IDC_HAND) : nullptr);
    Invalidate();
  }
}

void DeviceListView::OnMouseLeave() {
  if (hoverRow_ < 0 && hoverSlot_ < 0 && !hoverScroll_ && !hoverEmptyAdd_) return;
  hoverRow_ = -1;
  hoverSlot_ = -1;
  hoverScroll_ = false;
  hoverEmptyAdd_ = false;
  SetCursor(nullptr);
  Invalidate();
}

bool DeviceListView::OnMouseDown(Point2i local, MouseButton button) {
  if (button != MouseButton::Left || !NeedsScroll()) return false;
  if (local.x < ContentWidth(ViewDpi())) return false;

  draggingScroll_ = true;
  dragStartY_ = local.y;
  dragStartScroll_ = scroll_;
  Invalidate();
  return true;
}

void DeviceListView::OnMouseUp(Point2i local, MouseButton button) {
  const int dpi = ViewDpi();

  if (draggingScroll_) {
    draggingScroll_ = false;
    Invalidate();
    return;
  }

  if (devices_.empty()) {
    if (button == MouseButton::Left && EmptyAddBounds(dpi).Contains(local.x, local.y) && onAdd) onAdd();
    return;
  }

  const int row = HitRow(local, dpi);
  if (row < 0) return;
  const DeviceRow& device = devices_[row];

  // 本机行不提供任何操作或菜单
  if (device.local) return;

  if (button == MouseButton::Right) {
    const std::vector<ActionSlot> slots = LayoutActions(device, row, dpi);
    for (const ActionSlot& slot : slots) {
      if (slot.action == RowAction::More && slot.bounds.Contains(local.x, local.y)) {
        ShowRowMenu(device, Point2i{slot.bounds.left, slot.bounds.bottom + Dpi::Scale(6, dpi)});
        return;
      }
    }
    ShowRowMenu(device, local);
    return;
  }

  if (button != MouseButton::Left) return;

  // 行内按钮：先做动作再返回，回调可能整体替换列表
  const std::vector<ActionSlot> slots = LayoutActions(device, row, dpi);
  for (const ActionSlot& slot : slots) {
    if (!slot.bounds.Contains(local.x, local.y)) continue;
    if (slot.disabled) return;
    switch (slot.action) {
      case RowAction::Connect:
        Invoke(onConnect, device);
        return;
      case RowAction::Control:
        Invoke(onControl, device);
        return;
      case RowAction::Mirror:
        Invoke(onMirror, device);
        return;
      case RowAction::More:
        ShowRowMenu(device, Point2i{slot.bounds.left, slot.bounds.bottom + Dpi::Scale(6, dpi)});
        return;
    }
  }
}

void DeviceListView::OnWheel(int delta, Point2i) {
  if (!NeedsScroll()) return;
  const int rowHeight = RowHeight(ViewDpi());
  scroll_ = std::clamp(scroll_ - (delta * rowHeight / 120), 0, MaxScroll());
  Invalidate();
}

}  // namespace bm