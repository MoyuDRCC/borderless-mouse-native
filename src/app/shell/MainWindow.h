#pragma once

#include "app/ui/Control.h"
#include "core/net/PeerSession.h"
#include "app/UiModel.h"

#include <memory>
#include <vector>

namespace bm {

// 主窗口壳：左侧导航 + 五个页面 + 自定义标题栏。
// 控制器按 500ms 刷新模型后调用 Refresh()，页面自己从模型取需要的字段。
class MainWindow {
 public:
  MainWindow(Window& window, UiModel& model, UiActions& actions);
  ~MainWindow();
  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;

  // 建控件树并挂到窗口根控件；页面切换由左侧导航驱动
  void Build(Control& root);
  // 模型变化后重绘当前页与侧栏状态
  void Refresh();
  // 供托盘“打开日志”直接切页
  void ShowLogs();
  // 网络线程调用：弹配对码窗口并原地等待用户决定，aborted 置位时提前返回 false
  bool PromptPairing(const PairingRequest& request, const std::atomic<bool>& aborted);
  // 界面线程展示候选，按钮回调携带预览对应的修订号。
  void ShowMerge(const MergeCandidate& candidate);

 private:
  class Shell;
  std::unique_ptr<Shell> shell_;
};

}  // namespace bm
