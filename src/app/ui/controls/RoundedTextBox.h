#pragma once

#include <functional>
#include <string>

#include "app/ui/Control.h"

namespace bm {

// 圆角输入框：自绘底框与焦点高亮，文字与光标全部自绘（单行编辑）
class RoundedTextBox : public Control {
 public:
  explicit RoundedTextBox(std::wstring placeholder = std::wstring());

  const std::wstring& Text() const { return text_; }
  void SetText(std::wstring text);
  void SetPlaceholder(std::wstring placeholder);

  std::function<void()> onTextChanged;
  std::function<void()> onEnter;

  // 申请键盘焦点（宿主窗口记录焦点控件）
  void FocusInput();
  bool Focused() const;

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseMove(Point2i local) override;
  void OnMouseLeave() override;
  void OnKey(int vk, bool down) override;
  void OnChar(wchar_t ch) override;
  void OnTick() override;

 private:
  void Insert(std::wstring_view text);
  void NotifyChanged();

  std::wstring text_;
  std::wstring placeholder_;
  size_t caret_ = 0;
  bool caretVisible_ = true;
  bool hover_ = false;
};

}  // namespace bm
