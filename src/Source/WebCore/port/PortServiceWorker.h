// ============================================================================
// PortServiceWorker.h — in-process ServiceWorker wiring for the W10M/UWP WebCore
// port. This single-process port has no WebProcess/NetworkProcess split, so the
// SWServer lives in the SAME process as the page. The client<->server connection
// pair that WebKit2 bridges over IPC is here just direct C++ calls.
//
// Class map (mirrors WebKit2, minus IPC):
//   PortSWClientConnection   : WebCore::SWClientConnection      (page -> server)
//   PortSWServerConnection    : WebCore::SWServer::Connection    (server -> page)
//   PortSWContextConnection   : WebCore::SWServerToContextConnection (server -> SW script) [SCAFFOLD]
//   PortServiceWorkerProvider : WebCore::ServiceWorkerProvider   (owns the client connection)
//
// WebKit2 reference (logic mirrored, IPC removed):
//   WebProcess/Storage/WebSWClientConnection.{h,cpp}
//   NetworkProcess/ServiceWorker/WebSWServerConnection.{h,cpp}
//   NetworkProcess/ServiceWorker/WebSWServerToContextConnection.{h,cpp}
// ============================================================================
#pragma once

#if ENABLE(SERVICE_WORKER)

#include "ClientOrigin.h"
#include "PageIdentifier.h"
#include "SWClientConnection.h"
#include "SWContextManager.h"
#include "SWServer.h"
#include "SWServerToContextConnection.h"
#include "ServiceWorkerFetch.h"
#include "ServiceWorkerProvider.h"
#include <wtf/HashMap.h>
#include <wtf/Ref.h>
#include <wtf/RefPtr.h>
#include <wtf/WeakPtr.h>

namespace WebCore {
class ResourceLoader;
}

namespace WebCorePort {

class PortSWServerConnection;

// ---------------------------------------------------------------------------
// Page -> server side. Implements all 22 SWClientConnection pure virtuals.
// Forwards scheduling calls straight into the paired PortSWServerConnection;
// exposes deliver*() wrappers so the server side can push results back into the
// (protected) SWClientConnection base entry points.
// ---------------------------------------------------------------------------
class PortSWClientConnection final : public WebCore::SWClientConnection {
public:
    static Ref<PortSWClientConnection> create() { return adoptRef(*new PortSWClientConnection); }
    ~PortSWClientConnection() final;

    // Wired up right after both connections are constructed (they reference each other).
    void setServerConnection(PortSWServerConnection& serverConnection) { m_serverConnection = &serverConnection; }

    // ---- results pushed back from PortSWServerConnection into the WebCore base ----
    void deliverJobRejected(WebCore::ServiceWorkerJobIdentifier, const WebCore::ExceptionData&);
    void deliverRegistrationJobResolved(WebCore::ServiceWorkerJobIdentifier, WebCore::ServiceWorkerRegistrationData&&, WebCore::ShouldNotifyWhenResolved);
    void deliverStartScriptFetch(WebCore::ServiceWorkerJobIdentifier, const WebCore::ServiceWorkerRegistrationKey&, WebCore::FetchOptions::Cache);
    void deliverPostMessageToServiceWorkerClient(WebCore::ScriptExecutionContextIdentifier, WebCore::MessageWithMessagePorts&&, WebCore::ServiceWorkerData&&, String&& sourceOrigin);
    void deliverUpdateRegistrationState(WebCore::ServiceWorkerRegistrationIdentifier, WebCore::ServiceWorkerRegistrationState, const std::optional<WebCore::ServiceWorkerData>&);
    void deliverUpdateWorkerState(WebCore::ServiceWorkerIdentifier, WebCore::ServiceWorkerState);
    void deliverFireUpdateFoundEvent(WebCore::ServiceWorkerRegistrationIdentifier);
    void deliverSetRegistrationLastUpdateTime(WebCore::ServiceWorkerRegistrationIdentifier, WallTime);
    void deliverSetRegistrationUpdateViaCache(WebCore::ServiceWorkerRegistrationIdentifier, WebCore::ServiceWorkerUpdateViaCache);
    void deliverNotifyClientsOfControllerChange(const HashSet<WebCore::ScriptExecutionContextIdentifier>&, WebCore::ServiceWorkerData&& newController);

private:
    PortSWClientConnection();

    // SWClientConnection interface (22 pure virtuals).
    void scheduleJobInServer(const WebCore::ServiceWorkerJobData&) final;
    void finishFetchingScriptInServer(const WebCore::ServiceWorkerJobDataIdentifier&, const WebCore::ServiceWorkerRegistrationKey&, const WebCore::WorkerFetchResult&) final;
    void postMessageToServiceWorker(WebCore::ServiceWorkerIdentifier destination, WebCore::MessageWithMessagePorts&&, const WebCore::ServiceWorkerOrClientIdentifier& source) final;
    void registerServiceWorkerClient(const WebCore::SecurityOrigin& topOrigin, const WebCore::ServiceWorkerClientData&, const std::optional<WebCore::ServiceWorkerRegistrationIdentifier>&, const String& userAgent) final;
    void unregisterServiceWorkerClient(WebCore::ScriptExecutionContextIdentifier) final;
    void scheduleUnregisterJobInServer(WebCore::ServiceWorkerRegistrationIdentifier, WebCore::ServiceWorkerOrClientIdentifier, CompletionHandler<void(WebCore::ExceptionOr<bool>&&)>&&) final;
    void matchRegistration(WebCore::SecurityOriginData&& topOrigin, const URL& clientURL, RegistrationCallback&&) final;
    void getRegistrations(WebCore::SecurityOriginData&& topOrigin, const URL& clientURL, GetRegistrationsCallback&&) final;
    void whenRegistrationReady(const WebCore::SecurityOriginData& topOrigin, const URL& clientURL, WhenRegistrationReadyCallback&&) final;
    void addServiceWorkerRegistrationInServer(WebCore::ServiceWorkerRegistrationIdentifier) final;
    void removeServiceWorkerRegistrationInServer(WebCore::ServiceWorkerRegistrationIdentifier) final;
    void didResolveRegistrationPromise(const WebCore::ServiceWorkerRegistrationKey&) final;
    WebCore::SWServerConnectionIdentifier serverConnectionIdentifier() const final;
    bool mayHaveServiceWorkerRegisteredForOrigin(const WebCore::SecurityOriginData&) const final;

    // Optional features — reported unsupported synchronously.
    void subscribeToPushService(WebCore::ServiceWorkerRegistrationIdentifier, const Vector<uint8_t>& applicationServerKey, SubscribeToPushServiceCallback&&) final;
    void unsubscribeFromPushService(WebCore::ServiceWorkerRegistrationIdentifier, WebCore::PushSubscriptionIdentifier, UnsubscribeFromPushServiceCallback&&) final;
    void getPushSubscription(WebCore::ServiceWorkerRegistrationIdentifier, GetPushSubscriptionCallback&&) final;
    void getPushPermissionState(WebCore::ServiceWorkerRegistrationIdentifier, GetPushPermissionStateCallback&&) final;
    void enableNavigationPreload(WebCore::ServiceWorkerRegistrationIdentifier, ExceptionOrVoidCallback&&) final;
    void disableNavigationPreload(WebCore::ServiceWorkerRegistrationIdentifier, ExceptionOrVoidCallback&&) final;
    void setNavigationPreloadHeaderValue(WebCore::ServiceWorkerRegistrationIdentifier, String&&, ExceptionOrVoidCallback&&) final;
    void getNavigationPreloadState(WebCore::ServiceWorkerRegistrationIdentifier, ExceptionOrNavigationPreloadStateCallback&&) final;

    PortSWServerConnection* m_serverConnection { nullptr };
};

// ---------------------------------------------------------------------------
// Server -> page side. Implements the 12 SWServer::Connection pure virtuals by
// forwarding directly into the paired PortSWClientConnection. Also carries the
// "client calls the server" helpers (drive the SWServer using its public API).
// ---------------------------------------------------------------------------
class PortSWServerConnection final : public WebCore::SWServer::Connection {
public:
    PortSWServerConnection(WebCore::SWServer&, WebCore::SWServerConnectionIdentifier);
    ~PortSWServerConnection() final;

    void setClient(PortSWClientConnection& client) { m_client = &client; }

    // ---- called by PortSWClientConnection (page -> server) ----
    void clientScheduleJob(const WebCore::ServiceWorkerJobData&);
    void clientFinishFetchingScript(const WebCore::ServiceWorkerJobDataIdentifier&, const WebCore::ServiceWorkerRegistrationKey&, const WebCore::WorkerFetchResult&);
    void clientScheduleUnregisterJob(WebCore::ServiceWorkerRegistrationIdentifier, WebCore::ServiceWorkerOrClientIdentifier, CompletionHandler<void(WebCore::ExceptionOr<bool>&&)>&&);
    std::optional<WebCore::ServiceWorkerRegistrationData> clientMatchRegistration(const WebCore::SecurityOriginData& topOrigin, const URL& clientURL);
    Vector<WebCore::ServiceWorkerRegistrationData> clientGetRegistrations(const WebCore::SecurityOriginData& topOrigin, const URL& clientURL);
    void clientWhenRegistrationReady(const WebCore::SecurityOriginData& topOrigin, const URL& clientURL, CompletionHandler<void(std::optional<WebCore::ServiceWorkerRegistrationData>&&)>&&);
    void clientAddRegistration(WebCore::ServiceWorkerRegistrationIdentifier);
    void clientRemoveRegistration(WebCore::ServiceWorkerRegistrationIdentifier);
    void clientDidResolveRegistrationPromise(const WebCore::ServiceWorkerRegistrationKey&);
    void clientPostMessageToServiceWorker(WebCore::ServiceWorkerIdentifier destination, WebCore::MessageWithMessagePorts&&, const WebCore::ServiceWorkerOrClientIdentifier& source);
    void clientRegisterServiceWorkerClient(WebCore::SecurityOriginData&& topOrigin, WebCore::ServiceWorkerClientData&&, const std::optional<WebCore::ServiceWorkerRegistrationIdentifier>&, String&& userAgent);
    void clientUnregisterServiceWorkerClient(WebCore::ScriptExecutionContextIdentifier);

private:
    // SWServer::Connection interface — messages to the "client process" (here: direct calls).
    void updateRegistrationStateInClient(WebCore::ServiceWorkerRegistrationIdentifier, WebCore::ServiceWorkerRegistrationState, const std::optional<WebCore::ServiceWorkerData>&) final;
    void updateWorkerStateInClient(WebCore::ServiceWorkerIdentifier, WebCore::ServiceWorkerState) final;
    void fireUpdateFoundEvent(WebCore::ServiceWorkerRegistrationIdentifier) final;
    void setRegistrationLastUpdateTime(WebCore::ServiceWorkerRegistrationIdentifier, WallTime) final;
    void setRegistrationUpdateViaCache(WebCore::ServiceWorkerRegistrationIdentifier, WebCore::ServiceWorkerUpdateViaCache) final;
    void notifyClientsOfControllerChange(const HashSet<WebCore::ScriptExecutionContextIdentifier>& contextIdentifiers, const WebCore::ServiceWorkerData& newController) final;
    void postMessageToServiceWorkerClient(WebCore::ScriptExecutionContextIdentifier, const WebCore::MessageWithMessagePorts&, WebCore::ServiceWorkerIdentifier, const String& sourceOrigin) final;
    void contextConnectionCreated(WebCore::SWServerToContextConnection&) final;

    void rejectJobInClient(WebCore::ServiceWorkerJobIdentifier, const WebCore::ExceptionData&) final;
    void resolveRegistrationJobInClient(WebCore::ServiceWorkerJobIdentifier, const WebCore::ServiceWorkerRegistrationData&, WebCore::ShouldNotifyWhenResolved) final;
    void resolveUnregistrationJobInClient(WebCore::ServiceWorkerJobIdentifier, const WebCore::ServiceWorkerRegistrationKey&, bool registrationResult) final;
    void startScriptFetchInClient(WebCore::ServiceWorkerJobIdentifier, const WebCore::ServiceWorkerRegistrationKey&, WebCore::FetchOptions::Cache) final;

    URL clientURLFromIdentifier(WebCore::ServiceWorkerOrClientIdentifier);

    PortSWClientConnection* m_client { nullptr };
    HashMap<WebCore::ScriptExecutionContextIdentifier, WebCore::ClientOrigin> m_clientOrigins;
    HashMap<WebCore::ServiceWorkerJobIdentifier, CompletionHandler<void(WebCore::ExceptionOr<bool>&&)>> m_unregisterJobs;
};

// ---------------------------------------------------------------------------
// Server -> SW-script-runtime side. Runs the SW script in-process: builds a
// ServiceWorkerThreadProxy and drives it through SWContextManager. The paired
// SWContextManager::Connection (PortSWContextManagerConnection, below) routes the
// worker's callbacks back to the SWServer via this connection's base methods.
// ---------------------------------------------------------------------------
class PortSWContextConnection final : public WebCore::SWServerToContextConnection {
public:
    PortSWContextConnection(WebCore::SWServer&, WebCore::RegistrableDomain&&, std::optional<WebCore::ScriptExecutionContextIdentifier>);
    ~PortSWContextConnection() final;

    WebCore::SWServer* server() const { return m_server.get(); }

private:
    void installServiceWorkerContext(const WebCore::ServiceWorkerContextData&, const WebCore::ServiceWorkerData&, const String& userAgent, WebCore::WorkerThreadMode) final;
    void updateAppInitiatedValue(WebCore::ServiceWorkerIdentifier, WebCore::LastNavigationWasAppInitiated) final;
    void fireInstallEvent(WebCore::ServiceWorkerIdentifier) final;
    void fireActivateEvent(WebCore::ServiceWorkerIdentifier) final;
    void terminateWorker(WebCore::ServiceWorkerIdentifier) final;
    void didSaveScriptsToDisk(WebCore::ServiceWorkerIdentifier, const WebCore::ScriptBuffer&, const HashMap<URL, WebCore::ScriptBuffer>& importedScripts) final;
    void matchAllCompleted(uint64_t requestIdentifier, const Vector<WebCore::ServiceWorkerClientData>&) final;
    void firePushEvent(WebCore::ServiceWorkerIdentifier, const std::optional<Vector<uint8_t>>&, CompletionHandler<void(bool)>&&) final;
    void connectionIsNoLongerNeeded() final;
    void terminateDueToUnresponsiveness() final;

    WeakPtr<WebCore::SWServer> m_server;
};

// ---------------------------------------------------------------------------
// SW-script-runtime -> server side (the counterpart SWContextManager expects).
// In WebKit2 this is WebSWContextManagerConnection, which sends IPC back to the
// NetworkProcess. In-process it forwards each worker callback directly to the
// matching SWServerToContextConnection base method (which updates the SWServer).
// A single instance is installed via SWContextManager::singleton().setConnection.
// ---------------------------------------------------------------------------
class PortSWContextManagerConnection final : public WebCore::SWContextManager::Connection {
public:
    explicit PortSWContextManagerConnection(WebCore::SWServer&);

    // Called by PortSWContextConnection::matchAllCompleted to complete a matchAll.
    void completeMatchAll(uint64_t requestIdentifier, Vector<WebCore::ServiceWorkerClientData>&&);

private:
    // SWContextManager::Connection interface.
    void establishConnection(CompletionHandler<void()>&&) final;
    void postMessageToServiceWorkerClient(const WebCore::ScriptExecutionContextIdentifier& destinationIdentifier, const WebCore::MessageWithMessagePorts&, WebCore::ServiceWorkerIdentifier source, const String& sourceOrigin) final;
    void serviceWorkerStarted(std::optional<WebCore::ServiceWorkerJobDataIdentifier>, WebCore::ServiceWorkerIdentifier, bool doesHandleFetch) final;
    void serviceWorkerFailedToStart(std::optional<WebCore::ServiceWorkerJobDataIdentifier>, WebCore::ServiceWorkerIdentifier, const String& message) final;
    void didFinishInstall(std::optional<WebCore::ServiceWorkerJobDataIdentifier>, WebCore::ServiceWorkerIdentifier, bool wasSuccessful) final;
    void didFinishActivation(WebCore::ServiceWorkerIdentifier) final;
    void setServiceWorkerHasPendingEvents(WebCore::ServiceWorkerIdentifier, bool) final;
    void workerTerminated(WebCore::ServiceWorkerIdentifier) final;
    void skipWaiting(WebCore::ServiceWorkerIdentifier, CompletionHandler<void()>&&) final;
    void setScriptResource(WebCore::ServiceWorkerIdentifier, const URL&, const WebCore::ServiceWorkerContextData::ImportedScript&) final;
    void findClientByVisibleIdentifier(WebCore::ServiceWorkerIdentifier, const String&, FindClientByIdentifierCallback&&) final;
    void matchAll(WebCore::ServiceWorkerIdentifier, const WebCore::ServiceWorkerClientQueryOptions&, WebCore::ServiceWorkerClientsMatchAllCallback&&) final;
    void claim(WebCore::ServiceWorkerIdentifier, CompletionHandler<void(WebCore::ExceptionOr<void>&&)>&&) final;
    void didFailHeartBeatCheck(WebCore::ServiceWorkerIdentifier) final;
    bool isThrottleable() const final;
    WebCore::PageIdentifier pageIdentifier() const final;

    WeakPtr<WebCore::SWServer> m_server;
    WebCore::PageIdentifier m_pageIdentifier;
    uint64_t m_previousRequestIdentifier { 0 };
    HashMap<uint64_t, WebCore::ServiceWorkerClientsMatchAllCallback> m_matchAllRequests;
};

// ---------------------------------------------------------------------------
// Bridges a running SW's FetchEvent result back into a WebCore ResourceLoader.
// Mirrors WebServiceWorkerFetchTaskClient, but delivers directly to the loader
// instead of over IPC. On didNotHandle() it falls the load through to network.
// ---------------------------------------------------------------------------
class PortServiceWorkerFetchClient final : public WebCore::ServiceWorkerFetch::Client {
public:
    static Ref<PortServiceWorkerFetchClient> create(WebCore::ResourceLoader& loader) { return adoptRef(*new PortServiceWorkerFetchClient(loader)); }

private:
    explicit PortServiceWorkerFetchClient(WebCore::ResourceLoader&);

    void didReceiveResponse(const WebCore::ResourceResponse&) final;
    void didReceiveRedirection(const WebCore::ResourceResponse&) final;
    void didReceiveData(const WebCore::SharedBuffer&) final;
    void didReceiveFormDataAndFinish(Ref<WebCore::FormData>&&) final;
    void didFail(const WebCore::ResourceError&) final;
    void didFinish(const WebCore::NetworkLoadMetrics&) final;
    void didNotHandle() final;
    void cancel() final;
    void continueDidReceiveResponse() final;
    void convertFetchToDownload() final;

    void fallThroughToNetwork();

    RefPtr<WebCore::ResourceLoader> m_loader;
};

// ---------------------------------------------------------------------------
// The provider WebCore::ServiceWorkerProvider::singleton() resolves to. Owns the
// single in-process SWClientConnection.
// ---------------------------------------------------------------------------
class PortServiceWorkerProvider final : public WebCore::ServiceWorkerProvider {
public:
    explicit PortServiceWorkerProvider(Ref<PortSWClientConnection>&&);

    WebCore::SWClientConnection& serviceWorkerConnection() final;
    void terminateWorkerForTesting(WebCore::ServiceWorkerIdentifier, CompletionHandler<void()>&&) final;

private:
    Ref<PortSWClientConnection> m_connection;
};

// Build the in-process SWServer (in-memory store), the connection pair, and the
// provider; then hand the provider to WebCore. Idempotent. Call once at startup
// on the main run loop (see WebCoreDriver ensureInit()).
void installPortServiceWorkerProvider();

// Fetch-interception hook for the loader strategy. If the loader's request is
// controlled by an active, running service worker, dispatches a FetchEvent to it
// and returns true (the loader is now driven by the SW; on respondWith the response
// flows into the loader, on no respondWith it falls through to network). Returns
// false if there is no controlling SW — caller should schedule the normal load.
bool maybeStartServiceWorkerFetch(WebCore::ResourceLoader&);

} // namespace WebCorePort

#endif // ENABLE(SERVICE_WORKER)
