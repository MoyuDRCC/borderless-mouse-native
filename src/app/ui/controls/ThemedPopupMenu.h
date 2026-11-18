#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app/ui/Control.h"

namespace bm {

class Window;

// 现代扁平弹出菜单：设备行“···”菜单、下拉列表与日志右键菜单共用。
// 作为窗口覆盖层绘制（不额外创建窗口），点外部、Esc 或焦点丢失即收起。
class ThemedPopupMenu : public Control {
 public:
  struct Entry {
    std::wstring text;
    bool isSeparator = false;
    bool isDanger = false;
    bool isChecked = false;
    bool isEnabled = true;
    std::function<void()> onClick;
  };

  ThemedPopupMenu(std::vector<Entry> entries, int minLogicalWidth = 148, bool reserveCheckColumn = false);

  // 计算菜单尺寸并记录每一项的矩形；返回逻辑尺寸换算后的物理尺寸
  Size2i ContentSize(int dpi);

  // 弹出：anchor 为窗口客户区坐标下的菜单左上角
  static void Show(Window* host, Point2i anchor, std::vector<Entry> entries, int minLogicalWidth = 148,
                   bool reserveCheckColumn = false);

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseUp(Point2i local, MouseButton button) override;
  void OnMouseMove(Point2i local) override;
  void OnMouseLeave() override;
  void OnKey(int vk, bool down) override;

 private:
  int HitTest(Point2i local) const;
  void Activate(int index);

  std::vector<Entry> entries_;
  std::vector<RectI> rects_;
  int minLogicalWidth_;
  bool reserveCheckColumn_;
  int hover_ = -1;
};

}  // namespace bm
