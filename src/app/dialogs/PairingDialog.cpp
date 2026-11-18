#include "app/dialogs/PairingDialog.h"

#include <memory>
#include <utility>

#include "app/ui/Dpi.h"
#include "app/ui/controls/FlatButton.h"
#include "app/ui/controls/Paint.h"
#include "app/ui/Drawing.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

namespace {

// 指纹按固定长度折行，两行足够展示 64 位十六进制
constexpr size_t kFingerprintChunk = 32;

// 配对码逐字加空格，读起来不容易串位
std::wstring SpaceOut(const std::wstring& text) {
  std::wstring spaced;
  spaced.reserve(text.size() * 2);
  for (size_t index = 0; index < text.size(); ++index) {
    if (index > 0) spaced.push_back(L' ');
    spaced.push_back(text[index]);
  }
  return spaced;
}

}  // namespace

PairingDialog::PairingDialog() : Dialog(std::wstring(), std::wstring(), 520, 326) {
  auto allow = std::make_unique<FlatButton>(L"一致，允许配对", FlatButton::Style::Primary);
  allow_ = allow.get();
  allow_->onClick = [this] { Resolve(true); };
  AddControl(std::move(allow));

  auto cancel = std::make_unique<FlatButton>(L"取消", FlatButton::Style::Secondary);
  cancel_ = cancel.get();
  cancel_->onClick = [this] { Resolve(false); };
  AddControl(std::move(cancel));
}

void PairingDialog::SetRequest(const std::string& peerName, const std::string& code,
                               const std::string& fingerprint) {
  const std::wstring name = ToWide(peerName.empty() ? std::string("对端设备") : peerName);
  SetTitle(L"正在与 " + name + L" 建立信任配对");
  code_ = ToWide(code);
  fingerprint_ = ToWide(fingerprint);
}

void PairingDialog::LayoutContent(const RectI& card, int margin, int dpi) {
  int right = card.right - margin;
  const int height = Dpi::Scale(34, dpi);
  const int top = FooterTop(card, height, dpi);
  cancel_->SetBounds(FooterButton(right, top, Dpi::Scale(88, dpi), height, dpi));
  allow_->SetBounds(FooterButton(right, top, Dpi::Scale(138, dpi), height, dpi));
}

void PairingDialog::PaintContent(PaintContext& ctx, const RectI& card, int margin, int dpi) {
  const int left = card.left + margin;
  const int right = card.right - margin;

  paint::TextIn(ctx, L"请查看另一台电脑屏幕，确认双方显示的 6 位配对码完全一致：", paint::TextStyle::Body,
                RectI{left, card.top + Dpi::Scale(44, dpi), right, card.top + Dpi::Scale(66, dpi)},
                Theme::TextSecondary);

  // 配对码展示区：柔和浅蓝块 + 等宽大字
  const RectI codeBox{left, card.top + Dpi::Scale(78, dpi), right, card.top + Dpi::Scale(152, dpi)};
  draw::FillRound(ctx.graphics, codeBox, Dpi::Scale(Theme::RadiusSurface, dpi), Theme::AccentSoft);
  draw::TextCentered(ctx, SpaceOut(code_), Dpi::MonoFont(28.0f, true, dpi), codeBox, Theme::Accent);

  paint::TextIn(ctx, L"对方公钥指纹", paint::TextStyle::Caption,
                RectI{left, card.top + Dpi::Scale(166, dpi), right, card.top + Dpi::Scale(186, dpi)},
                Theme::TextSecondary);

  const RectI fingerprintBox{left, card.top + Dpi::Scale(188, dpi), right, card.top + Dpi::Scale(242, dpi)};
  draw::FillRound(ctx.graphics, fingerprintBox, Dpi::Scale(Theme::RadiusControl, dpi), Theme::SurfaceSubtle);

  // 指纹折成两行等宽小字
  const HFONT monoFont = Dpi::MonoFont(Theme::CaptionPx, false, dpi);
  for (int line = 0; line < 2; ++line) {
    const size_t start = static_cast<size_t>(line) * kFingerprintChunk;
    if (start >= fingerprint_.size()) break;
    const std::wstring chunk = fingerprint_.substr(start, kFingerprintChunk);
    const int lineTop = fingerprintBox.top + Dpi::Scale(10, dpi) + (line * Dpi::Scale(22, dpi));
    draw::TextWithin(ctx, chunk, monoFont,
                     RectI{fingerprintBox.left + Dpi::Scale(12, dpi), lineTop, fingerprintBox.right - Dpi::Scale(12, dpi),
                           lineTop + Dpi::Scale(20, dpi)},
                     Theme::Text);
  }
}

void PairingDialog::Resolve(bool accepted) {
  Hide();
  if (onResolved) onResolved(accepted);
}

}  // namespace bm
