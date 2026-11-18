#pragma once

#include <windows.h>

#include <memory>
#include <vector>

#include "app/ui/Drawing.h"
#include "app/ui/Dpi.h"
#include "core/input/InputEvent.h"

namespace bm {

class Window;

// 控件基类：矩形布局、父子关系、可见性、自绘、鼠标/滚轮/键盘事件与悬停状态。
// 坐标约定：Bounds() 是父控件坐标系；Paint/OnMouseX 收到的是本控件局部坐标。
class Control {
 public:
  Control() = default;
  virtual ~Control() = default;
  Control(const Control&) = delete;
  Control& operator=(const Control&) = delete;

  void SetBounds(const RectI& bounds);
  const RectI& Bounds() const { return bounds_; }
  RectI LocalBounds() const { return RectI{0, 0, bounds_.Width(), bounds_.Height()}; }

  void AddControl(std::unique_ptr<Control> control);
  void SetVisible(bool visible);
  bool Visible() const { return visible_; }
  void SetEnabled(bool enabled);
  bool Enabled() const { return enabled_; }
  void SetCursor(HCURSOR cursor) { cursor_ = cursor; }
  HCURSOR Cursor() const { return cursor_; }
  bool Hover() const { return hover_; }

  void Invalidate();

  Control* Parent() const { return parent_; }
  Window* Host() const { return host_; }
  Point2i AbsoluteOrigin() const;

  // 子类覆写点
  virtual void Paint(PaintContext& ctx);
  virtual void OnLayout() {}
  // 返回 true 表示已处理；false 时事件沿父链上浮
  virtual bool OnMouseDown(Point2i local, MouseButton button);
  virtual void OnMouseMove(Point2i local);
  virtual void OnMouseUp(Point2i local, MouseButton button);
  virtual void OnMouseLeave();
  virtual void OnWheel(int delta, Point2i local);
  virtual void OnKey(int vk, bool down);
  virtual void OnChar(wchar_t ch);
  virtual void OnTick() {}
  // 顶部标题条：本控件顶部多少逻辑像素仍交给窗口拖动，0 表示沿用窗口默认高度
  virtual int CaptionStripHeight() const { return 0; }
  // 标题条区域内是否仍接收鼠标：自绘的最小化/最大化/关闭按钮需要
  virtual bool CapturesCaption() const { return false; }

  // 框架分发入口：绘制树、定时、命中测试、窗口挂接
  void PaintTree(PaintContext& ctx);
  void TickTree();
  Control* HitTest(Point2i local);
  void AttachHost(Window* host);
  void SetHoverState(bool hover) { hover_ = hover; }

 private:
  RectI bounds_{};
  bool visible_ = true;
  bool enabled_ = true;
  bool hover_ = false;
  HCURSOR cursor_ = nullptr;
  Control* parent_ = nullptr;
  Window* host_ = nullptr;
  std::vector<std::unique_ptr<Control>> children_;
};

}  // namespace bm
