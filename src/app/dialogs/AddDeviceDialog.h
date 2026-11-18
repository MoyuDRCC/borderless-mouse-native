#pragma once

#include <functional>
#include <string>

#include "app/dialogs/Dialog.h"

namespace bm {

class FlatButton;
class RoundedTextBox;

// 添加设备弹窗：填写对端 IP（端口固定 35100），设备名可留空
class AddDeviceDialog : public Dialog {
 public:
  AddDeviceDialog();

  // 每次打开都清空输入并聚焦 IP 框
  void Show() override;

  std::function<void(const std::wstring& name, const std::wstring& address)> onAccepted;

 protected:
  void LayoutContent(const RectI& card, int margin, int dpi) override;
  void PaintContent(PaintContext& ctx, const RectI& card, int margin, int dpi) override;

 private:
  void Accept();

  RoundedTextBox* name_ = nullptr;
  RoundedTextBox* address_ = nullptr;
  FlatButton* confirm_ = nullptr;
  FlatButton* cancel_ = nullptr;
};

}  // namespace bm
