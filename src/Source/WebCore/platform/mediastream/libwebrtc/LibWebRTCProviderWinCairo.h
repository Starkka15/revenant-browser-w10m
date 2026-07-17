/*
 * Copyright (C) 2026 Revenant (W10M ARM32-UWP WebKit).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DAMAGES
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE.
 */

#pragma once

#if USE(LIBWEBRTC)

#include "LibWebRTCProvider.h"

namespace WebCore {

// The base LibWebRTCProvider constructor is protected — it is meant to be
// subclassed per platform (Cocoa/GStreamer do). Revenant's WinCairo/W10M port
// needs no platform specialization for phase 1 (data channels + ICE + DTLS-SRTP,
// no media codecs): the base provider is fully functional. This subclass exists
// only to expose a public constructor so LibWebRTCProvider::create() can build it.
class LibWebRTCProviderWinCairo final : public LibWebRTCProvider {
public:
    LibWebRTCProviderWinCairo() = default;
    ~LibWebRTCProviderWinCairo() = default;
};

} // namespace WebCore

#endif // USE(LIBWEBRTC)
