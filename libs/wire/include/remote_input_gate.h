//
// Host-side gate for inbound remote input (UU远程 calls this 观看模式).
//
// The remote session owns `enable_mouse_control_`, but nothing used to read it, so
// the on-screen switch could be flipped while the injector kept clicking. Every
// injector now asks this gate right before it touches SendInput / libinput, which
// makes the switch authoritative and keeps a single source of truth for both ends.
//
#pragma once

#include <atomic>

namespace crossdesk {

inline std::atomic<bool>& RemoteInputGate() {
  static std::atomic<bool> gate{true};
  return gate;
}

// true = remote input may be injected, false = the host is watching only.
inline bool RemoteInputAllowed() {
  return RemoteInputGate().load(std::memory_order_relaxed);
}

inline void SetRemoteInputAllowed(bool allowed) {
  RemoteInputGate().store(allowed, std::memory_order_relaxed);
}

}  // namespace crossdesk
