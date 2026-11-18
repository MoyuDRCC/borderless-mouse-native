#include "app/pages/LogPage.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "app/ui/controls/FlatButton.h"
#include "app/ui/controls/LogList.h"
#include "app/ui/controls/Paint.h"
#include "app/ui/controls/SegmentedControl.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

LogPage::LogPage(UiModel& model, UiActions& actions) : model_(model), actions_(actions) {
  auto filter = std::make_unique<SegmentedControl>(std::vector<std::wstring>{L"全部", L"警告", L"错误"});
  filter_ = filter.get();
  filter_->onSelected = [this](int index) {
    level_ = index;
    ApplyFilter();
  };
  AddControl(std::move(filter));

  auto clear = std::make_unique<FlatButton>(L"清空", FlatButton::Style::Ghost);
  clear_ = clear.get();
  clear_->onClick = [this] {
    if (actions_.app.clearLogs) actions_.app.clearLogs();
  };
  AddControl(std::move(clear));

  auto exportButton = std::make_unique<FlatButton>(L"导出诊断", FlatButton::Style::Secondary);
  export_ = exportButton.get();
  export_->onClick = [this] {
    if (actions_.app.exportDiagnostics) actions_.app.exportDiagnostics();
  };
  AddControl(std::move(exportButton));

  auto list = std::make_unique<LogList>(false);
  list_ = list.get();
  AddControl(std::move(list));
}

void LogPage::OnLayout() {
  const int dpi = ViewDpi();
  const int inset = Inset(dpi);
  const int contentTop = ContentTop(dpi);
  const int width = std::max(Dpi::Scale(280, dpi), Bounds().Width() - (inset * 2));
  const int toolbarTop = contentTop + Dpi::Scale(2, dpi);

  filter_->SetBounds(RectI{inset, toolbarTop, inset + Dpi::Scale(186, dpi), toolbarTop + Dpi::Scale(30, dpi)});

  const int exportLeft = inset + width - Dpi::Scale(94, dpi);
  export_->SetBounds(RectI{exportLeft, toolbarTop, exportLeft + Dpi::Scale(94, dpi), toolbarTop + Dpi::Scale(30, dpi)});
  const int clearLeft = exportLeft - Dpi::Scale(6, dpi) - Dpi::Scale(60, dpi);
  clear_->SetBounds(RectI{clearLeft, toolbarTop, clearLeft + Dpi::Scale(60, dpi), toolbarTop + Dpi::Scale(30, dpi)});

  const int diagnosticsHeight = Dpi::Scale(30, dpi);
  const int listTop = toolbarTop + Dpi::Scale(42, dpi);
  const int listBottom = Bounds().Height() - inset - diagnosticsHeight - Dpi::Scale(10, dpi);
  list_->SetBounds(RectI{inset, listTop, inset + width, listTop + std::max(Dpi::Scale(120, dpi), listBottom - listTop)});
  diagnosticsRect_ = RectI{inset, Bounds().Height() - inset - diagnosticsHeight, inset + width,
                           Bounds().Height() - inset};
}

void LogPage::OnTick() { Sync(false); }

void LogPage::Refresh(bool force) { Sync(force); }

bool LogPage::Matches(LogLevel level, int filter) {
  if (filter == 1) return level != LogLevel::Info;
  if (filter == 2) return level == LogLevel::Error;
  return true;
}

void LogPage::Sync(bool force) {
  UpdateDiagnostics();

  const size_t count = model_.logs.size();
  const uint64_t tail = count > 0 ? model_.logs[count - 1].timeMs : 0;
  if (force || count != all_.size() || tail != shownTail_) {
    all_ = model_.logs;
    ApplyFilter();
  }
}

void LogPage::ApplyFilter() {
  std::vector<LogEntry> entries;
  entries.reserve(all_.size());
  for (const LogEntry& entry : all_) {
    if (Matches(entry.level, level_)) entries.push_back(entry);
  }

  shownCount_ = entries.size();
  shownTail_ = all_.empty() ? 0 : all_.back().timeMs;
  list_->SetEntries(std::move(entries));
  list_->SetVisible(shownCount_ > 0);
  list_->Invalidate();
  Invalidate();
}

void LogPage::UpdateDiagnostics() {
  const UiStats& stats = model_.stats;
  std::wstring text = L"往返 " + std::to_wstring(stats.roundTripMs) + L" ms · 注入 " +
                      std::to_wstring(stats.lastInjectMs) + L"/" + std::to_wstring(stats.worstInjectMs) +
                      L" ms · 待发 " + std::to_wstring(stats.pendingFrames) + L" 帧";
  if (!model_.statusText.empty()) text += L" · " + ToWide(model_.statusText);
  if (text == diagnostics_) return;
  diagnostics_ = std::move(text);
  Invalidate();
}

void LogPage::Paint(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  paint::Fill(ctx, LocalBounds(), Theme::Window);
  paint::PageHeader(ctx, L"日志", L"导出的诊断会把设备名与用户目录换成代号，可直接用于排查问题");

  // 工具栏与列表之间的细分隔线
  paint::Separator(ctx, list_->Bounds().left, list_->Bounds().right,
                   list_->Bounds().top - Dpi::Scale(6, dpi));

  if (shownCount_ == 0) {
    paint::Text(ctx, L"当前筛选条件下暂无日志", paint::TextStyle::Caption,
                list_->Bounds().left + Dpi::Scale(6, dpi), list_->Bounds().top + Dpi::Scale(8, dpi),
                Theme::TextTertiary);
  }

  // 底部等宽诊断条
  if (diagnosticsRect_.IsEmpty()) return;
  draw::FillRound(ctx.graphics, diagnosticsRect_, Dpi::Scale(Theme::RadiusControl, dpi), Theme::SurfaceSubtle);
  const RectI textRect{diagnosticsRect_.left + Dpi::Scale(12, dpi), diagnosticsRect_.top,
                       std::max(diagnosticsRect_.left + Dpi::Scale(22, dpi),
                                diagnosticsRect_.right - Dpi::Scale(12, dpi)),
                       diagnosticsRect_.bottom};
  draw::TextWithin(ctx, diagnostics_, paint::Font(paint::TextStyle::MonoCaption, dpi), textRect,
                   Theme::TextSecondary);
}

}  // namespace bm
