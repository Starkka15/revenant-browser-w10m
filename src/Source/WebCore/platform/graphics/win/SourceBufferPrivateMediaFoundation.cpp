#include "config.h"
#include "SourceBufferPrivateMediaFoundation.h"

#if ENABLE(MEDIA_SOURCE)

#include "MediaSourcePrivateMediaFoundation.h"
#include "PlatformTimeRanges.h"
#include "ShellMseBridge.h"
#include "SourceBufferPrivateClient.h"
#include "AudioTrackPrivate.h"
#include "VideoTrackPrivate.h"
#include "InbandTextTrackPrivate.h"
#include "MediaDescription.h"
#include <wtf/MainThread.h>
#include <wtf/text/AtomString.h>

extern void PortImgLog(const char*);

namespace WebCore {

namespace {

// Minimal MediaDescription carrying just the codec string (from the addSourceBuffer content type).
class PortMediaDescription final : public MediaDescription {
public:
    static Ref<PortMediaDescription> create(const String& codec, bool isVideo) { return adoptRef(*new PortMediaDescription(codec, isVideo)); }
    AtomString codec() const final { return m_codec; }
    bool isVideo() const final { return m_isVideo; }
    bool isAudio() const final { return !m_isVideo; }
    bool isText() const final { return false; }
private:
    PortMediaDescription(const String& codec, bool isVideo) : m_codec(codec), m_isVideo(isVideo) { }
    AtomString m_codec;
    bool m_isVideo;
};

// VideoTrackPrivate has no create() factory (unlike AudioTrackPrivate); a trivial concrete subclass.
class PortVideoTrackPrivate final : public VideoTrackPrivate {
public:
    static Ref<PortVideoTrackPrivate> create() { return adoptRef(*new PortVideoTrackPrivate()); }
};

} // namespace

Ref<SourceBufferPrivateMediaFoundation> SourceBufferPrivateMediaFoundation::create(MediaSourcePrivateMediaFoundation& source, void* sbHandle)
{
    return adoptRef(*new SourceBufferPrivateMediaFoundation(source, sbHandle));
}

SourceBufferPrivateMediaFoundation::SourceBufferPrivateMediaFoundation(MediaSourcePrivateMediaFoundation& source, void* sbHandle)
    : m_mediaSource(&source)
    , m_sbHandle(sbHandle)
{
}

SourceBufferPrivateMediaFoundation::~SourceBufferPrivateMediaFoundation()
{
    // The MseSourceBuffer is owned by the MseStreamSource (freed when the source is destroyed);
    // we only hold a borrowed handle, so nothing to release here.
}

void SourceBufferPrivateMediaFoundation::append(Vector<uint8_t>&& data)
{
    if (!m_sbHandle || data.isEmpty()) {
        if (m_client)
            m_client->sourceBufferPrivateAppendComplete(SourceBufferPrivateClient::AppendResult::AppendSucceeded);
        return;
    }
    // Forward the segment to the WinRT MseSourceBuffer. It parses/decodes internally and fires
    // UpdateEnded asynchronously -> onUpdateEnded() completes the append and reports buffered ranges.
    // Log the top-level fMP4 box type (bytes 4..8) so we can tell init (ftyp/moov) from media (moof/mdat/sidx/styp).
    { char box[5] = "----";
      if (data.size() >= 8) { box[0] = data[4]; box[1] = data[5]; box[2] = data[6]; box[3] = data[7]; }
      char b[96]; snprintf(b, sizeof b, "mse: append#%d %d bytes box=%s isVideo=%d", ++m_appendSeq, (int)data.size(), box, m_isVideo ? 1 : 0); PortImgLog(b); }
    m_appendInFlight = true;
    PortMseAppend(m_sbHandle, data.data(), static_cast<int>(data.size()));
}

void SourceBufferPrivateMediaFoundation::onUpdateEnded()
{
    // A WinRT Remove() we issued (removeCodedFrames) completes with an UpdateEnded too: refresh the
    // (now smaller) real ranges and run WebCore's removal completion (fires updateend on the JS side).
    if (m_removeInFlight) {
        m_removeInFlight = false;
        pushBufferedRanges();
        if (auto completion = WTFMove(m_removeCompletion))
            completion();
        return;
    }
    // Every other legitimate WinRT UpdateEnded is the completion of an AppendBuffer WE issued. A
    // UpdateEnded with NO append in flight is therefore not an append completion — it's the
    // MediaEngine's own activity (e.g. the autoplay seek-to-0 flushing the MseSourceBuffer), during
    // which Buffered momentarily reads 0. If we ran pushBufferedRanges() here we would overwrite
    // WebCore's valid .buffered (0..6s) with EMPTY — which is exactly what froze YouTube ads: the page
    // sees its buffer vanish a frame after playback starts, the MediaEngine starves, and it never
    // resumes. Ignore it: do NOT push ranges and do NOT fire appendComplete (nothing is awaiting one).
    if (!m_appendInFlight) {
        PortImgLog("mse: ignoring spurious UpdateEnded (no append in flight) - preserving buffered ranges");
        return;
    }
    m_appendInFlight = false;
    // On the first append (the init segment), report the track to WebCore so the element reaches
    // HAVE_METADATA / fires loadedmetadata. Without this, players like YouTube append the init
    // segment then wait forever, never sending media. We synthesize the InitializationSegment from
    // the addSourceBuffer content type (one track per buffer) — no fMP4 parse needed. The append
    // isn't completed (updateend) until WebCore finishes processing it (the completion handler).
    if (!m_initReported && m_client) {
        m_initReported = true;
        SourceBufferPrivateClient::InitializationSegment segment;
        segment.duration = MediaTime::invalidTime();
        if (m_isVideo) {
            SourceBufferPrivateClient::InitializationSegment::VideoTrackInformation info;
            info.track = PortVideoTrackPrivate::create();
            info.description = PortMediaDescription::create(m_codecs, true);
            segment.videoTracks.append(WTFMove(info));
        } else {
            SourceBufferPrivateClient::InitializationSegment::AudioTrackInformation info;
            info.track = AudioTrackPrivate::create();
            info.description = PortMediaDescription::create(m_codecs, false);
            segment.audioTracks.append(WTFMove(info));
        }
        { char b[96]; snprintf(b, sizeof b, "mse: report init segment isVideo=%d codecs=%s", m_isVideo ? 1 : 0, m_codecs.utf8().data()); PortImgLog(b); }
        RefPtr<SourceBufferPrivateMediaFoundation> protectedThis(this);
        m_client->sourceBufferPrivateDidReceiveInitializationSegment(WTFMove(segment), [this, protectedThis = WTFMove(protectedThis)]() mutable {
            PortImgLog("mse: init segment processed by WebCore");
            pushBufferedRanges();
            if (m_client)
                m_client->sourceBufferPrivateAppendComplete(SourceBufferPrivateClient::AppendResult::AppendSucceeded);
        });
        return;
    }
    pushBufferedRanges();
    if (m_client)
        m_client->sourceBufferPrivateAppendComplete(SourceBufferPrivateClient::AppendResult::AppendSucceeded);
}

void SourceBufferPrivateMediaFoundation::onErrored(int hr)
{
    { char b[64]; snprintf(b, sizeof b, "mse: SourceBuffer ERROR hr=%d", hr); PortImgLog(b); }
    if (m_client)
        m_client->sourceBufferPrivateAppendError(true);
}

void SourceBufferPrivateMediaFoundation::pushBufferedRanges()
{
    if (!m_sbHandle)
        return;
    double starts[32], ends[32];
    int n = PortMseGetBuffered(m_sbHandle, starts, ends, 32);
    { char b[96]; snprintf(b, sizeof b, "mse: updateEnded buffered ranges=%d%s", n, n ? "" : " (EMPTY - no playable data!)");
      PortImgLog(b);
      if (n) { char c[96]; snprintf(c, sizeof c, "mse:   range[0]=%.2f..%.2f", starts[0], ends[0]); PortImgLog(c); } }
    PlatformTimeRanges ranges;
    for (int i = 0; i < n; ++i)
        ranges.add(MediaTime::createWithDouble(starts[i]), MediaTime::createWithDouble(ends[i]));
    setBufferedRanges(ranges);
}

void SourceBufferPrivateMediaFoundation::abort()
{
    PortImgLog("mse: abort() -> PortMseAbort (may clear buffered)");
    if (m_sbHandle)
        PortMseAbort(m_sbHandle);
}

void SourceBufferPrivateMediaFoundation::resetParserState()
{
    // WinRT MseSourceBuffer has no explicit parser-reset; Abort() discards the pending append,
    // which is the closest equivalent.
    PortImgLog("mse: resetParserState() -> PortMseAbort (may clear buffered)");
    if (m_sbHandle)
        PortMseAbort(m_sbHandle);
}

void SourceBufferPrivateMediaFoundation::removedFromMediaSource()
{
    PortImgLog("mse: removedFromMediaSource()");
    m_sbHandle = nullptr; // owned by the MseStreamSource; just drop our borrowed handle
}

void SourceBufferPrivateMediaFoundation::setActive(bool active)
{
    m_active = active;
}

void SourceBufferPrivateMediaFoundation::updateBufferedFromTrackBuffers(bool)
{
    // The base recomputes .buffered from WebCore track buffers — always empty here (the WinRT source
    // demuxes internally), so it wiped the real ranges to EMPTY. Called from SourceBuffer::
    // readyStateChanged when the MediaSource transitions to 'ended': endOfStream() then computed
    // duration = highest buffered end = 0 and the element could never fire 'ended' (endedPlayback()
    // requires duration > 0) — the YouTube ad -> content splice hang. The WinRT buffer is the single
    // source of truth for buffered ranges; reassert it.
    pushBufferedRanges();
}

void SourceBufferPrivateMediaFoundation::removeCodedFrames(const MediaTime& start, const MediaTime& end, const MediaTime&, bool, CompletionHandler<void()>&& completionHandler)
{
    { char b[96]; snprintf(b, sizeof b, "mse: remove %.2f..%.2f", start.toDouble(), end.isPositiveInfinite() ? -1.0 : end.toDouble()); PortImgLog(b); }
    if (!m_sbHandle) {
        completionHandler();
        return;
    }
    // Real removal happens in the WinRT MseSourceBuffer (it owns the demuxed frames). Async: its
    // UpdateEnded fires when done; onUpdateEnded() refreshes ranges and runs this completion.
    m_removeInFlight = true;
    m_removeCompletion = WTFMove(completionHandler);
    PortMseRemove(m_sbHandle, start.toDouble(), end.isPositiveInfinite() ? -1.0 : end.toDouble());
}

void SourceBufferPrivateMediaFoundation::evictCodedFrames(uint64_t, uint64_t, const MediaTime&, const MediaTime&, bool)
{
    // The WinRT MseStreamSource manages its own buffer memory; the base implementation evicts from
    // WebCore track buffers (empty) and would wipe .buffered via updateBufferedFromTrackBuffers.
}

} // namespace WebCore

// ---- C ABI callbacks from the shell's WinRT MseSourceBuffer events ----
// These fire on a WinRT threadpool/MTA thread (the async decode completion), NOT the
// WebCore main thread. onUpdateEnded/onErrored drive track setup -> DOM event queueing
// -> WindowEventLoop -> TimerBase::setNextFireTime, which RELEASE_ASSERTs main-thread
// affinity (abort otherwise — YouTube crashed here right after the audio init segment).
// Marshal to the main thread; the raw ctx is valid because an append is in flight (WebCore
// won't destroy the SourceBuffer until we call sourceBufferPrivateAppendComplete), and the
// protectedThis Ref is taken on the main thread inside the handler (SourceBufferPrivate is
// RefCounted, not thread-safe — no cross-thread ref/deref).
extern "C" void WebCoreMseSbUpdateEnded(void* sbCtx)
{
    if (!sbCtx)
        return;
    callOnMainThread([sbCtx] {
        static_cast<WebCore::SourceBufferPrivateMediaFoundation*>(sbCtx)->onUpdateEnded();
    });
}

extern "C" void WebCoreMseSbErrored(void* sbCtx, int hr)
{
    if (!sbCtx)
        return;
    callOnMainThread([sbCtx, hr] {
        static_cast<WebCore::SourceBufferPrivateMediaFoundation*>(sbCtx)->onErrored(hr);
    });
}

#endif // ENABLE(MEDIA_SOURCE)
