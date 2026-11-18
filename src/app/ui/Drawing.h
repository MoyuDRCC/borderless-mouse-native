#pragma once

#include "app/ui/GdiPlusSetup.h"

#include <string_view>

#include "app/ui/Dpi.h"

namespace bm {

// 自绘上下文：GDI+ 负责形状与路径，同一个 DC 上用 GDI 画文字（ClearType），
// 与 .NET 侧 Graphics + TextRenderer 的混用方式一一对应。
struct PaintContext {
  Gdiplus::Graphics& graphics;
  HDC dc;
  int dpi;
  RectI clip;      // 本控件局部坐标下的可见区域
  Point2i origin;  // 本控件在窗口客户区的绝对原点：GDI 文字与裁剪需要设备坐标
};

namespace draw {

// 几何：圆角矩形路径按半径钳制，避免小尺寸画出破口
void BuildRoundedRect(const RectI& rect, int radius, Gdiplus::GraphicsPath& path);
void FillRect(Gdiplus::Graphics& graphics, const RectI& rect, const Gdiplus::Color& color);
void FillRound(Gdiplus::Graphics& graphics, const RectI& rect, int radius, const Gdiplus::Color& color);
void StrokeRound(Gdiplus::Graphics& graphics, const RectI& rect, int radius, const Gdiplus::Color& color,
                 float width);
// 先填充后描边；两者 alpha 为 0 时跳过
void RoundRect(Gdiplus::Graphics& graphics, const RectI& rect, int radius, const Gdiplus::Color& fill,
               const Gdiplus::Color& stroke, float width);

// 文字
void Text(PaintContext& ctx, std::wstring_view text, HFONT font, int x, int y, const Gdiplus::Color& color);
void TextWithin(PaintContext& ctx, std::wstring_view text, HFONT font, const RectI& bounds,
                const Gdiplus::Color& color);
void TextCentered(PaintContext& ctx, std::wstring_view text, HFONT font, const RectI& bounds,
                  const Gdiplus::Color& color);
Size2i Measure(std::wstring_view text, HFONT font);

// 分隔线、胶囊徽章、颜色插值
void Separator(PaintContext& ctx, int x1, int x2, int y);
int Pill(PaintContext& ctx, std::wstring_view text, HFONT font, int x, int centerY,
         const Gdiplus::Color& background, const Gdiplus::Color& foreground);
Gdiplus::Color Lerp(const Gdiplus::Color& from, const Gdiplus::Color& to, float t);
COLORREF Ref(const Gdiplus::Color& color);

}  // namespace draw
}  // namespace bm
