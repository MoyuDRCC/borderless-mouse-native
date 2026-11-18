#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "app/UiModel.h"
#include "app/dialogs/Dialog.h"

namespace bm {

class FlatButton;

// 布局合并确认弹窗：左侧九格预览（未勾选的设备不参与），右侧逐台设备勾选。
// 本机设备锁定勾选；结果通过 onResolved 交回，页面或主窗口负责后续动作。
class TopologyMergeDialog : public Dialog {
 public:
  TopologyMergeDialog();

  void SetCandidate(const MergeCandidate& candidate, const std::string& localId);

  std::function<void(bool accepted)> onResolved;

  void OnMouseUp(Point2i local, MouseButton button) override;

 protected:
  void LayoutContent(const RectI& card, int margin, int dpi) override;
  void PaintContent(PaintContext& ctx, const RectI& card, int margin, int dpi) override;

 private:
  struct PreviewItem {
    std::wstring name;
    int row = 0;
    int column = 0;
    bool local = false;
    bool checked = true;
  };

  void Resolve(bool accepted);
  void PaintGrid(PaintContext& ctx, const RectI& card, int margin, int dpi);
  void PaintChoices(PaintContext& ctx, const RectI& card, int margin, int dpi);

  std::vector<PreviewItem> items_;
  RectI cells_[kLayoutCapacity]{};
  std::vector<std::pair<int, RectI>> choices_;  // 设备序号 + 行矩形
  FlatButton* accept_ = nullptr;
  FlatButton* retain_ = nullptr;
};

}  // namespace bm
