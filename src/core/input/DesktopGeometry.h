#pragma once

#include "core/platform/Common.h"

#include <utility>
#include <vector>

namespace bm {

struct RectI {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
  int Width() const { return right - left; }
  int Height() const { return bottom - top; }
  bool IsEmpty() const { return right <= left || bottom <= top; }
  bool Contains(int x, int y) const { return x >= left && x < right && y >= top && y < bottom; }
  bool operator==(const RectI&) const = default;
};

struct Point2i {
  int x = 0;
  int y = 0;
  bool operator==(const Point2i&) const = default;
};

struct Size2i {
  int width = 0;
  int height = 0;
  bool operator==(const Size2i&) const = default;
};

// 真实外边段：side 0 上 1 下 2 左 3 右
struct EdgeSegment {
  int side = 0;
  int coordinate = 0;
  int start = 0;
  int end = 0;
};

// 虚拟桌面零基显示器区域；坐标、可见区域与真实外边段的唯一几何规则
class DesktopGeometry {
 public:
  static constexpr int kMaxMonitors = 64;

  DesktopGeometry() = default;
  DesktopGeometry(Size2i size, std::vector<RectI> monitors);
  static DesktopGeometry Capture(Point2i* origin = nullptr);
  static void ValidateSize(Size2i size);

  bool Valid() const { return !monitors_.empty(); }
  Size2i Size() const { return size_; }
  const std::vector<RectI>& Monitors() const { return monitors_; }
  bool SameAs(const DesktopGeometry& other) const;

  bool Contains(Point2i point) const;
  std::pair<double, double> ClampD(double x, double y) const;
  Point2i ClampPixel(Point2i point) const;
  Point2i ClampToDesktop(Point2i point, Point2i origin) const;
  Point2i ParkingPoint(Point2i point, Point2i origin) const;
  Point2i ToPixel(double x, double y) const;
  std::pair<double, double> MapFrom(double x, double y, Size2i from) const;
  // 沿线段推进，碰壁消去一个轴；exitSide 为首次离开的边，无越界时为 -1
  std::pair<double, double> Move(double x, double y, double deltaX, double deltaY, int& exitSide) const;
  // 本机光标已夹在边缘时，用同一组外边段判断向外的原始位移
  int GetEdgeSide(Point2i cursor, double deltaX, double deltaY) const;
  std::pair<double, double> EntryPoint(int exitSide, double x, double y, Size2i from) const;

 private:
  bool ContainsD(double x, double y) const;
  RectI MonitorAt(Point2i point, Point2i origin) const;
  const EdgeSegment* FirstExit(double x, double y, double deltaX, double deltaY, double& fraction) const;
  void BuildEdges();

  Size2i size_{};
  std::vector<RectI> monitors_;
  std::vector<EdgeSegment> edges_;
};

}  // namespace bm
