#include "app/dialogs/ConfirmDialog.h"

#include <memory>
#include <utility>

#include "app/ui/controls/FlatButton.h"
#include "app/ui/Dpi.h"

namespace bm {

ConfirmDialog::ConfirmDialog() : Dialog(std::wstring(), std::wstring(), 400, 168) {
  auto confirm = std::make_unique<FlatButton>(L"确认", FlatButton::Style::Primary);
  confirm_ = confirm.get();
  confirm_->onClick = [this] {
    Hide();
    if (onConfirmed) onConfirmed();
  };
  AddControl(std::move(confirm));

  auto cancel = std::make_unique<FlatButton>(L"取消", FlatButton::Style::Secondary);
  cancel_ = cancel.get();
  cancel_->onClick = [this] { Hide(); };
  AddControl(std::move(cancel));
}

void ConfirmDialog::Ask(std::wstring title, std::wstring description) {
  SetTitle(std::move(title));
  SetDescription(std::move(description));
  Show();
}

void ConfirmDialog::LayoutContent(const RectI& card, int margin, int dpi) {
  int right = card.right - margin;
  const int buttonHeight = Dpi::Scale(34, dpi);
  const int buttonTop = FooterTop(card, buttonHeight, dpi);
  const int buttonWidth = Dpi::Scale(104, dpi);
  confirm_->SetBounds(FooterButton(right, buttonTop, buttonWidth, buttonHeight, dpi));
  cancel_->SetBounds(FooterButton(right, buttonTop, buttonWidth, buttonHeight, dpi));
}

}  // namespace bm
