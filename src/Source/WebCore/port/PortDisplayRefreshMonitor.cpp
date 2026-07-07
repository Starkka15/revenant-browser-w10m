#include "config.h"
#include "PortDisplayRefreshMonitor.h"

#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>

namespace WebCore {

// Monitors whose notification mechanism is currently STARTED (WebCore wants refresh ticks). WebCore
// idle-throttles by calling stopNotificationMechanism() when there's no pending animation/rAF work,
// which removes the monitor here so we stop ticking it until it re-registers. Created/destroyed and
// ticked on the main thread; the lock guards against the (rare) off-thread displayLinkFired contract.
static Lock s_activeLock;
static HashSet<PortDisplayRefreshMonitor*>& activeMonitors()
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
}

PortDisplayRefreshMonitor::~PortDisplayRefreshMonitor()
{
    Locker locker { s_activeLock };
    activeMonitors().remove(this);
}

bool PortDisplayRefreshMonitor::startNotificationMechanism()
{
    Locker locker { s_activeLock };
    activeMonitors().add(this);
    return true;
}

void PortDisplayRefreshMonitor::stopNotificationMechanism()
{
    Locker locker { s_activeLock };
    activeMonitors().remove(this);
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
        monitors.reserveInitialCapacity(activeMonitors().size());
        for (auto* monitor : activeMonitors())
            monitors.append(monitor);
    }
    for (auto& monitor : monitors)
        monitor->fireVsync();
}
