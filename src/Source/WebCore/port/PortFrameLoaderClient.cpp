// ============================================================================
// PortFrameLoaderClient.cpp — real FrameLoaderClient for the W10M render driver.
// Vendor-copied from EmptyFrameLoaderClient (its policy/didFinish/networking
// methods are `final`, so it can't be subclassed). Differs from Empty only in:
//   - navigation/response policy -> PolicyAction::Use (Ignore for new windows)
//   - dispatchDidFinishLoad / dispatchDidFail* -> stop the run loop + set state
//   - userAgent -> a real UA string (empty UA gets 403'd by many servers)
//   - isEmptyFrameLoaderClient -> false
// Networking context keeps the cookieless empty session for now (first URL
// milestone needs no cookies); a real curl NetworkStorageSession comes with auth.
// ============================================================================

#include "config.h"
#include <atomic>
#include "Document.h"

#include <wtf/URL.h>

// Per-site identity (defined below, next to userAgent): YouTube gets a Chrome/Android identity so it
// serves CLEAR MSE instead of FairPlay-DRM HLS; everything else keeps iOS Safari.
namespace WebCorePort {

bool urlWantsChromeIdentity(const WTF::URL& url)
{
    auto host = url.host();
    auto is = [&](const char* domain) {
        return equalIgnoringASCIICase(host, domain) || host.endsWithIgnoringASCIICase(makeString('.', domain));
    };
    return is("youtube.com") || is("youtu.be") || is("googlevideo.com") || is("youtube-nocookie.com");
}

// The identity of the CURRENT PAGE, for the JS-visible surface (navigator.platform/vendor), which has
// no URL argument to key off. Set when the main frame commits a load.
static std::atomic<bool> g_pageUsesChromeIdentity { false };
void setPageUsesChromeIdentity(bool v) { g_pageUsesChromeIdentity.store(v, std::memory_order_relaxed); }
bool pageUsesChromeIdentity() { return g_pageUsesChromeIdentity.load(std::memory_order_relaxed); }

} // namespace WebCorePort

#include "PortFrameLoaderClient.h"

// Implemented in the C++/CX shell: shows/hides the on-screen keyboard via Windows InputPane.
extern "C" void PortShowKeyboard(int show);

#include "DocumentLoader.h"
#include "FormState.h"
#include "Frame.h"
#include "FrameView.h"
#include "FrameLoader.h"
#include "FrameTree.h"
#include "ScriptController.h"
#include "HTMLFrameOwnerElement.h"
#include "FrameLoaderClient.h"
#include "CookieJarDB.h"
#include "FrameNetworkingContext.h"
#include "HistoryItem.h"
#include "NetworkStorageSession.h"
#include "Page.h"
#include "ResourceError.h"
#include "ResourceResponse.h"
#include "Widget.h"
#include <string>
#include <pal/SessionID.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/UniqueRef.h>

namespace WebCorePort { void portLog(const char*); const std::string& portDataPath(); }
using WebCorePort::portLog;

static std::string toUtf8(const WTF::String& s)
{
    auto c = s.utf8();
    return std::string(c.data(), c.length());
}

// Defined in WebCoreDriver.cpp. Persists JSC's bytecode cache to disk; called from
// dispatchDidFinishLoad below, because the cache was otherwise only ever written on eviction.
// (Must be declared at file scope: an extern "C" declaration inside a function body is C2598.)
extern "C" void WebCoreBytecodeCacheFlush();

namespace WebCore {

// Process-wide network session shared by the HTTP loader and document.cookie (one jar).
// PERSISTENT: a non-ephemeral session whose SQLite cookie jar lives at <LocalFolder>\cookies.db
// so logins survive app restart. WebKit's defaultCookieJarPath() uses
// localUserSpecificStorageDirectory(), which isn't reliably writable inside a W10M AppContainer —
// that's why this was ephemeral (:memory:). We instead inject a CookieJarDB at the shell-supplied
// LocalFolder path (WebCoreBrowserSetDataPath). If the shell never set a path, fall back to the
// in-memory ephemeral session (still functional for the run, just not persisted).
static NetworkStorageSession& portStorageSession()
{
    static NeverDestroyed<NetworkStorageSession> session([] {
        return WebCorePort::portDataPath().empty()
            ? PAL::SessionID::generateEphemeralSessionID()
            : PAL::SessionID::defaultSessionID();
    }());
    static bool s_dbConfigured = false;
    if (!s_dbConfigured) {
        s_dbConfigured = true;
        const std::string& dp = WebCorePort::portDataPath();
        if (!dp.empty()) {
            String path = String::fromUTF8((dp + "\\cookies.db").c_str());
            session.get().setCookieDatabase(makeUniqueRef<CookieJarDB>(path));
            portLog("cookies: persistent jar at LocalFolder\\cookies.db");
        } else
            portLog("cookies: no data path set -> in-memory (not persisted)");
    }
    return session.get();
}

// Exposed so the DOM cookie jar (CookieJar) shares the SAME session as the HTTP loader —
// otherwise document.cookie and Set-Cookie live in different jars and never see each other.
NetworkStorageSession& portSharedStorageSession() { return portStorageSession(); }

class PortFrameNetworkingContext final : public FrameNetworkingContext {
public:
    static Ref<PortFrameNetworkingContext> create(Frame* frame) { return adoptRef(*new PortFrameNetworkingContext(frame)); }

private:
    explicit PortFrameNetworkingContext(Frame*);

    bool shouldClearReferrerOnHTTPSToHTTPRedirect() const { return true; }
    NetworkStorageSession* storageSession() const final { return &portStorageSession(); }

#if PLATFORM(COCOA)
    bool localFileContentSniffingEnabled() const { return false; }
    SchedulePairHashSet* scheduledRunLoopPairs() const { return nullptr; }
    RetainPtr<CFDataRef> sourceApplicationAuditData() const { return nullptr; };
#endif

#if PLATFORM(COCOA) || PLATFORM(WIN)
    ResourceError blockedError(const ResourceRequest&) const final { return { }; }
#endif
};

void PortFrameLoaderClient::dispatchDecidePolicyForNewWindowAction(const NavigationAction&, const ResourceRequest&, FormState*, const String&, PolicyCheckIdentifier identifier, FramePolicyFunction&& function)
{
    // Deliver asynchronously (next run-loop turn) so the load stack unwinds first.
    RunLoop::main().dispatch([function = WTFMove(function), identifier]() mutable {
        function(PolicyAction::Ignore, identifier); // headless: no new windows
    });
}

void PortFrameLoaderClient::dispatchDecidePolicyForNavigationAction(const NavigationAction&, const ResourceRequest&, const ResourceResponse&, FormState*, PolicyDecisionMode, PolicyCheckIdentifier identifier, FramePolicyFunction&& function)
{
    portLog("client: decidePolicyForNavigationAction -> Use (async)");
    RunLoop::main().dispatch([function = WTFMove(function), identifier]() mutable {
        function(PolicyAction::Use, identifier); // allow the navigation to proceed
    });
}

void PortFrameLoaderClient::dispatchWillSendSubmitEvent(Ref<FormState>&&)
{
}

void PortFrameLoaderClient::dispatchWillSubmitForm(FormState&, CompletionHandler<void()>&& completionHandler)
{
    completionHandler();
}

Ref<DocumentLoader> PortFrameLoaderClient::createDocumentLoader(const ResourceRequest& request, const SubstituteData& substituteData)
{
    portLog("client: createDocumentLoader");
    return DocumentLoader::create(request, substituteData);
}

RefPtr<Frame> PortFrameLoaderClient::createFrame(const String& name, HTMLFrameOwnerElement& ownerElement)
{
    // Real subframe creation. Without this, EVERY <iframe> silently fails to load — which breaks
    // captchas (Turnstile/reCAPTCHA/hCaptcha all live in a 3rd-party iframe), embeds (YouTube/
    // maps/widgets), OAuth-in-iframe, and many SPAs. Give the child its own PortFrameLoaderClient,
    // create the Frame under the owner element, wire it into the parent's frame tree, and init the
    // loader. WebCore's SubframeLoader then drives the child's src load. Mirrors the WebKitLegacy
    // WebFrameLoaderClient::createFrame pattern.
    if (!m_frame)
        return nullptr;
    auto* page = m_frame->page();
    if (!page)
        return nullptr;

    auto subframeClient = makeUniqueRef<PortFrameLoaderClient>();
    auto* subframeClientPtr = subframeClient.ptr();
    auto childFrame = Frame::create(page, &ownerElement, WTFMove(subframeClient));
    subframeClientPtr->setFrame(childFrame.ptr());

    childFrame->tree().setName(name);
    m_frame->tree().appendChild(childFrame.get());
    childFrame->init();

    // init() can detach the frame (e.g. a navigation policy dropped it) — return null if so.
    if (!childFrame->page()) {
        portLog("iframe: createFrame -> child dropped during init() (returns null)");
        return nullptr;
    }

    { auto n = name.utf8(); char b[160]; snprintf(b, sizeof b, "iframe: createFrame OK name='%s' subframeCount=%u", n.data(), m_frame->tree().childCount()); portLog(b); }
    return childFrame;
}

RefPtr<Widget> PortFrameLoaderClient::createPlugin(const IntSize&, HTMLPlugInElement&, const URL&, const Vector<String>&, const Vector<String>&, const String&, bool)
{
    return nullptr;
}

std::optional<FrameIdentifier> PortFrameLoaderClient::frameID() const
{
    return std::nullopt;
}

std::optional<PageIdentifier> PortFrameLoaderClient::pageID() const
{
    return std::nullopt;
}

bool PortFrameLoaderClient::hasWebView() const
{
    return true; // mainly for assertions
}

void PortFrameLoaderClient::makeRepresentation(DocumentLoader*)
{
}

#if PLATFORM(IOS_FAMILY)

bool PortFrameLoaderClient::forceLayoutOnRestoreFromBackForwardCache()
{
    return false;
}

#endif

void PortFrameLoaderClient::forceLayoutForNonHTML()
{
}

void PortFrameLoaderClient::setCopiesOnScroll()
{
}

void PortFrameLoaderClient::detachedFromParent2()
{
}

void PortFrameLoaderClient::detachedFromParent3()
{
}

void PortFrameLoaderClient::convertMainResourceLoadToDownload(DocumentLoader*, const ResourceRequest&, const ResourceResponse&)
{
}

void PortFrameLoaderClient::assignIdentifierToInitialRequest(ResourceLoaderIdentifier, DocumentLoader*, const ResourceRequest&)
{
}

bool PortFrameLoaderClient::shouldUseCredentialStorage(DocumentLoader*, ResourceLoaderIdentifier)
{
    return false;
}

void PortFrameLoaderClient::dispatchWillSendRequest(DocumentLoader*, ResourceLoaderIdentifier, ResourceRequest& request, const ResourceResponse&)
{
    portLog(("client: willSendRequest " + toUtf8(request.url().string())).c_str());
}

void PortFrameLoaderClient::dispatchDidReceiveAuthenticationChallenge(DocumentLoader*, ResourceLoaderIdentifier, const AuthenticationChallenge&)
{
}

#if USE(PROTECTION_SPACE_AUTH_CALLBACK)

bool PortFrameLoaderClient::canAuthenticateAgainstProtectionSpace(DocumentLoader*, ResourceLoaderIdentifier, const ProtectionSpace&)
{
    return false;
}

#endif

#if PLATFORM(IOS_FAMILY)

RetainPtr<CFDictionaryRef> PortFrameLoaderClient::connectionProperties(DocumentLoader*, ResourceLoaderIdentifier)
{
    return nullptr;
}

#endif

void PortFrameLoaderClient::dispatchDidReceiveResponse(DocumentLoader*, ResourceLoaderIdentifier, const ResourceResponse& response)
{
    portLog(("client: didReceiveResponse http=" + std::to_string(response.httpStatusCode())
        + " mime=" + toUtf8(response.mimeType())).c_str());
}

void PortFrameLoaderClient::dispatchDidReceiveContentLength(DocumentLoader*, ResourceLoaderIdentifier, int)
{
}

void PortFrameLoaderClient::dispatchDidFinishLoading(DocumentLoader*, ResourceLoaderIdentifier)
{
}

#if ENABLE(DATA_DETECTION)

void PortFrameLoaderClient::dispatchDidFinishDataDetection(NSArray *)
{
}

#endif

void PortFrameLoaderClient::dispatchDidFailLoading(DocumentLoader*, ResourceLoaderIdentifier, const ResourceError&)
{
}

bool PortFrameLoaderClient::dispatchDidLoadResourceFromMemoryCache(DocumentLoader*, const ResourceRequest&, const ResourceResponse&, int)
{
    return false;
}

void PortFrameLoaderClient::dispatchDidDispatchOnloadEvents()
{
}

void PortFrameLoaderClient::dispatchDidReceiveServerRedirectForProvisionalLoad()
{
}

void PortFrameLoaderClient::dispatchDidCancelClientRedirect()
{
}

void PortFrameLoaderClient::dispatchWillPerformClientRedirect(const URL&, double, WallTime, LockBackForwardList)
{
}

void PortFrameLoaderClient::dispatchDidChangeLocationWithinPage()
{
}

void PortFrameLoaderClient::dispatchDidPushStateWithinPage()
{
}

void PortFrameLoaderClient::dispatchDidReplaceStateWithinPage()
{
}

void PortFrameLoaderClient::dispatchDidPopStateWithinPage()
{
}

void PortFrameLoaderClient::dispatchWillClose()
{
}

void PortFrameLoaderClient::dispatchDidStartProvisionalLoad()
{
    { int m = (m_frame && m_frame->isMainFrame()) ? 1 : 0; char b[64]; snprintf(b, sizeof b, "client: didStartProvisionalLoad isMain=%d", m); portLog(b); }
    // A main-frame navigation started (e.g. Enter submitted a search, or a link was tapped) —
    // dismiss the on-screen keyboard so it doesn't stay up over the new page. Runs on the main
    // thread (same as the render pump), where InputPane access is valid.
    if (m_frame && m_frame->isMainFrame())
        PortShowKeyboard(0);
    // A new navigation started: clear the completion flags so the shell's loading indicator
    // (blue progress bar + refresh↔stop) reflects THIS load, not the previous one.
    m_loadComplete = false;
    m_loadFailed = false;
}

void PortFrameLoaderClient::dispatchDidReceiveTitle(const StringWithDirection&)
{
}

void PortFrameLoaderClient::dispatchDidCommitLoad(std::optional<HasInsecureContent>, std::optional<UsedLegacyTLS>)
{
    { int m = (m_frame && m_frame->isMainFrame()) ? 1 : 0; char b[64]; snprintf(b, sizeof b, "client: didCommitLoad isMain=%d", m); portLog(b); }
    // Keep the JS-visible identity (navigator.platform/vendor) coherent with the UA we send for this
    // site. Main frame only: subframes must not flip the whole page's identity.
    if (m_frame && m_frame->isMainFrame()) {
        if (RefPtr document = m_frame->document()) {
            bool chrome = WebCorePort::urlWantsChromeIdentity(document->url());
            WebCorePort::setPageUsesChromeIdentity(chrome);
            char b[80]; snprintf(b, sizeof b, "identity: %s", chrome ? "Chrome/Android (YouTube: clear MSE, no FairPlay)" : "iOS 15.4 Safari");
            portLog(b);
        }
    }
}

void PortFrameLoaderClient::dispatchDidFailProvisionalLoad(const ResourceError& error, WillContinueLoading willContinue)
{
    portLog(("client: didFailProvisionalLoad code=" + std::to_string(error.errorCode())
        + " domain=" + toUtf8(error.domain())
        + " cancel=" + std::to_string(error.isCancellation())
        + " willContinue=" + std::to_string(willContinue == WillContinueLoading::Yes)
        + " desc=" + toUtf8(error.localizedDescription())).c_str());
    // A provisional load that will continue (http→https 301, cross-scheme handoff) or that was
    // cancelled is NOT a real failure — the loader keeps going. Tearing the page down here kills
    // legitimate navigations (and SPAs that swap the provisional load). Only genuine terminal
    // failures end the page.
    if (willContinue == WillContinueLoading::Yes || error.isCancellation())
        return;
    // A SUBFRAME provisional-load failure (Turnstile iframe blocked, ad frame 404) must NOT tear
    // down the whole page — only a main-frame failure ends the page. Let the parent's
    // FrameLoader::checkCompleted finalize the page load once the subframe is marked complete.
    if (m_frame && !m_frame->isMainFrame()) {
        portLog("client: didFailProvisionalLoad SUBFRAME -> not stopping page");
        return;
    }
    m_loadComplete = true;
    m_loadFailed = true;
    RunLoop::main().stop();
}

void PortFrameLoaderClient::dispatchDidFailLoad(const ResourceError& error)
{
    { int m = (m_frame && m_frame->isMainFrame()) ? 1 : 0; portLog(("client: didFailLoad isMain=" + std::to_string(m) + " code=" + std::to_string(error.errorCode())
        + " desc=" + toUtf8(error.localizedDescription())).c_str()); }
    if (m_frame && !m_frame->isMainFrame()) {
        portLog("client: didFailLoad SUBFRAME -> not stopping page");
        return;
    }
    m_loadComplete = true;
    m_loadFailed = true;
    RunLoop::main().stop();
}

void PortFrameLoaderClient::dispatchDidFinishDocumentLoad()
{
}

void PortFrameLoaderClient::dispatchDidFinishLoad()
{
    { int m = (m_frame && m_frame->isMainFrame()) ? 1 : 0; char b[64]; snprintf(b, sizeof b, "client: didFinishLoad isMain=%d", m); portLog(b); }
    // Only the MAIN frame finishing ends the page. A subframe finishing first must not stop the
    // main run loop (that would freeze rendering before the main document is done).
    if (m_frame && !m_frame->isMainFrame()) {
        portLog("client: didFinishLoad SUBFRAME -> not stopping page");
        return;
    }
    m_loadComplete = true;
    // Persist the bytecode this page compiled. Without this the disk cache only ever received scripts
    // that happened to be EVICTED from the in-memory CodeCacheMap, so the next launch hit none of it
    // (device log: "bcache: hit=0 miss=25 (0%)"). Load-complete is the right moment: the page's working
    // set of scripts is known and the main thread is about to go idle.
    WebCoreBytecodeCacheFlush();
    RunLoop::main().stop();
}

void PortFrameLoaderClient::dispatchDidReachLayoutMilestone(OptionSet<LayoutMilestone>)
{
}

void PortFrameLoaderClient::dispatchDidReachVisuallyNonEmptyState()
{
}

Frame* PortFrameLoaderClient::dispatchCreatePage(const NavigationAction&, NewFrameOpenerPolicy)
{
    return nullptr;
}

void PortFrameLoaderClient::dispatchShow()
{
}

void PortFrameLoaderClient::dispatchDecidePolicyForResponse(const ResourceResponse&, const ResourceRequest&, PolicyCheckIdentifier identifier, const String&, FramePolicyFunction&& function)
{
    portLog("client: decidePolicyForResponse -> Use (async)");
    RunLoop::main().dispatch([function = WTFMove(function), identifier]() mutable {
        function(PolicyAction::Use, identifier); // render the response (don't download/ignore)
    });
}

void PortFrameLoaderClient::cancelPolicyCheck()
{
}

void PortFrameLoaderClient::dispatchUnableToImplementPolicy(const ResourceError&)
{
}

void PortFrameLoaderClient::revertToProvisionalState(DocumentLoader*)
{
}

void PortFrameLoaderClient::setMainDocumentError(DocumentLoader*, const ResourceError&)
{
}

void PortFrameLoaderClient::setMainFrameDocumentReady(bool)
{
}

void PortFrameLoaderClient::startDownload(const ResourceRequest&, const String&)
{
}

void PortFrameLoaderClient::willChangeTitle(DocumentLoader*)
{
}

void PortFrameLoaderClient::didChangeTitle(DocumentLoader*)
{
}

void PortFrameLoaderClient::willReplaceMultipartContent()
{
}

void PortFrameLoaderClient::didReplaceMultipartContent()
{
}

void PortFrameLoaderClient::committedLoad(DocumentLoader* loader, const SharedBuffer& data)
{
    // Feed received bytes to the document parser — without this the page is blank.
    loader->commitData(data);
}

void PortFrameLoaderClient::finishedLoading(DocumentLoader*)
{
}

ResourceError PortFrameLoaderClient::cancelledError(const ResourceRequest&) const
{
    return { ResourceError::Type::Cancellation };
}

ResourceError PortFrameLoaderClient::blockedError(const ResourceRequest&) const
{
    return { };
}

ResourceError PortFrameLoaderClient::blockedByContentBlockerError(const ResourceRequest&) const
{
    return { };
}

ResourceError PortFrameLoaderClient::cannotShowURLError(const ResourceRequest&) const
{
    return { };
}

ResourceError PortFrameLoaderClient::interruptedForPolicyChangeError(const ResourceRequest&) const
{
    return { };
}

#if ENABLE(CONTENT_FILTERING)

ResourceError PortFrameLoaderClient::blockedByContentFilterError(const ResourceRequest&) const
{
    return { };
}

#endif

ResourceError PortFrameLoaderClient::cannotShowMIMETypeError(const ResourceResponse&) const
{
    return { };
}

ResourceError PortFrameLoaderClient::fileDoesNotExistError(const ResourceResponse&) const
{
    return { };
}

ResourceError PortFrameLoaderClient::pluginWillHandleLoadError(const ResourceResponse&) const
{
    return { };
}

bool PortFrameLoaderClient::shouldFallBack(const ResourceError&) const
{
    return false;
}

bool PortFrameLoaderClient::canHandleRequest(const ResourceRequest&) const
{
    return true; // we handle http(s)/etc. — false makes FrameLoader drop the load
}

bool PortFrameLoaderClient::canShowMIMEType(const String&) const
{
    return true; // render the response rather than treat it as a download
}

bool PortFrameLoaderClient::canShowMIMETypeAsHTML(const String&) const
{
    return true;
}

bool PortFrameLoaderClient::representationExistsForURLScheme(const String&) const
{
    return false;
}

String PortFrameLoaderClient::generatedMIMETypeForURLScheme(const String&) const
{
    return emptyString();
}

void PortFrameLoaderClient::frameLoadCompleted()
{
}

void PortFrameLoaderClient::restoreViewState()
{
}

void PortFrameLoaderClient::provisionalLoadStarted()
{
}

void PortFrameLoaderClient::didFinishLoad()
{
}

void PortFrameLoaderClient::prepareForDataSourceReplacement()
{
}

void PortFrameLoaderClient::updateCachedDocumentLoader(DocumentLoader&)
{
}

void PortFrameLoaderClient::setTitle(const StringWithDirection&, const URL&)
{
}

// ---- Per-site identity ----------------------------------------------------------------------
// Revenant's default identity is iOS 15.4 Safari (see the note in userAgent below) — the honest match
// for this engine, and what solved Cloudflare Turnstile.
//
// But YouTube keys its DELIVERY FORMAT off that identity: to an iPhone it serves the main video as
// native HLS whose manifest carries #EXT-X-KEY with a FairPlay key URI (skd://www.youtube.com/api/drm/
// fps). FairPlay keys are only ever issued to genuine Apple hardware — no CDM exists on Windows, the
// skd:// fetch returns E_NOTIMPL — so the decoder never gets its key and the player stays BLACK forever.
// (Verified on-device: the ads, which are delivered as CLEAR MSE fMP4, play perfectly; only the main
// video takes the HLS+FairPlay path.) Refusing HLS via canPlayType does NOT help: YouTube's iOS player
// sets video.src to the .m3u8 without ever consulting canPlayType.
//
// So on YouTube (and its media hosts) we present as Chrome on Android instead — the client YouTube
// serves CLEAR fMP4 over MSE, the exact path that already plays end-to-end here. Everywhere else keeps
// the iOS identity. Identity means UA *plus* the JS surface: navigator.platform and navigator.vendor
// are switched to match (see Navigator::platform / NavigatorBase::vendor), because a Chrome UA paired
// with an Apple vendor is an incoherent combo no real device reports — exactly the bot-tell we avoid
// everywhere else. TLS/JA3 is unaffected (the BoringSSL ClientHello profile is orthogonal, and Google
// does not gate youtube.com on it).

String PortFrameLoaderClient::userAgent(const URL& url) const
{
    // YouTube: Chrome/Android, to be served CLEAR MSE instead of FairPlay-DRM HLS (see note above).
    if (WebCorePort::urlWantsChromeIdentity(url))
        return "Mozilla/5.0 (Linux; Android 13; Pixel 6) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36"_s;

    // Revenant identity: iOS 15.4 Safari — the HONEST reflection of our actual engine. WebKit 2.36.8
    // IS the Safari-15.4 branch (AppleWebKit/605.1.15, Version/15.4); on iOS that engine ships with the
    // exact fingerprint our port already produces with ZERO spoofing: navigator.vendor="Apple Computer,
    // Inc.", no window.chrome, no navigator.userAgentData, no navigator.oscpu, maxTouchPoints=5. So this
    // is not a costume — it is what the engine genuinely is. Coherence is the whole point: a fingerprint
    // whose UA, platform (navigator.platform="iPhone", see NavigatorBase), vendor and JS-surface all agree
    // is what passes Cloudflare Turnstile's bot-scoring and Google's support gate.
    //
    // History: we previously tried (a) X11/Linux desktop — but navigator.platform="Linux x86_64" with an
    // Apple vendor + a Mobile UA is a combo no real device reports, a dead bot-tell; (b) a Windows-Phone/
    // Lumia envelope to dodge the Cloudflare Private Access Token (PAT) challenge — but on-device logs
    // showed CF issues PAT (401 WWW-Authenticate: PrivateToken) REGARDLESS of UA, and WP+Safari is itself
    // an impossible combo Google flags as unsupported. Neither cohered. iOS Safari is the one identity
    // that matches the engine end-to-end. PAT stays unanswerable (we can't mint Apple DeviceCheck tokens),
    // but PAT is opportunistic — CF falls back to the interactive Turnstile path when it goes unanswered.
    // The "Mobile/15E148" build token + Safari/604.1 are the real iOS-15.4 constants (not inflated).
    UNUSED_PARAM(url);
    return "Mozilla/5.0 (iPhone; CPU iPhone OS 15_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/15.4 Mobile/15E148 Safari/604.1"_s;
}

void PortFrameLoaderClient::savePlatformDataToCachedFrame(CachedFrame*)
{
}

void PortFrameLoaderClient::transitionToCommittedFromCachedFrame(CachedFrame*)
{
}

#if PLATFORM(IOS_FAMILY)

void PortFrameLoaderClient::didRestoreFrameHierarchyForCachedFrame()
{
}

#endif

void PortFrameLoaderClient::transitionToCommittedForNewPage()
{
    // Create the FrameView for the frame being committed. The DRIVER creates the MAIN frame's view
    // manually, but SUBFRAMES (iframes — Turnstile/reCAPTCHA/embeds/OAuth) rely on this client hook
    // to get their view during commit. Without it the committed (about:blank) document never gets a
    // DOMWindow, and DocumentLoader::checkLoadComplete() null-derefs document()->domWindow() (crash
    // seen creating Turnstile's iframe on charavault). Only create when the frame has no view yet —
    // that leaves the driver-managed main-frame view untouched.
    if (!m_frame || m_frame->view())
        return;

    IntSize size(720, 1280);
    if (auto* page = m_frame->page()) {
        if (auto* mainView = page->mainFrame().view())
            size = mainView->frameRect().size();
    }
    // WebCore re-sizes the subframe view to its <iframe> content box during layout; this is just the
    // initial size so the view/DOMWindow exist.
    m_frame->createView(size, std::nullopt, { }, { });
    portLog("iframe: transitionToCommittedForNewPage -> created subframe FrameView");
}


void PortFrameLoaderClient::didRestoreFromBackForwardCache()
{
}


void PortFrameLoaderClient::updateGlobalHistory()
{
}

void PortFrameLoaderClient::updateGlobalHistoryRedirectLinks()
{
}

bool PortFrameLoaderClient::shouldGoToHistoryItem(HistoryItem&) const
{
    return false;
}

void PortFrameLoaderClient::saveViewStateToItem(HistoryItem&)
{
}

bool PortFrameLoaderClient::canCachePage() const
{
    return false;
}

void PortFrameLoaderClient::didDisplayInsecureContent()
{
}

void PortFrameLoaderClient::didRunInsecureContent(SecurityOrigin&, const URL&)
{
}

void PortFrameLoaderClient::didDetectXSS(const URL&, bool)
{
}

ObjectContentType PortFrameLoaderClient::objectContentType(const URL&, const String&)
{
    return ObjectContentType::None;
}

String PortFrameLoaderClient::overrideMediaType() const
{
    return { };
}

void PortFrameLoaderClient::redirectDataToPlugin(Widget&)
{
}

void PortFrameLoaderClient::dispatchDidClearWindowObjectInWorld(DOMWrapperWorld&)
{
    // FPDIAG (diagnostic — keep until CF challenge passes): dump the JS-visible fingerprint surface
    // that Cloudflare Turnstile scores, from inside each frame's own JS world, so we can diff our
    // engine's values against a real WebKit/Safari and close every "not a real browser" tell. Uses
    // console.warn because the port's console bridge only forwards Error/Warning (PortChromeClient).
    // The dedupe flag is defined non-enumerable so it doesn't itself become a fingerprint signal.
    if (!m_frame)
        return;
    m_frame->script().executeScriptIgnoringException(String::fromUTF8(R"FPJS(
(function(){try{
 if(window.__fpd)return;try{Object.defineProperty(window,'__fpd',{value:1,enumerable:false,configurable:false,writable:false});}catch(e){window.__fpd=1;}
 var h=(location&&location.hostname)||'?';
 if(location&&/^(about:|data:|blob:)/.test(location.protocol||'')){return;}
 var W=function(m){try{console.warn('FPDIAG['+h+']: '+m)}catch(e){}};
 function nav(){var n=navigator,s=screen;
  W('ua='+n.userAgent);
  W('platform='+n.platform+' vendor='+n.vendor+' vendorSub='+n.vendorSub+' product='+n.product+' productSub='+n.productSub+' oscpu='+n.oscpu);
  W('appName='+n.appName+' appCodeName='+n.appCodeName+' appVersion='+n.appVersion);
  W('webdriver='+n.webdriver+' lang='+n.language+' langs='+((n.languages||[]).join(','))+' hwc='+n.hardwareConcurrency+' devmem='+n.deviceMemory+' maxTouch='+n.maxTouchPoints+' cookieEnabled='+n.cookieEnabled+' dnt='+n.doNotTrack+' pdfViewer='+n.pdfViewerEnabled);
  W('plugins='+(n.plugins?n.plugins.length:'?')+' mimeTypes='+(n.mimeTypes?n.mimeTypes.length:'?'));
  W('uaData='+(n.userAgentData?('mobile='+n.userAgentData.mobile+' platform='+n.userAgentData.platform):'undefined'));
  W('screen='+s.width+'x'+s.height+' avail='+s.availWidth+'x'+s.availHeight+' depth='+s.colorDepth+'/'+s.pixelDepth+' dpr='+window.devicePixelRatio+' inner='+window.innerWidth+'x'+window.innerHeight+' outer='+window.outerWidth+'x'+window.outerHeight);
  W('typeofChrome='+(typeof window.chrome)+' RTC='+(typeof window.RTCPeerConnection)+' webkitRTC='+(typeof window.webkitRTCPeerConnection)+' Notif='+(typeof window.Notification)+(window.Notification?(' perm='+Notification.permission):''));
  try{W('tz='+Intl.DateTimeFormat().resolvedOptions().timeZone+' tzoff='+(new Date()).getTimezoneOffset())}catch(e){W('tz-ERR '+e)}
  try{W('alertNative='+/\[native code\]/.test(window.alert.toString())+' fnToStr='+(function(){}).toString().slice(0,40))}catch(e){W('tostr-ERR '+e)}
  try{W('errstack0='+((new Error('x')).stack||'').split('\n')[0])}catch(e){}
 }
 function gl(){try{
  var c=document.createElement('canvas');var g=c.getContext('webgl')||c.getContext('experimental-webgl');
  if(!g){W('webgl=NULL');return;}
  W('webgl VENDOR='+g.getParameter(g.VENDOR)+' RENDERER='+g.getParameter(g.RENDERER));
  W('webgl VERSION='+g.getParameter(g.VERSION)+' GLSL='+g.getParameter(g.SHADING_LANGUAGE_VERSION));
  var d=g.getExtension('WEBGL_debug_renderer_info');
  if(d)W('webgl UNMASKED_VENDOR='+g.getParameter(d.UNMASKED_VENDOR_WEBGL)+' UNMASKED_RENDERER='+g.getParameter(d.UNMASKED_RENDERER_WEBGL));
  else W('webgl UNMASKED=NOEXT');
  W('webgl MAXTEX='+g.getParameter(g.MAX_TEXTURE_SIZE)+' extCount='+((g.getSupportedExtensions()||[]).length));
 }catch(e){W('webgl-ERR '+e)}}
 function cv(){try{
  var c=document.createElement('canvas');c.width=220;c.height=30;var x=c.getContext('2d');
  x.textBaseline='top';x.font='14px Arial';x.fillStyle='#f60';x.fillRect(0,0,110,20);x.fillStyle='#069';x.fillText('Revenant fp',2,15);
  var u=c.toDataURL();var k=0;for(var i=0;i<u.length;i++){k=((k<<5)-k+u.charCodeAt(i))|0;}
  W('canvas2d len='+u.length+' hash='+k);
 }catch(e){W('canvas2d-ERR '+e)}}
 function rtc(){try{
  if(typeof RTCPeerConnection!=='function'){W('RTC: NO RTCPeerConnection ctor (typeof='+(typeof RTCPeerConnection)+')');return;}
  var pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.l.google.com:19302'}]});
  W('RTC: pc created ok signalingState='+pc.signalingState+' iceGatheringState='+pc.iceGatheringState);
  var cands=[];
  pc.onicecandidate=function(e){
    if(e&&e.candidate&&e.candidate.candidate){cands.push(e.candidate.candidate);}
    else{W('RTC: ICE onicecandidate=null (end-of-candidates) count='+cands.length);}
  };
  pc.onicegatheringstatechange=function(){W('RTC: gatheringState='+pc.iceGatheringState+' count='+cands.length);};
  try{pc.createDataChannel('probe');W('RTC: dataChannel created');}catch(e){W('RTC dc-ERR '+e);}
  pc.createOffer().then(function(o){W('RTC: offer created type='+o.type+' sdpLen='+(o.sdp?o.sdp.length:0)+' hasIceUfrag='+(/a=ice-ufrag/.test(o.sdp||'')));return pc.setLocalDescription(o);})
    .then(function(){W('RTC: setLocalDescription ok, gathering...');})
    .catch(function(e){W('RTC offer-ERR '+e);});
  setTimeout(function(){W('RTC: +8s candidates='+cands.length+' gatheringState='+pc.iceGatheringState);
    for(var i=0;i<cands.length&&i<6;i++)W('RTC cand#'+i+'='+cands[i]);},8000);
 }catch(e){W('RTC-ERR '+e)}}
 function run(){nav();gl();cv();rtc();W('END');}
 if(document&&document.readyState!=='loading')run();
 else if(document)document.addEventListener('DOMContentLoaded',run);
 else setTimeout(run,50);
}catch(e){try{console.warn('FPDIAG-FATAL '+e)}catch(_){}}})();
)FPJS"), false);
}

#if PLATFORM(COCOA)

RemoteAXObjectRef PortFrameLoaderClient::accessibilityRemoteObject()
{
    return nullptr;
}

void PortFrameLoaderClient::willCacheResponse(DocumentLoader*, ResourceLoaderIdentifier, NSCachedURLResponse *response, CompletionHandler<void(NSCachedURLResponse *)>&& completionHandler) const
{
    completionHandler(response);
}

#endif

#if USE(CFURLCONNECTION)

bool PortFrameLoaderClient::shouldCacheResponse(DocumentLoader*, ResourceLoaderIdentifier, const ResourceResponse&, const unsigned char*, unsigned long long)
{
    return true;
}

#endif

bool PortFrameLoaderClient::isEmptyFrameLoaderClient() const
{
    return false; // real client — don't let FrameLoader shortcut empty-client paths
}

void PortFrameLoaderClient::prefetchDNS(const String&)
{
}

#if USE(QUICK_LOOK)

RefPtr<LegacyPreviewLoaderClient> PortFrameLoaderClient::createPreviewLoaderClient(const String&, const String&)
{
    return nullptr;
}

#endif

#if ENABLE(INTELLIGENT_TRACKING_PREVENTION)

bool PortFrameLoaderClient::hasFrameSpecificStorageAccess()
{
    return false;
}

#endif

inline PortFrameNetworkingContext::PortFrameNetworkingContext(Frame* frame)
    : FrameNetworkingContext { frame }
{
}

Ref<FrameNetworkingContext> PortFrameLoaderClient::createNetworkingContext()
{
    // Pass the real frame so FrameNetworkingContext::isValid() is true; a null frame
    // makes curl's ResourceHandle::start() bail and ResourceHandle::create() return null.
    return PortFrameNetworkingContext::create(m_frame);
}

void PortFrameLoaderClient::sendH2Ping(const URL& url, CompletionHandler<void(Expected<Seconds, ResourceError>&&)>&& completionHandler)
{
    ASSERT_NOT_REACHED();
    completionHandler(makeUnexpected(internalError(url)));
}

} // namespace WebCore
