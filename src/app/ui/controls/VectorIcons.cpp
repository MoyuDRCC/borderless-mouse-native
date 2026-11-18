#include "app/ui/controls/VectorIcons.h"

#include <algorithm>

namespace bm::VectorIcons {

namespace {

float StrokeWidth(int dpi, float logical) { return std::max(1.25f, Dpi::ScaleF(logical, dpi)); }

void SetRoundCaps(Gdiplus::Pen& pen) {
  pen.SetStartCap(Gdiplus::LineCapRound);
  pen.SetEndCap(Gdiplus::LineCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
}

void DrawRoundOutline(Gdiplus::Graphics& graphics, const Gdiplus::Pen& pen, float x, float y, float width,
                      float height, float radius) {
  const float d = std::min(radius * 2.0f, std::min(width, height));
  Gdiplus::GraphicsPath path;
  path.AddArc(x, y, d, d, 180, 90);
  path.AddArc(x + width - d, y, d, d, 270, 90);
  path.AddArc(x + width - d, y + height - d, d, d, 0, 90);
  path.AddArc(x, y + height - d, d, d, 90, 90);
  path.CloseFigure();
  graphics.DrawPath(&pen, &path);
}

// 显示器图标：设备列表、空状态与导航共用同一个形状
void DrawMonitorShape(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi) {
  const float s = static_cast<float>(bounds.Width()) / 16.0f;
  const float x = static_cast<float>(bounds.left);
  const float y = static_cast<float>(bounds.top);
  Gdiplus::Pen pen(color, StrokeWidth(dpi, 1.4f));
  SetRoundCaps(pen);
  DrawRoundOutline(graphics, pen, x + (1.8f * s), y + (2.5f * s), 12.4f * s, 8.2f * s, 1.5f * s);
  graphics.DrawLine(&pen, x + (8.0f * s), y + (10.7f * s), x + (8.0f * s), y + (13.3f * s));
  graphics.DrawLine(&pen, x + (5.0f * s), y + (13.5f * s), x + (11.0f * s), y + (13.5f * s));
}

}  // namespace

void DrawMonitor(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi) {
  DrawMonitorShape(graphics, bounds, color, dpi);
}

void DrawNav(Gdiplus::Graphics& graphics, NavSidebar::Section section, const RectI& bounds,
             const Gdiplus::Color& color, int dpi) {
  const float s = static_cast<float>(bounds.Width()) / 16.0f;
  const float x = static_cast<float>(bounds.left);
  const float y = static_cast<float>(bounds.top);

  if (section == NavSidebar::Section::Devices) {
    DrawMonitorShape(graphics, bounds, color, dpi);
    return;
  }

  Gdiplus::Pen pen(color, StrokeWidth(dpi, 1.45f));
  SetRoundCaps(pen);

  switch (section) {
    case NavSidebar::Section::Overview:
      // 仪表盘四宫格：左上大块，体现状态概览
      DrawRoundOutline(graphics, pen, x + (2.0f * s), y + (2.0f * s), 5.2f * s, 6.2f * s, 1.3f * s);
      DrawRoundOutline(graphics, pen, x + (8.8f * s), y + (2.0f * s), 5.2f * s, 4.2f * s, 1.3f * s);
      DrawRoundOutline(graphics, pen, x + (8.8f * s), y + (7.8f * s), 5.2f * s, 6.2f * s, 1.3f * s);
      DrawRoundOutline(graphics, pen, x + (2.0f * s), y + (9.8f * s), 5.2f * s, 4.2f * s, 1.3f * s);
      break;

    case NavSidebar::Section::Layout:
      // 双屏联动布局
      DrawRoundOutline(graphics, pen, x + (1.6f * s), y + (3.2f * s), 5.4f * s, 9.6f * s, 1.2f * s);
      DrawRoundOutline(graphics, pen, x + (9.0f * s), y + (3.2f * s), 5.4f * s, 9.6f * s, 1.2f * s);
      graphics.DrawLine(&pen, x + (7.0f * s), y + (8.0f * s), x + (9.0f * s), y + (8.0f * s));
      break;

    case NavSidebar::Section::Log:
      // 日志文档行
      DrawRoundOutline(graphics, pen, x + (2.2f * s), y + (2.0f * s), 11.6f * s, 12.0f * s, 1.5f * s);
      graphics.DrawLine(&pen, x + (4.8f * s), y + (5.5f * s), x + (11.2f * s), y + (5.5f * s));
      graphics.DrawLine(&pen, x + (4.8f * s), y + (8.2f * s), x + (11.2f * s), y + (8.2f * s));
      graphics.DrawLine(&pen, x + (4.8f * s), y + (10.9f * s), x + (8.8f * s), y + (10.9f * s));
      break;

    case NavSidebar::Section::Settings:
      // 三条水平滑块，留空圆心以适配任意底色
      graphics.DrawLine(&pen, x + (2.0f * s), y + (4.2f * s), x + (4.2f * s), y + (4.2f * s));
      graphics.DrawLine(&pen, x + (7.0f * s), y + (4.2f * s), x + (14.0f * s), y + (4.2f * s));
      graphics.DrawEllipse(&pen, x + (4.2f * s), y + (2.8f * s), 2.8f * s, 2.8f * s);

      graphics.DrawLine(&pen, x + (2.0f * s), y + (8.0f * s), x + (9.2f * s), y + (8.0f * s));
      graphics.DrawLine(&pen, x + (12.0f * s), y + (8.0f * s), x + (14.0f * s), y + (8.0f * s));
      graphics.DrawEllipse(&pen, x + (9.2f * s), y + (6.6f * s), 2.8f * s, 2.8f * s);

      graphics.DrawLine(&pen, x + (2.0f * s), y + (11.8f * s), x + (5.4f * s), y + (11.8f * s));
      graphics.DrawLine(&pen, x + (8.2f * s), y + (11.8f * s), x + (14.0f * s), y + (11.8f * s));
      graphics.DrawEllipse(&pen, x + (5.4f * s), y + (10.4f * s), 2.8f * s, 2.8f * s);
      break;

    default:
      break;
  }
}

void DrawMoreDots(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi) {
  const float r = std::max(1.5f, Dpi::ScaleF(1.5f, dpi));
  const float cx = static_cast<float>(bounds.left) + (static_cast<float>(bounds.Width()) / 2.0f);
  const float cy = static_cast<float>(bounds.top) + (static_cast<float>(bounds.Height()) / 2.0f);
  const float gap = Dpi::ScaleF(4.5f, dpi);
  Gdiplus::SolidBrush brush(color);
  graphics.FillEllipse(&brush, cx - gap - r, cy - r, r * 2.0f, r * 2.0f);
  graphics.FillEllipse(&brush, cx - r, cy - r, r * 2.0f, r * 2.0f);
  graphics.FillEllipse(&brush, cx + gap - r, cy - r, r * 2.0f, r * 2.0f);
}

void DrawChevronDown(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi) {
  const float cx = static_cast<float>(bounds.left) + (static_cast<float>(bounds.Width()) / 2.0f);
  const float cy = static_cast<float>(bounds.top) + (static_cast<float>(bounds.Height()) / 2.0f);
  const float w = Dpi::ScaleF(4.0f, dpi);
  const float h = Dpi::ScaleF(2.5f, dpi);
  Gdiplus::Pen pen(color, StrokeWidth(dpi, 1.4f));
  SetRoundCaps(pen);
  const Gdiplus::PointF points[3] = {{cx - w, cy - h}, {cx, cy + h}, {cx + w, cy - h}};
  graphics.DrawLines(&pen, points, 3);
}

void DrawCheck(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi) {
  const float cx = static_cast<float>(bounds.left) + (static_cast<float>(bounds.Width()) / 2.0f);
  const float cy = static_cast<float>(bounds.top) + (static_cast<float>(bounds.Height()) / 2.0f);
  const float s = static_cast<float>(bounds.Width()) / 16.0f;
  Gdiplus::Pen pen(color, StrokeWidth(dpi, 1.6f));
  SetRoundCaps(pen);
  const Gdiplus::PointF points[3] = {
      {cx - (4.0f * s), cy + (0.5f * s)}, {cx - (1.0f * s), cy + (3.5f * s)}, {cx + (4.5f * s), cy - (3.0f * s)}};
  graphics.DrawLines(&pen, points, 3);
}

void DrawCopy(Gdiplus::Graphics& graphics, const RectI& bounds, const Gdiplus::Color& color, int dpi) {
  const float s = static_cast<float>(bounds.Width()) / 16.0f;
  const float x = static_cast<float>(bounds.left);
  const float y = static_cast<float>(bounds.top);
  Gdiplus::Pen pen(color, StrokeWidth(dpi, 1.25f));
  SetRoundCaps(pen);
  DrawRoundOutline(graphics, pen, x + (5.5f * s), y + (5.5f * s), 8.0f * s, 8.0f * s, 1.2f * s);
  graphics.DrawLine(&pen, x + (3.0f * s), y + (10.0f * s), x + (3.0f * s), y + (3.8f * s));
  graphics.DrawArc(&pen, x + (3.0f * s), y + (2.5f * s), 2.5f * s, 2.5f * s, 180, 90);
  graphics.DrawLine(&pen, x + (4.2f * s), y + (2.5f * s), x + (10.5f * s), y + (2.5f * s));
}

}  // namespace bm::VectorIcons
