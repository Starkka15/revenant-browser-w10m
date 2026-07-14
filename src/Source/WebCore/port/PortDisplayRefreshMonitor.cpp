#include "config.h"
#include "PortDisplayRefreshMonitor.h"

#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>

namespace WebCore {

// ALL live monitors (constructed but not yet destroyed). We drive the display link ALWAYS-ON: every
// monitor is ticked every frame, exactly like a real 60Hz hardware display link. WebCore's
// start/stopNotificationMechanism is only a power hint — but our tick already runs every frame via
// CompositionTarget::Rendering at zero extra cost, and DisplayRefreshMonitor::displayLinkFired
// self-throttles (it dispatches for maxUnscheduledFireCount idle frames, then early-returns). The old
// design fired ONLY started monitors; when the page went idle WebCore stopped the mechanism, and a
// subsequent JS-driven rendering update (e.g. YouTube's client-side route change to /shorts) failed
// to reliably resume ticks, so the new content never laid out/composited (blank until a full reload).
// Ticking every live monitor closes that gap. Created/destroyed + ticked on the main thread; the lock
// guards the (rare) off-thread displayLinkFired contract.
static Lock s_activeLock;
static HashSet<PortDisplayRefreshMonitor*>& allMonitors()
{
    static NeverDestroyed<HashSet<PortDisplayRefreshMonitor*>> set;
    return set.get();
}

RefPtr<PortDisplayRefreshMonitor> PortDisplayRefreshMonitor::create(PlatformDisplayID displayID)
{
    return adoptRef(new PortDisplayRefreshMonitor(displayID));
}

PortDisplayRefreshMonitor::PortDisplayRefreshMonitor(PlatformDisplayID displayID)
    : DisplayRefreshMonitor(displayID)
    , m_currentUpdate({ 0, 60 }) // 60 fps display
{
    Locker locker { s_activeLock };
    allMonitors().add(this);
}

PortDisplayRefreshMonitor::~PortDisplayRefreshMonitor()
{
    Locker locker { s_activeLock };
    allMonitors().remove(this);
}

// start/stop are WebCore's power hints only; we tick every live monitor regardless (always-on link).
// displayLinkFired self-throttles idle frames, so this costs nothing on a static page.
bool PortDisplayRefreshMonitor::startNotificationMechanism()
{
    return true;
}

void PortDisplayRefreshMonitor::stopNotificationMechanism()
{
}

void PortDisplayRefreshMonitor::fireVsync()
{
    DisplayUpdate update = m_currentUpdate;
    m_currentUpdate = m_currentUpdate.nextUpdate();
    // Delivers to WebCore's clients (RenderingUpdateScheduler etc.) on the main thread; the base class
    // handles the "previous frame not done" / idle-fire-count bookkeeping.
    displayLinkFired(update);
}

PortDisplayRefreshMonitorFactory& PortDisplayRefreshMonitorFactory::singleton()
{
    static NeverDestroyed<PortDisplayRefreshMonitorFactory> factory;
    return factory.get();
}

RefPtr<DisplayRefreshMonitor> PortDisplayRefreshMonitorFactory::createDisplayRefreshMonitor(PlatformDisplayID displayID)
{
    return PortDisplayRefreshMonitor::create(displayID);
}

} // namespace WebCore

extern "C" void WebCoreBrowserVsyncTick()
{
    using namespace WebCore;
    // Snapshot the active monitors (ref-protected) under the lock, then fire OUTSIDE the lock —
    // fireVsync -> displayLinkFired can re-enter start/stopNotificationMechanism (same lock) and can
    // release the last client ref, so keep each alive across the call.
    Vector<RefPtr<PortDisplayRefreshMonitor>> monitors;
    {
        Locker locker { s_activeLock };
        monitors.reserveInitialCapacity(allMonitors().size());
        for (auto* monitor : allMonitors())
            monitors.append(monitor);
    }
    for (auto& monitor : monitors)
        monitor->fireVsync();
}
