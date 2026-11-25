/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-20
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 *
 * Pointer capture for the game-mouse mode. The controller keeps sending pixel
 * deltas instead of absolute coordinates; the OS pointer is confined to the
 * remote view and re-centred whenever it reaches an edge, so the deltas never
 * run out of window. Non-Windows builds keep the same interface as no-ops.
 */

#ifndef _GAME_MOUSE_H_
#define _GAME_MOUSE_H_

namespace crossdesk::platform {

// Confines the pointer to `window`'s client area and centres it.
bool BeginGameMouse(void* window);
// Releases the confinement and restores the previous clip state.
void EndGameMouse();
// Re-centres the confined pointer; call when it approaches a view edge.
bool WarpGameMouseToCentre(void* window);
bool GameMouseActive();

}  // namespace crossdesk::platform

#endif
