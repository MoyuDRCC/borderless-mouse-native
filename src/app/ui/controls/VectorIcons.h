#pragma once

#include "app/ui/GdiPlusSetup.h"

#include "app/ui/Dpi.h"
#include "app/ui/controls/NavSidebar.h"

namespace bm::VectorIcons {

// 纯 GDI+ 线性矢量图标：任意 DPI 锐利，零图标资源依赖。
// 所有坐标基于 16×16 逻辑网格缩放，描边圆头圆角，与 .NET 侧完全一致。
void DrawNav(Gdiplus::Graphics& graphics, NavSidebar::Section section, const RectI& bounds,
             const Gdiplus::Color& color, int dpi);
void DrawMonitor(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi);
void DrawMoreDots(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi);
void DrawChevronDown(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi);
void DrawCheck(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi);
void DrawCopy(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi);

}  // namespace bm::VectorIcons
