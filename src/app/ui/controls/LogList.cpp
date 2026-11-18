#include "app/ui/controls/LogList.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"
#include "app/ui/controls/ThemedPopupMenu.h"

namespace bm {

namespace {

std::wstring FormatTime(uint64_t timeMs) {
  const std::time_t seconds = static_cast<std::time_t>(timeMs / 1000);
  std::tm local{};
  localtime_s(&local, &seconds);
  wchar_t buffer[16]{};
  swprintf_s(buffer, L"%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
  return buffer;
}

const wchar_t* LevelText(LogLevel level) {
  switch (level) {
    case LogLevel::Error:
      return L"错误";
    case LogLevel::Warning:
      return L"警告";
    default:
      return L"信息";
  }
}

void WriteClipboardText(HWND owner, const std::wstring& text) {
  if (!OpenClipboard(owner)) return;
  EmptyClipboard();
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* target = GlobalLock(memory)) {
      memcpy(target, text.c_str(), bytes);
      GlobalUnlock(memory);
    }
    SetClipboardData(CF_UNICODETEXT, memory);
  }
  CloseClipboard();
}

}  // namespace

LogList::LogList(bool compact) : compact_(compact) {}

int LogList::ViewDpi() const { return Host() ? Host()->Dpi() : 96; }

int LogList::RowHeight(int dpi) const { return Dpi::Scale(compact_ ? 26 : 28, dpi); }

int LogList::MaxScroll() const {
  return std::max(0, (static_cast<int>(entries_.size()) * RowHeight(ViewDpi())) - Bounds().Height());
}

bool LogList::NeedsScroll() const {
  return (static_cast<int>(entries_.size()) * RowHeight(ViewDpi())) > Bounds().Height() &&
         Bounds().Height() > 0;
}

int LogList::HitRow(Point2i local, int dpi) const {
  if (entries_.empty() || local.y < 0 || local.y >= Bounds().Height()) return -1;
  const int index = (local.y + scroll_) / RowHeight(dpi);
  return index >= 0 && index < static_cast<int>(entries_.size()) ? index : -1;
}

void LogList::SetEntries(std::vector<LogEntry> entries) {
  entries_ = std::move(entries);
  selected_ = compact_ ? -1 : std::min(selected_, static_cast<int>(entries_.size()) - 1);
  scroll_ = MaxScroll();  // 总是停在最新一条
  Invalidate();
}

void LogList::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int width = Bounds().Width();
  const int height = Bounds().Height();
  draw::FillRect(ctx.graphics, RectI{0, 0, width, height}, Theme::Window);
  if (entries_.empty()) return;

  const int rowHeight = RowHeight(dpi);
  const int left = Dpi::Scale(6, dpi);
  const int timeWidth = Dpi::Scale(64, dpi);

  const HFONT textFont = Dpi::Font(Theme::CaptionPx + 0.5f, false, dpi);
  const HFONT monoFont = Dpi::MonoFont(Theme::CaptionPx, false, dpi);
  const HFONT badgeFont = Dpi::Font(Theme::MicroPx, true, dpi);

  const int savedDc = SaveDC(ctx.dc);
  IntersectClipRect(ctx.dc, ctx.origin.x, ctx.origin.y, ctx.origin.x + width, ctx.origin.y + height);
  const Gdiplus::GraphicsState state = ctx.graphics.Save();
  ctx.graphics.SetClip(Gdiplus::Rect(0, 0, width, height), Gdiplus::CombineModeIntersect);

  const int first = std::max(0, scroll_ / std::max(1, rowHeight));
  for (int index = first; index < static_cast<int>(entries_.size()); ++index) {
    const int top = (index * rowHeight) - scroll_;
    if (top >= height) break;
    const LogEntry& entry = entries_[index];
    const RectI row{0, top, width, top + rowHeight};

    if (!compact_ && index == selected_) {
      draw::FillRound(ctx.graphics,
                      RectI{left, top + Dpi::Scale(1, dpi), width - Dpi::Scale(2, dpi), top + rowHeight - Dpi::Scale(1, dpi)},
                      Dpi::Scale(4, dpi), Theme::AccentSoft);
    } else if (index > 0) {
      draw::Separator(ctx, left + Dpi::Scale(4, dpi), width - Dpi::Scale(4, dpi), top);
    }

    const int centerY = top + (rowHeight / 2);
    draw::TextWithin(ctx, FormatTime(entry.timeMs), monoFont,
                     RectI{left, top, left + timeWidth, top + rowHeight}, Theme::TextTertiary);

    Gdiplus::Color badgeBackground = Theme::SurfaceSubtle;
    Gdiplus::Color badgeForeground = Theme::TextSecondary;
    if (entry.level == LogLevel::Error) {
      badgeBackground = Theme::DangerSoft;
      badgeForeground = Theme::Danger;
    } else if (entry.level == LogLevel::Warning) {
      badgeBackground = Theme::WarningSoft;
      badgeForeground = Theme::Warning;
    }

    const int pillWidth = draw::Pill(ctx, LevelText(entry.level), badgeFont,
                                     left + timeWidth + Dpi::Scale(4, dpi), centerY, badgeBackground,
                                     badgeForeground);
    const int textLeft = left + timeWidth + pillWidth + Dpi::Scale(14, dpi);
    draw::TextWithin(ctx, ToWide(entry.text), textFont,
                     RectI{textLeft, top, width - Dpi::Scale(6, dpi), top + rowHeight}, Theme::Text);
  }

  ctx.graphics.Restore(state);
  RestoreDC(ctx.dc, savedDc);
}

bool LogList::OnMouseDown(Point2i local, MouseButton) {
  if (compact_) return false;
  const int row = HitRow(local, ViewDpi());
  if (row < 0) return false;
  if (Window* host = Host()) host->FocusControl(this);
  selected_ = row;
  Invalidate();
  return true;
}

void LogList::OnMouseUp(Point2i local, MouseButton button) {
  if (compact_ || button != MouseButton::Right) return;
  const int row = HitRow(local, ViewDpi());
  if (row < 0) return;

  selected_ = row;
  Invalidate();

  const std::string text = entries_[row].text;
  std::wstring formatted = L"[" + FormatTime(entries_[row].timeMs) + L"] [" +
                           LevelText(entries_[row].level) + L"] " + ToWide(text);

  std::vector<ThemedPopupMenu::Entry> entries;
  ThemedPopupMenu::Entry copy;
  copy.text = L"复制此条日志";
  copy.onClick = [formatted = std::move(formatted)] { WriteClipboardText(nullptr, formatted); };
  entries.push_back(std::move(copy));

  const Point2i origin = AbsoluteOrigin();
  ThemedPopupMenu::Show(Host(), Point2i{origin.x + local.x, origin.y + local.y}, std::move(entries), 136, false);
}

void LogList::OnWheel(int delta, Point2i) {
  if (!NeedsScroll()) return;
  const int rowHeight = RowHeight(ViewDpi());
  scroll_ = std::clamp(scroll_ - (delta * rowHeight / 120), 0, MaxScroll());
  Invalidate();
}

void LogList::OnKey(int vk, bool down) {
  if (!down || compact_ || selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) return;
  if (vk == 'C' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) CopyEntry(entries_[selected_]);
}

void LogList::CopyEntry(const LogEntry& entry) {
  const std::wstring text = L"[" + FormatTime(entry.timeMs) + L"] [" + LevelText(entry.level) + L"] " +
                            ToWide(entry.text);
  WriteClipboardText(Host() ? Host()->Handle() : nullptr, text);
}

}  // namespace bm
