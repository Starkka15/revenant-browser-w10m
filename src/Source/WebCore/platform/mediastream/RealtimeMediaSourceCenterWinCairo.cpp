/*
 * Revenant ARM32-UWP: platform capture factories for RealtimeMediaSourceCenter.
 *
 * RealtimeMediaSourceCenter::default{Audio,Video,Display}CaptureFactory() are
 * defined per-platform (Cocoa/GStreamer wire real camera/mic/screen capture).
 * getUserMedia device capture is phase-2 for the Revenant WebRTC port (phase-1
 * is RTCPeerConnection + data channels), so provide empty capture factories:
 * the center links and runs, and every getUserMedia attempt fails cleanly with
 * a "not supported" error rather than crashing. These become the insertion
 * point for a UWP MediaCapture backend later.
 */

#include "config.h"

#if ENABLE(MEDIA_STREAM)

#include "CaptureDeviceManager.h"
#include "DisplayCaptureManager.h"
#include "RealtimeMediaSource.h"
#include "RealtimeMediaSourceCenter.h"
#include "RealtimeMediaSourceFactory.h"
#include <wtf/NeverDestroyed.h>

namespace WebCore {

static const Vector<CaptureDevice>& emptyCaptureDevices()
{
    static NeverDestroyed<Vector<CaptureDevice>> devices;
    return devices.get();
}

class EmptyCaptureDeviceManager final : public CaptureDeviceManager {
    const Vector<CaptureDevice>& captureDevices() final { return emptyCaptureDevices(); }
};

class EmptyDisplayCaptureManager final : public DisplayCaptureManager {
    const Vector<CaptureDevice>& captureDevices() final { return emptyCaptureDevices(); }
};

class EmptyAudioCaptureFactory final : public AudioCaptureFactory {
public:
    CaptureSourceOrError createAudioCaptureSource(const CaptureDevice&, String&&, const MediaConstraints*) final { return CaptureSourceOrError { "Audio capture is not supported"_s }; }
    CaptureDeviceManager& audioCaptureDeviceManager() final { static NeverDestroyed<EmptyCaptureDeviceManager> manager; return manager.get(); }
    const Vector<CaptureDevice>& speakerDevices() const final { return emptyCaptureDevices(); }
};

class EmptyVideoCaptureFactory final : public VideoCaptureFactory {
public:
    CaptureSourceOrError createVideoCaptureSource(const CaptureDevice&, String&&, const MediaConstraints*) final { return CaptureSourceOrError { "Video capture is not supported"_s }; }
    CaptureDeviceManager& videoCaptureDeviceManager() final { static NeverDestroyed<EmptyCaptureDeviceManager> manager; return manager.get(); }
};

class EmptyDisplayCaptureFactory final : public DisplayCaptureFactory {
public:
    CaptureSourceOrError createDisplayCaptureSource(const CaptureDevice&, String&&, const MediaConstraints*) final { return CaptureSourceOrError { "Display capture is not supported"_s }; }
    DisplayCaptureManager& displayCaptureDeviceManager() final { static NeverDestroyed<EmptyDisplayCaptureManager> manager; return manager.get(); }
};

AudioCaptureFactory& RealtimeMediaSourceCenter::defaultAudioCaptureFactory()
{
    static NeverDestroyed<EmptyAudioCaptureFactory> factory;
    return factory.get();
}

VideoCaptureFactory& RealtimeMediaSourceCenter::defaultVideoCaptureFactory()
{
    static NeverDestroyed<EmptyVideoCaptureFactory> factory;
    return factory.get();
}

DisplayCaptureFactory& RealtimeMediaSourceCenter::defaultDisplayCaptureFactory()
{
    static NeverDestroyed<EmptyDisplayCaptureFactory> factory;
    return factory.get();
}

} // namespace WebCore

#endif // ENABLE(MEDIA_STREAM)
