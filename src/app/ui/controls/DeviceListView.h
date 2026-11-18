#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app/ui/Control.h"
#include "app/UiModel.h"

namespace bm {

// 设备列表：通栏扁平分隔、状态胶囊徽章、行内按钮与“···”收敛菜单、自绘细长滚动条。
// 行内按钮全部通过回调交给页面；空列表展示居中引导并提供手动添加。
class DeviceListView : public Control {
 public:
  static constexpr int LogicalRowHeight = 62;

  DeviceListView();

  void SetDevices(std::vector<DeviceRow> devices);
  const std::vector<DeviceRow>& Devices() const { return devices_; }

  std::function<void()> onAdd;
  std::function<void(const std::string&)> onConnect;
  std::function<void(const std::string&)> onDisconnect;
  std::function<void(const std::string&)> onControl;
  std::function<void(const std::string&)> onMirror;
  std::function<void(const std::string&)> onSettings;
  std::function<void(const std::string&)> onUnpair;
  std::function<void(const std::string&)> onRemove;

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseUp(Point2i local, MouseButton button) override;
  void OnMouseMove(Point2i local) override;
  void OnMouseLeave() override;
  void OnWheel(int delta, Point2i local) override;

 private:
  enum class RowAction { Connect, Control, Mirror, More };

  struct ActionSlot {
    RowAction action = RowAction::More;
    std::wstring label;
    RectI bounds;
    bool disabled = false;  // 连接中禁用“连接”按钮
  };

  int ViewDpi() const;
  int RowHeight(int dpi) const;
  int RowTop(int index, int dpi) const;
  int ContentWidth(int dpi) const;
  int MaxScroll() const;
  bool NeedsScroll() const;
  std::vector<ActionSlot> LayoutActions(const DeviceRow& device, int index, int dpi) const;
  RectI EmptyAddBounds(int dpi) const;
  RectI ScrollThumbBounds(int dpi) const;
  int HitRow(Point2i local, int dpi) const;
  void ClampScroll();
  void DrawEmptyState(PaintContext& ctx);
  void DrawActions(PaintContext& ctx, const std::vector<ActionSlot>& slots, int hoveredSlot);
  void ShowRowMenu(const DeviceRow& device, Point2i anchor);
  static void Invoke(const std::function<void(const std::string&)>& callback, const DeviceRow& device);

  std::vector<DeviceRow> devices_;
  int scroll_ = 0;
  int hoverRow_ = -1;
  int hoverSlot_ = -1;  // 行内按钮序号，-1 表示未悬停在按钮上
  bool hoverEmptyAdd_ = false;
  bool hoverScroll_ = false;
  bool draggingScroll_ = false;
  int dragStartY_ = 0;
  int dragStartScroll_ = 0;
};

}  // namespace bm
