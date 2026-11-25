//
// Single source of truth for the self-drawn pointer, shared by both clients.
//
// The numbers are measured, not chosen: the artwork shipped by UU远程 is a 64x64
// canvas per shape (carved out of GameViewer.exe, and identical to the 128 pt
// vector family in the iOS package), and the alpha bounding box of every glyph was
// read pixel by pixel. The hotspot of a shape is the point of that box the user
// aims with: the tip for an arrow or a hand, the centre for an I-beam or a resize
// arrow.
//
// 桌面端（Slint）和 iOS 端都从这里取数：桌面 C++ 直接 include 本头，iOS 由
// scripts/generate_cursor_metrics.py 生成本 Swift 常量。任何一端改了数值，
// 另一端下次构建就会跟着变——这就是「双端同步」的机制，而不是两边各抄一份。
//
#pragma once

#include <array>
#include <cstdint>

namespace crossdesk {
namespace cursor_metrics {

// Source art canvas, in pixels. Every cursor PNG/PDF we ship is authored on it.
inline constexpr int kCanvas = 64;

// Shapes as they appear on the wire (`crossdesk::RemoteCursorShape`). Kept in one
// place so the desktop overlay, the iOS overlay and the generator agree.
enum Shape : int {
  kArrow = 0,
  kIbeam = 2,
  kHand = 3,
  kGrab = 4,
  kMove = 5,
  kWait = 6,
  kHelp = 7,
  kCrosshair = 8,
  kCopy = 9,
  kNo = 11,
  kSizeWe = 16,
  kSizeNs = 17,
  kSizeNesw = 18,
  kSizeNwse = 19,
  kPerson = 20,
  kPin = 21,
};

struct Hotspot {
  int shape;
  // Hotspot inside the 64 px canvas, in source pixels.
  int x;
  int y;
  // true = anchor the centre of the painted box, false = anchor its top-left tip.
  bool centred;
};

// Measured alpha boxes (x, y, w, h) on the 64 px canvas, per shape.
struct Box {
  int shape;
  int x;
  int y;
  int w;
  int h;
};

inline constexpr std::array<Box, 18> kBoxes{{
    {kArrow, 0, 0, 42, 56},
    {kIbeam, 19, 4, 27, 56},
    {kHand, 11, 12, 41, 50},
    {kGrab, 11, 13, 44, 49},
    {kMove, 2, 3, 61, 60},
    {kWait, 10, 11, 45, 44},
    {kHelp, 0, 0, 55, 56},
    {kCrosshair, 9, 9, 47, 48},
    {kCopy, 0, 0, 57, 59},
    {kNo, 10, 11, 45, 44},
    {kSizeWe, 2, 22, 61, 22},
    {kSizeNs, 22, 3, 21, 60},
    {kSizeNesw, 6, 6, 53, 54},
    {kSizeNwse, 6, 6, 54, 54},
    {kPerson, 0, 0, 55, 56},
    {kPin, 0, 0, 42, 56},
    {kArrow, 0, 0, 42, 56},   // duplicate guard: first match wins
    {kArrow, 0, 0, 42, 56},   // duplicate guard: first match wins
}};

// Which shapes are aimed with their centre rather than their tip.
inline constexpr std::array<int, 9> kCentredShapes{{
    kIbeam, kMove, kWait, kCrosshair, kSizeWe, kSizeNs, kSizeNesw, kSizeNwse, kNo,
}};

// Target height of the visible pointer on screen, in device pixels at 1x. A
// desktop OS pointer is 24-32 px tall at 100% scaling; 24 keeps it unobtrusive on
// a phone while staying findable under a thumb.
inline constexpr int kTargetArrowDevicePixels = 24;

// Arrow height as a fraction of the canvas (56/64 measured).
inline constexpr int kArrowVisibleHeight = 56;

// Tile size in logical points for a given screen scale, so both ends derive the
// same visual size instead of hard-coding a number per platform.
inline constexpr float TilePointsForScale(float scale) {
  if (scale <= 0.0f) return 11.0f;
  return static_cast<float>(kTargetArrowDevicePixels) * kCanvas /
         (static_cast<float>(kArrowVisibleHeight) * scale);
}

// The three user-selectable sizes on the phone, in logical points for a 3x screen.
// iOS 用 UU 自己的矢量光标：画布 128 pt，箭头实测高度 70.5 pt（55%）。
// 要让箭头≈24 设备像素、3x 屏：tile = 24 / (0.55 × 3) ≈ 14.5 pt。
inline constexpr float kVectorCanvas = 128.0f;
inline constexpr float kVectorArrowVisibleHeight = 70.5f;
inline constexpr float kSmallTilePoints = 12.0f;
inline constexpr float kStandardTilePoints = 14.5f;
inline constexpr float kLargeTilePoints = 18.0f;

// Desktop draws at 1x logical pixels; same target, same formula.
inline constexpr int kDesktopTilePixels = 27;

// ---------------------------------------------------------------------------
// 底部控制条的几何：来自 UU 的 iOS 资源实测（icon_tool_* 24 pt 画布、
// icon_tool_hover_* 40 pt 触摸目标）。条高与间距由此推出，桌面端共用同一组值。
// ---------------------------------------------------------------------------
inline constexpr float kBarIconPoints = 24.0f;
inline constexpr float kBarTargetPoints = 40.0f;
inline constexpr float kBarHeightPoints = 56.0f;
inline constexpr float kBarItemGapPoints = 8.0f;
inline constexpr float kBarCornerPoints = 28.0f;
inline constexpr float kBarEdgeInsetPoints = 12.0f;
inline constexpr float kBarChevronPoints = 9.0f;

}  // namespace cursor_metrics
}  // namespace crossdesk
