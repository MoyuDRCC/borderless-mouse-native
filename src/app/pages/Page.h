#pragma once

#include "app/ui/Control.h"

namespace bm {

// 页面基类：五个页面共用的 DPI 与内容区度量，避免每页各算一遍 Theme 常量。
class Page : public Control {
 protected:
  int ViewDpi() const;
  // 内容区左右内缩
  int Inset(int dpi) const;
  // 标题下方的内容起点
  int ContentTop(int dpi) const;
  // 按最大阅读宽度夹取后的内容宽度
  int ReadingWidth(int dpi) const;
};

}  // namespace bm
