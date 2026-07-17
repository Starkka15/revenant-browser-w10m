/*
 * Revenant WinCairo/UWP port — real DeviceOrientation + DeviceMotion clients
 * backed by the phone's Windows.Devices.Sensors hardware (Inclinometer for
 * orientation; Accelerometer + Gyrometer for motion).
 *
 * WebCore ships the DeviceOrientationClient/DeviceMotionClient interfaces and the
 * controllers; a port must supply the concrete clients that feed real readings.
 * Without them the events exist (once ENABLE(DEVICE_ORIENTATION) is on) but never
 * fire. The Lumia 640 XL has an accelerometer + gyro + magnetometer, so this is a
 * full implementation, not a stub.
 */

#pragma once

#if ENABLE(DEVICE_ORIENTATION)

#include "DeviceMotionClient.h"
#include "DeviceMotionData.h"
#include "DeviceOrientationClient.h"
#include "DeviceOrientationData.h"
#include "Timer.h"
#include <memory>
#include <wtf/RefPtr.h>

namespace WebCore {
class Page;
class DeviceMotionController;
class DeviceOrientationController;
}

namespace WebCorePort {

// Opaque holder for the WinRT sensor COM pointers (kept out of the header so the
// Windows.Devices.Sensors ABI headers only leak into the .cpp).
struct SensorHandles;

class PortDeviceOrientationClient final : public WebCore::DeviceOrientationClient {
public:
    PortDeviceOrientationClient();
    ~PortDeviceOrientationClient() final;

    void setController(WebCore::DeviceOrientationController*) final;
    void startUpdating() final;
    void stopUpdating() final;
    WebCore::DeviceOrientationData* lastOrientation() const final { return m_lastOrientation.get(); }
    void deviceOrientationControllerDestroyed() final;

private:
    void timerFired();

    WebCore::DeviceOrientationController* m_controller { nullptr };
    RefPtr<WebCore::DeviceOrientationData> m_lastOrientation;
    WebCore::Timer m_timer;
    std::unique_ptr<SensorHandles> m_sensors;
    bool m_started { false };
};

class PortDeviceMotionClient final : public WebCore::DeviceMotionClient {
public:
    PortDeviceMotionClient();
    ~PortDeviceMotionClient() final;

    void setController(WebCore::DeviceMotionController*) final;
    void startUpdating() final;
    void stopUpdating() final;
    WebCore::DeviceMotionData* lastMotion() const final { return m_lastMotion.get(); }
    void deviceMotionControllerDestroyed() final;

private:
    void timerFired();

    WebCore::DeviceMotionController* m_controller { nullptr };
    RefPtr<WebCore::DeviceMotionData> m_lastMotion;
    WebCore::Timer m_timer;
    std::unique_ptr<SensorHandles> m_sensors;
    double m_intervalMs { 16 };
    bool m_started { false };
};

// Instantiates both clients and registers their controllers on the page.
void provideDeviceSensorsTo(WebCore::Page&);

} // namespace WebCorePort

#endif // ENABLE(DEVICE_ORIENTATION)
