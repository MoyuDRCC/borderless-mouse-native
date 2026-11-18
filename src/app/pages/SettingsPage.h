#pragma once

#include <string>
#include <utility>
#include <vector>

#include "app/pages/Page.h"
#include "app/ui/controls/Paint.h"
#include "app/UiModel.h"

namespace bm {

class FlatButton;
class FlatComboBox;
class ToggleSwitch;

// 设置页：固定页头 + 自绘细长滚动条的可滚动分组列表。
// 所有行都摆在一张画布控件上，滚轮与拖动滚动条只改变内容偏移。
class SettingsPage : public Page {
 public:
  SettingsPage(UiModel& model, UiActions& actions);

  void Paint(PaintContext& ctx) override;
  void OnLayout() override;
  void OnTick() override;
  void OnWheel(int delta, Point2i local) override;

  void Refresh(bool force);

 private:
  class Canvas;

  // 画布上的静态文字；bounds 是内容坐标（未减滚动偏移）
  struct Label {
    std::wstring text;
    RectI bounds;
    paint::TextStyle style = paint::TextStyle::Body;
    Gdiplus::Color color{255, 0, 0, 0};
    bool rightAligned = false;
  };
  int ContentWidth(int dpi) const;
  void Sync(bool force);
  void LayoutChildren();
  void ApplyScroll();
  int MaxScroll() const;
  bool NeedsScroll() const;
  RectI ThumbBounds(int dpi) const;
  RectI TrackHitRect(int dpi) const;
  void ScrollBy(int delta);
  bool BeginScrollDrag(Point2i local, MouseButton button);
  void DragScroll(Point2i local);
  void EndScrollDrag();
  void PaintCanvas(PaintContext& ctx);

  UiModel& model_;
  UiActions& actions_;

  Canvas* canvas_ = nullptr;
  ToggleSwitch* autoStart_ = nullptr;
  ToggleSwitch* hideCursor_ = nullptr;
  ToggleSwitch* clipboard_ = nullptr;
  FlatComboBox* mouseDpi_ = nullptr;
  FlatComboBox* limit_ = nullptr;
  FlatButton* export_ = nullptr;
  FlatButton* clear_ = nullptr;

  std::vector<std::pair<Control*, RectI>> placed_;  // 交互控件的内容坐标
  std::vector<Label> labels_;
  std::vector<std::pair<std::wstring, int>> sections_;
  std::vector<int> separators_;
  std::wstring version_;
  std::wstring protocolText_;
  int contentHeight_ = 0;
  int scroll_ = 0;
  bool updating_ = false;
  bool hoverScroll_ = false;
  bool draggingScroll_ = false;
  int dragStartY_ = 0;
  int dragStartScroll_ = 0;
};

}  // namespace bm
