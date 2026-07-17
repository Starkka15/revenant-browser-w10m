#include "config.h"
#include "MediaSourcePrivateMediaFoundation.h"

#if ENABLE(MEDIA_SOURCE)

#include "ContentType.h"
#include "MediaPlayerPrivateMediaFoundation.h"
#include "MediaSourcePrivateClient.h"
#include "ShellMseBridge.h"
#include "SourceBufferPrivateMediaFoundation.h"
#include <wtf/HashSet.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>

extern void PortImgLog(const char*);

namespace WebCore {

// Main-thread-only live registry (see the same pattern in SourceBufferPrivateMediaFoundation.cpp): the WinRT
// MseStreamSource event callbacks below arrive on a threadpool thread with a raw void* that WebCore may have
// destroyed on navigation. Register each instance; the callbacks marshal to the main thread and re-check
// membership before dereferencing, which is race-free since ctor/dtor and the lambda all run on the main thread.
static WTF::HashSet<const void*>& msLiveSet()
{
    static NeverDestroyed<WTF::HashSet<const void*>> set;
    return set;
}

Ref<MediaSourcePrivateMediaFoundation> MediaSourcePrivateMediaFoundation::create(MediaPlayerPrivateMediaFoundation& player, MediaSourcePrivateClient& client)
{
    return adoptRef(*new MediaSourcePrivateMediaFoundation(player, client));
}

MediaSourcePrivateMediaFoundation::MediaSourcePrivateMediaFoundation(MediaPlayerPrivateMediaFoundation& player, MediaSourcePrivateClient& client)
    : m_player(&player)
    , m_client(client)
{
    msLiveSet().add(this);
    m_srcHandle = PortMseCreate(this);
}

MediaSourcePrivateMediaFoundation::~MediaSourcePrivateMediaFoundation()
{
    msLiveSet().remove(this);
    for (auto& sb : m_sourceBuffers)
        sb->clearMediaSource();
    if (m_srcHandle)
        PortMseDestroy(m_srcHandle);
}

MediaSourcePrivate::AddStatus MediaSourcePrivateMediaFoundation::addSourceBuffer(const ContentType& contentType, bool, RefPtr<SourceBufferPrivate>& outPrivate)
{
    if (!m_srcHandle)
        return AddStatus::NotSupported;
    auto typeString = contentType.raw().utf8();
    if (!PortMseIsTypeSupported(m_srcHandle, typeString.data())) {
        { char b[160]; snprintf(b, sizeof b, "mse: addSourceBuffer REJECTED type=%s", typeString.data()); PortImgLog(b); }
        return AddStatus::NotSupported;
    }

    auto sourceBuffer = SourceBufferPrivateMediaFoundation::create(*this, nullptr);
    // Give the SourceBufferPrivate its track kind + codec so it can synthesize the init segment
    // WebKit MSE requires (one track per buffer). YouTube uses separate audio/video buffers.
    bool isVideo = contentType.containerType().startsWith("video");
    sourceBuffer->setTrackInfo(isVideo, contentType.parameter(ContentType::codecsParameter()));
    void* handle = PortMseAddSourceBuffer(m_srcHandle, typeString.data(), sourceBuffer.ptr());
    { char b[160]; snprintf(b, sizeof b, "mse: addSourceBuffer type=%s handle=%p", typeString.data(), handle); PortImgLog(b); }
    if (!handle)
        return AddStatus::NotSupported;
    sourceBuffer->setHandle(handle);

    m_sourceBuffers.append(sourceBuffer.copyRef());
    outPrivate = WTFMove(sourceBuffer);
    return AddStatus::Ok;
}

void MediaSourcePrivateMediaFoundation::durationChanged(const MediaTime& duration)
{
    if (!duration.isValid())
        return;
    { char b[64]; snprintf(b, sizeof b, "mse: durationChanged %.2fs", duration.toDouble()); PortImgLog(b); }
    if (m_srcHandle)
        PortMseSetDuration(m_srcHandle, duration.toDouble());
    // The element's duration comes from MediaPlayer::duration(); without this it stayed 0 (the WinRT
    // session's NaturalDurationChanged never fires for an MseStreamSource fed incrementally).
    if (m_player)
        m_player->setDurationFromMediaSource(duration.toDouble());
}

void MediaSourcePrivateMediaFoundation::markEndOfStream(EndOfStreamStatus status)
{
    m_isEnded = true;
    if (m_srcHandle) {
        int s = (status == EosNetworkError) ? 1 : (status == EosDecodeError) ? 2 : 0;
        PortMseEndOfStream(m_srcHandle, s);
    }
}

void MediaSourcePrivateMediaFoundation::unmarkEndOfStream()
{
    m_isEnded = false;
}

void MediaSourcePrivateMediaFoundation::setReadyState(MediaPlayer::ReadyState state)
{
    m_readyState = state;
    // This is MediaSource::monitorSourceBuffers() telling us what the spec says the readyState is,
    // computed from the buffered ranges. It was being stored and dropped; the media element never saw
    // it, so it never reached HAVE_FUTURE_DATA and never started playback. Forward it to the player.
    if (m_player)
        m_player->setReadyStateFromMediaSource(state);
}

void* MediaSourcePrivateMediaFoundation::copyMFMediaSource() const
{
    return m_srcHandle ? PortMseGetMFMediaSource(m_srcHandle) : nullptr;
}

void MediaSourcePrivateMediaFoundation::onOpened()
{
    // The WinRT MseStreamSource pipeline opened. WebCore's MediaSource was already opened via
    // setPrivateAndOpen(); nothing else required here for the first cut.
}

void MediaSourcePrivateMediaFoundation::onEnded()
{
    m_isEnded = true;
}

} // namespace WebCore

// ---- C ABI callbacks from the shell's WinRT MseStreamSource events ----
// These fire on a WinRT threadpool/MTA thread. Marshal to the WebCore main thread (onOpened/onEnded touch
// main-thread-affine state and the client) AND re-validate against the live registry, since the source can
// be destroyed on navigation before the queued lambda runs.
extern "C" void WebCoreMseSourceOpened(void* srcCtx)
{
    if (!srcCtx)
        return;
    callOnMainThread([srcCtx] {
        if (WebCore::msLiveSet().contains(srcCtx))
            static_cast<WebCore::MediaSourcePrivateMediaFoundation*>(srcCtx)->onOpened();
    });
}

extern "C" void WebCoreMseSourceEnded(void* srcCtx)
{
    if (!srcCtx)
        return;
    callOnMainThread([srcCtx] {
        if (WebCore::msLiveSet().contains(srcCtx))
            static_cast<WebCore::MediaSourcePrivateMediaFoundation*>(srcCtx)->onEnded();
    });
}

#endif // ENABLE(MEDIA_SOURCE)
