#include "app/pages/SettingsPage.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <utility>

#include "app/ui/controls/FlatButton.h"
#include "app/ui/controls/FlatComboBox.h"
#include "app/ui/controls/Paint.h"
#include "app/ui/controls/ToggleSwitch.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

namespace {

// 与 .NET 版相同的档位；界面显示值与生效值一致
constexpr int kDpiChoices[] = {400, 800, 1200, 1600, 2400, 3200, 6400};
constexpr int kLimitChoices[] = {1, 4, 8, 16, 32, 64};

int NearestIndex(const int* choices, int count, int value) {
  int best = 0;
  for (int index = 1; index < count; ++index) {
    if (std::abs(choices[index] - value) < std::abs(choices[best] - value)) best = index;
  }
  return best;
}

}  // namespace

// 滚动画布：绘制与滚轮都转回页面；内容裁剪由控件树本身完成
class SettingsPage::Canvas : public Control {
 public:
  explicit Canvas(SettingsPage& page) : page_(page) {}

  void Paint(PaintContext& ctx) override { page_.PaintCanvas(ctx); }
  void OnWheel(int delta, Point2i) override { page_.ScrollBy(delta); }
  bool OnMouseDown(Point2i local, MouseButton button) override {
    return page_.BeginScrollDrag(local, button);
  }
  void OnMouseMove(Point2i local) override { page_.DragScroll(local); }
  void OnMouseUp(Point2i, MouseButton) override { page_.EndScrollDrag(); }
  void OnMouseLeave() override { page_.EndScrollDrag(); }

 private:
  SettingsPage& page_;
};

SettingsPage::SettingsPage(UiModel& model, UiActions& actions) : model_(model), actions_(actions) {
  auto canvas = std::make_unique<Canvas>(*this);
  canvas_ = canvas.get();
  AddControl(std::move(canvas));

  auto autoStart = std::make_unique<ToggleSwitch>();
  autoStart_ = autoStart.get();
  autoStart_->onChanged = [this](bool checked) {
    if (!updating_ && actions_.app.setAutoStart) actions_.app.setAutoStart(checked);
  };
  canvas_->AddControl(std::move(autoStart));

  auto hideCursor = std::make_unique<ToggleSwitch>();
  hideCursor_ = hideCursor.get();
  hideCursor_->onChanged = [this](bool checked) {
    if (!updating_ && actions_.app.setHideCursor) actions_.app.setHideCursor(checked);
  };
  canvas_->AddControl(std::move(hideCursor));

  auto clipboard = std::make_unique<ToggleSwitch>();
  clipboard_ = clipboard.get();
  clipboard_->onChanged = [this](bool checked) {
    if (!updating_ && actions_.app.setClipboardSync) actions_.app.setClipboardSync(checked);
  };
  canvas_->AddControl(std::move(clipboard));

  auto mouseDpi = std::make_unique<FlatComboBox>();
  mouseDpi_ = mouseDpi.get();
  {
    std::vector<std::wstring> items;
    for (const int choice : kDpiChoices) items.push_back(std::to_wstring(choice));
    mouseDpi_->AddItems(std::move(items));
  }
  mouseDpi_->onSelectedIndexChanged = [this](int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kDpiChoices))) return;
    if (!updating_ && actions_.app.setMouseDpi) actions_.app.setMouseDpi(kDpiChoices[index]);
  };
  canvas_->AddControl(std::move(mouseDpi));

  auto limit = std::make_unique<FlatComboBox>();
  limit_ = limit.get();
  {
    std::vector<std::wstring> items;
    for (const int choice : kLimitChoices) items.push_back(std::to_wstring(choice) + L" MB");
    limit_->AddItems(std::move(items));
  }
  limit_->onSelectedIndexChanged = [this](int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kLimitChoices))) return;
    if (!updating_ && actions_.app.setClipboardLimit) actions_.app.setClipboardLimit(kLimitChoices[index]);
  };
  canvas_->AddControl(std::move(limit));

  auto exportButton = std::make_unique<FlatButton>(L"导出诊断", FlatButton::Style::Secondary);
  export_ = exportButton.get();
  export_->onClick = [this] {
    if (actions_.app.exportDiagnostics) actions_.app.exportDiagnostics();
  };
  canvas_->AddControl(std::move(exportButton));

  auto clearButton = std::make_unique<FlatButton>(L"清空日志", FlatButton::Style::Ghost);
  clear_ = clearButton.get();
  clear_->onClick = [this] {
    if (actions_.app.clearLogs) actions_.app.clearLogs();
  };
  canvas_->AddControl(std::move(clearButton));
}

int SettingsPage::ContentWidth(int dpi) const {
  const int inset = Inset(dpi);
  const int available = Bounds().Width() - Dpi::Scale(18, dpi) - (inset * 2);
  return std::max(Dpi::Scale(260, dpi), std::min(Dpi::Scale(Theme::MaxContentWidth, dpi), available));
}

void SettingsPage::OnLayout() {
  const int dpi = ViewDpi();
  const int contentTop = ContentTop(dpi);
  // 画布铺满页头以下的全部高度，超出的滚动内容由画布自身裁剪
  canvas_->SetBounds(RectI{0, contentTop, Bounds().Width(), Bounds().Height()});
  LayoutChildren();
}

void SettingsPage::LayoutChildren() {
  const int dpi = ViewDpi();
  const int inset = Inset(dpi);
  const int width = ContentWidth(dpi);
  const int left = inset;
  const int right = left + width;
  const int comboWidth = Dpi::Scale(124, dpi);

  labels_.clear();
  sections_.clear();
  separators_.clear();
  placed_.clear();

  // 开关行：左文案 + 右侧开关，说明文字在下一行
  const auto switchRow = [&](int top, Control* toggle, const wchar_t* caption, const wchar_t* note) {
    const Size2i logical = ToggleSwitch::LogicalSize();
    const int toggleWidth = Dpi::Scale(logical.width, dpi);
    const int toggleHeight = Dpi::Scale(logical.height, dpi);
    const int rowHeight = Dpi::Scale(22, dpi);
    const int toggleTop = top + ((rowHeight - toggleHeight) / 2);
    placed_.emplace_back(toggle, RectI{right - toggleWidth, toggleTop, right, toggleTop + toggleHeight});
    labels_.push_back(Label{caption, RectI{left, top, right - toggleWidth - Dpi::Scale(16, dpi), top + rowHeight},
                            paint::TextStyle::Body, Theme::Text, false});
    if (note != nullptr) {
      labels_.push_back(Label{note, RectI{left, top + Dpi::Scale(23, dpi), right, top + Dpi::Scale(47, dpi)},
                              paint::TextStyle::Caption, Theme::TextSecondary, false});
    }
  };

  // 下拉行：左文案 + 右侧下拉框
  const auto comboRow = [&](int top, Control* combo, const wchar_t* caption, const wchar_t* note) {
    const int comboHeight = Dpi::Scale(30, dpi);
    placed_.emplace_back(combo, RectI{right - comboWidth, top, right, top + comboHeight});
    labels_.push_back(Label{caption,
                            RectI{left, top + Dpi::Scale(5, dpi), right - comboWidth - Dpi::Scale(16, dpi),
                                  top + Dpi::Scale(25, dpi)},
                            paint::TextStyle::Body, Theme::Text, false});
    if (note != nullptr) {
      labels_.push_back(Label{note, RectI{left, top + Dpi::Scale(28, dpi), right, top + Dpi::Scale(52, dpi)},
                              paint::TextStyle::Caption, Theme::TextSecondary, false});
    }
  };

  int y = Dpi::Scale(8, dpi);

  sections_.emplace_back(L"常规", y);
  y += Dpi::Scale(30, dpi);
  switchRow(y, autoStart_, L"开机自动启动", L"登录 Windows 后自动在系统托盘后台运行。");
  y += Dpi::Scale(60, dpi);

  sections_.emplace_back(L"输入与指针", y);
  y += Dpi::Scale(30, dpi);
  comboRow(y, mouseDpi_, L"本机鼠标 DPI", L"只影响与对端设备的指针速度换算，不修改系统鼠标设置。");
  y += Dpi::Scale(64, dpi);
  separators_.push_back(y);
  y += Dpi::Scale(12, dpi);
  switchRow(y, hideCursor_, L"远控时隐藏本机光标",
            L"控制其他设备期间，停留在屏幕中央的本机光标不再显示形状。");
  y += Dpi::Scale(64, dpi);

  sections_.emplace_back(L"剪贴板", y);
  y += Dpi::Scale(30, dpi);
  switchRow(y, clipboard_, L"启用剪贴板同步", nullptr);
  y += Dpi::Scale(34, dpi);
  separators_.push_back(y);
  y += Dpi::Scale(12, dpi);
  comboRow(y, limit_, L"单条传输上限", L"文本与图片超过上限既不发送也不接收；文件按批次预算另算。");
  y += Dpi::Scale(66, dpi);

  sections_.emplace_back(L"诊断", y);
  y += Dpi::Scale(30, dpi);
  {
    const int exportWidth = Dpi::Scale(92, dpi);
    const int clearWidth = Dpi::Scale(82, dpi);
    const int buttonHeight = Dpi::Scale(30, dpi);
    const int exportLeft = right - exportWidth;
    placed_.emplace_back(export_, RectI{exportLeft, y, exportLeft + exportWidth, y + buttonHeight});
    const int clearLeft = exportLeft - Dpi::Scale(8, dpi) - clearWidth;
    placed_.emplace_back(clear_, RectI{clearLeft, y, clearLeft + clearWidth, y + buttonHeight});

    const int captionWidth =
        std::max(Dpi::Scale(120, dpi), width - exportWidth - clearWidth - Dpi::Scale(20, dpi));
    labels_.push_back(Label{L"日志与脱敏诊断包",
                            RectI{left, y + Dpi::Scale(4, dpi), left + captionWidth, y + Dpi::Scale(24, dpi)},
                            paint::TextStyle::Body, Theme::Text, false});
    labels_.push_back(Label{L"导出内容包含版本、设备与最近 200 行日志；设备名和用户目录会自动脱敏。",
                            RectI{left, y + Dpi::Scale(27, dpi), left + captionWidth, y + Dpi::Scale(51, dpi)},
                            paint::TextStyle::Caption, Theme::TextSecondary, false});
  }
  y += Dpi::Scale(66, dpi);

  sections_.emplace_back(L"关于", y);
  y += Dpi::Scale(30, dpi);
  labels_.push_back(Label{L"程序版本", RectI{left, y, left + Dpi::Scale(120, dpi), y + Dpi::Scale(24, dpi)},
                          paint::TextStyle::Body, Theme::TextSecondary, false});
  labels_.push_back(Label{version_.empty() ? L"—" : version_,
                          RectI{right - Dpi::Scale(160, dpi), y, right, y + Dpi::Scale(24, dpi)},
                          paint::TextStyle::Mono, Theme::Text, true});
  y += Dpi::Scale(30, dpi);
  separators_.push_back(y);
  y += Dpi::Scale(8, dpi);
  labels_.push_back(Label{L"协议版本", RectI{left, y, left + Dpi::Scale(120, dpi), y + Dpi::Scale(24, dpi)},
                          paint::TextStyle::Body, Theme::TextSecondary, false});
  labels_.push_back(Label{protocolText_,
                          RectI{right - Dpi::Scale(160, dpi), y, right, y + Dpi::Scale(24, dpi)},
                          paint::TextStyle::Mono, Theme::Text, true});
  y += Dpi::Scale(44, dpi);

  contentHeight_ = y;
  ApplyScroll();
}

void SettingsPage::ApplyScroll() {
  scroll_ = std::clamp(scroll_, 0, MaxScroll());
  const int offset = -scroll_;
  for (const auto& entry : placed_) {
    const RectI& content = entry.second;
    entry.first->SetBounds(RectI{content.left, content.top + offset, content.right, content.bottom + offset});
  }
}

int SettingsPage::MaxScroll() const {
  return std::max(0, contentHeight_ - canvas_->Bounds().Height());
}

bool SettingsPage::NeedsScroll() const {
  return contentHeight_ > canvas_->Bounds().Height() && canvas_->Bounds().Height() > 0;
}

RectI SettingsPage::TrackHitRect(int dpi) const {
  if (!NeedsScroll()) return RectI{};
  const int width = canvas_->Bounds().Width();
  return RectI{width - Dpi::Scale(18, dpi), 0, width, canvas_->Bounds().Height()};
}

RectI SettingsPage::ThumbBounds(int dpi) const {
  if (!NeedsScroll()) return RectI{};
  const int height = canvas_->Bounds().Height();
  const int trackHeight = std::max(1, height - Dpi::Scale(12, dpi));
  const int thumbHeight = std::max(Dpi::Scale(32, dpi),
                                   static_cast<int>(static_cast<double>(height) / contentHeight_ * trackHeight));
  const int travel = std::max(1, trackHeight - thumbHeight);
  const int thumbTop = Dpi::Scale(6, dpi) + static_cast<int>(
                                                static_cast<double>(scroll_) / std::max(1, MaxScroll()) * travel);
  const int thumbWidth = (hoverScroll_ || draggingScroll_) ? Dpi::Scale(7, dpi) : Dpi::Scale(5, dpi);
  const int left = canvas_->Bounds().Width() - thumbWidth - Dpi::Scale(6, dpi);
  return RectI{left, thumbTop, left + thumbWidth, thumbTop + thumbHeight};
}

void SettingsPage::ScrollBy(int delta) {
  if (!NeedsScroll()) return;
  const int step = Dpi::Scale(48, ViewDpi());
  const int next = std::clamp(scroll_ - (delta * step / 120), 0, MaxScroll());
  if (next == scroll_) return;
  scroll_ = next;
  ApplyScroll();
  canvas_->Invalidate();
}

bool SettingsPage::BeginScrollDrag(Point2i local, MouseButton button) {
  if (button != MouseButton::Left || !NeedsScroll()) return false;
  if (!TrackHitRect(ViewDpi()).Contains(local.x, local.y)) return false;
  draggingScroll_ = true;
  dragStartY_ = local.y;
  dragStartScroll_ = scroll_;
  canvas_->Invalidate();
  return true;
}

void SettingsPage::DragScroll(Point2i local) {
  const int dpi = ViewDpi();
  if (draggingScroll_) {
    const int height = canvas_->Bounds().Height();
    const int trackHeight = std::max(1, height - Dpi::Scale(12, dpi));
    const int thumbHeight = std::max(Dpi::Scale(32, dpi),
                                     static_cast<int>(static_cast<double>(height) / contentHeight_ * trackHeight));
    const int travel = std::max(1, trackHeight - thumbHeight);
    const int delta = local.y - dragStartY_;
    scroll_ = std::clamp(dragStartScroll_ + static_cast<int>(static_cast<double>(delta) / travel * MaxScroll()), 0,
                         MaxScroll());
    ApplyScroll();
    canvas_->Invalidate();
    return;
  }

  const bool over = NeedsScroll() && TrackHitRect(dpi).Contains(local.x, local.y);
  if (over == hoverScroll_) return;
  hoverScroll_ = over;
  canvas_->Invalidate();
}

void SettingsPage::EndScrollDrag() {
  if (!draggingScroll_ && !hoverScroll_) return;
  draggingScroll_ = false;
  hoverScroll_ = false;
  canvas_->Invalidate();
}

void SettingsPage::OnWheel(int delta, Point2i) { ScrollBy(delta); }

void SettingsPage::OnTick() { Sync(false); }

void SettingsPage::Refresh(bool force) { Sync(force); }

void SettingsPage::Sync(bool force) {
  const std::wstring version = model_.version.empty() ? L"—" : L"v" + ToWide(model_.version);
  const std::wstring protocol = L"v" + std::to_wstring(model_.protocolVersion);
  if (force || version != version_ || protocol != protocolText_) {
    version_ = version;
    protocolText_ = protocol;
    LayoutChildren();
    Invalidate();
  }

  updating_ = true;
  autoStart_->SetChecked(model_.settings.autoStart);
  hideCursor_->SetChecked(model_.settings.hideCursorWhileRemote);
  clipboard_->SetChecked(model_.settings.clipboardEnabled);
  mouseDpi_->SetSelectedIndex(
      NearestIndex(kDpiChoices, static_cast<int>(std::size(kDpiChoices)), model_.settings.mouseDpi));
  limit_->SetSelectedIndex(
      NearestIndex(kLimitChoices, static_cast<int>(std::size(kLimitChoices)), model_.settings.clipboardLimitMb));
  updating_ = false;
}

void SettingsPage::Paint(PaintContext& ctx) {
  paint::Fill(ctx, LocalBounds(), Theme::Window);
  paint::PageHeader(ctx, L"设置", L"本机偏好；窗口关闭后从系统托盘恢复");
}

void SettingsPage::PaintCanvas(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  const int offset = -scroll_;
  const int inset = Inset(dpi);
  const int width = ContentWidth(dpi);

  paint::Fill(ctx, LocalBounds(), Theme::Window);

  for (const auto& section : sections_) {
    paint::Text(ctx, section.first, paint::TextStyle::Section, inset, section.second + offset, Theme::Text);
    paint::Separator(ctx, inset, inset + width, section.second + offset + Dpi::Scale(22, dpi));
  }
  for (const int y : separators_) paint::Separator(ctx, inset, inset + width, y + offset);

  for (const Label& label : labels_) {
    const RectI bounds{label.bounds.left, label.bounds.top + offset, label.bounds.right,
                       label.bounds.bottom + offset};
    if (bounds.bottom < 0 || bounds.top > Bounds().Height()) continue;
    if (label.rightAligned) {
      // 右对齐：先量宽度再定左边界，避免控件库缺少右对齐文字入口
      const Size2i size = paint::Measure(ctx, label.text, label.style);
      const int textLeft = std::max(bounds.left, bounds.right - size.width);
      paint::TextIn(ctx, label.text, label.style,
                    RectI{textLeft, bounds.top, bounds.right + Dpi::Scale(2, dpi), bounds.bottom}, label.color);
    } else {
      paint::TextIn(ctx, label.text, label.style, bounds, label.color);
    }
  }

  if (!NeedsScroll()) return;
  const RectI thumb = ThumbBounds(dpi);
  const Gdiplus::Color color =
      draggingScroll_ ? Theme::TextSecondary : (hoverScroll_ ? Theme::BorderStrong : Theme::ToggleTrackOff);
  draw::FillRound(ctx.graphics, thumb, thumb.Width() / 2, color);
}

}  // namespace bm
