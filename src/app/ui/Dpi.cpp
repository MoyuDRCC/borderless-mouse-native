#include "app/ui/Dpi.h"

#include <string.h>

#include <cstdint>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace bm {

RectI Intersect(const RectI& a, const RectI& b) {
  RectI result{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
               std::min(a.bottom, b.bottom)};
  if (result.right < result.left) result.right = result.left;
  if (result.bottom < result.top) result.bottom = result.top;
  return result;
}

namespace {

// 字体族只在首次使用时解析一次：正文取消息框字体，等宽族按优先级挑第一个已安装的
struct Families {
  std::wstring text;
  std::wstring mono;
};

int CALLBACK CollectFamily(const LOGFONTW* logFont, const TEXTMETRICW*, DWORD, LPARAM param) {
  auto* names = reinterpret_cast<std::vector<std::wstring>*>(param);
  names->emplace_back(logFont->lfFaceName);
  return 1;  // 继续枚举
}

Families ResolveFamilies() {
  Families families{L"Segoe UI", L"Consolas"};

  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) &&
      metrics.lfMessageFont.lfFaceName[0] != L'\0') {
    families.text = metrics.lfMessageFont.lfFaceName;
  }

  std::vector<std::wstring> installed;
  LOGFONTW filter{};
  filter.lfCharSet = DEFAULT_CHARSET;
  if (HDC dc = GetDC(nullptr)) {
    EnumFontFamiliesExW(dc, &filter, CollectFamily, reinterpret_cast<LPARAM>(&installed), 0);
    ReleaseDC(nullptr, dc);
  }

  for (const wchar_t* preferred : {L"Cascadia Mono", L"Cascadia Code", L"Consolas"}) {
    for (const std::wstring& name : installed) {
      if (_wcsicmp(name.c_str(), preferred) == 0) {
        families.mono = name;
        return families;
      }
    }
  }
  return families;
}

const Families& CachedFamilies() {
  static const Families families = ResolveFamilies();
  return families;
}

std::unordered_map<uint64_t, HFONT>& FontCache() {
  static std::unordered_map<uint64_t, HFONT> cache;
  return cache;
}

// 缓存键：等宽标记 + 字重 + 像素高度；物理高度已含 dpi，跨显示器切换自然命中不同键
uint64_t FontKey(int pixelHeight, int weight, bool mono) {
  return (static_cast<uint64_t>(mono ? 1 : 0) << 32) | (static_cast<uint64_t>(weight) << 16) |
         static_cast<uint64_t>(pixelHeight);
}

HFONT CachedFont(const std::wstring& family, float logicalPx, bool bold, int dpi, bool mono) {
  const int pixelHeight =
      std::max(1, static_cast<int>(std::lround(logicalPx * static_cast<float>(dpi) / 96.0f)));
  const int weight = bold ? FW_BOLD : FW_NORMAL;
  const uint64_t key = FontKey(pixelHeight, weight, mono);

  auto& cache = FontCache();
  const auto found = cache.find(key);
  if (found != cache.end()) return found->second;

  LOGFONTW logFont{};
  logFont.lfHeight = -pixelHeight;  // 负高度：按字符像素高度指定，与 .NET 的 GraphicsUnit.Pixel 一致
  logFont.lfWeight = weight;
  logFont.lfCharSet = DEFAULT_CHARSET;
  logFont.lfQuality = CLEARTYPE_QUALITY;
  wcsncpy_s(logFont.lfFaceName, LF_FACESIZE, family.c_str(), _TRUNCATE);

  const HFONT font = CreateFontIndirectW(&logFont);
  cache.emplace(key, font);
  return font;
}

}  // namespace

namespace Dpi {

int Scale(int logicalPx, int dpi) { return MulDiv(logicalPx, dpi, 96); }

float ScaleF(float logicalPx, int dpi) { return logicalPx * static_cast<float>(dpi) / 96.0f; }

HFONT Font(float logicalPx, bool bold, int dpi) {
  return CachedFont(CachedFamilies().text, logicalPx, bold, dpi, false);
}

HFONT MonoFont(float logicalPx, bool bold, int dpi) {
  return CachedFont(CachedFamilies().mono, logicalPx, bold, dpi, true);
}

void ReleaseFontCache() {
  for (const auto& entry : FontCache()) DeleteObject(entry.second);
  FontCache().clear();
}

}  // namespace Dpi
}  // namespace bm
