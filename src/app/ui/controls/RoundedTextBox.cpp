#include "app/ui/controls/RoundedTextBox.h"

#include <algorithm>
#include <utility>

#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

namespace {

// 读取剪贴板文本：占用时静默返回空串，和 .NET 侧的处理一致
std::wstring ReadClipboardText() {
  std::wstring text;
  if (!OpenClipboard(nullptr)) return text;
  if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
    if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(handle))) {
      text.assign(chars);
      GlobalUnlock(handle);
    }
  }
  CloseClipboard();
  return text;
}

}  // namespace

RoundedTextBox::RoundedTextBox(std::wstring placeholder) : placeholder_(std::move(placeholder)) {
  SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
}

void RoundedTextBox::SetText(std::wstring text) {
  text_ = std::move(text);
  caret_ = text_.size();
  caretVisible_ = true;
  Invalidate();
}

void RoundedTextBox::SetPlaceholder(std::wstring placeholder) {
  placeholder_ = std::move(placeholder);
  Invalidate();
}

bool RoundedTextBox::Focused() const {
  const Window* host = Host();
  return host && host->FocusedControl() == this;
}

void RoundedTextBox::FocusInput() {
  if (Window* host = Host()) host->FocusControl(this);
  caretVisible_ = true;
}

void RoundedTextBox::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int width = Bounds().Width();
  const int height = Bounds().Height();
  const bool focused = Focused();

  const Gdiplus::Color fill = focused ? Theme::Window : Theme::SurfaceSubtle;
  const Gdiplus::Color border = focused ? Theme::Accent : (hover_ ? Theme::BorderStrong : Theme::Border);
  const float borderWidth = focused ? std::max(1.5f, Dpi::ScaleF(1.5f, dpi))
                                    : static_cast<float>(std::max(1, Dpi::Scale(1, dpi)));
  draw::RoundRect(ctx.graphics, RectI{0, 0, width - 1, height - 1},
                  Dpi::Scale(Theme::RadiusControl, dpi), fill, border, borderWidth);

  const HFONT font = Dpi::Font(Theme::BodyPx, false, dpi);
  const int padX = Dpi::Scale(11, dpi);
  const RectI textRect{padX, 0, std::max(padX, width - padX), height};

  if (text_.empty()) {
    draw::TextWithin(ctx, placeholder_, font, textRect, Theme::TextTertiary);
  } else {
    draw::TextWithin(ctx, text_, font, textRect, Theme::Text);
  }

  if (!focused || !caretVisible_) return;

  // 光标：文本末尾或插入点前画 1px 竖线
  const std::wstring prefix = text_.substr(0, std::min(caret_, text_.size()));
  const int caretX = padX + draw::Measure(prefix, font).width;
  const int caretHeight = draw::Measure(L"Ag", font).height;
  const int caretTop = (height - caretHeight) / 2;
  const int caretWidth = std::max(1, Dpi::Scale(1, dpi));
  draw::FillRect(ctx.graphics, RectI{caretX, caretTop, caretX + caretWidth, caretTop + caretHeight},
                 Theme::Text);
}

bool RoundedTextBox::OnMouseDown(Point2i, MouseButton button) {
  if (button != MouseButton::Left) return false;
  FocusInput();
  return true;
}

void RoundedTextBox::OnMouseMove(Point2i) {
  if (hover_) return;
  hover_ = true;
  Invalidate();
}

void RoundedTextBox::OnMouseLeave() {
  if (!hover_) return;
  hover_ = false;
  Invalidate();
}

void RoundedTextBox::OnTick() {
  if (!Focused()) {
    caretVisible_ = true;
    return;
  }
  caretVisible_ = !caretVisible_;
  Invalidate();
}

void RoundedTextBox::OnChar(wchar_t ch) {
  if (ch < 0x20 || ch == 0x7F) return;  // 控制字符交给 OnKey
  const wchar_t buffer[2] = {ch, L'\0'};
  Insert(buffer);
}

void RoundedTextBox::Insert(std::wstring_view text) {
  if (text.empty()) return;
  caret_ = std::min(caret_, text_.size());
  text_.insert(caret_, text);
  caret_ += text.size();
  caretVisible_ = true;
  Invalidate();
  NotifyChanged();
}

void RoundedTextBox::NotifyChanged() {
  if (onTextChanged) onTextChanged();
}

void RoundedTextBox::OnKey(int vk, bool down) {
  if (!down) return;

  if (vk == VK_LEFT) {
    if (caret_ > 0) --caret_;
  } else if (vk == VK_RIGHT) {
    if (caret_ < text_.size()) ++caret_;
  } else if (vk == VK_HOME) {
    caret_ = 0;
  } else if (vk == VK_END) {
    caret_ = text_.size();
  } else if (vk == VK_BACK) {
    if (caret_ == 0) return;
    text_.erase(caret_ - 1, 1);
    --caret_;
    NotifyChanged();
  } else if (vk == VK_DELETE) {
    if (caret_ >= text_.size()) return;
    text_.erase(caret_, 1);
    NotifyChanged();
  } else if (vk == VK_RETURN) {
    if (onEnter) onEnter();
    return;
  } else if (vk == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
    Insert(ReadClipboardText());
  } else {
    return;
  }

  caretVisible_ = true;
  Invalidate();
}

}  // namespace bm
