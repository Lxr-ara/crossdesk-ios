/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-20
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#include "platform/common/gui/game_mouse.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "rd_log.h"

namespace crossdesk::platform {
namespace {

bool g_active = false;
RECT g_previous_clip{};
bool g_had_clip = false;

RECT ClientRectOnScreen(HWND window) {
  RECT rect{};
  GetClientRect(window, &rect);
  POINT top_left{rect.left, rect.top};
  POINT bottom_right{rect.right, rect.bottom};
  ClientToScreen(window, &top_left);
  ClientToScreen(window, &bottom_right);
  return RECT{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
}

}  // namespace

bool BeginGameMouse(void* window) {
  const auto handle = static_cast<HWND>(window);
  if (!handle) return false;
  if (!g_active) {
    g_previous_clip = RECT{};
    g_had_clip = GetClipCursor(&g_previous_clip) != 0;
  }
  const auto rect = ClientRectOnScreen(handle);
  if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
  if (!ClipCursor(&rect)) {
    LOG_WARN("Game mouse: ClipCursor failed, error={}", GetLastError());
    return false;
  }
  g_active = true;
  return WarpGameMouseToCentre(window);
}

void EndGameMouse() {
  if (!g_active) return;
  ClipCursor(g_had_clip ? &g_previous_clip : nullptr);
  g_active = false;
}

bool WarpGameMouseToCentre(void* window) {
  const auto handle = static_cast<HWND>(window);
  if (!handle) return false;
  const auto rect = ClientRectOnScreen(handle);
  const int x = rect.left + (rect.right - rect.left) / 2;
  const int y = rect.top + (rect.bottom - rect.top) / 2;
  if (!SetCursorPos(x, y)) {
    LOG_WARN("Game mouse: SetCursorPos failed, error={}", GetLastError());
    return false;
  }
  return true;
}

bool GameMouseActive() { return g_active; }

}  // namespace crossdesk::platform

#else

namespace crossdesk::platform {

bool BeginGameMouse(void*) { return false; }
void EndGameMouse() {}
bool WarpGameMouseToCentre(void*) { return false; }
bool GameMouseActive() { return false; }

}  // namespace crossdesk::platform

#endif
