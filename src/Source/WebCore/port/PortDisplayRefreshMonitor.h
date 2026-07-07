/*
 * PortDisplayRefreshMonitor — real display-link (vsync) integration for the W10M port.
 *
 * WebCore's RenderingUpdateScheduler (which drives requestAnimationFrame, CSS/GIF animation timing,
 * and the "update the rendering" cadence) schedules itself against a DisplayRefreshMonitor. The port
 * previously provided none (PortChromeClient::displayRefreshMonitorFactory() returned nullptr), so
 * the scheduler never fired and we had to hand-pump updateRendering() every frame. This gives WebCore
 * a real display link driven by the shell's 60Hz CompositionTarget::Rendering vsync, so its own
 * machinery owns the timing (and idle-throttles when there's no animation).
 */

#pragma once

#include "DisplayRefreshMonitor.h"
#include "DisplayRefreshMonitorFactory.h"
#include "DisplayUpdate.h"

namespace WebCore {

class PortDisplayRefreshMonitor final : public DisplayRefreshMonitor {
public:
    static RefPtr<PortDisplayRefreshMonitor> create(PlatformDisplayID);
    virtual ~PortDisplayRefreshMonitor();

    // Called once per real vsync from the render tick (main thread) while this monitor is active.
    void fireVsync();

private:
    explicit PortDisplayRefreshMonitor(PlatformDisplayID);

    bool startNotificationMechanism() final; // register for vsync ticks
    void stopNotificationMechanism() final;  // unregister (idle throttle)

    DisplayUpdate m_currentUpdate;
};

class PortDisplayRefreshMonitorFactory final : public DisplayRefreshMonitorFactory {
public:
    static PortDisplayRefreshMonitorFactory& singleton();
    RefPtr<DisplayRefreshMonitor> createDisplayRefreshMonitor(PlatformDisplayID) final;
};

} // namespace WebCore

// Fire one vsync tick to every active monitor. Call ONCE per real display refresh (the shell's
// CompositionTarget::Rendering @60Hz), on the main thread, before compositing the frame.
extern "C" void WebCoreBrowserVsyncTick();
