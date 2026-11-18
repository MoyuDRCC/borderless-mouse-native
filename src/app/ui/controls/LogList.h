#pragma once

#include <string>
#include <vector>

#include "app/ui/Control.h"
#include "app/UiModel.h"

namespace bm {

// 运行日志列表：等宽时间戳、柔和胶囊级别标签、清晰正文，支持选中与 Ctrl+C / 右键复制。
// compact 模式用于概览页的“最近事件”，不参与选中。
class LogList : public Control {
 public:
  explicit LogList(bool compact = false);

  void SetEntries(std::vector<LogEntry> entries);
  const std::vector<LogEntry>& Entries() const { return entries_; }

  void Paint(PaintContext& ctx) override;
  bool OnMouseDown(Point2i local, MouseButton button) override;
  void OnMouseUp(Point2i local, MouseButton button) override;
  void OnWheel(int delta, Point2i local) override;
  void OnKey(int vk, bool down) override;

 private:
  int ViewDpi() const;
  int RowHeight(int dpi) const;
  int MaxScroll() const;
  bool NeedsScroll() const;
  int HitRow(Point2i local, int dpi) const;
  void CopyEntry(const LogEntry& entry);

  std::vector<LogEntry> entries_;
  bool compact_;
  int scroll_ = 0;
  int selected_ = -1;
};

}  // namespace bm
