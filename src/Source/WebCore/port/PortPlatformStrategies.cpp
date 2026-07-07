// ============================================================================
// PortPlatformStrategies.cpp  —  minimal PlatformStrategies for the in-process
// W10M/UWP render driver. Compiled into WebCore.lib (see PlatformWin.cmake).
//
// WebCore::Page requires a global PlatformStrategies. For the first milestone
// (render a local HTML string -> cairo -> pixels) no resources are loaded, so a
// no-op LoaderStrategy is sufficient and dependency-free. The real network loader
// (WebResourceLoadScheduler) is swapped in for milestone #3 (real URLs/images).
//
// install once at driver init:  WebCorePort::installPortPlatformStrategies();
// ============================================================================

#include "config.h"

#include "BlobRegistry.h"
#include "BlobRegistryImpl.h"
#include "LoaderStrategy.h"
#include "MediaStrategy.h"
#include "PlatformStrategies.h"
#include "ResourceError.h"
#include "ResourceResponse.h"
#include "SubresourceLoader.h"   // complete type for RefPtr<SubresourceLoader> in loadResource()
#include "WebResourceLoadScheduler.h" // real curl-backed LoaderStrategy
#include <wtf/NeverDestroyed.h>

namespace WebCorePort {

using namespace WebCore;

// ---- no-op LoaderStrategy ---------------------------------------------------
// Every CompletionHandler MUST be invoked (WTF::CompletionHandler asserts on
// destruction if not called). Loads simply fail; for inline HTML they are never
// triggered.
class NoOpLoaderStrategy final : public LoaderStrategy {
    void loadResource(Frame&, CachedResource&, ResourceRequest&&, const ResourceLoaderOptions&, CompletionHandler<void(RefPtr<SubresourceLoader>&&)>&& completionHandler) final { completionHandler(nullptr); }
    void loadResourceSynchronously(FrameLoader&, ResourceLoaderIdentifier, const ResourceRequest&, ClientCredentialPolicy, const FetchOptions&, const HTTPHeaderMap&, ResourceError& error, ResourceResponse&, Vector<uint8_t>&) final { error = ResourceError { }; }
    void pageLoadCompleted(Page&) final { }
    void browsingContextRemoved(Frame&) final { }
    void remove(ResourceLoader*) final { }
    void setDefersLoading(ResourceLoader&, bool) final { }
    void crossOriginRedirectReceived(ResourceLoader*, const URL&) final { }
    void servePendingRequests(ResourceLoadPriority) final { }
    void suspendPendingRequests() final { }
    void resumePendingRequests() final { }
    void startPingLoad(Frame&, ResourceRequest&, const HTTPHeaderMap&, const FetchOptions&, ContentSecurityPolicyImposition, PingLoadCompletionHandler&&) final { } // WTF::Function — no must-call assert
    void preconnectTo(FrameLoader&, const URL&, StoredCredentialsPolicy, ShouldPreconnectAsFirstParty, PreconnectCompletionHandler&&) final { }
    void setCaptureExtraNetworkLoadMetricsEnabled(bool) final { }
    bool isOnLine() const final { return true; }
    void addOnlineStateChangeListener(Function<void(bool)>&&) final { }
    void isResourceLoadFinished(CachedResource&, CompletionHandler<void(bool)>&& callback) final { callback(true); }
};

// No-op BlobRegistry — the static-HTML render path creates no blob URLs. Real
// Real blob support: a BlobRegistry (the abstract, process-facing interface) that WRAPS a
// WebCore::BlobRegistryImpl (the actual in-memory store + blob: resource loader). Forwards
// each virtual to the impl (adapting the arg differences between the two APIs), and exposes
// the impl via blobRegistryImpl() so the "blob" ResourceHandle constructor can serve loads.
// Previously a no-op stub → createObjectURL registered nothing and blob: fetches fell through
// to curl and failed (broke Cloudflare Turnstile captchas, workers, object URLs).
class PortBlobRegistry final : public BlobRegistry {
    WebCore::BlobRegistryImpl m_impl;
public:
    void registerFileBlobURL(const URL& url, Ref<BlobDataFileReference>&& ref, const String&, const String& contentType) final { m_impl.registerFileBlobURL(url, WTFMove(ref), contentType); }
    void registerBlobURL(const URL& url, Vector<BlobPart>&& parts, const String& contentType) final { m_impl.registerBlobURL(url, WTFMove(parts), contentType); }
    void registerBlobURL(const URL& url, const URL& srcURL, const PolicyContainer& policy) final { m_impl.registerBlobURL(url, srcURL, policy); }
    void registerBlobURLOptionallyFileBacked(const URL& url, const URL& srcURL, RefPtr<BlobDataFileReference>&& ref, const String& contentType) final { m_impl.registerBlobURLOptionallyFileBacked(url, srcURL, WTFMove(ref), contentType, PolicyContainer { }); }
    void registerBlobURLForSlice(const URL& url, const URL& srcURL, long long start, long long end, const String& contentType) final { m_impl.registerBlobURLForSlice(url, srcURL, start, end, contentType); }
    void unregisterBlobURL(const URL& url) final { m_impl.unregisterBlobURL(url); }
    void registerBlobURLHandle(const URL& url) final { m_impl.registerBlobURLHandle(url); }
    void unregisterBlobURLHandle(const URL& url) final { m_impl.unregisterBlobURLHandle(url); }
    unsigned long long blobSize(const URL& url) final { return m_impl.blobSize(url); }
    void writeBlobsToTemporaryFilesForIndexedDB(const Vector<String>& urls, CompletionHandler<void(Vector<String>&&)>&& ch) final { m_impl.writeBlobsToTemporaryFilesForIndexedDB(urls, WTFMove(ch)); }
    BlobRegistryImpl* blobRegistryImpl() final { return &m_impl; }
};

class PortPlatformStrategies final : public PlatformStrategies {
    // Real curl-backed loader. The inline-HTML render path (DocumentWriter) loads no
    // subresources so never touches it; URL loads (WebCoreRenderUrl) drive it.
    LoaderStrategy* createLoaderStrategy() final { return new WebResourceLoadScheduler; }
    PasteboardStrategy* createPasteboardStrategy() final { return nullptr; } // render never touches the pasteboard
    MediaStrategy* createMediaStrategy() final { class PortMediaStrategy final : public MediaStrategy { }; return new PortMediaStrategy; }
    BlobRegistry* createBlobRegistry() final { return new PortBlobRegistry; }
};

} // namespace WebCorePort

namespace WebCorePort {

void installPortPlatformStrategies()
{
    static WTF::NeverDestroyed<PortPlatformStrategies> strategies;
    if (!WebCore::hasPlatformStrategies())
        WebCore::setPlatformStrategies(&strategies.get());
}

} // namespace WebCorePort
