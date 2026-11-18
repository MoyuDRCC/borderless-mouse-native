#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app/ui/Control.h"

namespace bm {

// 自绘圆角下拉选择框：取代生硬的原生 ComboBox，下拉列表复用 ThemedPopupMenu（带勾选列）
class FlatComboBox : public Control {
 public:
  FlatComboBox();

  void AddItems(std::vector<std::wstring> items);
  const std::vector<std::wstring>& Items() const { return items_; }
  int SelectedIndex() const { return selected_; }
  void SetSelectedIndex(int index);
  const std::wstring& SelectedText() const;

  std::function<void(int)> onSelectedIndexChanged;

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseMove(Point2i local) override;
  void OnMouseLeave() override;

 private:
  void OpenDropDown();

  std::vector<std::wstring> items_;
  int selected_ = -1;
};

}  // namespace bm
