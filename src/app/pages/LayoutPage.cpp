#include "app/pages/LayoutPage.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "app/ui/controls/FlatButton.h"
#include "app/ui/controls/Paint.h"
#include "app/ui/Drawing.h"
#include "app/dialogs/ConfirmDialog.h"
#include "app/dialogs/TopologyMergeDialog.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

namespace {

// 网格几何：与 .NET LayoutEditor 的常量一致（逻辑像素）
constexpr int kCellWidth = 160;
constexpr int kCellHeight = 72;
constexpr int kCellGap = 18;
constexpr int kPoolItemHeight = 32;

}  // namespace

// 3×3 拓扑网格：点阵背景、相邻设备桥接、设备块拖放与未摆放池
class LayoutGrid : public Control {
 public:
  LayoutGrid(UiModel& model, UiActions& actions) : model_(model), actions_(actions) {}

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseMove(Point2i local) override;
  void OnMouseUp(Point2i local, MouseButton button) override;
  void OnMouseLeave() override;

 private:
  struct PoolItem {
    std::string id;
    std::wstring name;
    RectI bounds;
  };

  int ViewDpi() const;
  void UpdateGeometry();
  const TopologyDevice* PlacedAt(int row, int column) const;
  const DeviceRow* RowOf(const std::string& id) const;
  std::wstring NameOf(const TopologyDevice& device) const;
  bool IsLocal(const TopologyDevice& device) const;
  int HitCell(Point2i local) const;
  int HitPool(Point2i local) const;
  void CancelDrag();
  void DrawDotGrid(PaintContext& ctx, const RectI& area);
  void DrawConnections(PaintContext& ctx);
  void PaintCell(PaintContext& ctx, int index);
  void PaintPool(PaintContext& ctx);
  void PaintDragGhost(PaintContext& ctx);

  UiModel& model_;
  UiActions& actions_;

  RectI cells_[kLayoutCapacity]{};
  std::vector<PoolItem> pool_;
  RectI poolBounds_{};

  bool dragging_ = false;
  std::string dragId_;
  std::wstring dragName_;
  bool dragFromGrid_ = false;
  bool dragLocal_ = false;
  int dragRow_ = -1;
  int dragColumn_ = -1;
  Point2i dragPoint_{};
  int dropCell_ = -1;
  bool dropIntoPool_ = false;
  int hoverCell_ = -1;
  int hoverPool_ = -1;
};

int LayoutGrid::ViewDpi() const { return Host() ? Host()->Dpi() : 96; }

const TopologyDevice* LayoutGrid::PlacedAt(int row, int column) const {
  for (const TopologyDevice& device : model_.topology.devices) {
    if (device.state == TopologyDeviceState::Placed && device.row == row && device.column == column) {
      return &device;
    }
  }
  return nullptr;
}

const DeviceRow* LayoutGrid::RowOf(const std::string& id) const {
  for (const DeviceRow& row : model_.devices) {
    if (row.id == id) return &row;
  }
  return nullptr;
}

std::wstring LayoutGrid::NameOf(const TopologyDevice& device) const {
  if (const DeviceRow* row = RowOf(device.deviceId)) return ToWide(row->alias.empty() ? row->name : row->alias);
  return device.name.empty() ? L"未知设备" : ToWide(device.name);
}

// 本机设备：拓扑里以设备名与本机名对应，本机必须保留在网格内
bool LayoutGrid::IsLocal(const TopologyDevice& device) const {
  return !model_.localId.empty() && device.deviceId == model_.localId;
}

void LayoutGrid::UpdateGeometry() {
  const int dpi = ViewDpi();
  const int gap = Dpi::Scale(kCellGap, dpi);
  const int available = std::max(Dpi::Scale(240, dpi), Bounds().Width() - Dpi::Scale(16, dpi));
  const int maxCellWidth =
      std::max(Dpi::Scale(110, dpi), (available - (gap * (kLayoutColumns - 1))) / kLayoutColumns);
  const int cellWidth = std::min(Dpi::Scale(kCellWidth, dpi), maxCellWidth);
  const int cellHeight = Dpi::Scale(kCellHeight, dpi);
  const int totalWidth = (cellWidth * kLayoutColumns) + (gap * (kLayoutColumns - 1));
  const int totalHeight = (cellHeight * kLayoutRows) + (gap * (kLayoutRows - 1));
  const int startX = std::max(Dpi::Scale(8, dpi), (Bounds().Width() - totalWidth) / 2);
  const int maxStartY = std::max(Dpi::Scale(10, dpi), (Bounds().Height() - Dpi::Scale(96, dpi) - totalHeight) / 2);
  const int startY = std::min(Dpi::Scale(20, dpi), maxStartY);

  for (int row = 0; row < kLayoutRows; ++row) {
    for (int column = 0; column < kLayoutColumns; ++column) {
      const int left = startX + (column * (cellWidth + gap));
      const int top = startY + (row * (cellHeight + gap));
      cells_[(row * kLayoutColumns) + column] =
          RectI{left, top, left + cellWidth, top + cellHeight};
    }
  }

  // 未摆放池：横向流式排列，超出宽度就换行
  pool_.clear();
  int poolTop = startY + totalHeight + Dpi::Scale(48, dpi);
  int x = Dpi::Scale(4, dpi);
  poolBounds_ = RectI{0, poolTop - Dpi::Scale(26, dpi), Bounds().Width(),
                      std::max(Dpi::Scale(kPoolItemHeight + 28, dpi),
                               Bounds().Height() - poolTop + Dpi::Scale(26, dpi))};

  const HFONT font = Dpi::Font(Theme::CaptionPx, false, dpi);
  for (const auto& entry : model_.pooled) {
    const std::wstring name = entry.second.empty() ? ToWide(entry.first) : ToWide(entry.second);
    const int itemWidth =
        std::min(draw::Measure(name, font).width + Dpi::Scale(28, dpi),
                 std::max(Dpi::Scale(80, dpi), Bounds().Width() - Dpi::Scale(16, dpi)));
    if (x + itemWidth > Bounds().Width() - Dpi::Scale(4, dpi) && x > Dpi::Scale(4, dpi)) {
      x = Dpi::Scale(4, dpi);
      poolTop += Dpi::Scale(kPoolItemHeight + 8, dpi);
    }
    pool_.push_back(PoolItem{entry.first, name,
                             RectI{x, poolTop, x + itemWidth, poolTop + Dpi::Scale(kPoolItemHeight, dpi)}});
    x += itemWidth + Dpi::Scale(8, dpi);
  }
}

int LayoutGrid::HitCell(Point2i local) const {
  for (int index = 0; index < kLayoutCapacity; ++index) {
    if (cells_[index].Contains(local.x, local.y)) return index;
  }
  return -1;
}

int LayoutGrid::HitPool(Point2i local) const {
  for (size_t index = 0; index < pool_.size(); ++index) {
    if (pool_[index].bounds.Contains(local.x, local.y)) return static_cast<int>(index);
  }
  return -1;
}

void LayoutGrid::DrawDotGrid(PaintContext& ctx, const RectI& area) {
  if (area.IsEmpty()) return;
  const int dpi = ctx.dpi;
  const int step = Dpi::Scale(20, dpi);
  const int dot = std::max(1, Dpi::Scale(2, dpi));
  for (int x = Dpi::Scale(10, dpi); x < area.right; x += step) {
    for (int y = Dpi::Scale(10, dpi); y < area.bottom; y += step) {
      draw::FillRound(ctx.graphics, RectI{x, y, x + dot, y + dot}, dot / 2, Theme::Separator);
    }
  }
}

void LayoutGrid::DrawConnections(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  Gdiplus::Pen bridge(Theme::AccentBorder, std::max(1.5f, Dpi::ScaleF(1.8f, dpi)));
  Gdiplus::SolidBrush node(Theme::Accent);
  const int nodeSize = std::max(4, Dpi::Scale(6, dpi));

  for (int row = 0; row < kLayoutRows; ++row) {
    for (int column = 0; column < kLayoutColumns; ++column) {
      if (PlacedAt(row, column) == nullptr) continue;
      const RectI current = cells_[(row * kLayoutColumns) + column];

      if (column + 1 < kLayoutColumns && PlacedAt(row, column + 1) != nullptr) {
        const RectI right = cells_[(row * kLayoutColumns) + column + 1];
        const int y = current.top + (current.Height() / 2);
        ctx.graphics.DrawLine(&bridge, current.right, y, right.left, y);
        const int middle = (current.right + right.left) / 2;
        ctx.graphics.FillEllipse(&node, middle - (nodeSize / 2), y - (nodeSize / 2), nodeSize, nodeSize);
      }

      if (row + 1 < kLayoutRows && PlacedAt(row + 1, column) != nullptr) {
        const RectI bottom = cells_[((row + 1) * kLayoutColumns) + column];
        const int x = current.left + (current.Width() / 2);
        ctx.graphics.DrawLine(&bridge, x, current.bottom, x, bottom.top);
        const int middle = (current.bottom + bottom.top) / 2;
        ctx.graphics.FillEllipse(&node, x - (nodeSize / 2), middle - (nodeSize / 2), nodeSize, nodeSize);
      }
    }
  }
}

void LayoutGrid::PaintCell(PaintContext& ctx, int index) {
  const int dpi = ctx.dpi;
  const int row = index / kLayoutColumns;
  const int column = index % kLayoutColumns;
  const RectI cell = cells_[index];
  const TopologyDevice* device = PlacedAt(row, column);
  const int radius = Dpi::Scale(Theme::RadiusSurface, dpi);

  if (device == nullptr) {
    if (index == dropCell_) {
      draw::RoundRect(ctx.graphics, cell, radius, Theme::AccentSoft, Theme::Accent,
                      static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));
    } else {
      if (dragging_) {
        draw::FillRound(ctx.graphics, cell, radius, Gdiplus::Color(140, 0xF6, 0xF7, 0xF9));
      }
      // 空格用虚线描边
      Gdiplus::GraphicsPath path;
      draw::BuildRoundedRect(cell, radius, path);
      Gdiplus::Pen pen(dragging_ ? Theme::BorderStrong : Theme::Separator,
                       static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));
      pen.SetDashStyle(Gdiplus::DashStyleDash);
      ctx.graphics.DrawPath(&pen, &path);
    }
    return;
  }

  const bool isLocal = IsLocal(*device);
  const DeviceRow* deviceRow = RowOf(device->deviceId);
  const bool online = isLocal || (deviceRow != nullptr && deviceRow->connected);
  const bool hovered = index == hoverCell_;
  const bool dropTarget = index == dropCell_;

  draw::FillRound(ctx.graphics, cell, radius, isLocal ? Theme::AccentSoft : Theme::Window);
  draw::StrokeRound(ctx.graphics, cell, radius,
                    (dropTarget || hovered) ? Theme::Accent : (isLocal ? Theme::AccentBorder : Theme::Border),
                    static_cast<float>(std::max(1, Dpi::Scale(dropTarget ? 2 : 1, dpi))));

  const int alpha = (dragging_ && dragId_ == device->deviceId) ? 95 : 255;

  // 左侧状态色条
  const int stripeLeft = cell.left + Dpi::Scale(7, dpi);
  const int stripeWidth = std::max(2, Dpi::Scale(3, dpi));
  const Gdiplus::Color stripe = isLocal ? Theme::Accent : (online ? Theme::Success : Theme::ToggleTrackOff);
  draw::FillRound(ctx.graphics,
                  RectI{stripeLeft, cell.top + Dpi::Scale(12, dpi), stripeLeft + stripeWidth,
                        cell.bottom - Dpi::Scale(12, dpi)},
                  stripeWidth / 2, Gdiplus::Color(static_cast<BYTE>(alpha), stripe.GetR(), stripe.GetG(),
                                                  stripe.GetB()));

  const int textLeft = stripeLeft + stripeWidth + Dpi::Scale(10, dpi);
  const int textRight = std::max(textLeft + Dpi::Scale(24, dpi), cell.right - Dpi::Scale(10, dpi));
  const std::wstring name = NameOf(*device);
  draw::TextWithin(ctx, name, Dpi::Font(Theme::BodyPx, true, dpi),
                   RectI{textLeft, cell.top + Dpi::Scale(12, dpi), textRight, cell.top + Dpi::Scale(32, dpi)},
                   Gdiplus::Color(static_cast<BYTE>(alpha), Theme::Text.GetR(), Theme::Text.GetG(),
                                  Theme::Text.GetB()));

  // 副标题对齐 .NET 的 StateText：在线写已连接，离线再按是否配对区分
  std::wstring subtitle;
  if (isLocal) {
    subtitle = L"本机中心";
  } else if (online) {
    subtitle = L"已连接";
  } else if (deviceRow != nullptr && deviceRow->paired) {
    subtitle = L"未连接";
  } else {
    subtitle = L"待配对";
  }
  const Gdiplus::Color pillBackground =
      isLocal ? Theme::Window : (online ? Theme::SuccessSoft : Theme::SurfaceSubtle);
  const Gdiplus::Color pillForeground =
      isLocal ? Theme::Accent : (online ? Theme::Success : Theme::TextSecondary);
  draw::Pill(ctx, subtitle, Dpi::Font(Theme::MicroPx, true, dpi), textLeft,
             cell.top + Dpi::Scale(48, dpi), pillBackground, pillForeground);
}

void LayoutGrid::PaintPool(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int separatorY = poolBounds_.top - Dpi::Scale(4, dpi);

  if (dropIntoPool_) {
    draw::RoundRect(ctx.graphics,
                    RectI{0, separatorY + Dpi::Scale(4, dpi), Bounds().Width() - Dpi::Scale(1, dpi),
                          std::max(separatorY + Dpi::Scale(52, dpi),
                                   Bounds().Height() - Dpi::Scale(6, dpi))},
                    Dpi::Scale(Theme::RadiusSurface, dpi), Theme::AccentSoft, Theme::Accent,
                    static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));
  }

  draw::Separator(ctx, Dpi::Scale(4, dpi), Bounds().Width() - Dpi::Scale(4, dpi), separatorY);
  draw::Text(ctx, L"未摆放设备（拖入上方网格即可跨屏；本机必须保留）",
             Dpi::Font(Theme::CaptionPx, false, dpi), Dpi::Scale(4, dpi),
             separatorY + Dpi::Scale(12, dpi), Theme::TextSecondary);
  if (pool_.empty()) {
    draw::Text(ctx, L"所有设备均已参与布局", Dpi::Font(Theme::CaptionPx, false, dpi), Dpi::Scale(4, dpi),
               separatorY + Dpi::Scale(36, dpi), Theme::TextTertiary);
  }

  const HFONT font = Dpi::Font(Theme::CaptionPx, false, dpi);
  for (size_t index = 0; index < pool_.size(); ++index) {
    const PoolItem& item = pool_[index];
    const bool hovered = static_cast<int>(index) == hoverPool_;
    const Gdiplus::Color fill = hovered ? Theme::RowHover : Theme::SurfaceSubtle;
    const Gdiplus::Color border = hovered ? Theme::BorderStrong : Theme::Border;
    draw::RoundRect(ctx.graphics, item.bounds, Dpi::Scale(Theme::RadiusControl, dpi), fill, border,
                    static_cast<float>(std::max(1, Dpi::Scale(1, dpi))));
    draw::TextCentered(ctx, item.name, font, item.bounds, Theme::Text);
  }
}

void LayoutGrid::PaintDragGhost(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int width = Dpi::Scale(142, dpi);
  const int height = Dpi::Scale(46, dpi);
  const RectI follow{dragPoint_.x - (width / 2), dragPoint_.y - (height / 2),
                     dragPoint_.x + (width - (width / 2)), dragPoint_.y + (height - (height / 2))};
  draw::RoundRect(ctx.graphics, follow, Dpi::Scale(Theme::RadiusSurface, dpi),
                  Gdiplus::Color(230, Theme::Accent.GetR(), Theme::Accent.GetG(), Theme::Accent.GetB()),
                  Gdiplus::Color(0, 0, 0, 0), 0.0f);
  draw::TextCentered(ctx, dragName_, Dpi::Font(Theme::BodyPx, true, dpi), follow,
                     Gdiplus::Color(255, 255, 255, 255));
}

void LayoutGrid::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  UpdateGeometry();

  paint::Fill(ctx, LocalBounds(), Theme::Window);
  DrawDotGrid(ctx, RectI{0, 0, Bounds().Width(), std::max(0, poolBounds_.top - Dpi::Scale(8, dpi))});
  DrawConnections(ctx);

  for (int index = 0; index < kLayoutCapacity; ++index) PaintCell(ctx, index);
  PaintPool(ctx);
  if (dragging_) PaintDragGhost(ctx);
}

bool LayoutGrid::OnMouseDown(Point2i local, MouseButton button) {
  if (button != MouseButton::Left) return false;
  UpdateGeometry();

  const int cell = HitCell(local);
  if (cell >= 0) {
    const TopologyDevice* device = PlacedAt(cell / kLayoutColumns, cell % kLayoutColumns);
    if (device == nullptr) return true;
    dragging_ = true;
    dragId_ = device->deviceId;
    dragName_ = NameOf(*device);
    dragFromGrid_ = true;
    dragLocal_ = IsLocal(*device);
    dragRow_ = cell / kLayoutColumns;
    dragColumn_ = cell % kLayoutColumns;
    dragPoint_ = local;
    Invalidate();
    return true;
  }

  const int pooled = HitPool(local);
  if (pooled < 0) return true;
  dragging_ = true;
  dragId_ = pool_[pooled].id;
  dragName_ = pool_[pooled].name;
  dragFromGrid_ = false;
  dragLocal_ = false;
  dragRow_ = -1;
  dragColumn_ = -1;
  dragPoint_ = local;
  Invalidate();
  return true;
}

void LayoutGrid::OnMouseMove(Point2i local) {
  if (dragging_) {
    dragPoint_ = local;
    dropCell_ = HitCell(local);
    // 只有已摆放的设备才能拖回池中，本机则不允许
    dropIntoPool_ = dragFromGrid_ && !dragLocal_ && poolBounds_.Contains(local.x, local.y);
    Invalidate();
    return;
  }

  UpdateGeometry();
  const int cell = HitCell(local);
  int occupied = -1;
  if (cell >= 0 && PlacedAt(cell / kLayoutColumns, cell % kLayoutColumns) != nullptr) occupied = cell;
  const int pooled = HitPool(local);

  if (occupied != hoverCell_ || pooled != hoverPool_) {
    hoverCell_ = occupied;
    hoverPool_ = pooled;
    SetCursor((occupied >= 0 || pooled >= 0) ? LoadCursorW(nullptr, IDC_HAND) : nullptr);
    Invalidate();
  }
}

void LayoutGrid::CancelDrag() {
  dragging_ = false;
  dragFromGrid_ = false;
  dragLocal_ = false;
  dragRow_ = -1;
  dragColumn_ = -1;
  dropCell_ = -1;
  dropIntoPool_ = false;
  SetCursor(nullptr);
  Invalidate();
}

void LayoutGrid::OnMouseUp(Point2i local, MouseButton button) {
  if (button != MouseButton::Left || !dragging_) return;

  const int target = dropCell_ >= 0 ? dropCell_ : HitCell(local);
  const bool intoPool = dropIntoPool_ || HitPool(local) >= 0;
  const bool fromGrid = dragFromGrid_;
  const int originRow = dragRow_;
  const int originColumn = dragColumn_;
  const std::string id = dragId_;
  const bool localDevice = dragLocal_;

  CancelDrag();

  if (target >= 0) {
    const int row = target / kLayoutColumns;
    const int column = target % kLayoutColumns;
    if (fromGrid && row == originRow && column == originColumn) return;
    if (!fromGrid && PlacedAt(row, column) != nullptr) return;  // 池中的设备只放到空格
    if (fromGrid) {
      if (actions_.layout.moveLayoutCell) actions_.layout.moveLayoutCell(originRow, originColumn, row, column);
    } else if (actions_.layout.placeDevice) {
      actions_.layout.placeDevice(id, row, column);
    }
    return;
  }

  if (intoPool && fromGrid && !localDevice && actions_.layout.poolDevice) actions_.layout.poolDevice(id, true);
}

void LayoutGrid::OnMouseLeave() {
  if (dragging_) return;  // 拖拽中离开控件仍继续跟随
  if (hoverCell_ < 0 && hoverPool_ < 0) return;
  hoverCell_ = -1;
  hoverPool_ = -1;
  SetCursor(nullptr);
  Invalidate();
}

// ---- 布局页本身 ----

LayoutPage::LayoutPage(UiModel& model, UiActions& actions) : model_(model), actions_(actions) {
  auto grid = std::make_unique<LayoutGrid>(model, actions);
  grid_ = grid.get();
  AddControl(std::move(grid));

  auto merge = std::make_unique<FlatButton>(L"待处理合并", FlatButton::Style::Secondary);
  merge_ = merge.get();
  merge_->SetActive(true);
  merge_->SetVisible(false);
  merge_->onClick = [this] { ShowPendingMerge(); };
  AddControl(std::move(merge));

  auto undo = std::make_unique<FlatButton>(L"撤销布局", FlatButton::Style::Secondary);
  undo_ = undo.get();
  undo_->SetEnabled(false);
  undo_->onClick = [this] { ShowUndoConfirm(); };
  AddControl(std::move(undo));
}

void LayoutPage::OnLayout() {
  const int dpi = ViewDpi();
  const int inset = Inset(dpi);
  const int contentTop = ContentTop(dpi);
  const int width = std::max(Dpi::Scale(280, dpi), Bounds().Width() - (inset * 2));
  const int height = std::max(Dpi::Scale(240, dpi), Bounds().Height() - contentTop - inset);
  grid_->SetBounds(RectI{inset, contentTop, inset + width, contentTop + height});

  const int buttonTop = Dpi::Scale(40, dpi);
  const int buttonBottom = buttonTop + Dpi::Scale(32, dpi);
  const int undoRight = Bounds().Width() - inset;
  const int undoLeft = undoRight - Dpi::Scale(88, dpi);
  undo_->SetBounds(RectI{undoLeft, buttonTop, undoRight, buttonBottom});

  const int mergeRight = undoLeft - Dpi::Scale(8, dpi);
  const int mergeLeft = mergeRight - Dpi::Scale(104, dpi);
  merge_->SetBounds(RectI{mergeLeft, buttonTop, mergeRight, buttonBottom});
}

void LayoutPage::OnTick() { Sync(false); }

void LayoutPage::Refresh(bool force) { Sync(force); }

void LayoutPage::Sync(bool force) {
  const bool pending = !model_.merges.empty();
  const bool canUndo = model_.canUndoLayout;
  if (force || pending != pendingMerge_ || canUndo != canUndo_ || model_.merges.size() != mergeCount_) {
    pendingMerge_ = pending;
    canUndo_ = canUndo;
    mergeCount_ = model_.merges.size();
    merge_->SetVisible(pending);
    undo_->SetEnabled(canUndo);
    Invalidate();
  }
  // 拓扑、池与设备数变化时重画网格
  if (force || model_.topology.clock != topologyClock_ || model_.pooled.size() != pooledCount_ ||
      model_.devices.size() != deviceCount_) {
    topologyClock_ = model_.topology.clock;
    pooledCount_ = model_.pooled.size();
    deviceCount_ = model_.devices.size();
    grid_->Invalidate();
  }
}

void LayoutPage::ShowPendingMerge() {
  Window* host = Host();
  if (!host || !host->Root() || model_.merges.empty()) return;

  if (!mergeDialog_) {
    auto dialog = std::make_unique<TopologyMergeDialog>();
    mergeDialog_ = dialog.get();
    host->Root()->AddControl(std::move(dialog));
  }

  const MergeCandidate& candidate = model_.merges.front();
  mergeDialog_->onResolved = [this, revision = candidate.revision](bool accepted) {
    if (actions_.layout.resolveMerge) actions_.layout.resolveMerge(revision, accepted);
  };
  mergeDialog_->SetCandidate(candidate, model_.localId);
  mergeDialog_->Show();
}

void LayoutPage::ShowUndoConfirm() {
  Window* host = Host();
  if (!host || !host->Root() || !model_.canUndoLayout) return;

  if (!undoDialog_) {
    auto dialog = std::make_unique<ConfirmDialog>();
    undoDialog_ = dialog.get();
    dialog->onConfirmed = [this] { ApplyUndo(); };
    host->Root()->AddControl(std::move(dialog));
  }

  undoClock_ = model_.topology.clock;
  undoDialog_->Ask(L"确认撤销布局", L"撤销将恢复到上一版布局，且不能再次撤销。");
}

void LayoutPage::ApplyUndo() {
  // 确认期间布局被对端改过，用户看到的确认内容已不适用，放弃这次撤销
  if (model_.topology.clock != undoClock_) {
    if (actions_.layout.report) actions_.layout.report("布局已变化，请重新确认撤销。");
    return;
  }
  if (actions_.layout.undoLayout) actions_.layout.undoLayout();
}

void LayoutPage::Paint(PaintContext& ctx) {
  paint::Fill(ctx, LocalBounds(), Theme::Window);
  paint::PageHeader(ctx, L"布局", L"拖动设备块安排上下左右；相邻蓝色节点表示鼠标可跨屏边界");
}

}  // namespace bm
