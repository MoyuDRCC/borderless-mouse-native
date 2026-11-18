#include "app/dialogs/TopologyMergeDialog.h"

#include <algorithm>
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

// 右栏勾选行高与左栏九格的内缩（逻辑像素）
constexpr int kRowHeight = 22;
constexpr int kCellGap = 8;
constexpr int kPanelPadding = 10;

}  // namespace

TopologyMergeDialog::TopologyMergeDialog() : Dialog(L"确认设备布局合并", std::wstring(), 620, 420) {
  auto accept = std::make_unique<FlatButton>(L"合并并保存", FlatButton::Style::Primary);
  accept_ = accept.get();
  accept_->onClick = [this] { Resolve(true); };
  AddControl(std::move(accept));

  auto retain = std::make_unique<FlatButton>(L"保留现状", FlatButton::Style::Secondary);
  retain_ = retain.get();
  retain_->onClick = [this] { Resolve(false); };
  AddControl(std::move(retain));
}

void TopologyMergeDialog::SetCandidate(const MergeCandidate& candidate, const std::string& localId) {
  const std::wstring peerName = ToWide(candidate.peerName.empty() ? std::string("对端设备") : candidate.peerName);
  SetDescription(peerName + L" 发来的布局与当前布局不同，请核对下方九格预览后再决定。");
  items_.clear();
  items_.reserve(candidate.preview.size());
  for (const TopologyDevice& device : candidate.preview) {
    PreviewItem item;
    item.name = device.name.empty() ? ToWide(device.deviceId) : ToWide(device.name);
    item.row = device.row;
    item.column = device.column;
    item.local = !localId.empty() && device.deviceId == localId;
    item.checked = true;
    items_.push_back(std::move(item));
  }
}

void TopologyMergeDialog::LayoutContent(const RectI& card, int margin, int dpi) {
  // 左栏九格
  const RectI gridPanel{card.left + margin, card.top + Dpi::Scale(96, dpi), card.left + margin + Dpi::Scale(348, dpi),
                        card.top + Dpi::Scale(314, dpi)};
  const int gap = Dpi::Scale(kCellGap, dpi);
  const int pad = Dpi::Scale(kPanelPadding, dpi);
  const int cellWidth = (gridPanel.Width() - (pad * 2) - (gap * (kLayoutColumns - 1))) / kLayoutColumns;
  const int cellHeight = (gridPanel.Height() - (pad * 2) - (gap * (kLayoutRows - 1))) / kLayoutRows;
  for (int row = 0; row < kLayoutRows; ++row) {
    for (int column = 0; column < kLayoutColumns; ++column) {
      const int left = gridPanel.left + pad + (column * (cellWidth + gap));
      const int top = gridPanel.top + pad + (row * (cellHeight + gap));
      cells_[(row * kLayoutColumns) + column] = RectI{left, top, left + cellWidth, top + cellHeight};
    }
  }

  // 右栏设备勾选行
  choices_.clear();
  const int listLeft = card.right - margin - Dpi::Scale(212, dpi) + Dpi::Scale(8, dpi);
  const int listRight = card.right - margin - Dpi::Scale(8, dpi);
  int top = card.top + Dpi::Scale(96, dpi) + Dpi::Scale(8, dpi);
  for (size_t index = 0; index < items_.size() && index < static_cast<size_t>(kLayoutCapacity); ++index) {
    choices_.emplace_back(static_cast<int>(index),
                          RectI{listLeft, top, listRight, top + Dpi::Scale(kRowHeight, dpi)});
    top += Dpi::Scale(kRowHeight, dpi);
  }

  int right = card.right - margin;
  const int buttonHeight = Dpi::Scale(34, dpi);
  const int buttonTop = FooterTop(card, buttonHeight, dpi);
  const int buttonWidth = Dpi::Scale(104, dpi);
  accept_->SetBounds(FooterButton(right, buttonTop, buttonWidth, buttonHeight, dpi));
  retain_->SetBounds(FooterButton(right, buttonTop, buttonWidth, buttonHeight, dpi));
}

void TopologyMergeDialog::OnMouseUp(Point2i local, MouseButton button) {
  if (button != MouseButton::Left) return;
  for (const auto& entry : choices_) {
    if (!entry.second.Contains(local.x, local.y)) continue;
    PreviewItem& item = items_[static_cast<size_t>(entry.first)];
    if (item.local) return;  // 本机必须保留
    item.checked = !item.checked;
    Invalidate();
    return;
  }
}

void TopologyMergeDialog::Resolve(bool accepted) {
  Hide();
  if (onResolved) onResolved(accepted);
}

void TopologyMergeDialog::PaintGrid(PaintContext& ctx, const RectI& card, int margin, int dpi) {
  const RectI panel{card.left + margin, card.top + Dpi::Scale(96, dpi),
                    card.left + margin + Dpi::Scale(348, dpi), card.top + Dpi::Scale(314, dpi)};
  draw::FillRound(ctx.graphics, panel, Dpi::Scale(Theme::RadiusControl, dpi), Theme::SurfaceSubtle);

  const HFONT font = Dpi::Font(Theme::MicroPx + 0.5f, true, dpi);
  for (int row = 0; row < kLayoutRows; ++row) {
    for (int column = 0; column < kLayoutColumns; ++column) {
      const RectI cell = cells_[(row * kLayoutColumns) + column];
      const PreviewItem* hit = nullptr;
      for (const PreviewItem& item : items_) {
        if (item.checked && item.row == row && item.column == column) {
          hit = &item;
          break;
        }
      }

      if (hit == nullptr) {
        draw::StrokeRound(ctx.graphics, cell, Dpi::Scale(Theme::RadiusControl, dpi), Theme::Border,
                          static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));
        continue;
      }

      const Gdiplus::Color fill = hit->local ? Theme::AccentSoft : Theme::Window;
      const Gdiplus::Color border = hit->local ? Theme::AccentBorder : Theme::Border;
      draw::RoundRect(ctx.graphics, cell, Dpi::Scale(Theme::RadiusControl, dpi), fill, border,
                      static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));
      draw::TextCentered(ctx, hit->name, font,
                         RectI{cell.left + Dpi::Scale(4, dpi), cell.top, cell.right - Dpi::Scale(4, dpi),
                               cell.bottom},
                         hit->local ? Theme::Accent : Theme::Text);
    }
  }
}

void TopologyMergeDialog::PaintChoices(PaintContext& ctx, const RectI& card, int margin, int dpi) {
  const RectI panel{card.right - margin - Dpi::Scale(212, dpi), card.top + Dpi::Scale(96, dpi),
                    card.right - margin, card.top + Dpi::Scale(314, dpi)};
  draw::FillRound(ctx.graphics, panel, Dpi::Scale(Theme::RadiusControl, dpi), Theme::SurfaceSubtle);

  const HFONT font = Dpi::Font(Theme::CaptionPx, false, dpi);
  const int checkbox = Dpi::Scale(14, dpi);
  for (const auto& entry : choices_) {
    const PreviewItem& item = items_[static_cast<size_t>(entry.first)];
    const RectI row = entry.second;
    const int boxTop = row.top + ((row.Height() - checkbox) / 2);
    const int boxLeft = row.left + Dpi::Scale(4, dpi);

    if (item.checked) {
      draw::RoundRect(ctx.graphics, RectI{boxLeft, boxTop, boxLeft + checkbox, boxTop + checkbox},
                      Dpi::Scale(3, dpi), Theme::Accent, Theme::Accent, 0.0f);
      Gdiplus::Pen pen(Gdiplus::Color(255, 255, 255, 255), std::max(1.0f, Dpi::ScaleF(1.4f, dpi)));
      pen.SetStartCap(Gdiplus::LineCapRound);
      pen.SetEndCap(Gdiplus::LineCapRound);
      ctx.graphics.DrawLine(&pen, boxLeft + Dpi::Scale(3, dpi), boxTop + Dpi::Scale(7, dpi),
                            boxLeft + Dpi::Scale(6, dpi), boxTop + Dpi::Scale(11, dpi));
      ctx.graphics.DrawLine(&pen, boxLeft + Dpi::Scale(6, dpi), boxTop + Dpi::Scale(11, dpi),
                            boxLeft + Dpi::Scale(11, dpi), boxTop + Dpi::Scale(4, dpi));
    } else {
      draw::StrokeRound(ctx.graphics, RectI{boxLeft, boxTop, boxLeft + checkbox, boxTop + checkbox},
                        Dpi::Scale(3, dpi), Theme::BorderStrong,
                        static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));
    }

    std::wstring text = item.name;
    if (item.local) text += L"（本机，必须保留）";
    draw::TextWithin(ctx, text, font,
                     RectI{boxLeft + checkbox + Dpi::Scale(8, dpi), row.top, row.right, row.bottom},
                     item.checked ? Theme::Text : Theme::TextTertiary);
  }
}

void TopologyMergeDialog::PaintContent(PaintContext& ctx, const RectI& card, int margin, int dpi) {
  paint::Text(ctx, L"九格预览", paint::TextStyle::BodyBold, card.left + margin, card.top + Dpi::Scale(72, dpi),
              Theme::Text);
  paint::Text(ctx, L"参与合并的设备", paint::TextStyle::BodyBold, card.right - margin - Dpi::Scale(212, dpi),
              card.top + Dpi::Scale(72, dpi), Theme::Text);

  PaintGrid(ctx, card, margin, dpi);
  PaintChoices(ctx, card, margin, dpi);

  paint::TextIn(ctx, L"取消勾选的设备会移入未摆放；本机设备必须保留在网格内。", paint::TextStyle::Caption,
                RectI{card.left + margin, card.top + Dpi::Scale(322, dpi), card.right - margin,
                      card.top + Dpi::Scale(342, dpi)},
                Theme::TextTertiary);
}

}  // namespace bm