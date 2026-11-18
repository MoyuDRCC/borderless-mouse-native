#include "app/pages/Page.h"

#include <algorithm>

#include "app/ui/Dpi.h"
#include "app/ui/Theme.h"
#include "app/ui/Window.h"

namespace bm {

int Page::ViewDpi() const { return Host() ? Host()->Dpi() : 96; }

int Page::Inset(int dpi) const { return Dpi::Scale(Theme::PageInset, dpi); }

int Page::ContentTop(int dpi) const { return Dpi::Scale(Theme::PageContentTop, dpi); }

int Page::ReadingWidth(int dpi) const {
  const int inset = Inset(dpi);
  return std::max(Dpi::Scale(280, dpi),
                  std::min(Dpi::Scale(Theme::MaxContentWidth, dpi), Bounds().Width() - (inset * 2)));
}

}  // namespace bm
