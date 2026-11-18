#pragma once

#include <windows.h>

#include "core/input/DesktopGeometry.h"

// 几何基元（RectI / Point2i / Size2i）统一定义在 core/DesktopGeometry.h：
// 界面侧的布局同样使用逻辑像素（96 DPI 基准），绘制前再由 Dpi 换算成物理像素。
namespace bm {

// 由左上角与尺寸构造矩形
inline RectI MakeRect(Point2i origin, int width, int height) {
  return RectI{origin.x, origin.y, origin.x + width, origin.y + height};
}

// 两矩形交集；无交集时返回空矩形而不是负尺寸
RectI Intersect(const RectI& a, const RectI& b);

namespace Dpi {

// 逻辑像素 → 物理像素：物理 = 逻辑 × dpi / 96
int Scale(int logicalPx, int dpi);
float ScaleF(float logicalPx, int dpi);

// 正文字体：系统消息框字体族（通常 Segoe UI），按 像素高度 + 字重 缓存，永不调用方释放
HFONT Font(float logicalPx, bool bold, int dpi);

// 等宽字体：Cascadia Mono → Cascadia Code → Consolas
HFONT MonoFont(float logicalPx, bool bold, int dpi);

// 进程退出前释放字体缓存
void ReleaseFontCache();

}  // namespace Dpi
}  // namespace bm
