#include "app/dialogs/DeviceSettingsDialog.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

#include "app/ui/Dpi.h"
#include "app/ui/controls/FlatButton.h"
#include "app/ui/controls/FlatComboBox.h"
#include "app/ui/controls/Paint.h"
#include "app/ui/controls/RoundedTextBox.h"
#include "app/ui/controls/ToggleSwitch.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

namespace {

// 0 表示跟随本机 DPI
constexpr int kDpiChoices[] = {0, 400, 800, 1200, 1600, 2400, 3200, 6400};
constexpr int kSpeedPercent[] = {50, 100, 200};

// 各行相对卡片顶部的逻辑像素
constexpr int kAliasTop = 92;
constexpr int kAliasHeight = 34;
constexpr int kDpiTop = 136;
constexpr int kRowHeight = 30;
constexpr int kClipboardTop = 184;
constexpr int kReverseTop = 224;
constexpr int kSpeedTop = 266;
constexpr int kComboWidth = 148;

int IndexOfValue(const int* choices, int count, int value) {
  for (int index = 0; index < count; ++index) {
    if (choices[index] == value) return index;
  }
  return 0;
}

}  // namespace

DeviceSettingsDialog::DeviceSettingsDialog() : Dialog(L"设备设置", std::wstring(), 432, 392) {
  auto alias = std::make_unique<RoundedTextBox>(L"留空则显示对端设备名称");
  alias_ = alias.get();
  AddControl(std::move(alias));

  auto dpi = std::make_unique<FlatComboBox>();
  dpi_ = dpi.get();
  {
    std::vector<std::wstring> items;
    items.reserve(std::size(kDpiChoices));
    for (const int choice : kDpiChoices) {
      items.push_back(choice == 0 ? L"跟随本机" : std::to_wstring(choice));
    }
    dpi_->AddItems(std::move(items));
  }
  AddControl(std::move(dpi));

  auto clipboard = std::make_unique<ToggleSwitch>();
  clipboard_ = clipboard.get();
  AddControl(std::move(clipboard));

  auto reverse = std::make_unique<ToggleSwitch>();
  reverse_ = reverse.get();
  AddControl(std::move(reverse));

  auto speed = std::make_unique<FlatComboBox>();
  speed_ = speed.get();
  speed_->AddItems(std::vector<std::wstring>{L"慢（50%）", L"标准（100%）", L"快（200%）"});
  AddControl(std::move(speed));

  auto confirm = std::make_unique<FlatButton>(L"保存", FlatButton::Style::Primary);
  confirm_ = confirm.get();
  confirm_->onClick = [this] { Accept(); };
  AddControl(std::move(confirm));

  auto cancel = std::make_unique<FlatButton>(L"取消", FlatButton::Style::Secondary);
  cancel_ = cancel.get();
  cancel_->onClick = [this] { Hide(); };
  AddControl(std::move(cancel));
}

void DeviceSettingsDialog::SetDevice(const std::wstring& displayName, const DeviceRow& device) {
  SetDescription(displayName);
  alias_->SetText(ToWide(device.alias));
  dpi_->SetSelectedIndex(IndexOfValue(kDpiChoices, static_cast<int>(std::size(kDpiChoices)), device.pointerDpi));
  clipboard_->SetChecked(device.clipboardEnabled);
  reverse_->SetChecked(device.wheelReverse);
  speed_->SetSelectedIndex(IndexOfValue(kSpeedPercent, static_cast<int>(std::size(kSpeedPercent)),
                                        device.wheelScalePercent));
}

void DeviceSettingsDialog::LayoutContent(const RectI& card, int margin, int dpi) {
  const int right = card.right - margin;
  const int comboWidth = Dpi::Scale(kComboWidth, dpi);
  const Size2i toggle = ToggleSwitch::LogicalSize();
  const int toggleWidth = Dpi::Scale(toggle.width, dpi);
  const int toggleHeight = Dpi::Scale(toggle.height, dpi);
  const int rowHeight = Dpi::Scale(kRowHeight, dpi);

  alias_->SetBounds(RectI{card.left + margin, card.top + Dpi::Scale(kAliasTop, dpi), right,
                          card.top + Dpi::Scale(kAliasTop + kAliasHeight, dpi)});
  const int dpiTop = card.top + Dpi::Scale(kDpiTop, dpi);
  dpi_->SetBounds(RectI{right - comboWidth, dpiTop, right, dpiTop + rowHeight});

  const int clipboardTop = card.top + Dpi::Scale(kClipboardTop, dpi);
  clipboard_->SetBounds(RectI{right - toggleWidth, clipboardTop, right, clipboardTop + toggleHeight});
  const int reverseTop = card.top + Dpi::Scale(kReverseTop, dpi);
  reverse_->SetBounds(RectI{right - toggleWidth, reverseTop, right, reverseTop + toggleHeight});

  const int speedTop = card.top + Dpi::Scale(kSpeedTop, dpi);
  speed_->SetBounds(RectI{right - comboWidth, speedTop, right, speedTop + rowHeight});

  int buttonRight = right;
  const int height = Dpi::Scale(32, dpi);
  const int top = FooterTop(card, height, dpi);
  confirm_->SetBounds(FooterButton(buttonRight, top, Dpi::Scale(88, dpi), height, dpi));
  cancel_->SetBounds(FooterButton(buttonRight, top, Dpi::Scale(88, dpi), height, dpi));
}

void DeviceSettingsDialog::PaintContent(PaintContext& ctx, const RectI& card, int margin, int dpi) {
  const int left = card.left + margin;
  const int right = card.right - margin;
  auto label = [&](const wchar_t* text, int top, int height, paint::TextStyle style) {
    paint::TextIn(ctx, text, style, RectI{left, card.top + Dpi::Scale(top, dpi), right,
                                          card.top + Dpi::Scale(top + height, dpi)},
                  style == paint::TextStyle::Caption ? Theme::TextSecondary : Theme::Text);
  };
  auto separator = [&](int top) {
    paint::Separator(ctx, left, right, card.top + Dpi::Scale(top, dpi));
  };

  label(L"显示别名", 68, 20, paint::TextStyle::Caption);
  label(L"鼠标 DPI", kDpiTop, kRowHeight, paint::TextStyle::Body);
  separator(176);
  label(L"剪贴板同步", kClipboardTop + 2, 22, paint::TextStyle::Body);
  separator(216);
  label(L"滚轮方向反转", kReverseTop + 2, 22, paint::TextStyle::Body);
  separator(256);
  label(L"滚轮速度", kSpeedTop, kRowHeight, paint::TextStyle::Body);
  paint::TextIn(ctx, L"DPI 用于与对端换算指针速度；滚轮与剪贴板设置只对本机生效。", paint::TextStyle::Caption,
                RectI{left, card.top + Dpi::Scale(306, dpi), right, card.top + Dpi::Scale(326, dpi)},
                Theme::TextTertiary);
}

void DeviceSettingsDialog::Accept() {
  DeviceSettingsValues values;
  values.alias = alias_->Text();
  values.pointerDpi = kDpiChoices[std::max(0, dpi_->SelectedIndex())];
  values.clipboard = clipboard_->Checked();
  values.wheelReverse = reverse_->Checked();
  values.wheelScalePercent = kSpeedPercent[std::max(0, speed_->SelectedIndex())];

  Hide();
  if (onAccepted) onAccepted(values);
}

}  // namespace bm
