#pragma once

#include <functional>
#include <string>

#include "app/dialogs/Dialog.h"

namespace bm {

class FlatButton;

// 首次配对核对弹窗：展示对端名称、6 位配对码与公钥指纹。
// 只接收字符串，不依赖网络层类型；关闭后通过 onResolved 交回用户决定。
class PairingDialog : public Dialog {
 public:
  PairingDialog();

  void SetRequest(const std::string& peerName, const std::string& code, const std::string& fingerprint);

  std::function<void(bool accepted)> onResolved;

 protected:
  void LayoutContent(const RectI& card, int margin, int dpi) override;
  void PaintContent(PaintContext& ctx, const RectI& card, int margin, int dpi) override;

 private:
  void Resolve(bool accepted);

  std::wstring code_;
  std::wstring fingerprint_;
  FlatButton* allow_ = nullptr;
  FlatButton* cancel_ = nullptr;
};

}  // namespace bm
