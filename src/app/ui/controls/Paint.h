#pragma once

#include <algorithm>
#include <string_view>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"

namespace bm::paint {

// 页面复用的薄封装：页面只表达“这是什么文字 / 这是一条分隔线”，
// 圆角路径、字号换算、GDI 文字标志全部留在这里，页面不碰 GDI+ 细节。

enum class TextStyle { Title, Section, Body, BodyBold, Caption, Micro, Mono, MonoCaption };

// 语义字号 → 缓存字体（对应 PageBase 暴露的那一组 Font 属性）
inline HFONT Font(TextStyle style, int dpi) {
  switch (style) {
    case TextStyle::Title:
      return Dpi::Font(Theme::TitlePx, true, dpi);
    case TextStyle::Section:
      return Dpi::Font(Theme::SectionPx, true, dpi);
    case TextStyle::Body:
      return Dpi::Font(Theme::BodyPx, false, dpi);
    case TextStyle::BodyBold:
      return Dpi::Font(Theme::BodyPx, true, dpi);
    case TextStyle::Caption:
      return Dpi::Font(Theme::CaptionPx, false, dpi);
    case TextStyle::Micro:
      return Dpi::Font(Theme::MicroPx, false, dpi);
    case TextStyle::Mono:
      return Dpi::MonoFont(Theme::BodyPx, false, dpi);
    case TextStyle::MonoCaption:
      return Dpi::MonoFont(Theme::CaptionPx, false, dpi);
  }
  return Dpi::Font(Theme::BodyPx, false, dpi);
}

inline void Fill(PaintContext& ctx, const RectI& rect, const Gdiplus::Color& color) {
  draw::FillRect(ctx.graphics, rect, color);
}

// 填充 + 1px 描边（逻辑像素宽），alpha 为 0 的一侧自动跳过
inline void Round(PaintContext& ctx, const RectI& rect, int radius, const Gdiplus::Color& fill,
                  const Gdiplus::Color& stroke) {
  draw::RoundRect(ctx.graphics, rect, radius, fill, stroke,
                  static_cast<float>(std::max(1, Dpi::Scale(1, ctx.dpi))));
}

inline void Separator(PaintContext& ctx, int x1, int x2, int y) { draw::Separator(ctx, x1, x2, y); }

// 胶囊徽章：返回总宽度，方便行内文字顺次排列
inline int Pill(PaintContext& ctx, std::wstring_view text, int x, int centerY,
                const Gdiplus::Color& background, const Gdiplus::Color& foreground) {
  return draw::Pill(ctx, text, Font(TextStyle::Micro, ctx.dpi), x, centerY, background, foreground);
}

inline void Text(PaintContext& ctx, std::wstring_view text, TextStyle style, int x, int y,
                 const Gdiplus::Color& color) {
  draw::Text(ctx, text, Font(style, ctx.dpi), x, y, color);
}

// 在矩形内绘制：左对齐、垂直居中、超出省略
inline void TextIn(PaintContext& ctx, std::wstring_view text, TextStyle style, const RectI& bounds,
                   const Gdiplus::Color& color) {
  draw::TextWithin(ctx, text, Font(style, ctx.dpi), bounds, color);
}

inline void TextCentered(PaintContext& ctx, std::wstring_view text, TextStyle style, const RectI& bounds,
                         const Gdiplus::Color& color) {
  draw::TextCentered(ctx, text, Font(style, ctx.dpi), bounds, color);
}

inline Size2i Measure(PaintContext& ctx, std::wstring_view text, TextStyle style) {
  return draw::Measure(text, Font(style, ctx.dpi));
}

// 页面栅格：与 .NET 版 PageBase 的 PageInset / ContentTop 一致（逻辑像素 28 / 104）
inline int Inset(int dpi) { return Dpi::Scale(Theme::PageInset, dpi); }
inline int ContentTop(int dpi) { return Dpi::Scale(Theme::PageContentTop, dpi); }

// 页头：标题 + 副标题 + 1px 细分割线
inline void PageHeader(PaintContext& ctx, std::wstring_view title, std::wstring_view subtitle) {
  const int inset = Inset(ctx.dpi);
  Text(ctx, title, TextStyle::Title, inset, Dpi::Scale(Theme::PageTitleY, ctx.dpi), Theme::Text);
  if (!subtitle.empty()) {
    Text(ctx, subtitle, TextStyle::Caption, inset, Dpi::Scale(Theme::PageSubtitleY, ctx.dpi),
         Theme::TextSecondary);
  }
  Separator(ctx, inset, ctx.clip.right - inset, Dpi::Scale(Theme::PageDividerY, ctx.dpi));
}

// 设置行（开关）：左文案、右 ToggleSwitch，对应 PageBase.PlaceSwitchRow
inline void ToggleRow(int dpi, const RectI& row, RectI& caption, RectI& toggle) {
  const int toggleWidth = Dpi::Scale(38, dpi);   // ToggleSwitch::LogicalSize()
  const int toggleHeight = Dpi::Scale(20, dpi);
  const int rowHeight = Dpi::Scale(22, dpi);
  const int captionWidth = std::max(Dpi::Scale(120, dpi), row.Width() - toggleWidth - Dpi::Scale(16, dpi));

  caption = RectI{row.left, row.top, row.left + captionWidth, row.top + rowHeight};
  const int toggleTop = row.top + ((rowHeight - toggleHeight) / 2);
  toggle = RectI{row.right - toggleWidth, toggleTop, row.right, toggleTop + toggleHeight};
}

// 设置行（下拉框）：左文案、右 FlatComboBox，对应 PageBase.PlaceComboRow
inline void ComboRow(int dpi, const RectI& row, int comboWidth, RectI& caption, RectI& combo) {
  const int comboHeight = Dpi::Scale(30, dpi);
  const int captionWidth = std::max(Dpi::Scale(120, dpi), row.Width() - comboWidth - Dpi::Scale(16, dpi));

  combo = RectI{row.right - comboWidth, row.top, row.right, row.top + comboHeight};
  caption = RectI{row.left, row.top + Dpi::Scale(5, dpi), row.left + captionWidth,
                  row.top + Dpi::Scale(25, dpi)};
}

}  // namespace bm::paint
