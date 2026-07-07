// ============================================================================
// PortServiceWorker.cpp — see PortServiceWorker.h.
// In-process ServiceWorker connection pair for the single-process W10M/UWP port.
// ============================================================================
#include "config.h"
#include "PortServiceWorker.h"

#if ENABLE(SERVICE_WORKER)

#include "CacheStorageProvider.h"
#include "ClientOrigin.h"
#include "EmptyClients.h"
#include "Exception.h"
#include "ExceptionCode.h"
#include "ExceptionData.h"
#include "FetchIdentifier.h"
#include "FormData.h"
#include "MessageWithMessagePorts.h"
#include "NavigationPreloadState.h"
#include "NetworkLoadMetrics.h"
#include "NotificationClient.h"
#include "Page.h"
#include "PageConfiguration.h"
#include "ProcessIdentifier.h"
#include "PushPermissionState.h"
#include "PushSubscriptionData.h"
#include "RegistrableDomain.h"
#include "ResourceError.h"
#include "ResourceLoader.h"
#include "ResourceLoaderOptions.h"
#include "ResourceLoaderTypes.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SWOriginStore.h"
#include "SWServerRegistration.h"
#include "SWServerWorker.h"
#include "SecurityOrigin.h"
#include "SecurityOriginData.h"
#include "ServiceWorkerClientData.h"
#include "ServiceWorkerContextData.h"
#include "ServiceWorkerData.h"
#include "ServiceWorkerFetch.h"
#include "ServiceWorkerJobData.h"
#include "ServiceWorkerRegistrationData.h"
#include "ServiceWorkerRegistrationKey.h"
#include "ServiceWorkerThreadProxy.h"
#include "ServiceWorkerTypes.h"
#include "SharedBuffer.h"
#include "UserAgent.h"
#include "WorkerFetchResult.h"
#include <pal/SessionID.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <cstdio>

namespace WebCorePort {

using namespace WebCore;

void portLog(const char*);
// Lifecycle trace so we can see WHERE the SW register/install/activate/ready cycle stalls (PWAs
// await navigator.serviceWorker.ready before rendering, so a stall = stuck on splash).
static void swDiag(const char* fmt, long a = 0, long b = 0)
{
    char buf[160];
    snprintf(buf, sizeof buf, fmt, a, b);
    portLog(buf);
}

// The single in-process server + the SWContextManager-side connection, at file
// scope so the loader fetch hook and the SWServer callbacks can reach them.
static SWServer* s_portSWServer = nullptr;
static PortSWContextManagerConnection* s_portContextManagerConnection = nullptr;

// Empty (in-memory) CacheStorageProvider shared by all SW pages. Caches.match will
// resolve to "no match" rather than crash — Cache API persistence is a follow-up.
static CacheStorageProvider& portCacheStorageProvider()
{
    static NeverDestroyed<Ref<CacheStorageProvider>> provider(CacheStorageProvider::create());
    return provider.get().get();
}

// A functional in-memory origin store. WebKit2 backs this with shared memory so the
// WebProcess can cheaply answer mayHaveServiceWorkerRegisteredForOrigin() without a
// round trip. In-process we don't need that fast path (the server is a direct call),
// so the store just needs to satisfy the interface; counting is handled by the base.
class PortSWOriginStore final : public WebCore::SWOriginStore {
public:
    PortSWOriginStore() = default;

private:
    void importComplete() final { }
    void addToStore(const SecurityOriginData&) final { }
    void removeFromStore(const SecurityOriginData&) final { }
    void clearStore() final { }
};

// ---------------------------------------------------------------------------
// PortSWClientConnection
// ---------------------------------------------------------------------------
PortSWClientConnection::PortSWClientConnection() = default;
PortSWClientConnection::~PortSWClientConnection() = default;

void PortSWClientConnection::scheduleJobInServer(const ServiceWorkerJobData& jobData)
{
    swDiag("SW-DIAG scheduleJob conn=%ld", !!m_serverConnection);
    if (m_serverConnection)
        m_serverConnection->clientScheduleJob(jobData);
}

void PortSWClientConnection::finishFetchingScriptInServer(const ServiceWorkerJobDataIdentifier& jobDataIdentifier, const ServiceWorkerRegistrationKey& registrationKey, const WorkerFetchResult& result)
{
    swDiag("SW-DIAG finishFetchingScript");
    if (m_serverConnection)
        m_serverConnection->clientFinishFetchingScript(jobDataIdentifier, registrationKey, result);
}

void PortSWClientConnection::postMessageToServiceWorker(ServiceWorkerIdentifier destination, MessageWithMessagePorts&& message, const ServiceWorkerOrClientIdentifier& source)
{
    if (m_serverConnection)
        m_serverConnection->clientPostMessageToServiceWorker(destination, WTFMove(message), source);
}

void PortSWClientConnection::registerServiceWorkerClient(const SecurityOrigin& topOrigin, const ServiceWorkerClientData& data, const std::optional<ServiceWorkerRegistrationIdentifier>& controllingServiceWorkerRegistrationIdentifier, const String& userAgent)
{
    if (m_serverConnection)
        m_serverConnection->clientRegisterServiceWorkerClient(SecurityOriginData { topOrigin.data() }, ServiceWorkerClientData { data }, controllingServiceWorkerRegistrationIdentifier, String { userAgent });
}

void PortSWClientConnection::unregisterServiceWorkerClient(ScriptExecutionContextIdentifier contextIdentifier)
{
    if (m_serverConnection)
        m_serverConnection->clientUnregisterServiceWorkerClient(contextIdentifier);
}

void PortSWClientConnection::scheduleUnregisterJobInServer(ServiceWorkerRegistrationIdentifier registrationIdentifier, ServiceWorkerOrClientIdentifier documentIdentifier, CompletionHandler<void(ExceptionOr<bool>&&)>&& completionHandler)
{
    if (!m_serverConnection)
        return completionHandler(Exception { InvalidStateError, "No service worker server"_s });
    m_serverConnection->clientScheduleUnregisterJob(registrationIdentifier, documentIdentifier, WTFMove(completionHandler));
}

void PortSWClientConnection::matchRegistration(SecurityOriginData&& topOrigin, const URL& clientURL, RegistrationCallback&& callback)
{
    if (!m_serverConnection)
        return callback(std::nullopt);
    auto reg = m_serverConnection->clientMatchRegistration(topOrigin, clientURL);
    swDiag("SW-DIAG matchRegistration has=%ld", !!reg);
    callback(WTFMove(reg));
}

void PortSWClientConnection::getRegistrations(SecurityOriginData&& topOrigin, const URL& clientURL, GetRegistrationsCallback&& callback)
{
    if (!m_serverConnection)
        return callback({ });
    callback(m_serverConnection->clientGetRegistrations(topOrigin, clientURL));
}

void PortSWClientConnection::whenRegistrationReady(const SecurityOriginData& topOrigin, const URL& clientURL, WhenRegistrationReadyCallback&& callback)
{
    swDiag("SW-DIAG whenRegistrationReady enter conn=%ld", !!m_serverConnection);
    if (!m_serverConnection)
        return;
    m_serverConnection->clientWhenRegistrationReady(topOrigin, clientURL, [callback = WTFMove(callback)](std::optional<ServiceWorkerRegistrationData>&& result) mutable {
        swDiag("SW-DIAG whenRegistrationReady FIRED has=%ld", !!result);
        if (result)
            callback(WTFMove(*result));
    });
}

void PortSWClientConnection::addServiceWorkerRegistrationInServer(ServiceWorkerRegistrationIdentifier identifier)
{
    if (m_serverConnection)
        m_serverConnection->clientAddRegistration(identifier);
}

void PortSWClientConnection::removeServiceWorkerRegistrationInServer(ServiceWorkerRegistrationIdentifier identifier)
{
    if (m_serverConnection)
        m_serverConnection->clientRemoveRegistration(identifier);
}

void PortSWClientConnection::didResolveRegistrationPromise(const ServiceWorkerRegistrationKey& key)
{
    if (m_serverConnection)
        m_serverConnection->clientDidResolveRegistrationPromise(key);
}

SWServerConnectionIdentifier PortSWClientConnection::serverConnectionIdentifier() const
{
    return m_serverConnection ? m_serverConnection->identifier() : SWServerConnectionIdentifier { };
}

bool PortSWClientConnection::mayHaveServiceWorkerRegisteredForOrigin(const SecurityOriginData&) const
{
    // In-process the server is a direct call, so always allow the query to proceed.
    return true;
}

// ---- optional features: reported unsupported, synchronously ----
void PortSWClientConnection::subscribeToPushService(ServiceWorkerRegistrationIdentifier, const Vector<uint8_t>&, SubscribeToPushServiceCallback&& callback)
{
    callback(Exception { NotSupportedError, "Push service is not supported"_s });
}

void PortSWClientConnection::unsubscribeFromPushService(ServiceWorkerRegistrationIdentifier, PushSubscriptionIdentifier, UnsubscribeFromPushServiceCallback&& callback)
{
    callback(false);
}

void PortSWClientConnection::getPushSubscription(ServiceWorkerRegistrationIdentifier, GetPushSubscriptionCallback&& callback)
{
    callback(std::optional<PushSubscriptionData> { std::nullopt });
}

void PortSWClientConnection::getPushPermissionState(ServiceWorkerRegistrationIdentifier, GetPushPermissionStateCallback&& callback)
{
    callback(PushPermissionState::Denied);
}

void PortSWClientConnection::enableNavigationPreload(ServiceWorkerRegistrationIdentifier, ExceptionOrVoidCallback&& callback)
{
    callback(Exception { NotSupportedError, "Navigation preload is not supported"_s });
}

void PortSWClientConnection::disableNavigationPreload(ServiceWorkerRegistrationIdentifier, ExceptionOrVoidCallback&& callback)
{
    callback(Exception { NotSupportedError, "Navigation preload is not supported"_s });
}

void PortSWClientConnection::setNavigationPreloadHeaderValue(ServiceWorkerRegistrationIdentifier, String&&, ExceptionOrVoidCallback&& callback)
{
    callback(Exception { NotSupportedError, "Navigation preload is not supported"_s });
}

void PortSWClientConnection::getNavigationPreloadState(ServiceWorkerRegistrationIdentifier, ExceptionOrNavigationPreloadStateCallback&& callback)
{
    callback(Exception { NotSupportedError, "Navigation preload is not supported"_s });
}

// ---- deliver*(): push results from the server into the WebCore base entry points ----
void PortSWClientConnection::deliverJobRejected(ServiceWorkerJobIdentifier jobIdentifier, const ExceptionData& exceptionData)
{
    jobRejectedInServer(jobIdentifier, exceptionData);
}

void PortSWClientConnection::deliverRegistrationJobResolved(ServiceWorkerJobIdentifier jobIdentifier, ServiceWorkerRegistrationData&& data, ShouldNotifyWhenResolved shouldNotifyWhenResolved)
{
    registrationJobResolvedInServer(jobIdentifier, WTFMove(data), shouldNotifyWhenResolved);
}

void PortSWClientConnection::deliverStartScriptFetch(ServiceWorkerJobIdentifier jobIdentifier, const ServiceWorkerRegistrationKey& key, FetchOptions::Cache cachePolicy)
{
    startScriptFetchForServer(jobIdentifier, key, cachePolicy);
}

void PortSWClientConnection::deliverPostMessageToServiceWorkerClient(ScriptExecutionContextIdentifier destination, MessageWithMessagePorts&& message, ServiceWorkerData&& source, String&& sourceOrigin)
{
    postMessageToServiceWorkerClient(destination, WTFMove(message), WTFMove(source), WTFMove(sourceOrigin));
}

void PortSWClientConnection::deliverUpdateRegistrationState(ServiceWorkerRegistrationIdentifier identifier, ServiceWorkerRegistrationState state, const std::optional<ServiceWorkerData>& data)
{
    updateRegistrationState(identifier, state, data);
}

void PortSWClientConnection::deliverUpdateWorkerState(ServiceWorkerIdentifier identifier, ServiceWorkerState state)
{
    updateWorkerState(identifier, state);
}

void PortSWClientConnection::deliverFireUpdateFoundEvent(ServiceWorkerRegistrationIdentifier identifier)
{
    fireUpdateFoundEvent(identifier);
}

void PortSWClientConnection::deliverSetRegistrationLastUpdateTime(ServiceWorkerRegistrationIdentifier identifier, WallTime lastUpdateTime)
{
    setRegistrationLastUpdateTime(identifier, lastUpdateTime);
}

void PortSWClientConnection::deliverSetRegistrationUpdateViaCache(ServiceWorkerRegistrationIdentifier identifier, ServiceWorkerUpdateViaCache updateViaCache)
{
    setRegistrationUpdateViaCache(identifier, updateViaCache);
}

void PortSWClientConnection::deliverNotifyClientsOfControllerChange(const HashSet<ScriptExecutionContextIdentifier>& contextIdentifiers, ServiceWorkerData&& newController)
{
    notifyClientsOfControllerChange(contextIdentifiers, WTFMove(newController));
}

// ---------------------------------------------------------------------------
// PortSWServerConnection
// ---------------------------------------------------------------------------
PortSWServerConnection::PortSWServerConnection(SWServer& server, SWServerConnectionIdentifier identifier)
    : SWServer::Connection(server, identifier)
{
}

PortSWServerConnection::~PortSWServerConnection()
{
    for (const auto& keyValue : m_clientOrigins)
        server().unregisterServiceWorkerClient(keyValue.value, keyValue.key);
    for (auto& completionHandler : m_unregisterJobs.values())
        completionHandler(false);
}

// ---- page -> server ----
void PortSWServerConnection::clientScheduleJob(const ServiceWorkerJobData& jobData)
{
    ASSERT(identifier() == jobData.connectionIdentifier());
    server().scheduleJob(ServiceWorkerJobData { jobData });
}

void PortSWServerConnection::clientFinishFetchingScript(const ServiceWorkerJobDataIdentifier& jobDataIdentifier, const ServiceWorkerRegistrationKey& registrationKey, const WorkerFetchResult& result)
{
    finishFetchingScriptInServer(jobDataIdentifier, registrationKey, result);
}

URL PortSWServerConnection::clientURLFromIdentifier(ServiceWorkerOrClientIdentifier contextIdentifier)
{
    return WTF::switchOn(contextIdentifier, [&](ScriptExecutionContextIdentifier clientIdentifier) -> URL {
        auto iterator = m_clientOrigins.find(clientIdentifier);
        if (iterator == m_clientOrigins.end())
            return { };
        auto clientData = server().serviceWorkerClientWithOriginByID(iterator->value, clientIdentifier);
        if (!clientData)
            return { };
        return clientData->url;
    }, [&](ServiceWorkerIdentifier serviceWorkerIdentifier) -> URL {
        auto* worker = server().workerByID(serviceWorkerIdentifier);
        if (!worker)
            return { };
        return worker->data().scriptURL;
    });
}

void PortSWServerConnection::clientScheduleUnregisterJob(ServiceWorkerRegistrationIdentifier registrationIdentifier, ServiceWorkerOrClientIdentifier contextIdentifier, CompletionHandler<void(ExceptionOr<bool>&&)>&& completionHandler)
{
    auto* registration = server().getRegistration(registrationIdentifier);
    if (!registration)
        return completionHandler(false);

    auto clientURL = clientURLFromIdentifier(contextIdentifier);
    if (!clientURL.isValid())
        return completionHandler(Exception { InvalidStateError, "Client is unknown"_s });

    auto jobIdentifier = ServiceWorkerJobIdentifier::generateThreadSafe();
    ASSERT(!m_unregisterJobs.contains(jobIdentifier));
    m_unregisterJobs.add(jobIdentifier, WTFMove(completionHandler));

    server().scheduleUnregisterJob(ServiceWorkerJobDataIdentifier { identifier(), jobIdentifier }, *registration, contextIdentifier, WTFMove(clientURL));
}

std::optional<ServiceWorkerRegistrationData> PortSWServerConnection::clientMatchRegistration(const SecurityOriginData& topOrigin, const URL& clientURL)
{
    if (auto* registration = doRegistrationMatching(topOrigin, clientURL))
        return registration->data();
    return std::nullopt;
}

Vector<ServiceWorkerRegistrationData> PortSWServerConnection::clientGetRegistrations(const SecurityOriginData& topOrigin, const URL& clientURL)
{
    return server().getRegistrations(topOrigin, clientURL);
}

void PortSWServerConnection::clientWhenRegistrationReady(const SecurityOriginData& topOrigin, const URL& clientURL, CompletionHandler<void(std::optional<ServiceWorkerRegistrationData>&&)>&& callback)
{
    whenRegistrationReady(topOrigin, clientURL, WTFMove(callback));
}

void PortSWServerConnection::clientAddRegistration(ServiceWorkerRegistrationIdentifier identifier)
{
    addServiceWorkerRegistrationInServer(identifier);
}

void PortSWServerConnection::clientRemoveRegistration(ServiceWorkerRegistrationIdentifier identifier)
{
    removeServiceWorkerRegistrationInServer(identifier);
}

void PortSWServerConnection::clientDidResolveRegistrationPromise(const ServiceWorkerRegistrationKey& key)
{
    didResolveRegistrationPromise(key);
}

void PortSWServerConnection::clientPostMessageToServiceWorker(ServiceWorkerIdentifier destinationIdentifier, MessageWithMessagePorts&& message, const ServiceWorkerOrClientIdentifier& sourceIdentifier)
{
    auto* destinationWorker = server().workerByID(destinationIdentifier);
    if (!destinationWorker)
        return;

    std::optional<ServiceWorkerOrClientData> sourceData;
    WTF::switchOn(sourceIdentifier, [&](ServiceWorkerIdentifier identifier) {
        if (auto* sourceWorker = server().workerByID(identifier))
            sourceData = ServiceWorkerOrClientData { sourceWorker->data() };
    }, [&](ScriptExecutionContextIdentifier identifier) {
        if (auto clientData = destinationWorker->findClientByIdentifier(identifier))
            sourceData = ServiceWorkerOrClientData { *clientData };
    });

    if (!sourceData)
        return;

    // Deliver a page->worker postMessage (e.g. registration.active.postMessage(...)) to the
    // RUNNING service worker. Mirrors WebSWContextManagerConnection::postMessageToServiceWorker:
    // make sure the worker is running, then hand the message to its ServiceWorkerThreadProxy.
    // We are in-process (no NetworkProcess/WebProcess IPC split), so instead of sending IPC we
    // call the proxy directly. Previously a no-op, so SW message channels silently dropped.
    server().runServiceWorkerIfNecessary(destinationIdentifier, [destinationIdentifier, message = WTFMove(message), sourceData = WTFMove(*sourceData)](auto* contextConnection) mutable {
        if (!contextConnection)
            return;
        if (auto* proxy = SWContextManager::singleton().serviceWorkerThreadProxy(destinationIdentifier))
            proxy->postMessageToServiceWorker(WTFMove(message), WTFMove(sourceData));
    });
}

void PortSWServerConnection::clientRegisterServiceWorkerClient(SecurityOriginData&& topOrigin, ServiceWorkerClientData&& data, const std::optional<ServiceWorkerRegistrationIdentifier>& controllingServiceWorkerRegistrationIdentifier, String&& userAgent)
{
    auto contextOrigin = SecurityOriginData::fromURL(data.url);
    auto clientOrigin = ClientOrigin { WTFMove(topOrigin), WTFMove(contextOrigin) };
    m_clientOrigins.add(data.identifier, clientOrigin);
    server().registerServiceWorkerClient(WTFMove(clientOrigin), WTFMove(data), controllingServiceWorkerRegistrationIdentifier, WTFMove(userAgent));
}

void PortSWServerConnection::clientUnregisterServiceWorkerClient(ScriptExecutionContextIdentifier clientIdentifier)
{
    auto iterator = m_clientOrigins.find(clientIdentifier);
    if (iterator == m_clientOrigins.end())
        return;
    auto clientOrigin = iterator->value;
    server().unregisterServiceWorkerClient(clientOrigin, clientIdentifier);
    m_clientOrigins.remove(iterator);
}

// ---- server -> page (SWServer::Connection interface) ----
void PortSWServerConnection::rejectJobInClient(ServiceWorkerJobIdentifier jobIdentifier, const ExceptionData& exceptionData)
{
    if (auto completionHandler = m_unregisterJobs.take(jobIdentifier))
        return completionHandler(exceptionData.toException());
    if (m_client)
        m_client->deliverJobRejected(jobIdentifier, exceptionData);
}

void PortSWServerConnection::resolveRegistrationJobInClient(ServiceWorkerJobIdentifier jobIdentifier, const ServiceWorkerRegistrationData& data, ShouldNotifyWhenResolved shouldNotifyWhenResolved)
{
    if (m_client)
        m_client->deliverRegistrationJobResolved(jobIdentifier, ServiceWorkerRegistrationData { data }, shouldNotifyWhenResolved);
}

void PortSWServerConnection::resolveUnregistrationJobInClient(ServiceWorkerJobIdentifier jobIdentifier, const ServiceWorkerRegistrationKey&, bool unregistrationResult)
{
    if (auto completionHandler = m_unregisterJobs.take(jobIdentifier))
        completionHandler(unregistrationResult);
}

void PortSWServerConnection::startScriptFetchInClient(ServiceWorkerJobIdentifier jobIdentifier, const ServiceWorkerRegistrationKey& registrationKey, FetchOptions::Cache cachePolicy)
{
    if (m_client)
        m_client->deliverStartScriptFetch(jobIdentifier, registrationKey, cachePolicy);
}

void PortSWServerConnection::updateRegistrationStateInClient(ServiceWorkerRegistrationIdentifier identifier, ServiceWorkerRegistrationState state, const std::optional<ServiceWorkerData>& data)
{
    if (m_client)
        m_client->deliverUpdateRegistrationState(identifier, state, data);
}

void PortSWServerConnection::updateWorkerStateInClient(ServiceWorkerIdentifier identifier, ServiceWorkerState state)
{
    swDiag("SW-DIAG workerState=%ld (0=parsed 1=installing 2=installed 3=activating 4=activated 5=redundant) client=%ld", static_cast<long>(state), !!m_client);
    if (m_client)
        m_client->deliverUpdateWorkerState(identifier, state);
}

void PortSWServerConnection::fireUpdateFoundEvent(ServiceWorkerRegistrationIdentifier identifier)
{
    if (m_client)
        m_client->deliverFireUpdateFoundEvent(identifier);
}

void PortSWServerConnection::setRegistrationLastUpdateTime(ServiceWorkerRegistrationIdentifier identifier, WallTime lastUpdateTime)
{
    if (m_client)
        m_client->deliverSetRegistrationLastUpdateTime(identifier, lastUpdateTime);
}

void PortSWServerConnection::setRegistrationUpdateViaCache(ServiceWorkerRegistrationIdentifier identifier, ServiceWorkerUpdateViaCache updateViaCache)
{
    if (m_client)
        m_client->deliverSetRegistrationUpdateViaCache(identifier, updateViaCache);
}

void PortSWServerConnection::notifyClientsOfControllerChange(const HashSet<ScriptExecutionContextIdentifier>& contextIdentifiers, const ServiceWorkerData& newController)
{
    if (m_client)
        m_client->deliverNotifyClientsOfControllerChange(contextIdentifiers, ServiceWorkerData { newController });
}

void PortSWServerConnection::postMessageToServiceWorkerClient(ScriptExecutionContextIdentifier destinationContextIdentifier, const MessageWithMessagePorts& message, ServiceWorkerIdentifier sourceIdentifier, const String& sourceOrigin)
{
    auto* sourceServiceWorker = server().workerByID(sourceIdentifier);
    if (!sourceServiceWorker || !m_client)
        return;
    m_client->deliverPostMessageToServiceWorkerClient(destinationContextIdentifier, MessageWithMessagePorts { message }, ServiceWorkerData { sourceServiceWorker->data() }, String { sourceOrigin });
}

void PortSWServerConnection::contextConnectionCreated(SWServerToContextConnection&)
{
    // In-process there is no cross-process throttle/registration handshake to do here.
}

// ---------------------------------------------------------------------------
// PortSWContextConnection — runs the SW script in-process.
// ---------------------------------------------------------------------------
PortSWContextConnection::PortSWContextConnection(SWServer& server, RegistrableDomain&& registrableDomain, std::optional<ScriptExecutionContextIdentifier> serviceWorkerPageIdentifier)
    : SWServerToContextConnection(WTFMove(registrableDomain), serviceWorkerPageIdentifier)
    , m_server(server)
{
    // Install the SWContextManager-side connection once (the worker runtime end).
    if (!SWContextManager::singleton().connection()) {
        auto managerConnection = makeUnique<PortSWContextManagerConnection>(server);
        s_portContextManagerConnection = managerConnection.get();
        SWContextManager::singleton().setConnection(WTFMove(managerConnection));
    }
    server.addContextConnection(*this);
}

PortSWContextConnection::~PortSWContextConnection()
{
    if (m_server && m_server->contextConnectionForRegistrableDomain(registrableDomain()) == this)
        m_server->removeContextConnection(*this);
}

// Mirrors WebSWContextManagerConnection::installServiceWorker, calling directly.
void PortSWContextConnection::installServiceWorkerContext(const ServiceWorkerContextData& contextData, const ServiceWorkerData& workerData, const String& userAgent, WorkerThreadMode workerThreadMode)
{
    auto pageConfiguration = pageConfigurationWithEmptyClients(m_server ? m_server->sessionID() : PAL::SessionID::defaultSessionID());

    // Copy the bits we need before the contextData is moved into the thread proxy.
    auto scriptURL = contextData.scriptURL;
    auto topOrigin = contextData.registration.key.topOrigin();
    auto referrerPolicy = contextData.referrerPolicy;
    auto lastNavigationWasAppInitiated = contextData.lastNavigationWasAppInitiated;

    auto effectiveUserAgent = userAgent.isNull() ? standardUserAgent() : userAgent;

    auto page = makeUniqueRef<Page>(WTFMove(pageConfiguration));
    page->setupForRemoteWorker(scriptURL, topOrigin, referrerPolicy);

    std::unique_ptr<NotificationClient> notificationClient;

    auto serviceWorkerThreadProxy = ServiceWorkerThreadProxy::create(WTFMove(page), ServiceWorkerContextData { contextData }, ServiceWorkerData { workerData }, WTFMove(effectiveUserAgent), workerThreadMode, portCacheStorageProvider(), WTFMove(notificationClient));

    serviceWorkerThreadProxy->setLastNavigationWasAppInitiated(lastNavigationWasAppInitiated == LastNavigationWasAppInitiated::Yes);

    swDiag("SW-DIAG installServiceWorkerContext (script running on worker thread)");
    SWContextManager::singleton().registerServiceWorkerThreadForInstall(WTFMove(serviceWorkerThreadProxy));
}

void PortSWContextConnection::updateAppInitiatedValue(ServiceWorkerIdentifier serviceWorkerIdentifier, LastNavigationWasAppInitiated lastNavigationWasAppInitiated)
{
    if (auto* proxy = SWContextManager::singleton().serviceWorkerThreadProxy(serviceWorkerIdentifier))
        proxy->setLastNavigationWasAppInitiated(lastNavigationWasAppInitiated == LastNavigationWasAppInitiated::Yes);
}

void PortSWContextConnection::fireInstallEvent(ServiceWorkerIdentifier identifier)
{
    swDiag("SW-DIAG fireInstallEvent");
    SWContextManager::singleton().fireInstallEvent(identifier);
}

void PortSWContextConnection::fireActivateEvent(ServiceWorkerIdentifier identifier)
{
    swDiag("SW-DIAG fireActivateEvent");
    SWContextManager::singleton().fireActivateEvent(identifier);
}

void PortSWContextConnection::terminateWorker(ServiceWorkerIdentifier identifier)
{
    SWContextManager::singleton().terminateWorker(identifier, SWContextManager::workerTerminationTimeout, nullptr);
}

void PortSWContextConnection::didSaveScriptsToDisk(ServiceWorkerIdentifier, const ScriptBuffer&, const HashMap<URL, ScriptBuffer>&)
{
    // No on-disk script cache in-process; nothing to do.
}

void PortSWContextConnection::matchAllCompleted(uint64_t requestIdentifier, const Vector<ServiceWorkerClientData>& clientsData)
{
    if (s_portContextManagerConnection)
        s_portContextManagerConnection->completeMatchAll(requestIdentifier, Vector<ServiceWorkerClientData> { clientsData });
}

void PortSWContextConnection::firePushEvent(ServiceWorkerIdentifier identifier, const std::optional<Vector<uint8_t>>& data, CompletionHandler<void(bool)>&& callback)
{
    SWContextManager::singleton().firePushEvent(identifier, std::optional<Vector<uint8_t>> { data }, WTFMove(callback));
}

void PortSWContextConnection::connectionIsNoLongerNeeded()
{
    // Owned by the SWServer's context-connection map for the process lifetime.
}

void PortSWContextConnection::terminateDueToUnresponsiveness()
{
    // No separate SW process to kill in-process; leave the worker running.
}

// ---------------------------------------------------------------------------
// PortSWContextManagerConnection — worker runtime -> server. Each callback is
// routed to the matching SWServerToContextConnection base method (which updates
// the SWServer via the global SWServerWorker registry), mirroring the IPC that
// WebSWContextManagerConnection sends to WebSWServerToContextConnection.
// ---------------------------------------------------------------------------
static SWServerToContextConnection* contextConnectionForWorker(ServiceWorkerIdentifier identifier)
{
    auto* worker = SWServerWorker::existingWorkerForIdentifier(identifier);
    return worker ? worker->contextConnection() : nullptr;
}

PortSWContextManagerConnection::PortSWContextManagerConnection(SWServer& server)
    : m_server(server)
    , m_pageIdentifier(PageIdentifier::generate())
{
}

void PortSWContextManagerConnection::establishConnection(CompletionHandler<void()>&& completionHandler)
{
    completionHandler();
}

void PortSWContextManagerConnection::postMessageToServiceWorkerClient(const ScriptExecutionContextIdentifier& destinationIdentifier, const MessageWithMessagePorts& message, ServiceWorkerIdentifier sourceIdentifier, const String& sourceOrigin)
{
    if (!m_server)
        return;
    if (auto* connection = m_server->connection(destinationIdentifier.processIdentifier()))
        connection->postMessageToServiceWorkerClient(destinationIdentifier, message, sourceIdentifier, sourceOrigin);
}

void PortSWContextManagerConnection::serviceWorkerStarted(std::optional<ServiceWorkerJobDataIdentifier> jobDataIdentifier, ServiceWorkerIdentifier serviceWorkerIdentifier, bool doesHandleFetch)
{
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->scriptContextStarted(jobDataIdentifier, serviceWorkerIdentifier, doesHandleFetch);
}

void PortSWContextManagerConnection::serviceWorkerFailedToStart(std::optional<ServiceWorkerJobDataIdentifier> jobDataIdentifier, ServiceWorkerIdentifier serviceWorkerIdentifier, const String& message)
{
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->scriptContextFailedToStart(jobDataIdentifier, serviceWorkerIdentifier, message);
}

void PortSWContextManagerConnection::didFinishInstall(std::optional<ServiceWorkerJobDataIdentifier> jobDataIdentifier, ServiceWorkerIdentifier serviceWorkerIdentifier, bool wasSuccessful)
{
    swDiag("SW-DIAG didFinishInstall ok=%ld conn=%ld", wasSuccessful, !!contextConnectionForWorker(serviceWorkerIdentifier));
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->didFinishInstall(jobDataIdentifier, serviceWorkerIdentifier, wasSuccessful);
}

void PortSWContextManagerConnection::didFinishActivation(ServiceWorkerIdentifier serviceWorkerIdentifier)
{
    swDiag("SW-DIAG didFinishActivation conn=%ld", !!contextConnectionForWorker(serviceWorkerIdentifier));
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->didFinishActivation(serviceWorkerIdentifier);
}

void PortSWContextManagerConnection::setServiceWorkerHasPendingEvents(ServiceWorkerIdentifier serviceWorkerIdentifier, bool hasPendingEvents)
{
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->setServiceWorkerHasPendingEvents(serviceWorkerIdentifier, hasPendingEvents);
}

void PortSWContextManagerConnection::workerTerminated(ServiceWorkerIdentifier serviceWorkerIdentifier)
{
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->workerTerminated(serviceWorkerIdentifier);
}

void PortSWContextManagerConnection::skipWaiting(ServiceWorkerIdentifier serviceWorkerIdentifier, CompletionHandler<void()>&& callback)
{
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->skipWaiting(serviceWorkerIdentifier, WTFMove(callback));
    else
        callback();
}

void PortSWContextManagerConnection::setScriptResource(ServiceWorkerIdentifier serviceWorkerIdentifier, const URL& url, const ServiceWorkerContextData::ImportedScript& script)
{
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->setScriptResource(serviceWorkerIdentifier, URL { url }, ServiceWorkerContextData::ImportedScript { script });
}

void PortSWContextManagerConnection::findClientByVisibleIdentifier(ServiceWorkerIdentifier serviceWorkerIdentifier, const String& clientIdentifier, FindClientByIdentifierCallback&& callback)
{
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->findClientByVisibleIdentifier(serviceWorkerIdentifier, clientIdentifier, WTFMove(callback));
    else
        callback({ });
}

void PortSWContextManagerConnection::matchAll(ServiceWorkerIdentifier serviceWorkerIdentifier, const ServiceWorkerClientQueryOptions& options, ServiceWorkerClientsMatchAllCallback&& callback)
{
    auto requestIdentifier = ++m_previousRequestIdentifier;
    m_matchAllRequests.add(requestIdentifier, WTFMove(callback));
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->matchAll(requestIdentifier, serviceWorkerIdentifier, options);
    else
        completeMatchAll(requestIdentifier, { });
}

void PortSWContextManagerConnection::completeMatchAll(uint64_t requestIdentifier, Vector<ServiceWorkerClientData>&& clientsData)
{
    if (auto callback = m_matchAllRequests.take(requestIdentifier))
        callback(WTFMove(clientsData));
}

void PortSWContextManagerConnection::claim(ServiceWorkerIdentifier serviceWorkerIdentifier, CompletionHandler<void(ExceptionOr<void>&&)>&& callback)
{
    auto* connection = contextConnectionForWorker(serviceWorkerIdentifier);
    if (!connection)
        return callback({ });
    connection->claim(serviceWorkerIdentifier, [callback = WTFMove(callback)](std::optional<ExceptionData>&& error) mutable {
        callback(error ? ExceptionOr<void> { error->toException() } : ExceptionOr<void> { });
    });
}

void PortSWContextManagerConnection::didFailHeartBeatCheck(ServiceWorkerIdentifier serviceWorkerIdentifier)
{
    if (auto* connection = contextConnectionForWorker(serviceWorkerIdentifier))
        connection->didFailHeartBeatCheck(serviceWorkerIdentifier);
}

bool PortSWContextManagerConnection::isThrottleable() const
{
    return false;
}

PageIdentifier PortSWContextManagerConnection::pageIdentifier() const
{
    return m_pageIdentifier;
}

// ---------------------------------------------------------------------------
// PortServiceWorkerFetchClient — SW FetchEvent result -> WebCore ResourceLoader.
// The SW runs on the main thread (shouldRunServiceWorkersOnMainThreadForTesting),
// so these callbacks arrive on the main thread and can drive the loader directly.
// ---------------------------------------------------------------------------
PortServiceWorkerFetchClient::PortServiceWorkerFetchClient(ResourceLoader& loader)
    : m_loader(&loader)
{
}

void PortServiceWorkerFetchClient::didReceiveResponse(const ResourceResponse& response)
{
    ASSERT(isMainThread());
    if (m_loader)
        m_loader->didReceiveResponse(response, [] { });
}

void PortServiceWorkerFetchClient::didReceiveRedirection(const ResourceResponse&)
{
    // A redirect response from the SW needs willSendRequest handling; not modeled
    // yet. Fall through to network so the load still completes correctly.
    fallThroughToNetwork();
}

void PortServiceWorkerFetchClient::didReceiveData(const SharedBuffer& buffer)
{
    ASSERT(isMainThread());
    if (m_loader)
        m_loader->didReceiveBuffer(buffer, buffer.size(), DataPayloadBytes);
}

void PortServiceWorkerFetchClient::didReceiveFormDataAndFinish(Ref<FormData>&&)
{
    // Streaming a FormData body back into the loader is a follow-up; fall through.
    fallThroughToNetwork();
}

void PortServiceWorkerFetchClient::didFail(const ResourceError& error)
{
    ASSERT(isMainThread());
    if (auto loader = std::exchange(m_loader, nullptr))
        loader->didFail(error);
}

void PortServiceWorkerFetchClient::didFinish(const NetworkLoadMetrics& metrics)
{
    ASSERT(isMainThread());
    if (auto loader = std::exchange(m_loader, nullptr))
        loader->didFinishLoading(metrics);
}

void PortServiceWorkerFetchClient::didNotHandle()
{
    // The SW did not call respondWith(): perform the normal network load.
    fallThroughToNetwork();
}

void PortServiceWorkerFetchClient::cancel()
{
    if (auto loader = std::exchange(m_loader, nullptr))
        loader->cancel();
}

void PortServiceWorkerFetchClient::continueDidReceiveResponse()
{
    // We do not gate on a continue handshake in-process.
}

void PortServiceWorkerFetchClient::convertFetchToDownload()
{
    // Downloads from a SW-intercepted fetch are not supported in this port.
    if (auto loader = std::exchange(m_loader, nullptr))
        loader->cancel();
}

void PortServiceWorkerFetchClient::fallThroughToNetwork()
{
    ASSERT(isMainThread());
    if (auto loader = std::exchange(m_loader, nullptr))
        loader->start();
}

// ---------------------------------------------------------------------------
// Loader fetch-interception hook.
// ---------------------------------------------------------------------------
bool maybeStartServiceWorkerFetch(ResourceLoader& loader)
{
    if (!isMainThread() || !s_portSWServer)
        return false;

    const auto& options = loader.options();
    if (options.serviceWorkersMode == ServiceWorkersMode::None)
        return false;

    if (!options.serviceWorkerRegistrationIdentifier)
        return false;

    auto* registration = s_portSWServer->getRegistration(*options.serviceWorkerRegistrationIdentifier);
    if (!registration)
        return false;

    auto* worker = registration->activeWorker();
    if (!worker)
        return false;

    // Only intercept once the worker is already running. Pre-warming the worker for
    // the first controlled fetch (server().runServiceWorkerIfNecessary) is a follow-up;
    // until then such a load falls through to the network normally.
    auto* proxy = SWContextManager::singleton().serviceWorkerThreadProxy(worker->identifier());
    if (!proxy)
        return false;

    auto request = loader.request();
    auto referrer = request.httpReferrer();
    FetchOptions fetchOptions = options;

    proxy->startFetch(Process::identifier(), FetchIdentifier::generate(), PortServiceWorkerFetchClient::create(loader), WTFMove(request), WTFMove(referrer), WTFMove(fetchOptions), false, { }, { });
    return true;
}

// ---------------------------------------------------------------------------
// PortServiceWorkerProvider
// ---------------------------------------------------------------------------
PortServiceWorkerProvider::PortServiceWorkerProvider(Ref<PortSWClientConnection>&& connection)
    : m_connection(WTFMove(connection))
{
}

SWClientConnection& PortServiceWorkerProvider::serviceWorkerConnection()
{
    return m_connection.get();
}

void PortServiceWorkerProvider::terminateWorkerForTesting(ServiceWorkerIdentifier, CompletionHandler<void()>&& callback)
{
    callback();
}

// ---------------------------------------------------------------------------
// installPortServiceWorkerProvider()
// ---------------------------------------------------------------------------
void installPortServiceWorkerProvider()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

    // Ephemeral session => no on-disk RegistrationStore, import completes immediately
    // so scheduled jobs run right away (see SWServer ctor + runOrDelayTaskForImport).
    auto sessionID = PAL::SessionID::generateEphemeralSessionID();

    // hasServiceWorkerEntitlement=true bypasses app-bound-domain gating in
    // SWServer::scheduleJob (we have no UIProcess to answer GetAppBoundDomains).
    // s_portSWServer (file scope) lets the SWServer callbacks + the loader fetch hook
    // reach the server without a dangling capture.
    s_portSWServer = new SWServer(makeUniqueRef<PortSWOriginStore>(), /* processTerminationDelayEnabled */ true, String { }, sessionID,
        /* shouldRunServiceWorkersOnMainThreadForTesting */ true, /* hasServiceWorkerEntitlement */ true,
        // SoftUpdateCallback — soft update not wired yet.
        [](ServiceWorkerJobData&&, bool, ResourceRequest&&, CompletionHandler<void(const WorkerFetchResult&)>&& completionHandler) {
            completionHandler(WorkerFetchResult { });
        },
        // CreateContextConnectionCallback — spin up an in-process context connection on demand.
        // The connection registers itself with the server (addContextConnection) and is owned
        // by the server's context-connection map for the process lifetime (intentional leak).
        [](const RegistrableDomain& domain, std::optional<ScriptExecutionContextIdentifier> pageID, CompletionHandler<void()>&& completionHandler) {
            if (s_portSWServer)
                new PortSWContextConnection(*s_portSWServer, RegistrableDomain { domain }, pageID);
            completionHandler();
        },
        // AppBoundDomainsCallback — none.
        [](CompletionHandler<void(HashSet<RegistrableDomain>&&)>&& completionHandler) {
            completionHandler({ });
        });
    auto* server = s_portSWServer;

    // Wire the connection pair (each references the other).
    auto clientConnection = PortSWClientConnection::create();
    auto serverConnection = makeUnique<PortSWServerConnection>(*server, Process::identifier());
    auto* rawServerConnection = serverConnection.get();
    rawServerConnection->setClient(clientConnection.get());
    clientConnection->setServerConnection(*rawServerConnection);
    server->addConnection(WTFMove(serverConnection));

    static NeverDestroyed<PortServiceWorkerProvider> provider(WTFMove(clientConnection));
    ServiceWorkerProvider::setSharedProvider(provider.get());
    ServiceWorkerProvider::singleton().setMayHaveRegisteredServiceWorkers();
}

} // namespace WebCorePort

#endif // ENABLE(SERVICE_WORKER)
