#pragma once

#include <functional>
#include <string>

#include "app/dialogs/Dialog.h"

namespace bm {

class FlatButton;

// 通用确认弹窗：一段后果说明 + 取消 / 确认，用于撤销布局这类不能回退的操作
class ConfirmDialog : public Dialog {
 public:
  ConfirmDialog();

  // 标题与说明由调用方在打开前给出；确认后走 onConfirmed
  void Ask(std::wstring title, std::wstring description);

  std::function<void()> onConfirmed;

 protected:
  void LayoutContent(const RectI& card, int margin, int dpi) override;

 private:
  FlatButton* confirm_ = nullptr;
  FlatButton* cancel_ = nullptr;
};

}  // namespace bm
