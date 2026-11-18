#pragma once

#include <string>

#include "app/pages/Page.h"
#include "app/UiModel.h"

namespace bm {

class ConfirmDialog;
class FlatButton;
class LayoutGrid;
class TopologyMergeDialog;

// 布局页：3×3 网格拖放 + 未摆放池 + 待处理合并与撤销。
// 网格是本页内部的拖放控件，页头右侧的合并/撤销按钮由 LayoutPage 自己排版。
class LayoutPage : public Page {
 public:
  LayoutPage(UiModel& model, UiActions& actions);

  void Paint(PaintContext& ctx) override;
  void OnLayout() override;
  void OnTick() override;

  void Refresh(bool force);

 private:
  void Sync(bool force);
  void ShowPendingMerge();
  void ShowUndoConfirm();
  void ApplyUndo();

  UiModel& model_;
  UiActions& actions_;

  LayoutGrid* grid_ = nullptr;
  FlatButton* merge_ = nullptr;
  FlatButton* undo_ = nullptr;
  TopologyMergeDialog* mergeDialog_ = nullptr;
  ConfirmDialog* undoDialog_ = nullptr;

  // 打开确认框时的布局时钟：确认期间布局变了就放弃这次撤销
  int64_t undoClock_ = 0;
  bool pendingMerge_ = false;
  bool canUndo_ = false;
  size_t mergeCount_ = 0;
  int64_t topologyClock_ = 0;
  size_t pooledCount_ = 0;
  size_t deviceCount_ = 0;
};

}  // namespace bm
