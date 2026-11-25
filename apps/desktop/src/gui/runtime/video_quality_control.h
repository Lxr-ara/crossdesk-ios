/*
 * @Author: DI JUNKUN
 * @Date: 2026-11-21
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _VIDEO_QUALITY_CONTROL_H_
#define _VIDEO_QUALITY_CONTROL_H_

#include <string>

#include <video_quality_preset.h>

#include "minirtc.h"

namespace crossdesk {

// One live-reconfigure push of a picture tier to one connected peer.
//
// The three MiniRTC calls it makes are the same per-peer live entry points the
// preview-subscription path (peer_data_callbacks.cpp) already uses for
// bandwidth/resolution/cadence changes. They rewrite the outgoing encoder in
// place, so a tier switch never closes or re-establishes the connection.
//
// |wire_id| is the transport peer id the caller already resolved (see
// PeerEventHandler::WireId), not the GUI session key. |origin| only labels the
// log line ("manual" for a one-tap pick, "auto-degrade" for the ladder).
//
// Returns the change set that landed. applied == true means all three surfaces
// accepted the tier and the peer moved from one complete tier to the next.
video_quality::ChangeResult ApplyPeerVideoQualityPreset(
    MiniRtcPeerPtr* peer, const std::string& wire_id, int preset,
    const char* origin);

}  // namespace crossdesk

#endif
