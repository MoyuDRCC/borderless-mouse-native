#pragma once

// GDI+ 入口：WIN32_LEAN_AND_MEAN 下 gdiplus.h 需要 objidl/propidl 先就位，
// 否则 GdiplusBitmap.h / GdiplusFlat.h 会报 IStream、PROPID 未声明。
#include <windows.h>

#include <objidl.h>
#include <propidl.h>

#include <gdiplus.h>
