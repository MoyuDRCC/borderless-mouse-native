#pragma once

#include "app/ui/GdiPlusSetup.h"

// 集中管理配色、圆角与字号：所有自绘控件从这里取值。
// 采用“单层干净画布 + 细分隔线 + 柔和语义块”的扁平设计，不使用嵌套卡片边框。
// 字号一律是 96 DPI 下的逻辑像素，由 Dpi::Scale 换算成物理像素，避免在 GDI 里被二次折算。
namespace bm::Theme {

// 基础画布与表面层级
inline const Gdiplus::Color Window(255, 0xFF, 0xFF, 0xFF);
inline const Gdiplus::Color Sidebar(255, 0xF4, 0xF5, 0xF7);
inline const Gdiplus::Color SidebarHover(255, 0xE9, 0xEC, 0xF0);
inline const Gdiplus::Color SurfaceSubtle(255, 0xF6, 0xF7, 0xF9);
inline const Gdiplus::Color RowHover(255, 0xF2, 0xF4, 0xF8);

// 纯净主表面（兼容旧引用）
inline const Gdiplus::Color Card(255, 0xFF, 0xFF, 0xFF);

// 文字阶梯
inline const Gdiplus::Color Text(255, 0x18, 0x1A, 0x1F);
inline const Gdiplus::Color TextSecondary(255, 0x5C, 0x62, 0x6E);
inline const Gdiplus::Color TextTertiary(255, 0x8D, 0x94, 0xA0);

// 分隔线与边框
inline const Gdiplus::Color Separator(255, 0xEC, 0xEE, 0xF2);
inline const Gdiplus::Color Border(255, 0xDC, 0xE0, 0xE6);
inline const Gdiplus::Color BorderStrong(255, 0xC0, 0xC6, 0xD0);

// 品牌强调色
inline const Gdiplus::Color Accent(255, 0x25, 0x63, 0xEB);
inline const Gdiplus::Color AccentHover(255, 0x1D, 0x4E, 0xD8);
inline const Gdiplus::Color AccentSoft(255, 0xEF, 0xF6, 0xFF);
inline const Gdiplus::Color AccentBorder(255, 0xBF, 0xDB, 0xFE);

// 语义色组（前景 + 柔和水彩底色）
inline const Gdiplus::Color Success(255, 0x15, 0x80, 0x3D);
inline const Gdiplus::Color SuccessSoft(255, 0xEC, 0xFD, 0xF3);
inline const Gdiplus::Color Warning(255, 0xB4, 0x53, 0x09);
inline const Gdiplus::Color WarningSoft(255, 0xFF, 0xFB, 0xEB);
inline const Gdiplus::Color Danger(255, 0xDC, 0x26, 0x26);
inline const Gdiplus::Color DangerHover(255, 0xB9, 0x1C, 0x1C);
inline const Gdiplus::Color DangerSoft(255, 0xFE, 0xF2, 0xF2);

// 开关轨道
inline const Gdiplus::Color ToggleTrackOff(255, 0xCB, 0xD1, 0xDA);
inline const Gdiplus::Color ToggleTrackHover(255, 0xB5, 0xBD, 0xC9);

// 控件与容器圆角（逻辑像素）
constexpr int RadiusControl = 6;
constexpr int RadiusSurface = 8;

// 侧栏宽度与内容区阅读限宽（逻辑像素）
constexpr int SidebarWidth = 196;
constexpr int MaxContentWidth = 760;

// 字号阶梯（96 DPI 逻辑像素）
constexpr float TitlePx = 22.0f;
constexpr float SectionPx = 14.0f;
constexpr float BodyPx = 13.5f;
constexpr float CaptionPx = 12.0f;
constexpr float MicroPx = 11.0f;

// 无缝标题栏与页面栅格（逻辑像素）：窗口命中测试与页面排版共用同一套数字
constexpr int CaptionHeight = 34;      // 顶部标题条高度，也是拖动窗口的高度
constexpr int SidebarBrandHeight = 56; // 侧栏品牌区，同样可拖动窗口
constexpr int PageInset = 28;          // 页面左右边距
constexpr int PageTitleY = 36;         // 页标题顶边
constexpr int PageSubtitleY = 65;      // 副标题顶边
constexpr int PageDividerY = 92;       // 页头分隔线
constexpr int PageContentTop = 104;    // 内容区起点

}  // namespace bm::Theme
