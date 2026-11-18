#include "core/input/DesktopGeometry.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bm {

namespace {
constexpr int kTop = 0, kBottom = 1, kLeft = 2, kRight = 3;

bool IsVertical(int side) { return side == kLeft || side == kRight; }

bool Outward(int side, double deltaX, double deltaY) {
  switch (side) {
    case kTop: return deltaY < 0;
    case kBottom: return deltaY > 0;
    case kLeft: return deltaX < 0;
    default: return deltaX > 0;
  }
}

double SquaredDistance(double x, double y, double otherX, double otherY) {
  return (x - otherX) * (x - otherX) + (y - otherY) * (y - otherY);
}
}  // namespace

void DesktopGeometry::ValidateSize(Size2i size) {
  if (size.width < 1 || size.width > 1000000 || size.height < 1 || size.height > 1000000)
    throw std::invalid_argument("桌面每轴必须处于1至1000000像素之间");
}

DesktopGeometry::DesktopGeometry(Size2i size, std::vector<RectI> monitors) {
  ValidateSize(size);
  if (monitors.empty() || monitors.size() > static_cast<size_t>(kMaxMonitors))
    throw std::invalid_argument("显示器数量必须处于1至64之间");
  for (const RectI& monitor : monitors) {
    // 先提升到 long long 再相加，不能让恶意矩形借 int 溢出绕过范围验证
    if (monitor.left < 0 || monitor.top < 0 || monitor.Width() <= 0 || monitor.Height() <= 0 ||
        static_cast<long long>(monitor.left) + monitor.Width() > size.width ||
        static_cast<long long>(monitor.top) + monitor.Height() > size.height)
      throw std::invalid_argument("显示器必须是虚拟桌面内的非空矩形");
  }
  int minLeft = monitors[0].left, minTop = monitors[0].top, maxRight = monitors[0].right, maxBottom = monitors[0].bottom;
  for (const RectI& monitor : monitors) {
    minLeft = std::min(minLeft, monitor.left);
    minTop = std::min(minTop, monitor.top);
    maxRight = std::max(maxRight, monitor.right);
    maxBottom = std::max(maxBottom, monitor.bottom);
  }
  if (minLeft != 0 || minTop != 0 || maxRight != size.width || maxBottom != size.height)
    throw std::invalid_argument("显示器外接范围与桌面尺寸不一致");

  std::sort(monitors.begin(), monitors.end(), [](const RectI& left, const RectI& right) {
    if (left.left != right.left) return left.left < right.left;
    if (left.top != right.top) return left.top < right.top;
    if (left.Width() != right.Width()) return left.Width() < right.Width();
    return left.Height() < right.Height();
  });
  size_ = size;
  monitors_ = std::move(monitors);
  BuildEdges();
}

DesktopGeometry DesktopGeometry::Capture(Point2i* origin) {
  std::vector<RectI> monitors;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR, HDC, LPRECT bounds, LPARAM data) -> BOOL {
        reinterpret_cast<std::vector<RectI>*>(data)->push_back(RectI{bounds->left, bounds->top, bounds->right, bounds->bottom});
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&monitors));
  if (monitors.empty()) throw std::runtime_error("无法取得本机显示器区域");
  int left = monitors[0].left, top = monitors[0].top, right = monitors[0].right, bottom = monitors[0].bottom;
  for (const RectI& monitor : monitors) {
    left = std::min(left, monitor.left);
    top = std::min(top, monitor.top);
    right = std::max(right, monitor.right);
    bottom = std::max(bottom, monitor.bottom);
  }
  if (origin) *origin = Point2i{left, top};
  std::vector<RectI> relative;
  relative.reserve(monitors.size());
  for (const RectI& monitor : monitors) relative.push_back(RectI{monitor.left - left, monitor.top - top, monitor.right - left, monitor.bottom - top});
  return DesktopGeometry(Size2i{right - left, bottom - top}, std::move(relative));
}

bool DesktopGeometry::SameAs(const DesktopGeometry& other) const {
  return size_ == other.size_ && monitors_ == other.monitors_;
}

bool DesktopGeometry::Contains(Point2i point) const {
  for (const RectI& monitor : monitors_)
    if (monitor.Contains(point.x, point.y)) return true;
  return false;
}

bool DesktopGeometry::ContainsD(double x, double y) const {
  for (const RectI& monitor : monitors_)
    if (x >= monitor.left && x < monitor.right && y >= monitor.top && y < monitor.bottom) return true;
  return false;
}

std::pair<double, double> DesktopGeometry::ClampD(double x, double y) const {
  if (ContainsD(x, y)) return {x, y};
  double best = 1e308, resultX = 0, resultY = 0;
  for (const RectI& monitor : monitors_) {
    double candidateX = std::clamp(x, static_cast<double>(monitor.left), static_cast<double>(monitor.right - 1));
    double candidateY = std::clamp(y, static_cast<double>(monitor.top), static_cast<double>(monitor.bottom - 1));
    double distance = SquaredDistance(x, y, candidateX, candidateY);
    if (distance < best) {
      best = distance;
      resultX = candidateX;
      resultY = candidateY;
    }
  }
  return {resultX, resultY};
}

Point2i DesktopGeometry::ClampPixel(Point2i point) const {
  auto [x, y] = ClampD(point.x, point.y);
  return Point2i{static_cast<int>(x), static_cast<int>(y)};
}

Point2i DesktopGeometry::ClampToDesktop(Point2i point, Point2i origin) const {
  Point2i visible = ClampPixel(Point2i{point.x - origin.x, point.y - origin.y});
  return Point2i{origin.x + visible.x, origin.y + visible.y};
}

RectI DesktopGeometry::MonitorAt(Point2i point, Point2i origin) const {
  Point2i offset{point.x - origin.x, point.y - origin.y};
  for (const RectI& monitor : monitors_)
    if (monitor.Contains(offset.x, offset.y)) return monitor;
  throw std::out_of_range("光标不在任何显示器上");
}

Point2i DesktopGeometry::ParkingPoint(Point2i point, Point2i origin) const {
  RectI monitor = MonitorAt(point, origin);
  return Point2i{origin.x + monitor.left + (monitor.Width() - 1) / 2, origin.y + monitor.top + (monitor.Height() - 1) / 2};
}

Point2i DesktopGeometry::ToPixel(double x, double y) const {
  auto [clampedX, clampedY] = ClampD(x, y);
  // Math.Round 是四舍六入五取偶，nearbyint 在默认舍入模式下与之相同
  return ClampPixel(Point2i{static_cast<int>(std::nearbyint(clampedX)), static_cast<int>(std::nearbyint(clampedY))});
}

std::pair<double, double> DesktopGeometry::MapFrom(double x, double y, Size2i from) const {
  double mappedX = std::clamp(x / std::max(1, from.width - 1), 0.0, 1.0) * (size_.width - 1);
  double mappedY = std::clamp(y / std::max(1, from.height - 1), 0.0, 1.0) * (size_.height - 1);
  return ClampD(mappedX, mappedY);
}

std::pair<double, double> DesktopGeometry::Move(double x, double y, double deltaX, double deltaY, int& exitSide) const {
  std::tie(x, y) = ClampD(x, y);
  exitSide = -1;
  for (int pass = 0; pass < 2; pass++) {
    double fraction = 0;
    const EdgeSegment* hit = FirstExit(x, y, deltaX, deltaY, fraction);
    if (!hit) {
      std::tie(x, y) = ClampD(x + deltaX, y + deltaY);
      return {x, y};
    }
    if (exitSide < 0) exitSide = hit->side;
    x += deltaX * fraction;
    y += deltaY * fraction;
    deltaX *= 1 - fraction;
    deltaY *= 1 - fraction;
    if (IsVertical(hit->side)) {
      x = hit->coordinate - (hit->side == kRight ? 1 : 0);
      deltaX = 0;
    } else {
      y = hit->coordinate - (hit->side == kBottom ? 1 : 0);
      deltaY = 0;
    }
    std::tie(x, y) = ClampD(x, y);
  }
  std::tie(x, y) = ClampD(x + deltaX, y + deltaY);
  return {x, y};
}

int DesktopGeometry::GetEdgeSide(Point2i cursor, double deltaX, double deltaY) const {
  for (const EdgeSegment& edge : edges_) {
    if (!Outward(edge.side, deltaX, deltaY)) continue;
    int normal = IsVertical(edge.side) ? cursor.x : cursor.y;
    int tangent = IsVertical(edge.side) ? cursor.y : cursor.x;
    int inside = edge.coordinate - ((edge.side == kBottom || edge.side == kRight) ? 1 : 0);
    if (normal == inside && tangent >= edge.start && tangent < edge.end) return edge.side;
  }
  return -1;
}

std::pair<double, double> DesktopGeometry::EntryPoint(int exitSide, double x, double y, Size2i from) const {
  int entrySide = exitSide == kTop ? kBottom : exitSide == kBottom ? kTop : exitSide == kLeft ? kRight : exitSide == kRight ? kLeft : -1;
  if (entrySide < 0) throw std::invalid_argument("离开边非法");
  double mappedX = std::clamp(x / std::max(1, from.width - 1), 0.0, 1.0) * (size_.width - 1);
  double mappedY = std::clamp(y / std::max(1, from.height - 1), 0.0, 1.0) * (size_.height - 1);
  double requested = IsVertical(entrySide) ? mappedY : mappedX;
  double best = 1e308, bestOutside = 1e308, resultX = 0, resultY = 0;
  for (const EdgeSegment& edge : edges_) {
    if (edge.side != entrySide) continue;
    double tangent = std::clamp(requested, static_cast<double>(edge.start), static_cast<double>(edge.end - 1));
    double normal = edge.coordinate - ((entrySide == kBottom || entrySide == kRight) ? 1 : 0);
    double candidateX = IsVertical(entrySide) ? normal : tangent;
    double candidateY = IsVertical(entrySide) ? tangent : normal;
    double distance = std::abs(tangent - requested);
    // 同一切向距离时选最靠外的边段，避免跨过前方的显示器
    double outside = entrySide == kTop ? candidateY
                     : entrySide == kBottom ? size_.height - 1 - candidateY
                     : entrySide == kLeft ? candidateX
                                          : size_.width - 1 - candidateX;
    if (distance < best || (distance == best && outside < bestOutside)) {
      best = distance;
      bestOutside = outside;
      resultX = candidateX;
      resultY = candidateY;
    }
  }
  return ClampD(resultX, resultY);
}

const EdgeSegment* DesktopGeometry::FirstExit(double x, double y, double deltaX, double deltaY, double& fraction) const {
  fraction = 1e308;
  const EdgeSegment* result = nullptr;
  for (const EdgeSegment& edge : edges_) {
    if (!Outward(edge.side, deltaX, deltaY)) continue;
    bool vertical = IsVertical(edge.side);
    double amount = vertical ? deltaX : deltaY;
    double time = (edge.coordinate - (vertical ? x : y)) / amount;
    if (time < 0 || time > 1 || time >= fraction) continue;
    double tangent = vertical ? y + deltaY * time : x + deltaX * time;
    if (tangent < edge.start || tangent > edge.end) continue;
    // 角点可能只是与另一显示器相接；只把紧随交点进入不可见区域的轨迹视为越界
    double probe = 0.000001 / std::max(std::abs(deltaX), std::abs(deltaY));
    if (ContainsD(x + deltaX * (time + probe), y + deltaY * (time + probe))) continue;
    fraction = time;
    result = &edge;
  }
  return result;
}

void DesktopGeometry::BuildEdges() {
  edges_.clear();
  const int sides[4] = {kRight, kLeft, kBottom, kTop};
  for (const RectI& monitor : monitors_) {
    for (int side : sides) {
      bool vertical = IsVertical(side);
      int coordinate = side == kTop ? monitor.top : side == kBottom ? monitor.bottom : side == kLeft ? monitor.left : monitor.right;
      int outside = coordinate - ((side == kTop || side == kLeft) ? 1 : 0);
      std::vector<std::pair<int, int>> segments{{vertical ? monitor.top : monitor.left, vertical ? monitor.bottom : monitor.right}};
      for (const RectI& other : monitors_) {
        bool covered = vertical ? (outside >= other.left && outside < other.right) : (outside >= other.top && outside < other.bottom);
        if (!covered) continue;
        int start = vertical ? other.top : other.left;
        int end = vertical ? other.bottom : other.right;
        std::vector<std::pair<int, int>> remaining;
        for (auto [segmentStart, segmentEnd] : segments) {
          if (end <= segmentStart || start >= segmentEnd) {
            remaining.emplace_back(segmentStart, segmentEnd);
          } else {
            if (segmentStart < start) remaining.emplace_back(segmentStart, start);
            if (end < segmentEnd) remaining.emplace_back(end, segmentEnd);
          }
        }
        segments = std::move(remaining);
      }
      for (auto [start, end] : segments) edges_.push_back(EdgeSegment{side, coordinate, start, end});
    }
  }
}

}  // namespace bm
