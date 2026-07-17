/*
 * Revenant ARM32-UWP: platform factory glue for libwebrtc media tracks.
 *
 * The four Realtime{Outgoing,Incoming}{Audio,Video}Source classes are abstract;
 * their create() is platform-specific (Cocoa and GStreamer each provide one that
 * subclasses to convert real camera/mic frames). Phase-1 of the Revenant WebRTC
 * port targets ICE + DTLS-SRTP + SCTP data channels only (the goal is a real
 * RTCPeerConnection for Cloudflare/fingerprinting and data channels), so media
 * send/receive is not yet wired. These concrete subclasses satisfy the abstract
 * bases so RTCPeerConnection links and runs; the per-sample hooks are no-ops and
 * become the insertion point when the camera/mic pipeline lands in phase-2.
 */

#include "config.h"

#if USE(LIBWEBRTC)

#include "RealtimeIncomingAudioSource.h"
#include "RealtimeIncomingVideoSource.h"
#include "RealtimeOutgoingAudioSource.h"
#include "RealtimeOutgoingVideoSource.h"

namespace WebCore {

class RealtimeOutgoingAudioSourceWinCairo final : public RealtimeOutgoingAudioSource {
public:
    static Ref<RealtimeOutgoingAudioSourceWinCairo> create(Ref<MediaStreamTrackPrivate>&& track)
    {
        return adoptRef(*new RealtimeOutgoingAudioSourceWinCairo(WTFMove(track)));
    }

private:
    explicit RealtimeOutgoingAudioSourceWinCairo(Ref<MediaStreamTrackPrivate>&& track)
        : RealtimeOutgoingAudioSource(WTFMove(track)) { }

    void audioSamplesAvailable(const MediaTime&, const PlatformAudioData&, const AudioStreamDescription&, size_t) final { }
};

Ref<RealtimeOutgoingAudioSource> RealtimeOutgoingAudioSource::create(Ref<MediaStreamTrackPrivate>&& audioSource)
{
    return RealtimeOutgoingAudioSourceWinCairo::create(WTFMove(audioSource));
}

class RealtimeOutgoingVideoSourceWinCairo final : public RealtimeOutgoingVideoSource {
public:
    static Ref<RealtimeOutgoingVideoSourceWinCairo> create(Ref<MediaStreamTrackPrivate>&& track)
    {
        return adoptRef(*new RealtimeOutgoingVideoSourceWinCairo(WTFMove(track)));
    }

private:
    explicit RealtimeOutgoingVideoSourceWinCairo(Ref<MediaStreamTrackPrivate>&& track)
        : RealtimeOutgoingVideoSource(WTFMove(track)) { }

    rtc::scoped_refptr<webrtc::VideoFrameBuffer> createBlackFrame(size_t, size_t) final { return nullptr; }
    void videoSampleAvailable(MediaSample&, VideoSampleMetadata) final { }
};

Ref<RealtimeOutgoingVideoSource> RealtimeOutgoingVideoSource::create(Ref<MediaStreamTrackPrivate>&& videoSource)
{
    return RealtimeOutgoingVideoSourceWinCairo::create(WTFMove(videoSource));
}

class RealtimeIncomingAudioSourceWinCairo final : public RealtimeIncomingAudioSource {
public:
    static Ref<RealtimeIncomingAudioSourceWinCairo> create(rtc::scoped_refptr<webrtc::AudioTrackInterface>&& track, String&& id)
    {
        return adoptRef(*new RealtimeIncomingAudioSourceWinCairo(WTFMove(track), WTFMove(id)));
    }

private:
    RealtimeIncomingAudioSourceWinCairo(rtc::scoped_refptr<webrtc::AudioTrackInterface>&& track, String&& id)
        : RealtimeIncomingAudioSource(WTFMove(track), WTFMove(id)) { }
};

Ref<RealtimeIncomingAudioSource> RealtimeIncomingAudioSource::create(rtc::scoped_refptr<webrtc::AudioTrackInterface>&& track, String&& id)
{
    return RealtimeIncomingAudioSourceWinCairo::create(WTFMove(track), WTFMove(id));
}

class RealtimeIncomingVideoSourceWinCairo final : public RealtimeIncomingVideoSource {
public:
    static Ref<RealtimeIncomingVideoSourceWinCairo> create(rtc::scoped_refptr<webrtc::VideoTrackInterface>&& track, String&& id)
    {
        return adoptRef(*new RealtimeIncomingVideoSourceWinCairo(WTFMove(track), WTFMove(id)));
    }

private:
    RealtimeIncomingVideoSourceWinCairo(rtc::scoped_refptr<webrtc::VideoTrackInterface>&& track, String&& id)
        : RealtimeIncomingVideoSource(WTFMove(track), WTFMove(id)) { }

    void OnFrame(const webrtc::VideoFrame&) final { }
};

Ref<RealtimeIncomingVideoSource> RealtimeIncomingVideoSource::create(rtc::scoped_refptr<webrtc::VideoTrackInterface>&& track, String&& id)
{
    return RealtimeIncomingVideoSourceWinCairo::create(WTFMove(track), WTFMove(id));
}

} // namespace WebCore

#endif // USE(LIBWEBRTC)
