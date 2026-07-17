/*
 * Revenant WinCairo/UWP port — DeviceOrientation + DeviceMotion clients backed by
 * Windows.Devices.Sensors. See PortDeviceSensorClients.h.
 */

#include "config.h"
#include "PortDeviceSensorClients.h"

#if ENABLE(DEVICE_ORIENTATION)

#include "DeviceMotionController.h"
#include "DeviceOrientationController.h"
#include "Page.h"
#include <wtf/Seconds.h>

#include <windows.h>
#include <roapi.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.devices.sensors.h>

namespace AWDS = ABI::Windows::Devices::Sensors;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;

namespace WebCorePort {

// Standard gravity, converting the accelerometer's g units to m/s^2 (the unit the
// DeviceMotionEvent acceleration fields use).
static constexpr double kStandardGravity = 9.80665;

struct SensorHandles {
    WTF_MAKE_STRUCT_FAST_ALLOCATED; // required by WTF makeUnique<>
    ComPtr<AWDS::IAccelerometer> accelerometer;
    ComPtr<AWDS::IGyrometer> gyrometer;
    ComPtr<AWDS::IInclinometer> inclinometer;
};

template<typename Statics, typename Sensor>
static ComPtr<Sensor> getDefaultSensor(const wchar_t* runtimeClass)
{
    ComPtr<Statics> statics;
    if (FAILED(RoGetActivationFactory(HStringReference(runtimeClass).Get(), IID_PPV_ARGS(&statics))) || !statics)
        return nullptr;
    ComPtr<Sensor> sensor;
    statics->GetDefault(&sensor);
    return sensor;
}

// =================== Orientation (Inclinometer) ===================

PortDeviceOrientationClient::PortDeviceOrientationClient()
    : m_timer(*this, &PortDeviceOrientationClient::timerFired)
{
}

PortDeviceOrientationClient::~PortDeviceOrientationClient() = default;

void PortDeviceOrientationClient::setController(WebCore::DeviceOrientationController* controller)
{
    m_controller = controller;
}

void PortDeviceOrientationClient::startUpdating()
{
    if (m_started)
        return;
    m_started = true;

    if (!m_sensors) {
        m_sensors = makeUnique<SensorHandles>();
        m_sensors->inclinometer = getDefaultSensor<AWDS::IInclinometerStatics, AWDS::IInclinometer>(RuntimeClass_Windows_Devices_Sensors_Inclinometer);
    }
    // Fire at ~60 Hz; the DeviceOrientationController coalesces to the event loop.
    m_timer.startRepeating(WTF::Seconds(1.0 / 60.0));
}

void PortDeviceOrientationClient::stopUpdating()
{
    m_started = false;
    m_timer.stop();
}

void PortDeviceOrientationClient::deviceOrientationControllerDestroyed()
{
    stopUpdating();
    m_controller = nullptr;
}

void PortDeviceOrientationClient::timerFired()
{
    if (!m_controller || !m_sensors || !m_sensors->inclinometer)
        return;

    ComPtr<AWDS::IInclinometerReading> reading;
    if (FAILED(m_sensors->inclinometer->GetCurrentReading(&reading)) || !reading)
        return;

    float pitch = 0, roll = 0, yaw = 0;
    reading->get_PitchDegrees(&pitch);
    reading->get_RollDegrees(&roll);
    reading->get_YawDegrees(&yaw);

    // W3C DeviceOrientation: alpha = rotation about z (compass/yaw), beta = x (front-
    // back pitch), gamma = y (left-right roll). absolute=false — the default
    // inclinometer is device-relative, not true-north referenced.
    m_lastOrientation = WebCore::DeviceOrientationData::create(
        static_cast<double>(yaw), static_cast<double>(pitch), static_cast<double>(roll),
        std::optional<bool>(false));
    m_controller->didChangeDeviceOrientation(m_lastOrientation.get());
}

// =================== Motion (Accelerometer + Gyrometer) ===================

PortDeviceMotionClient::PortDeviceMotionClient()
    : m_timer(*this, &PortDeviceMotionClient::timerFired)
{
}

PortDeviceMotionClient::~PortDeviceMotionClient() = default;

void PortDeviceMotionClient::setController(WebCore::DeviceMotionController* controller)
{
    m_controller = controller;
}

void PortDeviceMotionClient::startUpdating()
{
    if (m_started)
        return;
    m_started = true;

    if (!m_sensors) {
        m_sensors = makeUnique<SensorHandles>();
        m_sensors->accelerometer = getDefaultSensor<AWDS::IAccelerometerStatics, AWDS::IAccelerometer>(RuntimeClass_Windows_Devices_Sensors_Accelerometer);
        m_sensors->gyrometer = getDefaultSensor<AWDS::IGyrometerStatics, AWDS::IGyrometer>(RuntimeClass_Windows_Devices_Sensors_Gyrometer);

        // Report the hardware's minimum interval as the DeviceMotionEvent interval.
        if (m_sensors->accelerometer) {
            UINT32 minInterval = 0;
            if (SUCCEEDED(m_sensors->accelerometer->get_MinimumReportInterval(&minInterval)) && minInterval)
                m_intervalMs = minInterval < 16 ? 16 : minInterval;
            m_sensors->accelerometer->put_ReportInterval(static_cast<UINT32>(m_intervalMs));
        }
        if (m_sensors->gyrometer)
            m_sensors->gyrometer->put_ReportInterval(static_cast<UINT32>(m_intervalMs));
    }
    m_timer.startRepeating(WTF::Seconds(m_intervalMs / 1000.0));
}

void PortDeviceMotionClient::stopUpdating()
{
    m_started = false;
    m_timer.stop();
}

void PortDeviceMotionClient::deviceMotionControllerDestroyed()
{
    stopUpdating();
    m_controller = nullptr;
}

void PortDeviceMotionClient::timerFired()
{
    if (!m_controller || !m_sensors)
        return;

    RefPtr<WebCore::DeviceMotionData::Acceleration> accelerationIncludingGravity;
    if (m_sensors->accelerometer) {
        ComPtr<AWDS::IAccelerometerReading> reading;
        if (SUCCEEDED(m_sensors->accelerometer->GetCurrentReading(&reading)) && reading) {
            double ax = 0, ay = 0, az = 0;
            reading->get_AccelerationX(&ax);
            reading->get_AccelerationY(&ay);
            reading->get_AccelerationZ(&az);
            accelerationIncludingGravity = WebCore::DeviceMotionData::Acceleration::create(
                ax * kStandardGravity, ay * kStandardGravity, az * kStandardGravity);
        }
    }

    RefPtr<WebCore::DeviceMotionData::RotationRate> rotationRate;
    if (m_sensors->gyrometer) {
        ComPtr<AWDS::IGyrometerReading> reading;
        if (SUCCEEDED(m_sensors->gyrometer->GetCurrentReading(&reading)) && reading) {
            double gx = 0, gy = 0, gz = 0;
            reading->get_AngularVelocityX(&gx);
            reading->get_AngularVelocityY(&gy);
            reading->get_AngularVelocityZ(&gz);
            // W3C rotationRate: alpha about z, beta about x, gamma about y (deg/s).
            rotationRate = WebCore::DeviceMotionData::RotationRate::create(gz, gx, gy);
        }
    }

    if (!accelerationIncludingGravity && !rotationRate)
        return;

    // Linear acceleration (gravity removed) needs sensor fusion Windows doesn't expose
    // through the raw Accelerometer, so it stays null (spec-permitted).
    m_lastMotion = WebCore::DeviceMotionData::create(
        nullptr, WTFMove(accelerationIncludingGravity), WTFMove(rotationRate),
        std::optional<double>(m_intervalMs));
    m_controller->didChangeDeviceMotion(m_lastMotion.get());
}

// =================== Wiring ===================

void provideDeviceSensorsTo(WebCore::Page& page)
{
    WebCore::provideDeviceOrientationTo(page, *new PortDeviceOrientationClient);
    WebCore::DeviceMotionController::provideTo(&page, WebCore::DeviceMotionController::supplementName(),
        makeUnique<WebCore::DeviceMotionController>(*new PortDeviceMotionClient));
}

} // namespace WebCorePort

#endif // ENABLE(DEVICE_ORIENTATION)
