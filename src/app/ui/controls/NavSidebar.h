#pragma once

#include <functional>
#include <string>

#include "app/ui/Control.h"

namespace bm {

// 左侧导航：品牌块 + 应用名、五个页面入口（矢量图标 + 选中态 + 徽章）与底部连接状态
class NavSidebar : public Control {
 public:
  enum class Section { Overview, Devices, Layout, Log, Settings };

  NavSidebar();

  Section Selected() const { return selected_; }
  void SetSelected(Section section);
  void SetStatus(std::wstring status, bool connected, std::wstring version, int connectedCount = 0);
  void SetPendingMerge(bool pending);

  std::function<void(Section)> onSectionSelected;

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseUp(Point2i local, MouseButton button) override;
  void OnMouseMove(Point2i local) override;
  void OnMouseLeave() override;
  // 品牌区比默认标题条更高，整块都能拖动窗口
  int CaptionStripHeight() const override;

 private:
  int SidebarDpi() const;
  RectI ItemBounds(int index, int dpi) const;
  int HitItem(Point2i local, int dpi) const;

  Section selected_ = Section::Overview;
  std::wstring status_ = L"未连接";
  std::wstring version_;
  bool connected_ = false;
  bool pendingMerge_ = false;
  int connectedCount_ = 0;
  int hover_ = -1;
  bool pressed_ = false;
};

}  // namespace bm
