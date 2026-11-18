#pragma once

#include <functional>
#include <string>

#include "app/ui/Control.h"

namespace bm {

// 现代扁平按钮：6px 圆角，Primary / Secondary / Ghost / Danger，带悬停、按下与选中态
class FlatButton : public Control {
 public:
  enum class Style { Primary, Secondary, Ghost, Danger };

  explicit FlatButton(std::wstring text, Style style = Style::Secondary);

  void SetText(std::wstring text);
  const std::wstring& Text() const { return text_; }
  void SetStyle(Style style);
  Style Kind() const { return style_; }
  // 选中态：浅蓝底、强调色描边与文字
  void SetActive(bool active);
  bool Active() const { return active_; }

  std::function<void()> onClick;

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseUp(Point2i local, MouseButton button) override;
  void OnMouseLeave() override;

 private:
  std::wstring text_;
  Style style_;
  bool active_ = false;
  bool pressed_ = false;
};

}  // namespace bm
