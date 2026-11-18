#pragma once

#include <functional>
#include <string>

#include "app/pages/Page.h"
#include "app/UiModel.h"

namespace bm {

class FlatButton;
class LogList;
class ToggleSwitch;

// 概览页：顶部状态控制条（键鼠共享与剪贴板同步开关）、本机信息三列、最近事件流。
// 只读 UiModel，用户动作用 UiActions 转发；"查看全部" 属于页面导航，由主窗口注入回调。
class OverviewPage : public Page {
 public:
  OverviewPage(UiModel& model, UiActions& actions);

  // 切到日志页（导航由主窗口负责）
  std::function<void()> onShowLogs;

  void Paint(PaintContext& ctx) override;
  void OnLayout() override;
  void OnTick() override;

  // 把模型快照拉到界面上；force 用于刚切到本页时强制重读
  void Refresh(bool force);

 private:
  void Sync(bool force);
  void PaintHero(PaintContext& ctx);
  void PaintLocalInfo(PaintContext& ctx);
  void PaintRecent(PaintContext& ctx);
  void CopyAddress();
  void ResetCopyLabel();

  UiModel& model_;
  UiActions& actions_;

  ToggleSwitch* sharing_ = nullptr;
  ToggleSwitch* clipboard_ = nullptr;
  FlatButton* copy_ = nullptr;
  FlatButton* more_ = nullptr;
  LogList* recent_ = nullptr;

  std::wstring localName_;
  std::wstring localAddress_;
  std::wstring localPort_;
  std::wstring statusText_;
  bool connected_ = false;
  size_t recentCount_ = 0;
  uint64_t recentTail_ = 0;
  bool updating_ = false;
  int copyResetTicks_ = 0;

  RectI heroRect_{};
  RectI infoRect_{};
  int recentHeaderY_ = 0;
};

}  // namespace bm
