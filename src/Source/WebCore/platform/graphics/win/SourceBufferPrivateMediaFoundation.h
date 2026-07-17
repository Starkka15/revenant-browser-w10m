// SourceBufferPrivateMediaFoundation — a WebCore SourceBufferPrivate backed by a WinRT
// MseSourceBuffer (see ShellMse.cpp). Thin: append() forwards bytes to the WinRT buffer, which
// demuxes+decodes internally; when its async UpdateEnded fires we push the WinRT buffered ranges
// into WebCore and complete the append. No demuxer / no sample enqueuing on our side.
#pragma once

#if ENABLE(MEDIA_SOURCE)

#include "SourceBufferPrivate.h"

namespace WebCore {

class MediaSourcePrivateMediaFoundation;

class SourceBufferPrivateMediaFoundation final : public SourceBufferPrivate {
public:
    static Ref<SourceBufferPrivateMediaFoundation> create(MediaSourcePrivateMediaFoundation&, void* sbHandle);
    virtual ~SourceBufferPrivateMediaFoundation();

    // Called from the shell's WinRT event handlers (UI thread == main thread here).
    void onUpdateEnded();
    void onErrored(int hr);

    void clearMediaSource() { m_mediaSource = nullptr; }
    void* handle() const { return m_sbHandle; }
    void setHandle(void* h) { m_sbHandle = h; } // set after PortMseAddSourceBuffer returns
    // WebKit MSE requires the SourceBufferPrivate to report the init segment (tracks) so the element
    // reaches HAVE_METADATA / fires loadedmetadata; without it players like YouTube never append
    // media. We synthesize it from the addSourceBuffer content type (one track per buffer).
    void setTrackInfo(bool isVideo, const String& codecs) { m_isVideo = isVideo; m_codecs = codecs; }

    // Reassert the WinRT buffer's real ranges after the shell's emergency memory trim bypassed
    // removeCodedFrames (retries while Buffered transiently reads 0). Main thread only; sbCtx is
    // validated against the live-instance set before any deref.
    static void refreshRangesAfterExternalTrim(void* sbCtx, int retriesLeft);

private:
    SourceBufferPrivateMediaFoundation(MediaSourcePrivateMediaFoundation&, void* sbHandle);

    // SourceBufferPrivate (pure virtuals in a RELEASE_LOG_DISABLED build).
    void append(Vector<uint8_t>&&) final;
    void abort() final;
    void resetParserState() final;
    void removedFromMediaSource() final;
    MediaPlayer::ReadyState readyState() const final { return m_readyState; }
    void setReadyState(MediaPlayer::ReadyState state) final { m_readyState = state; }
    void setActive(bool) final;

    // Track buffers are ALWAYS EMPTY on this backend (the WinRT MseSourceBuffer demuxes internally;
    // WebCore never sees samples), so every base-class algorithm that derives state from them must be
    // overridden or it clobbers the real state:
    //  - updateBufferedFromTrackBuffers: called from SourceBuffer::readyStateChanged (MediaSource
    //    open/ended!), appendCompleted and removeCodedFrames. The base wiped .buffered to EMPTY, so
    //    MediaSource::endOfStream() computed duration=0 -> the 'ended' event never fired -> YouTube
    //    hung forever at the ad -> content splice. Reassert the WinRT buffer's real ranges instead.
    //  - removeCodedFrames: forward to the WinRT buffer's own Remove() (async; completes on its
    //    UpdateEnded), then refresh ranges.
    //  - evictCodedFrames: no-op; the WinRT source manages its own buffer memory.
    void updateBufferedFromTrackBuffers(bool sourceIsEnded) final;
    void removeCodedFrames(const MediaTime& start, const MediaTime& end, const MediaTime& currentMediaTime, bool isEnded, CompletionHandler<void()>&&) final;
    void evictCodedFrames(uint64_t newDataSize, uint64_t maximumBufferSize, const MediaTime& currentTime, const MediaTime& duration, bool isEnded) final;

    void pushBufferedRanges();

    MediaSourcePrivateMediaFoundation* m_mediaSource { nullptr };
    void* m_sbHandle { nullptr }; // MseSourceBuffer (shell side)
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };
    bool m_active { false };
    bool m_isVideo { false };
    String m_codecs;
    bool m_initReported { false }; // report the synthetic init segment exactly once (first append)
    int m_appendSeq { 0 };         // diag: count appends we forward to the WinRT buffer
    bool m_appendInFlight { false }; // diag: distinguish a real post-append UpdateEnded from a spurious one
    bool m_removeInFlight { false }; // a WinRT Remove() is pending; its UpdateEnded completes it
    CompletionHandler<void()> m_removeCompletion; // WebCore's coded-frame-removal completion
};

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
