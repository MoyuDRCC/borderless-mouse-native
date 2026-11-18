#include "app/pages/OverviewPage.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <utility>

#include "app/ui/controls/FlatButton.h"
#include "app/ui/controls/LogList.h"
#include "app/ui/controls/Paint.h"
#include "app/ui/controls/ToggleSwitch.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"
#include "core/clipboard/ClipboardContent.h"

namespace bm {

namespace {

// 概览只展示最近几条事件，完整日志在日志页
constexpr size_t kRecentItems = 6;

}  // namespace

OverviewPage::OverviewPage(UiModel& model, UiActions& actions) : model_(model), actions_(actions) {
  auto sharing = std::make_unique<ToggleSwitch>();
  sharing_ = sharing.get();
  sharing_->onChanged = [this](bool checked) {
    if (!updating_ && actions_.app.setSharing) actions_.app.setSharing(checked);
  };
  AddControl(std::move(sharing));

  auto clipboard = std::make_unique<ToggleSwitch>();
  clipboard_ = clipboard.get();
  clipboard_->onChanged = [this](bool checked) {
    if (!updating_ && actions_.app.setClipboardSync) actions_.app.setClipboardSync(checked);
  };
  AddControl(std::move(clipboard));

  auto copy = std::make_unique<FlatButton>(L"复制", FlatButton::Style::Ghost);
  copy_ = copy.get();
  copy_->onClick = [this] { CopyAddress(); };
  AddControl(std::move(copy));

  auto more = std::make_unique<FlatButton>(L"查看全部 →", FlatButton::Style::Ghost);
  more_ = more.get();
  more_->onClick = [this] {
    if (onShowLogs) onShowLogs();
  };
  AddControl(std::move(more));

  auto recent = std::make_unique<LogList>(true);
  recent_ = recent.get();
  AddControl(std::move(recent));
}

void OverviewPage::OnLayout() {
  const int dpi = ViewDpi();
  const int inset = Inset(dpi);
  const int width = ReadingWidth(dpi);
  const int contentTop = ContentTop(dpi);

  // 状态控制条：无边框柔和底色块
  const int heroTop = contentTop + Dpi::Scale(4, dpi);
  heroRect_ = RectI{inset, heroTop, inset + width, heroTop + Dpi::Scale(96, dpi)};

  const Size2i toggle = ToggleSwitch::LogicalSize();
  const int toggleWidth = Dpi::Scale(toggle.width, dpi);
  const int toggleHeight = Dpi::Scale(toggle.height, dpi);
  const int toggleLeft = heroRect_.right - Dpi::Scale(20, dpi) - toggleWidth;
  const int row1 = heroRect_.top + Dpi::Scale(20, dpi);
  const int row2 = heroRect_.top + Dpi::Scale(56, dpi);
  sharing_->SetBounds(RectI{toggleLeft, row1, toggleLeft + toggleWidth, row1 + toggleHeight});
  clipboard_->SetBounds(RectI{toggleLeft, row2, toggleLeft + toggleWidth, row2 + toggleHeight});

  // 本机信息三列：设备名 34%、地址 44%、端口 22%
  const int infoTop = heroRect_.bottom + Dpi::Scale(56, dpi);
  infoRect_ = RectI{inset, infoTop, inset + width, infoTop + Dpi::Scale(56, dpi)};
  const int col1Width = static_cast<int>(width * 0.34);
  const int col2Width = static_cast<int>(width * 0.44);
  const int ipColumnRight = infoRect_.left + col1Width + col2Width;
  const int copyTop = infoRect_.top + Dpi::Scale(4, dpi);
  copy_->SetBounds(RectI{ipColumnRight - Dpi::Scale(68, dpi), copyTop,
                         ipColumnRight - Dpi::Scale(12, dpi), copyTop + Dpi::Scale(22, dpi)});

  // 最近事件
  recentHeaderY_ = infoRect_.bottom + Dpi::Scale(34, dpi);
  const int moreTop = recentHeaderY_ - Dpi::Scale(4, dpi);
  more_->SetBounds(RectI{inset + width - Dpi::Scale(92, dpi), moreTop, inset + width,
                         moreTop + Dpi::Scale(26, dpi)});

  const int listTop = recentHeaderY_ + Dpi::Scale(28, dpi);
  const int listHeight = std::max(Dpi::Scale(110, dpi), Bounds().Height() - listTop - inset);
  recent_->SetBounds(RectI{inset, listTop, inset + width, listTop + listHeight});
}

void OverviewPage::OnTick() {
  if (copyResetTicks_ > 0 && --copyResetTicks_ == 0) ResetCopyLabel();
  Sync(false);
}

void OverviewPage::Refresh(bool force) { Sync(force); }

void OverviewPage::Sync(bool force) {
  std::wstring name = model_.localName.empty() ? L"—" : ToWide(model_.localName);
  std::wstring address = model_.localAddress.empty() ? L"—" : ToWide(model_.localAddress);
  std::wstring port = std::to_wstring(model_.port);
  std::wstring status = model_.statusText.empty() ? L"未连接" : ToWide(model_.statusText);
  bool connected = false;
  for (const DeviceRow& device : model_.devices) {
    if (device.connected) {
      connected = true;
      break;
    }
  }

  updating_ = true;
  sharing_->SetChecked(model_.settings.sharingEnabled);
  clipboard_->SetChecked(model_.settings.clipboardEnabled);
  updating_ = false;

  const bool localChanged = force || name != localName_ || address != localAddress_ || port != localPort_;
  const bool statusChanged = force || status != statusText_ || connected != connected_;
  if (localChanged) {
    localName_ = std::move(name);
    localAddress_ = std::move(address);
    localPort_ = std::move(port);
    // 没有可用地址时不提供复制
    copy_->SetVisible(localAddress_ != L"—" && localAddress_ != L"未联网");
    Invalidate();
  }
  if (statusChanged) {
    statusText_ = std::move(status);
    connected_ = connected;
    Invalidate();
  }

  const size_t count = model_.events.size();
  const uint64_t tail = count > 0 ? model_.events[count - 1].timeMs : 0;
  if (force || count != recentCount_ || tail != recentTail_) {
    recentCount_ = count;
    recentTail_ = tail;
    std::vector<LogEntry> entries;
    const size_t first = count > kRecentItems ? count - kRecentItems : 0;
    entries.assign(model_.events.begin() + static_cast<ptrdiff_t>(first), model_.events.end());
    recent_->SetEntries(std::move(entries));
    recent_->SetVisible(count > 0);
    if (recent_->Bounds().Height() > 0) Invalidate();
  }
}

void OverviewPage::Paint(PaintContext& ctx) {
  paint::Fill(ctx, LocalBounds(), Theme::Window);
  paint::PageHeader(ctx, L"概览", L"本机状态、核心共享开关与最近事件");
  PaintHero(ctx);
  PaintLocalInfo(ctx);
  PaintRecent(ctx);
}

void OverviewPage::PaintHero(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  if (heroRect_.IsEmpty()) return;

  draw::FillRound(ctx.graphics, heroRect_, Dpi::Scale(Theme::RadiusSurface, dpi),
                  connected_ ? Theme::AccentSoft : Theme::SurfaceSubtle);

  const int pad = Dpi::Scale(20, dpi);
  const int dotSize = Dpi::Scale(10, dpi);
  const int dotTop = heroRect_.top + Dpi::Scale(26, dpi);
  draw::FillRound(ctx.graphics,
                  RectI{heroRect_.left + pad, dotTop, heroRect_.left + pad + dotSize, dotTop + dotSize},
                  dotSize / 2, connected_ ? Theme::Success : Theme::TextTertiary);

  // 右侧开关块左侧的竖向微分割线
  const int dividerX = heroRect_.right - Dpi::Scale(204, dpi);
  const int lineWidth = std::max(1, Dpi::Scale(1, dpi));
  draw::FillRect(ctx.graphics,
                 RectI{dividerX, heroRect_.top + Dpi::Scale(18, dpi), dividerX + lineWidth,
                       heroRect_.bottom - Dpi::Scale(18, dpi)},
                 connected_ ? Theme::AccentBorder : Theme::Border);

  const HFONT statusFont = Dpi::Font(16.0f, true, dpi);
  const int statusLeft = heroRect_.left + pad + dotSize + Dpi::Scale(10, dpi);
  const int statusRight = std::max(statusLeft + Dpi::Scale(80, dpi), dividerX - Dpi::Scale(12, dpi));
  draw::TextWithin(ctx, statusText_, statusFont,
                   RectI{statusLeft, heroRect_.top + Dpi::Scale(19, dpi), statusRight,
                         heroRect_.top + Dpi::Scale(45, dpi)},
                   Theme::Text);

  const std::wstring hint = connected_ ? L"将鼠标移至相邻屏幕边缘即可跨设备控制"
                                       : L"在“设备”页连接其他电脑，即可开始无界跨屏";
  draw::TextWithin(ctx, hint, paint::Font(paint::TextStyle::Caption, dpi),
                   RectI{heroRect_.left + pad, heroRect_.top + Dpi::Scale(52, dpi), statusRight,
                         heroRect_.top + Dpi::Scale(72, dpi)},
                   Theme::TextSecondary);

  // 开关右侧的文字标签与开关同高
  const int labelLeft = dividerX + Dpi::Scale(18, dpi);
  const RectI sharingBounds = sharing_->Bounds();
  const RectI clipboardBounds = clipboard_->Bounds();
  const int labelRight = std::max(labelLeft + Dpi::Scale(40, dpi), sharingBounds.left - Dpi::Scale(8, dpi));
  draw::TextWithin(ctx, L"键鼠共享", paint::Font(paint::TextStyle::Body, dpi),
                   RectI{labelLeft, sharingBounds.top - Dpi::Scale(1, dpi), labelRight,
                         sharingBounds.bottom + Dpi::Scale(1, dpi)},
                   Theme::Text);
  draw::TextWithin(ctx, L"剪贴板同步", paint::Font(paint::TextStyle::Body, dpi),
                   RectI{labelLeft, clipboardBounds.top - Dpi::Scale(1, dpi), labelRight,
                         clipboardBounds.bottom + Dpi::Scale(1, dpi)},
                   Theme::Text);
}

void OverviewPage::PaintLocalInfo(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  if (infoRect_.IsEmpty()) return;

  paint::Text(ctx, L"本机信息", paint::TextStyle::Section, infoRect_.left, infoRect_.top - Dpi::Scale(28, dpi),
              Theme::Text);

  const int col1Width = static_cast<int>(infoRect_.Width() * 0.34);
  const int col2Width = static_cast<int>(infoRect_.Width() * 0.44);
  const int col1X = infoRect_.left;
  const int col2X = infoRect_.left + col1Width + Dpi::Scale(16, dpi);
  const int col3X = infoRect_.left + col1Width + col2Width + Dpi::Scale(16, dpi);

  const int lineWidth = std::max(1, Dpi::Scale(1, dpi));
  draw::FillRect(ctx.graphics,
                 RectI{infoRect_.left + col1Width, infoRect_.top + Dpi::Scale(6, dpi),
                       infoRect_.left + col1Width + lineWidth, infoRect_.bottom - Dpi::Scale(8, dpi)},
                 Theme::Separator);
  draw::FillRect(ctx.graphics,
                 RectI{infoRect_.left + col1Width + col2Width, infoRect_.top + Dpi::Scale(6, dpi),
                       infoRect_.left + col1Width + col2Width + lineWidth,
                       infoRect_.bottom - Dpi::Scale(8, dpi)},
                 Theme::Separator);

  const int labelY = infoRect_.top + Dpi::Scale(4, dpi);
  const int valueY = infoRect_.top + Dpi::Scale(26, dpi);
  paint::Text(ctx, L"设备名称", paint::TextStyle::Caption, col1X, labelY, Theme::TextSecondary);
  paint::TextIn(ctx, localName_, paint::TextStyle::BodyBold,
                RectI{col1X, valueY, col1X + std::max(Dpi::Scale(40, dpi), col1Width - Dpi::Scale(14, dpi)),
                      valueY + Dpi::Scale(22, dpi)},
                Theme::Text);

  paint::Text(ctx, L"局域网地址", paint::TextStyle::Caption, col2X, labelY, Theme::TextSecondary);
  paint::TextIn(ctx, localAddress_, paint::TextStyle::Mono,
                RectI{col2X, valueY, col2X + std::max(Dpi::Scale(40, dpi), col2Width - Dpi::Scale(28, dpi)),
                      valueY + Dpi::Scale(22, dpi)},
                Theme::Text);

  paint::Text(ctx, L"监听端口", paint::TextStyle::Caption, col3X, labelY, Theme::TextSecondary);
  paint::TextIn(ctx, localPort_, paint::TextStyle::Mono,
                RectI{col3X, valueY, std::max(col3X + Dpi::Scale(40, dpi), infoRect_.right),
                      valueY + Dpi::Scale(22, dpi)},
                Theme::Text);

  paint::Separator(ctx, infoRect_.left, infoRect_.right, infoRect_.bottom + Dpi::Scale(8, dpi));
}

void OverviewPage::PaintRecent(PaintContext& ctx) {
  const int dpi = ctx.dpi;
  paint::Text(ctx, L"最近事件", paint::TextStyle::Section, recent_->Bounds().left, recentHeaderY_, Theme::Text);
  if (recentCount_ == 0) {
    paint::Text(ctx, L"暂无运行事件", paint::TextStyle::Caption, recent_->Bounds().left + Dpi::Scale(4, dpi),
                recent_->Bounds().top + Dpi::Scale(8, dpi), Theme::TextTertiary);
  }
}

void OverviewPage::CopyAddress() {
  if (localAddress_ == L"—" || localAddress_ == L"未联网" || localAddress_.empty()) return;
  const Window* host = Host();
  bool copied = false;
  try {
    copied = host && ClipboardWriter::WriteLocalText(host->Handle(), localAddress_);
  } catch (const std::exception&) {
  }
  copy_->SetText(copied ? L"已复制" : L"复制失败");
  copyResetTicks_ = 3;  // 三个 500ms 滴答后恢复文案
}

void OverviewPage::ResetCopyLabel() { copy_->SetText(L"复制"); }

}  // namespace bm
