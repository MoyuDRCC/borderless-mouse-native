#include "app/ui/Drawing.h"

#include <algorithm>
#include <cmath>

#include "app/ui/Theme.h"

namespace bm::draw {

namespace {

// 测量专用 DC：只做 GetTextExtentPoint32，不参与绘制
HDC MeasureDc() {
  static HDC dc = GetDC(nullptr);
  return dc;
}

// 单行文字的目标矩形：右侧给足宽度，实际裁剪由 DC 裁剪区负责
RECT SingleLineBox(PaintContext& ctx, int x, int y) {
  const int left = ctx.origin.x + x;
  const int top = ctx.origin.y + y;
  return RECT{left, top, left + 8192, top + 4096};
}

RECT DeviceRect(PaintContext& ctx, const RectI& bounds) {
  return RECT{bounds.left + ctx.origin.x, bounds.top + ctx.origin.y, bounds.right + ctx.origin.x,
              bounds.bottom + ctx.origin.y};
}

void DrawTextFlags(PaintContext& ctx, std::wstring_view text, HFONT font, RECT box, UINT flags,
                   const Gdiplus::Color& color) {
  const HGDIOBJ previousFont = SelectObject(ctx.dc, font);
  const int previousMode = SetBkMode(ctx.dc, TRANSPARENT);
  const COLORREF previousColor = SetTextColor(ctx.dc, Ref(color));
  DrawTextW(ctx.dc, text.data(), static_cast<int>(text.size()), &box, flags);
  SetTextColor(ctx.dc, previousColor);
  SetBkMode(ctx.dc, previousMode);
  SelectObject(ctx.dc, previousFont);
}

}  // namespace

COLORREF Ref(const Gdiplus::Color& color) { return RGB(color.GetR(), color.GetG(), color.GetB()); }

void BuildRoundedRect(const RectI& rect, int radius, Gdiplus::GraphicsPath& path) {
  const int maxRadius = std::min(rect.Width(), rect.Height()) / 2;
  const int clamped = std::min(radius, maxRadius);
  if (clamped <= 0) {
    path.AddRectangle(Gdiplus::Rect(rect.left, rect.top, rect.Width(), rect.Height()));
    return;
  }

  const int d = clamped * 2;
  path.AddArc(rect.left, rect.top, d, d, 180, 90);
  path.AddArc(rect.right - d, rect.top, d, d, 270, 90);
  path.AddArc(rect.right - d, rect.bottom - d, d, d, 0, 90);
  path.AddArc(rect.left, rect.bottom - d, d, d, 90, 90);
  path.CloseFigure();
}

void FillRect(Gdiplus::Graphics& graphics, const RectI& rect, const Gdiplus::Color& color) {
  if (rect.IsEmpty()) return;
  Gdiplus::SolidBrush brush(color);
  graphics.FillRectangle(&brush, rect.left, rect.top, rect.Width(), rect.Height());
}

void FillRound(Gdiplus::Graphics& graphics, const RectI& rect, int radius, const Gdiplus::Color& color) {
  if (rect.IsEmpty()) return;
  Gdiplus::GraphicsPath path;
  BuildRoundedRect(rect, radius, path);
  Gdiplus::SolidBrush brush(color);
  graphics.FillPath(&brush, &path);
}

void StrokeRound(Gdiplus::Graphics& graphics, const RectI& rect, int radius, const Gdiplus::Color& color,
                 float width) {
  if (rect.IsEmpty()) return;
  Gdiplus::GraphicsPath path;
  BuildRoundedRect(rect, radius, path);
  Gdiplus::Pen pen(color, std::max(1.0f, width));
  graphics.DrawPath(&pen, &path);
}

void RoundRect(Gdiplus::Graphics& graphics, const RectI& rect, int radius, const Gdiplus::Color& fill,
               const Gdiplus::Color& stroke, float width) {
  if (fill.GetA() > 0) FillRound(graphics, rect, radius, fill);
  if (stroke.GetA() > 0) StrokeRound(graphics, rect, radius, stroke, width);
}

void Text(PaintContext& ctx, std::wstring_view text, HFONT font, int x, int y, const Gdiplus::Color& color) {
  if (text.empty()) return;
  DrawTextFlags(ctx, text, font, SingleLineBox(ctx, x, y), DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX,
                color);
}

void TextWithin(PaintContext& ctx, std::wstring_view text, HFONT font, const RectI& bounds,
                const Gdiplus::Color& color) {
  if (text.empty()) return;
  DrawTextFlags(ctx, text, font, DeviceRect(ctx, bounds),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, color);
}

void TextCentered(PaintContext& ctx, std::wstring_view text, HFONT font, const RectI& bounds,
                  const Gdiplus::Color& color) {
  if (text.empty()) return;
  DrawTextFlags(ctx, text, font, DeviceRect(ctx, bounds),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, color);
}

Size2i Measure(std::wstring_view text, HFONT font) {
  if (text.empty()) return Size2i{0, 0};
  const HDC dc = MeasureDc();
  const HGDIOBJ previous = SelectObject(dc, font);
  SIZE size{};
  GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
  SelectObject(dc, previous);
  return Size2i{size.cx, size.cy};
}

void Separator(PaintContext& ctx, int x1, int x2, int y) {
  if (x2 <= x1) return;
  const int thickness = std::max(1, Dpi::Scale(1, ctx.dpi));
  FillRect(ctx.graphics, RectI{x1, y, x2, y + thickness}, Theme::Separator);
}

int Pill(PaintContext& ctx, std::wstring_view text, HFONT font, int x, int centerY,
         const Gdiplus::Color& background, const Gdiplus::Color& foreground) {
  const Size2i textSize = Measure(text, font);
  const int padX = Dpi::Scale(8, ctx.dpi);
  const int height = std::max(Dpi::Scale(20, ctx.dpi), textSize.height + Dpi::Scale(4, ctx.dpi));
  const int width = textSize.width + (padX * 2);
  const RectI bounds{x, centerY - (height / 2), x + width, centerY - (height / 2) + height};

  FillRound(ctx.graphics, bounds, height / 2, background);
  TextCentered(ctx, text, font, bounds, foreground);
  return width;
}

Gdiplus::Color Lerp(const Gdiplus::Color& from, const Gdiplus::Color& to, float t) {
  const float clamped = std::clamp(t, 0.0f, 1.0f);
  const auto mix = [clamped](BYTE start, BYTE end) {
    return static_cast<BYTE>(std::lround(start + ((end - start) * clamped)));
  };
  return Gdiplus::Color(mix(from.GetA(), to.GetA()), mix(from.GetR(), to.GetR()),
                        mix(from.GetG(), to.GetG()), mix(from.GetB(), to.GetB()));
}

}  // namespace bm::draw
