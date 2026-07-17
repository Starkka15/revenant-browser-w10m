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

#include "config.h"
#include "LibWebRTCProviderWinCairo.h"

#if USE(LIBWEBRTC)

// LibWebRTCProvider holds a RefPtr<LibWebRTCAudioModule> member; constructing the
// provider in create() needs the complete type to instantiate/destroy that RefPtr.
#include "LibWebRTCAudioModule.h"

// rtc::InitializeSSL() must run once before any peer connection is created — it does
// boringssl's global library init + seeds the RNG. Upstream WebKit calls it during
// WebProcess/GPUProcess startup; this single-process W10M port has no such hook, so a
// real `new RTCPeerConnection()` (e.g. Cloudflare Turnstile's probe) faulted building its
// DTLS identity against uninitialized crypto state. Initialize it lazily, once.
#include <webrtc/rtc_base/ssl_adapter.h>
#include <mutex>

namespace WebCorePort { void portLog(const char*); }

// Revenant ARM32-UWP (WinCairo) LibWebRTCProvider glue.
//
// The base WebCore::LibWebRTCProvider (LibWebRTCProvider.cpp) is a fully
// functional, platform-agnostic provider: createPeerConnectionFactory() builds
// the factory from builtin audio codecs + a dummy audio module, createPeer-
// Connection() has the "Default WK1 implementation", and the video encoder/
// decoder factories return nullptr (no media codecs — phase 1 is ICE + DTLS-SRTP
// + SCTP data channels, which need no codecs). So unlike Cocoa/GStreamer we do
// NOT subclass; we just supply the three statics the base .cpp leaves undefined
// under USE(LIBWEBRTC) && !PLATFORM(COCOA): create(), webRTCAvailable() and
// registerWebKitVP8Decoder().

namespace WebCore {

UniqueRef<LibWebRTCProvider> LibWebRTCProvider::create()
{
    // One-time boringssl global init + RNG seed. Without this a real peer connection faults
    // while generating its DTLS identity. rtc::InitializeSSL() is itself idempotent, but guard
    // with call_once so we never race it from multiple pages/threads.
    static std::once_flag sslOnce;
    std::call_once(sslOnce, [] {
        bool ok = rtc::InitializeSSL();
        WebCorePort::portLog(ok ? "webrtc: rtc::InitializeSSL() OK" : "webrtc: rtc::InitializeSSL() FAILED");
    });
    return makeUniqueRef<LibWebRTCProviderWinCairo>();
}

bool LibWebRTCProvider::webRTCAvailable()
{
    // libwebrtc is statically linked into the binary (webrtc.lib), so it is
    // always present — no weak-link probe like the Cocoa framework path.
    return true;
}

void LibWebRTCProvider::registerWebKitVP8Decoder()
{
    // The WebKit-specific VP8 decoder registration is a Cocoa/VideoToolbox
    // concern. libwebrtc's builtin software VP8 is used directly here; nothing
    // to register.
}

} // namespace WebCore

#endif // USE(LIBWEBRTC)
