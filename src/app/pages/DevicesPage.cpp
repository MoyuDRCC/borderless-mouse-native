#include "app/pages/DevicesPage.h"

#include <memory>
#include <utility>
#include <vector>

#include "app/ui/controls/DeviceListView.h"
#include "app/ui/controls/FlatButton.h"
#include "app/ui/controls/Paint.h"
#include "app/dialogs/AddDeviceDialog.h"
#include "app/dialogs/DeviceSettingsDialog.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

namespace {

// 手动添加的对端固定走默认服务端口，与 .NET 版一致
constexpr int kServicePort = 35100;

std::wstring DisplayText(const DeviceRow& device) {
  return ToWide(device.alias.empty() ? device.name : device.alias);
}

// 只比较列表真正画出来的字段，模型每 500ms 刷新一次时不打断悬停与滚动
bool SameRow(const DeviceRow& left, const DeviceRow& right) {
  return left.id == right.id && left.name == right.name && left.address == right.address &&
         left.port == right.port && left.version == right.version && left.paired == right.paired &&
         left.connected == right.connected && left.connecting == right.connecting &&
         left.discovered == right.discovered &&
         left.controlling == right.controlling && left.controlled == right.controlled &&
         left.alias == right.alias;
}

}  // namespace

DevicesPage::DevicesPage(UiModel& model, UiActions& actions) : model_(model), actions_(actions) {
  auto list = std::make_unique<DeviceListView>();
  list_ = list.get();
  list_->onAdd = [this] { OpenAddDevice(); };
  list_->onConnect = [this](const std::string& id) {
    if (actions_.connection.connect) actions_.connection.connect(id);
  };
  list_->onDisconnect = [this](const std::string& id) {
    if (actions_.connection.disconnect) actions_.connection.disconnect(id);
  };
  list_->onControl = [this](const std::string& id) {
    if (actions_.connection.startControl) actions_.connection.startControl(id);
  };
  list_->onMirror = [this](const std::string& id) {
    if (actions_.connection.startMirror) actions_.connection.startMirror(id);
  };
  list_->onSettings = [this](const std::string& id) { OpenSettings(id); };
  list_->onUnpair = [this](const std::string& id) {
    if (actions_.connection.unpair) actions_.connection.unpair(id);
  };
  list_->onRemove = [this](const std::string& id) {
    if (actions_.connection.removeDevice) actions_.connection.removeDevice(id);
  };
  AddControl(std::move(list));

  auto add = std::make_unique<FlatButton>(L"＋ 添加设备", FlatButton::Style::Primary);
  add_ = add.get();
  add_->onClick = [this] { OpenAddDevice(); };
  AddControl(std::move(add));
}

void DevicesPage::OnLayout() {
  const int dpi = ViewDpi();
  const int inset = Inset(dpi);
  const int contentTop = ContentTop(dpi);
  const int width = std::max(Dpi::Scale(260, dpi), Bounds().Width() - (inset * 2));
  const int height = std::max(Dpi::Scale(140, dpi), Bounds().Height() - contentTop - inset);

  const int addTop = Dpi::Scale(40, dpi);
  const int addLeft = Bounds().Width() - inset - Dpi::Scale(104, dpi);
  add_->SetBounds(RectI{addLeft, addTop, addLeft + Dpi::Scale(104, dpi), addTop + Dpi::Scale(32, dpi)});
  list_->SetBounds(RectI{inset, contentTop, inset + width, contentTop + height});
}

void DevicesPage::OnTick() { Sync(false); }

void DevicesPage::Refresh(bool force) { Sync(force); }

void DevicesPage::Sync(bool force) {
  if (!force && shown_.size() == model_.devices.size()) {
    bool same = true;
    for (size_t index = 0; index < shown_.size(); ++index) {
      if (!SameRow(shown_[index], model_.devices[index])) {
        same = false;
        break;
      }
    }
    if (same) return;
  }
  shown_ = model_.devices;
  list_->SetDevices(shown_);
}

const DeviceRow* DevicesPage::FindDevice(const std::string& id) const {
  for (const DeviceRow& device : model_.devices) {
    if (device.id == id) return &device;
  }
  return nullptr;
}

void DevicesPage::OpenAddDevice() {
  Window* host = Host();
  if (!host || !host->Root()) return;

  if (!addDialog_) {
    auto dialog = std::make_unique<AddDeviceDialog>();
    addDialog_ = dialog.get();
    dialog->onAccepted = [this](const std::wstring& name, const std::wstring& address) {
      if (actions_.connection.addDevice) actions_.connection.addDevice(ToUtf8(name), ToUtf8(address), kServicePort);
    };
    host->Root()->AddControl(std::move(dialog));
  }
  addDialog_->Show();
}

void DevicesPage::OpenSettings(const std::string& id) {
  Window* host = Host();
  const DeviceRow* device = FindDevice(id);
  if (!host || !host->Root() || !device) return;

  if (!settingsDialog_) {
    auto dialog = std::make_unique<DeviceSettingsDialog>();
    settingsDialog_ = dialog.get();
    dialog->onAccepted = [this](const DeviceSettingsValues& values) {
      const DeviceRow* current = FindDevice(settingsTarget_);
      if (!current || !actions_.connection.saveDeviceSettings) return;
      DeviceRow updated = *current;
      updated.alias = ToUtf8(values.alias);
      updated.pointerDpi = values.pointerDpi;
      updated.clipboardEnabled = values.clipboard;
      updated.wheelReverse = values.wheelReverse;
      updated.wheelScalePercent = values.wheelScalePercent;
      actions_.connection.saveDeviceSettings(settingsTarget_, updated);
    };
    host->Root()->AddControl(std::move(dialog));
  }

  settingsTarget_ = id;
  settingsDialog_->SetDevice(DisplayText(*device), *device);
  settingsDialog_->Show();
}

void DevicesPage::Paint(PaintContext& ctx) {
  paint::Fill(ctx, LocalBounds(), Theme::Window);
  paint::PageHeader(ctx, L"设备",
                    L"连上后把鼠标推到相邻屏幕边缘即可过去；点“控制”立即接管，点“镜像”按位置同步");
}

}  // namespace bm
