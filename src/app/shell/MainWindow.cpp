#include "app/shell/MainWindow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app/ui/controls/CaptionButtons.h"
#include "app/ui/controls/NavSidebar.h"
#include "app/ui/controls/Paint.h"
#include "app/dialogs/PairingDialog.h"
#include "app/dialogs/TopologyMergeDialog.h"
#include "app/pages/DevicesPage.h"
#include "app/pages/LayoutPage.h"
#include "app/pages/LogPage.h"
#include "app/pages/OverviewPage.h"
#include "app/pages/SettingsPage.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

namespace {

// 页面顺序与 NavSidebar::Section 的枚举顺序一一对应
constexpr int kPageCount = 5;
// 配对确认必须在会话握手超时（90 秒）之前返回，布局合并确认可以等更久
constexpr int kPairingWaitMs = 88000;
constexpr int kMergeWaitMs = 300000;

}  // namespace

class MainWindow::Shell {
 public:
  class View;

  Shell(Window& window, UiModel& model, UiActions& actions);

  void Build(Control& root);
  void Refresh();
  void ShowLogs();
  bool PromptPairing(const PairingRequest& request, const std::atomic<bool>& aborted);
  void ShowMerge(const MergeCandidate& candidate);

 private:
  // 跨线程决定的交接点：UI 线程写入，等待线程读取
  struct Decision {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool accepted = false;
  };

  void PumpJobs();
  void PresentPairing(const std::shared_ptr<Decision>& decision, PairingRequest request);
  bool WaitDecision(const std::shared_ptr<Decision>& decision, const std::atomic<bool>& aborted,
                    int timeoutMs);
  void FinishMerge(bool accepted);

  Window& window_;
  UiModel& model_;
  UiActions& actions_;
  View* view_ = nullptr;
  PairingDialog* pairing_ = nullptr;
  std::shared_ptr<Decision> activePairing_;
  std::vector<std::pair<std::shared_ptr<Decision>, PairingRequest>> pendingPairings_;
  TopologyMergeDialog* mergeDialog_ = nullptr;
  uint64_t mergeRevision_ = 0;
  std::chrono::steady_clock::time_point mergeDeadline_;

  std::mutex queueMutex_;
  std::vector<std::function<void()>> queue_;  // 其他线程投递到 UI 线程的工作
};

// 视图：侧栏 + 五个页面 + 标题按钮的唯一排版者，本身铺满根控件
class MainWindow::Shell::View : public Control {
 public:
  View(Shell& owner, UiModel& model, UiActions& actions);

  void Paint(PaintContext& ctx) override;
  void OnLayout() override;
  void OnTick() override;

  void ShowSection(NavSidebar::Section section);
  void RefreshPage(bool force);
  NavSidebar* Sidebar() { return sidebar_; }

 private:
  void SyncToParent();

  Shell& owner_;
  NavSidebar* sidebar_ = nullptr;
  CaptionButtons* caption_ = nullptr;
  OverviewPage* overview_ = nullptr;
  DevicesPage* devices_ = nullptr;
  LayoutPage* layout_ = nullptr;
  LogPage* log_ = nullptr;
  SettingsPage* settings_ = nullptr;
  Control* pages_[kPageCount]{};
  int current_ = 0;
};

MainWindow::Shell::View::View(Shell& owner, UiModel& model, UiActions& actions) : owner_(owner) {
  auto sidebar = std::make_unique<NavSidebar>();
  sidebar_ = sidebar.get();
  sidebar_->onSectionSelected = [this](NavSidebar::Section section) { ShowSection(section); };
  AddControl(std::move(sidebar));

  auto overview = std::make_unique<OverviewPage>(model, actions);
  overview->onShowLogs = [this] { ShowSection(NavSidebar::Section::Log); };
  overview_ = overview.get();
  pages_[0] = overview.get();
  AddControl(std::move(overview));

  auto devices = std::make_unique<DevicesPage>(model, actions);
  devices_ = devices.get();
  pages_[1] = devices.get();
  AddControl(std::move(devices));

  auto layout = std::make_unique<LayoutPage>(model, actions);
  layout_ = layout.get();
  pages_[2] = layout.get();
  AddControl(std::move(layout));

  auto log = std::make_unique<LogPage>(model, actions);
  log_ = log.get();
  pages_[3] = log.get();
  AddControl(std::move(log));

  auto settings = std::make_unique<SettingsPage>(model, actions);
  settings_ = settings.get();
  pages_[4] = settings.get();
  AddControl(std::move(settings));

  // 标题按钮最后加入：命中测试按倒序，它压住页面内容
  auto caption = std::make_unique<CaptionButtons>();
  caption_ = caption.get();
  AddControl(std::move(caption));

  for (Control* page : pages_) page->SetVisible(false);
  pages_[0]->SetVisible(true);
}

void MainWindow::Shell::View::SyncToParent() {
  Control* parent = Parent();
  if (parent == nullptr) return;
  const RectI target = parent->LocalBounds();
  if (!(Bounds() == target)) SetBounds(target);
}

void MainWindow::Shell::View::OnLayout() {
  const int dpi = Host() ? Host()->Dpi() : 96;
  const int width = Bounds().Width();
  const int height = Bounds().Height();

  // 侧栏固定 196 逻辑像素，页面填满剩余区域（对应 .NET 的 Dock.Left + Dock.Fill）
  const int sidebarWidth = Dpi::Scale(Theme::SidebarWidth, dpi);
  sidebar_->SetBounds(RectI{0, 0, sidebarWidth, height});
  for (Control* page : pages_) page->SetBounds(RectI{sidebarWidth, 0, width, height});

  const Size2i logical = CaptionButtons::LogicalSize();
  const int captionWidth = Dpi::Scale(logical.width, dpi);
  const int captionHeight = Dpi::Scale(logical.height, dpi);
  caption_->SetBounds(RectI{width - captionWidth, 0, width, captionHeight});
}

void MainWindow::Shell::View::OnTick() {
  SyncToParent();
  owner_.PumpJobs();
}

void MainWindow::Shell::View::Paint(PaintContext& ctx) {
  SyncToParent();
  paint::Fill(ctx, LocalBounds(), Theme::Window);
}

void MainWindow::Shell::View::ShowSection(NavSidebar::Section section) {
  const int index = static_cast<int>(section);
  if (index < 0 || index >= kPageCount) return;

  sidebar_->SetSelected(section);
  for (int page = 0; page < kPageCount; ++page) pages_[page]->SetVisible(page == index);
  current_ = index;
  RefreshPage(true);
  Invalidate();
}

void MainWindow::Shell::View::RefreshPage(bool force) {
  switch (current_) {
    case 1:
      devices_->Refresh(force);
      break;
    case 2:
      layout_->Refresh(force);
      break;
    case 3:
      log_->Refresh(force);
      break;
    case 4:
      settings_->Refresh(force);
      break;
    default:
      overview_->Refresh(force);
      break;
  }
}

MainWindow::Shell::Shell(Window& window, UiModel& model, UiActions& actions)
    : window_(window), model_(model), actions_(actions) {}

void MainWindow::Shell::Build(Control& root) {
  if (view_ != nullptr) return;

  auto view = std::make_unique<View>(*this, model_, actions_);
  view_ = view.get();
  root.AddControl(std::move(view));

  // 根控件此刻可能还没有尺寸（窗口尚未创建），先按客户区给一次，之后由 OnTick 跟随
  RectI target = root.LocalBounds();
  if (target.IsEmpty() && window_.Handle() != nullptr) {
    RECT client{};
    GetClientRect(window_.Handle(), &client);
    target = RectI{0, 0, client.right, client.bottom};
    root.SetBounds(target);
  }
  view_->SetBounds(target);
  view_->ShowSection(NavSidebar::Section::Overview);
}

void MainWindow::Shell::Refresh() {
  PumpJobs();
  if (view_ == nullptr) return;

  // 本机行不算连接数
  int connected = 0;
  for (const DeviceRow& device : model_.devices) {
    if (device.connected && device.id != model_.localId) ++connected;
  }
  const std::wstring status = connected > 0 ? L"已连接 " + std::to_wstring(connected) + L" 台" : L"未连接";
  const std::wstring version = model_.version.empty() ? std::wstring() : L"v" + ToWide(model_.version);
  view_->Sidebar()->SetStatus(status, connected > 0, version, connected);
  view_->Sidebar()->SetPendingMerge(!model_.merges.empty());
  view_->RefreshPage(false);
}

void MainWindow::Shell::ShowLogs() {
  if (view_ == nullptr) return;
  view_->ShowSection(NavSidebar::Section::Log);
}

void MainWindow::Shell::PumpJobs() {
  std::vector<std::function<void()>> jobs;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    jobs.swap(queue_);
  }
  for (const auto& job : jobs) job();
  if (activePairing_) {
    auto decision = activePairing_;
    std::lock_guard lock(decision->mutex);
    if (decision->done) {
      pairing_->Hide();
      activePairing_.reset();
    }
  }
  while (!activePairing_ && !pendingPairings_.empty()) {
    auto [decision, request] = std::move(pendingPairings_.front());
    pendingPairings_.erase(pendingPairings_.begin());
    PresentPairing(decision, std::move(request));
  }
  if (mergeRevision_ != 0 && std::chrono::steady_clock::now() >= mergeDeadline_) FinishMerge(false);
}

void MainWindow::Shell::PresentPairing(const std::shared_ptr<Decision>& decision, PairingRequest request) {
  Control* root = window_.Root();
  if (root == nullptr) return;
  {
    std::lock_guard lock(decision->mutex);
    if (decision->done) return;
  }

  if (pairing_ == nullptr) {
    auto dialog = std::make_unique<PairingDialog>();
    pairing_ = dialog.get();
    root->AddControl(std::move(dialog));
  }

  pairing_->SetRequest(request.peerName, request.code, request.peerFingerprint);
  activePairing_ = decision;
  pairing_->onResolved = [decision](bool accepted) {
    std::lock_guard<std::mutex> lock(decision->mutex);
    if (decision->done) return;
    decision->accepted = accepted;
    decision->done = true;
    decision->cv.notify_all();
  };

  // 窗口藏在托盘时也要能看到配对码
  if (!IsWindowVisible(window_.Handle())) window_.Show();
  pairing_->Show();
}

void MainWindow::Shell::ShowMerge(const MergeCandidate& candidate) {
  Control* root = window_.Root();
  if (!root) {
    if (actions_.layout.resolveMerge) actions_.layout.resolveMerge(candidate.revision, false);
    return;
  }

  if (mergeDialog_ == nullptr) {
    auto dialog = std::make_unique<TopologyMergeDialog>();
    mergeDialog_ = dialog.get();
    root->AddControl(std::move(dialog));
  }

  mergeRevision_ = candidate.revision;
  mergeDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kMergeWaitMs);
  mergeDialog_->SetCandidate(candidate, model_.localId);
  mergeDialog_->onResolved = [this](bool accepted) { FinishMerge(accepted); };

  if (!IsWindowVisible(window_.Handle())) window_.Show();
  mergeDialog_->Show();
}

void MainWindow::Shell::FinishMerge(bool accepted) {
  const uint64_t revision = std::exchange(mergeRevision_, 0);
  mergeDialog_->Hide();
  if (actions_.layout.resolveMerge) actions_.layout.resolveMerge(revision, accepted);
}

bool MainWindow::Shell::WaitDecision(const std::shared_ptr<Decision>& decision,
                                     const std::atomic<bool>& aborted, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

  // 网络/会话线程在这里等 UI 线程交结果；50ms 一次检查 aborted，对端放弃时立刻返回
  std::unique_lock<std::mutex> lock(decision->mutex);
  for (;;) {
    if (decision->done) return decision->accepted;
    if (aborted.load(std::memory_order_acquire) || std::chrono::steady_clock::now() >= deadline) {
      // 只撤销自己的确认请求，界面线程按请求回收对应弹窗。
      decision->done = true;
      return false;
    }
    decision->cv.wait_for(lock, std::chrono::milliseconds(50));
  }
}

bool MainWindow::Shell::PromptPairing(const PairingRequest& request, const std::atomic<bool>& aborted) {
  if (view_ == nullptr) return false;

  auto decision = std::make_shared<Decision>();
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.push_back([this, decision, request] { pendingPairings_.emplace_back(decision, request); });
  }
  return WaitDecision(decision, aborted, kPairingWaitMs);
}

MainWindow::MainWindow(Window& window, UiModel& model, UiActions& actions)
    : shell_(std::make_unique<Shell>(window, model, actions)) {}

MainWindow::~MainWindow() = default;

void MainWindow::Build(Control& root) { shell_->Build(root); }

void MainWindow::Refresh() { shell_->Refresh(); }

void MainWindow::ShowLogs() { shell_->ShowLogs(); }

bool MainWindow::PromptPairing(const PairingRequest& request, const std::atomic<bool>& aborted) {
  return shell_->PromptPairing(request, aborted);
}

void MainWindow::ShowMerge(const MergeCandidate& candidate) { shell_->ShowMerge(candidate); }

}  // namespace bm
