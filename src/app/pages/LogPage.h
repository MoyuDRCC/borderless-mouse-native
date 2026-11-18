#pragma once

#include <string>
#include <vector>

#include "app/pages/Page.h"
#include "app/UiModel.h"

namespace bm {

class FlatButton;
class LogList;
class SegmentedControl;

// 日志页：级别筛选 + 可复制日志流 + 导出/清空 + 底部等宽诊断条。
class LogPage : public Page {
 public:
  LogPage(UiModel& model, UiActions& actions);

  void Paint(PaintContext& ctx) override;
  void OnLayout() override;
  void OnTick() override;

  void Refresh(bool force);

 private:
  void Sync(bool force);
  void ApplyFilter();
  void UpdateDiagnostics();
  static bool Matches(LogLevel level, int filter);

  UiModel& model_;
  UiActions& actions_;

  SegmentedControl* filter_ = nullptr;
  FlatButton* clear_ = nullptr;
  FlatButton* export_ = nullptr;
  LogList* list_ = nullptr;

  std::vector<LogEntry> all_;
  size_t shownCount_ = 0;
  uint64_t shownTail_ = 0;
  int level_ = 0;  // 0 全部、1 警告、2 错误
  std::wstring diagnostics_;
  RectI diagnosticsRect_{};
};

}  // namespace bm
