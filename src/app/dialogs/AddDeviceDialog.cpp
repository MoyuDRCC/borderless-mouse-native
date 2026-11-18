#include "app/dialogs/AddDeviceDialog.h"

#include <memory>
#include <utility>

#include "app/ui/controls/FlatButton.h"
#include "app/ui/controls/Paint.h"
#include "app/ui/controls/RoundedTextBox.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

namespace {

// 两行输入框的位置（相对卡片顶部的逻辑像素）
constexpr int kFieldTop = 78;
constexpr int kFieldHeight = 36;
constexpr int kFieldGap = 16;
constexpr int kLabelWidth = 76;

}  // namespace

AddDeviceDialog::AddDeviceDialog()
    : Dialog(L"添加设备", L"输入对端电脑的局域网 IP，服务端口固定为 35100。", 400, 248) {
  auto name = std::make_unique<RoundedTextBox>(L"例如：客厅台式机（可选）");
  name_ = name.get();
  AddControl(std::move(name));

  auto address = std::make_unique<RoundedTextBox>(L"例如：192.168.1.25");
  address_ = address.get();
  address_->onEnter = [this] { Accept(); };
  AddControl(std::move(address));

  auto confirm = std::make_unique<FlatButton>(L"添加", FlatButton::Style::Primary);
  confirm_ = confirm.get();
  confirm_->onClick = [this] { Accept(); };
  AddControl(std::move(confirm));

  auto cancel = std::make_unique<FlatButton>(L"取消", FlatButton::Style::Secondary);
  cancel_ = cancel.get();
  cancel_->onClick = [this] { Hide(); };
  AddControl(std::move(cancel));
}

void AddDeviceDialog::Show() {
  name_->SetText(std::wstring());
  address_->SetText(std::wstring());
  Dialog::Show();
  if (Host()) Host()->FocusControl(address_);
}

void AddDeviceDialog::LayoutContent(const RectI& card, int margin, int dpi) {
  const int fieldLeft = card.left + margin + Dpi::Scale(kLabelWidth, dpi);
  const int fieldRight = card.right - margin;
  name_->SetBounds(RectI{fieldLeft, card.top + Dpi::Scale(kFieldTop, dpi), fieldRight,
                         card.top + Dpi::Scale(kFieldTop + kFieldHeight, dpi)});
  const int addressTop = card.top + Dpi::Scale(kFieldTop + kFieldHeight + kFieldGap, dpi);
  address_->SetBounds(RectI{fieldLeft, addressTop, fieldRight, addressTop + Dpi::Scale(kFieldHeight, dpi)});

  int right = fieldRight;
  const int height = Dpi::Scale(32, dpi);
  const int top = FooterTop(card, height, dpi);
  confirm_->SetBounds(FooterButton(right, top, Dpi::Scale(88, dpi), height, dpi));
  cancel_->SetBounds(FooterButton(right, top, Dpi::Scale(88, dpi), height, dpi));
}

void AddDeviceDialog::PaintContent(PaintContext& ctx, const RectI& card, int margin, int dpi) {
  const int fieldLeft = card.left + margin + Dpi::Scale(kLabelWidth, dpi);
  const int labelRight = fieldLeft - Dpi::Scale(3, dpi);
  paint::TextIn(ctx, L"设备名", paint::TextStyle::Body,
                RectI{card.left + margin, card.top + Dpi::Scale(kFieldTop, dpi), labelRight,
                      card.top + Dpi::Scale(kFieldTop + kFieldHeight, dpi)},
                Theme::TextSecondary);
  const int addressTop = card.top + Dpi::Scale(kFieldTop + kFieldHeight + kFieldGap, dpi);
  paint::TextIn(ctx, L"IP 地址", paint::TextStyle::Body,
                RectI{card.left + margin, addressTop, labelRight, addressTop + Dpi::Scale(kFieldHeight, dpi)},
                Theme::TextSecondary);
}

void AddDeviceDialog::Accept() {
  const std::wstring address = address_->Text();
  if (address.empty()) {
    address_->FocusInput();
    return;
  }
  const std::wstring name = name_->Text().empty() ? address : name_->Text();
  Hide();
  if (onAccepted) onAccepted(name, address);
}

}  // namespace bm
