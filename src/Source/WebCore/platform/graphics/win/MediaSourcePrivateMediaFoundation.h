// MediaSourcePrivateMediaFoundation — a WebCore MediaSourcePrivate backed by a WinRT MseStreamSource
// (see ShellMse.cpp). Owns the MseStreamSource; each SourceBuffer maps to a WinRT MseSourceBuffer.
// The MseStreamSource's IMFMediaSource is handed to MediaPlayerPrivateMediaFoundation's IMFMediaEngine
// so playback (decode-to-texture + accelerated compositing) works unchanged, sourced from MSE.
#pragma once

#if ENABLE(MEDIA_SOURCE)

#include "MediaSourcePrivate.h"
#include <wtf/Vector.h>

namespace WebCore {

class MediaPlayerPrivateMediaFoundation;
class MediaSourcePrivateClient;
class SourceBufferPrivateMediaFoundation;

class MediaSourcePrivateMediaFoundation final : public MediaSourcePrivate {
public:
    static Ref<MediaSourcePrivateMediaFoundation> create(MediaPlayerPrivateMediaFoundation&, MediaSourcePrivateClient&);
    virtual ~MediaSourcePrivateMediaFoundation();

    void* mseHandle() const { return m_srcHandle; }
    // Returns a ref'd IMFMediaSource* (as void*) the caller feeds to IMFMediaEngine and then releases.
    void* copyMFMediaSource() const;

    // From the shell's WinRT MseStreamSource events (UI thread == main thread here).
    void onOpened();
    void onEnded();

    // Called from MediaPlayerPrivateMediaFoundation's destructor. This object is RefCounted and ALSO
    // referenced by WebCore's MediaSource (setPrivateAndOpen), so it can outlive the player — some
    // element-teardown paths invalidate the player without detaching the MediaSource first, and a late
    // MediaSource::monitorSourceBuffers()/durationChanged() would then call into a freed player.
    void clearPlayer() { m_player = nullptr; }

private:
    MediaSourcePrivateMediaFoundation(MediaPlayerPrivateMediaFoundation&, MediaSourcePrivateClient&);

    // MediaSourcePrivate
    AddStatus addSourceBuffer(const ContentType&, bool webMParserEnabled, RefPtr<SourceBufferPrivate>&) final;
    void durationChanged(const MediaTime&) final;
    void markEndOfStream(EndOfStreamStatus) final;
    void unmarkEndOfStream() final;
    bool isEnded() const final { return m_isEnded; }
    MediaPlayer::ReadyState readyState() const final { return m_readyState; }
    void setReadyState(MediaPlayer::ReadyState) final;
    void waitForSeekCompleted() final { }
    void seekCompleted() final { }

    MediaPlayerPrivateMediaFoundation* m_player; // nulled by clearPlayer() when the player dies first
    MediaSourcePrivateClient& m_client;
    void* m_srcHandle { nullptr }; // MseStreamSource (shell side)
    Vector<RefPtr<SourceBufferPrivateMediaFoundation>> m_sourceBuffers;
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };
    bool m_isEnded { false };
};

} // namespace WebCore

#endif // ENABLE(MEDIA_SOURCE)
