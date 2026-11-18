#pragma once

#include <string>

#include "app/ui/Control.h"

namespace bm {

// 覆盖层对话框基类：整窗半透明遮罩 + 居中卡片 + 标题与说明 + 底部分隔线和按钮行。
// 显隐、尺寸跟随父控件、点遮罩只吞事件都在基类里；子类只写卡片内的排版与绘制。
class Dialog : public Control {
 public:
  virtual void Show();
  virtual void Hide();

  void Paint(PaintContext& ctx) final;
  void OnLayout() final;
  void OnTick() override;
  bool OnMouseDown(Point2i local, MouseButton button) override;

 protected:
  Dialog(std::wstring title, std::wstring description, int logicalWidth, int logicalHeight);

  void SetTitle(std::wstring title);
  void SetDescription(std::wstring description);
  int ViewDpi() const;
  RectI CardRect(int dpi) const;
  // 底部按钮行：右对齐，right 作为游标依次左移，间距固定 10 逻辑像素
  RectI FooterButton(int& right, int top, int width, int height, int dpi) const;
  int FooterTop(const RectI& card, int height, int dpi) const;

  // 卡片内排版与绘制；标题、说明与底部分隔线由基类处理
  virtual void LayoutContent(const RectI& card, int margin, int dpi) = 0;
  virtual void PaintContent(PaintContext&, const RectI&, int, int) {}

 private:
  std::wstring title_;
  std::wstring description_;
  int logicalWidth_ = 0;
  int logicalHeight_ = 0;
};

}  // namespace bm