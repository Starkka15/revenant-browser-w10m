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

    void pushBufferedRanges();

    MediaSourcePrivateMediaFoundation* m_mediaSource { nullptr };
    void* m_sbHandle { nullptr }; // MseSourceBuffer (shell side)
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };
    bool m_active { false };
    bool m_isVideo { false };
    String m_codecs;
    bool m_initReported { false }; // report the synthetic init segment exactly once (first append)
};

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
