// ============================================================================
// WebCoreDriver.cpp  —  WebCore headless render driver (W10M/UWP).
//
//   WebCoreRenderHtml: inline HTML string -> DocumentWriter -> layout -> paint.
//     (no network; mirrors WebCore::SVGImage::dataChanged + ::draw)
//   WebCoreRenderUrl:  http(s):// URL -> real curl fetch via WebResourceLoad-
//     Scheduler + PortFrameLoaderClient -> FrameLoader::load -> pump run loop
//     until the load resolves -> layout -> paint.
//
// Both paint into a Cairo ARGB32 image surface, then swizzle to straight RGBA8888.
// ============================================================================

#include "config.h"
#include "WebCoreDriver.h"

#include "Color.h"
#include "Document.h"
#include "Element.h"
#include "HTMLTextFormControlElement.h"
#include "EventLoop.h"
#include "HitTestRequest.h"
#include "HitTestResult.h"
#include "ScriptController.h"
#include <cmath>
#include "GLContext.h"
#include "GraphicsContext.h"
#include "GraphicsLayer.h"
#include "GraphicsLayerClient.h"
#include "GraphicsLayerTextureMapper.h"
#include "PlatformDisplay.h"
#include "TextureMapper.h"
#include "TextureMapperGL.h"
#include "TextureMapperLayer.h"
#include "DocumentLoader.h"
#include "DocumentWriter.h"
#include "EmptyClients.h"          // pageConfigurationWithEmptyClients
#include "EventHandler.h"
#include "FocusController.h"
#include "Frame.h"
#include "PlatformKeyboardEvent.h"
#include "BackForwardController.h"
#include <unicode/utf16.h>
#include "BlobRegistry.h"
#include "BlobRegistryImpl.h"
#include "FrameLoadRequest.h"
#include "FrameLoader.h"
#include "Geolocation.h"
#include "GeolocationClient.h"
#include "GeolocationController.h"
#include "GeolocationPositionData.h"
#include "NotificationClient.h"
#include "NotificationController.h"
#include "ProgressTracker.h"
#include "ResourceHandle.h"
#include "FrameView.h"
#include "GraphicsContextCairo.h"
#include "IntRect.h"
#include "IntSize.h"
#include "Page.h"
#include "PageConfiguration.h"
#include "PlatformMouseEvent.h"
#include "EditorClient.h"      // complete type needed for config.editorClient (UniqueRef) assignment
#include "PortChromeClient.h"
#include "PortEditorClient.h"
#include "PortStorageProvider.h"
#include "PortServiceWorker.h"
#include "PortCacheStorage.h"
#include "CacheStorageProvider.h"
#include "CookieJar.h"
#include "StorageSessionProvider.h"
#include "StorageNamespaceProvider.h"
#include "RuntimeEnabledFeatures.h"
#include "PortFrameLoaderClient.h"
#include "ResourceRequest.h"
#include "Settings.h"
#include "SharedBuffer.h"
#include "SubstituteData.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> // GL_KHR_debug: glDebugMessageCallbackKHR + GL_DEBUG_* (ANGLE error reporting)
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/Watchdog.h>
#include <JavaScriptCore/Options.h>
#include <cairo.h>
#include <cstring>
#include <fstream>
#include <mutex>
#include <atomic>
#include <thread>
#include <pal/SessionID.h>
#include <string>
#include <vector>
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>
#include <wtf/URL.h>

namespace WebCorePort { void installPortPlatformStrategies(); }

using namespace WebCore;

// --- on-device diagnostic log (written to a path the shell points at LocalState) ---
static std::wstring g_logPath;
static std::mutex g_logMutex;

extern "C" void PortSetDebugLogPathW(const wchar_t* widePath)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logPath = widePath ? widePath : L"";
}

namespace WebCorePort {
void portLog(const char* msg)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logPath.empty())
        return;
    // Keep the log file OPEN across the session instead of open+append+close on every call. The
    // per-call open/close (directory lookup + handle create) is a major perf drain on the phone's
    // flash when logging is frequent. Flush per line so the log stays durable for mid-session pulls
    // and crash analysis. (g_logPath is set once via WebCoreBrowserSetDataPath before any logging.)
    static std::ofstream s_logFile(g_logPath, std::ios::app);
    if (s_logFile) {
        s_logFile << msg << '\n';
        // Flush periodically, NOT per line. Per-line flush() = one flash write() syscall for every
        // log line, and hot paths emit thousands of lines per frame — a major perf drain on the
        // phone. Batching keeps the log durable enough for mid-session pulls (at most the last ~63
        // lines can be lost on a hard kill; the SEH crash handler flushes explicitly on faults).
        static unsigned s_since = 0;
        // Media/MSE diagnostics flush immediately: the frame-server path can hard-terminate the app
        // (OS memory kill = no .dmp), and a batched tail would lose the death point. Cheap because
        // these lines are rare vs the per-frame hot paths.
        if (msg && (!strncmp(msg, "mse", 3) || !strncmp(msg, "mf:", 3) || !strncmp(msg, "mem:", 4) || !strncmp(msg, "gate:", 5)
                || strstr(msg, "frame-server") || strstr(msg, "EXCEPTION") || strstr(msg, "THREW") || strstr(msg, "FAILED"))) {
            s_logFile.flush(); s_since = 0;
        } else if (++s_since >= 64) { s_logFile.flush(); s_since = 0; }
    }
}
}
using WebCorePort::portLog;

// Lets the C++/CX shell write into the same diagnostic log (e.g. memory-usage samples from
// Windows::System::MemoryManager, which is only reachable from the CX side).
extern "C" void WebCoreBrowserLog(const char* msg) { if (msg) portLog(msg); }

static void startMainStallDetector(); // defined below (near the render pump / g_renderStage)
extern "C" void WebCoreBrowserVsyncTick(); // PortDisplayRefreshMonitor.cpp — fires WebCore's display link
extern "C" void PortShowKeyboard(int show); // C++/CX shell: show/hide the on-screen keyboard (InputPane)
extern "C" void WebCoreBrowserKeepCompositing(unsigned frames); // GPU-offload gate: keep compositing N frames

// PlatformScreenWin.cpp (UWP path): sets the value behind window.screen / @media (device-width).
// Defaults to a hardcoded 360x640 until the port pushes the real device size. Feeds Frame::screenSize().
namespace WebCore { void setPlatformScreenBounds(const FloatRect&); }

// Global forwarder so image-render diagnostics (in cairo/svg TUs) can log without
// pulling in the WebCorePort namespace.
void PortImgLog(const char* s) { WebCorePort::portLog(s); }

// Global forwarder for TextureMapper paint diagnostics (TextureMapperLayer.cpp), used to localize
// which layer's GL draw faults inside ANGLE on x.com.
void PortTexmapPaintLog(const char* s) { WebCorePort::portLog(s); }

// Serve blob: URLs from the in-memory BlobRegistryImpl instead of letting them fall through
// to curl (which fails "bad/illegal format"). Registered as the "blob" scheme handler so
// ResourceHandle::create routes blob loads here (createObjectURL, Turnstile, workers, media).
static Ref<WebCore::ResourceHandle> createBlobResourceHandle(const WebCore::ResourceRequest& request, WebCore::ResourceHandleClient* client)
{
    return WebCore::blobRegistry().blobRegistryImpl()->createResourceHandle(request, client);
}

static void ensureInit()
{
    static bool inited = false;
    if (inited)
        return;
    inited = true;
    JSC::initialize();
    WTF::initializeMainThread();

    // Runaway-script watchdog. Set BEFORE the VM is created so VM setup auto-arms it (VM.cpp reads
    // Options::watchdog()). Without a limit, a single JS execution that never returns (infinite loop
    // on a missing/wrong API — the Google/YouTube freeze) hangs the whole browser with no crash/dump.
    // 10s of CPU time is far above any legit synchronous JS; genuine runaways get terminated (and
    // their JS stack logged from Watchdog::shouldTerminate via g_watchdogTerminationLog).
    // 60s (not 10s): Cloudflare Turnstile / hCaptcha run a legit CPU-heavy proof-of-work that takes
    // well over 10s on this slow ARM32 device — a 10s limit killed the Turnstile widget ("stuck on
    // Verifying"). 60s still eventually breaks a genuine runaway (rare; the real freezes were native
    // deadlocks the JS watchdog can't see anyway) while letting the PoW challenges complete.
    JSC::Options::setOption("watchdog=60000");
    JSC::g_watchdogTerminationLog = [](const char* m) { portLog(m); };
    WebCorePort::installPortPlatformStrategies();
    WebCore::ResourceHandle::registerBuiltinConstructor("blob"_s, createBlobResourceHandle);
#if ENABLE(SERVICE_WORKER)
    WebCorePort::installPortServiceWorkerProvider();
#endif
}

// Paint an already-laid-out frame view into a straight-RGBA8888 buffer.
static int paintViewToRGBA(FrameView* view, int w, int h, uint8_t* outRGBA)
{
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return -7;
    }
    cairo_t* cr = cairo_create(surface);
    if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return -8;
    }

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0); // opaque white page background
    cairo_paint(cr);

    {
        GraphicsContextCairo context(cr);
        view->paintContents(context, IntRect(0, 0, w, h));
    }
    cairo_surface_flush(surface);

    // Cairo ARGB32 little-endian = premultiplied B,G,R,A; caller wants straight RGBA.
    const unsigned char* src = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < h; ++y) {
        const unsigned char* sp = src + y * stride;
        uint8_t* dp = outRGBA + y * w * 4;
        for (int x = 0; x < w; ++x) {
            unsigned b = sp[0], g = sp[1], r = sp[2], a = sp[3];
            if (a && a != 255) {
                r = r * 255u / a;
                g = g * 255u / a;
                b = b * 255u / a;
            }
            dp[0] = static_cast<uint8_t>(r);
            dp[1] = static_cast<uint8_t>(g);
            dp[2] = static_cast<uint8_t>(b);
            dp[3] = static_cast<uint8_t>(a);
            sp += 4;
            dp += 4;
        }
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return 0;
}

// GPU bring-up probe: does ANGLE create an EGL(D3D11) display + offscreen GL context
// inside the W10M AppContainer? Logs the EGL/GL vendor+renderer. Returns 0 on success.
static std::string hex(unsigned v)
{
    char b[16];
    snprintf(b, sizeof b, "0x%04x", v);
    return b;
}

// Get + initialize an ANGLE display of a specific D3D11 device type; returns the
// display on success (eglInitialize OK), EGL_NO_DISPLAY otherwise. maxMajor!=0 caps
// the requested feature level (e.g. 9.3 — required for ANGLE to even TRY FL9_3, which
// is the max the Lumia's Adreno supports; default omits 9_3 so hardware init fails).
static EGLDisplay tryAngleDisplay(const char* label, EGLint deviceType, EGLint maxMajor = 0, EGLint maxMinor = 0)
{
    EGLint attrs[16];
    int i = 0;
    attrs[i++] = EGL_PLATFORM_ANGLE_TYPE_ANGLE;        attrs[i++] = EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE;
    attrs[i++] = EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE; attrs[i++] = deviceType;
    if (maxMajor) {
        attrs[i++] = EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE; attrs[i++] = maxMajor;
        attrs[i++] = EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE; attrs[i++] = maxMinor;
    }
    attrs[i++] = EGL_NONE;
    EGLDisplay d = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attrs);
    if (d == EGL_NO_DISPLAY) {
        portLog((std::string("gl: [") + label + "] getPlatformDisplayEXT null err=" + hex(eglGetError())).c_str());
        return EGL_NO_DISPLAY;
    }
    EGLint maj = 0, min = 0;
    if (!eglInitialize(d, &maj, &min)) {
        portLog((std::string("gl: [") + label + "] eglInitialize FAILED err=" + hex(eglGetError())).c_str());
        return EGL_NO_DISPLAY;
    }
    const char* vendor = eglQueryString(d, EGL_VENDOR);
    portLog((std::string("gl: [") + label + "] INIT OK ver=" + std::to_string(maj) + "." + std::to_string(min)
        + " vendor=" + (vendor ? vendor : "?")).c_str());
    return d;
}

extern "C" int WebCoreGlSelfTest()
{
    ensureInit();

    // Try hardware at default feature levels (fails — Adreno is FL9_3), then hardware
    // capped to FL9_3 (should succeed on the GPU), then WARP software fallback.
    EGLDisplay d = tryAngleDisplay("D3D11-hw", EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE);
    if (d == EGL_NO_DISPLAY)
        d = tryAngleDisplay("D3D11-hw-fl93", EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE, 9, 3);
    if (d == EGL_NO_DISPLAY)
        d = tryAngleDisplay("D3D11-warp", EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE);
    if (d == EGL_NO_DISPLAY) {
        portLog("gl: all ANGLE D3D11 init paths failed");
        return -1;
    }

    // Bring up a minimal config + pbuffer context to read GL_RENDERER (real backend).
    EGLint cfgAttrs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint numCfg = 0;
    if (!eglChooseConfig(d, cfgAttrs, &cfg, 1, &numCfg) || numCfg < 1) {
        portLog(("gl: eglChooseConfig failed err=" + hex(eglGetError())).c_str());
        return -2;
    }
    EGLint pbAttrs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(d, cfg, pbAttrs);
    EGLint ctxAttrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctxAttrs);
    if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(d, surf, surf, ctx)) {
        portLog(("gl: surface/context/makeCurrent failed err=" + hex(eglGetError())).c_str());
        return -3;
    }
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* glVer = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    portLog((std::string("gl: RENDERER=") + (renderer ? renderer : "?") + " VERSION=" + (glVer ? glVer : "?")).c_str());
    return 0;
}

// Composite the page's layer tree (wrapped in a non-composited content layer that
// paints the FrameView content that isn't in its own composited layer) into the
// framebuffer `targetFBO` at w×h, via TextureMapperGL on the GPU. (Mirrors
// AcceleratedCompositingContext.) The GL context must already be current.
static void compositeTree(WebCore::GraphicsLayer* rootLayer, WebCore::FrameView* view, WebCore::TextureMapper& tm, int w, int h, GLuint targetFBO)
{
    using namespace WebCore;
    struct ContentClient final : public GraphicsLayerClient {
        FrameView& view;
        ContentClient(FrameView& v) : view(v) { }
        void paintContents(const GraphicsLayer*, GraphicsContext& ctx, const FloatRect& rect, GraphicsLayerPaintBehavior) override
        {
            ctx.save();
            ctx.clip(rect);
            view.paint(ctx, enclosingIntRect(rect));
            ctx.restore();
        }
    } contentClient(*view);

    auto contentLayerRef = GraphicsLayer::create(nullptr, contentClient);
    GraphicsLayer& contentLayer = contentLayerRef.get();
    contentLayer.setDrawsContent(true);
    contentLayer.setContentsOpaque(true);
    contentLayer.setSize(FloatSize(w, h));
    contentLayer.addChild(*rootLayer);
    contentLayer.setNeedsDisplay();

    // Flush layer tree into backing stores (cairo tiles uploaded as GL textures). This
    // may bind its own FBOs, so bind the target framebuffer AFTER it, before painting.
    contentLayer.flushCompositingStateForThisLayerOnly();
    rootLayer->flushCompositingStateForThisLayerOnly();
    view->flushCompositingStateIncludingSubframes();
    downcast<GraphicsLayerTextureMapper>(contentLayer).updateBackingStoreIncludingSubLayers(tm);

    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
    glViewport(0, 0, w, h);
    glClearColor(1, 1, 1, 1); // opaque white page background
    glClear(GL_COLOR_BUFFER_BIT);

    tm.beginPainting();          // captures the bound framebuffer as render target
    downcast<GraphicsLayerTextureMapper>(contentLayer).layer().paint(tm);
    tm.endPainting();

    // Detach the page's compositing root before our local content layer is destroyed
    // (WebCore's RenderLayerCompositor owns it, not us).
    rootLayer->removeFromParent();
}

// Headless GPU compositor: render the layer tree to an offscreen FBO, read back to
// straight RGBA8888 (top-down). Returns 0 ok.
static int compositeToRGBA(WebCore::GraphicsLayer* rootLayer, WebCore::FrameView* view, int w, int h, uint8_t* outRGBA)
{
    using namespace WebCore;
    auto context = GLContext::createOffscreenContext(&PlatformDisplay::sharedDisplayForCompositing());
    if (!context || !context->makeContextCurrent()) {
        portLog("comp: GL offscreen context/makeCurrent failed");
        return -20;
    }
    auto textureMapper = TextureMapperGL::create();

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        portLog("comp: FBO incomplete");
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        return -21;
    }

    compositeTree(rootLayer, view, *textureMapper, w, h, fbo);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, outRGBA);

    // GL is bottom-up; flip rows to top-down.
    std::vector<uint8_t> row(static_cast<size_t>(w) * 4);
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* a = outRGBA + static_cast<size_t>(y) * w * 4;
        uint8_t* b = outRGBA + static_cast<size_t>(h - 1 - y) * w * 4;
        memcpy(row.data(), a, row.size());
        memcpy(a, b, row.size());
        memcpy(b, row.data(), row.size());
    }

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    portLog("comp: GPU composite done");
    return 0;
}

// ===================== Live interactive browser (CoreWindow) =====================
namespace {
double g_deviceScale = 1.0;   // device pixels per CSS pixel (mobile DPR)
int g_cssW = 0, g_cssH = 0;   // CSS-px viewport (physical / deviceScale)

// GPU-tree paint crash fallback (declared here so BrowserContentClient::paintContents below can
// see it). Some pages build a composited layer tree that faults deep inside ANGLE during the
// TextureMapper draw on this weak GPU (Adreno 305, D3D FL9_3); when stage c5* crashes repeatedly
// we render that page flat via cairo with FlattenCompositingLayers (full page, no GPU draw).
// Reset on navigation so the next page gets a fresh attempt at GPU compositing.
int g_gpuTreeCrashes = 0;
bool g_flatOnlyFallback = false;

// Persistent non-composited content layer: paints the CURRENT FrameView (recreated on
// navigation, so query it dynamically — never hold a stale reference). The layer is
// physical-px sized; the paint is scaled by the DPR so the CSS-px page fills it crisply.
class BrowserContentClient final : public WebCore::GraphicsLayerClient {
public:
    WebCore::Frame* frame = nullptr;
    void paintContents(const WebCore::GraphicsLayer*, WebCore::GraphicsContext& ctx, const WebCore::FloatRect& clip, WebCore::GraphicsLayerPaintBehavior) override
    {
        if (!frame)
            return;
        if (WebCore::FrameView* v = frame->view()) {
            // clip is the region TextureMapper is re-rasterizing, in layer (physical) px.
            // Convert to CSS px so WebCore only paints that area (dirty-rect rendering).
            WebCore::FloatRect cssClip(clip.x() / g_deviceScale, clip.y() / g_deviceScale,
                clip.width() / g_deviceScale, clip.height() / g_deviceScale);
            cssClip.intersect(WebCore::FloatRect(0, 0, g_cssW, g_cssH));
            if (cssClip.isEmpty())
                cssClip = WebCore::FloatRect(0, 0, g_cssW, g_cssH); // full paint if no clip given
            ctx.save();
            ctx.scale(g_deviceScale);
            ctx.clip(cssClip);
            // GPU-crash fallback: when the composited layer tree faults inside ANGLE for this page
            // (some pages do on this weak GPU), we stop drawing the GPU tree and render everything
            // through this flat cairo paint instead. Normally FrameView::paint STOPS at compositing
            // layer boundaries (composited content is the GPU tree's job) — so in fallback we add
            // FlattenCompositingLayers, which makes it paint the composited layers flattened into
            // cairo too. Result: the FULL page renders on the CPU with no GPU draw to crash.
            auto savedBehavior = v->paintBehavior();
            if (g_flatOnlyFallback)
                v->setPaintBehavior(savedBehavior | WebCore::PaintBehavior::FlattenCompositingLayers);
            v->paint(ctx, WebCore::enclosingIntRect(cssClip));
            if (g_flatOnlyFallback)
                v->setPaintBehavior(savedBehavior);
            ctx.restore();
        }
    }
};

std::unique_ptr<WebCore::GLContext> g_glContext;
std::unique_ptr<WebCore::TextureMapper> g_textureMapper;
std::unique_ptr<WebCore::Page> g_page;
WebCore::PortChromeClient* g_chrome = nullptr; // owned by the page config
WebCore::PortFrameLoaderClient* g_loaderClient = nullptr;
BrowserContentClient g_contentClient;
RefPtr<WebCore::GraphicsLayer> g_contentLayer;
// Scale wrapper between g_contentLayer and the composited root. The composited layer tree is in
// CSS px but the GL viewport is physical px, so composited layers (e.g. <video>, canvas) must be
// scaled by the device pixel ratio or they render at CSS size in the top-left (~1/DPR). This
// wrapper carries a scale(DPR) transform anchored at the origin; g_contentLayer's own flat paint
// is already physical (paintContents scales by g_deviceScale), so only the composited child tree
// needs it.
RefPtr<WebCore::GraphicsLayer> g_scaleLayer;
WebCore::GraphicsLayer* g_attachedRoot = nullptr;
int g_winW = 0, g_winH = 0;
bool g_needsRepaint = true; // re-raster the page only when content actually changes
int g_postLoadFrames = 0;   // keep repainting a while after load (late image decodes)
}

// Create the GL context on the CoreWindow (IInspectable*), build a Page with
// accelerated compositing, and start loading `url`. Returns 0 on success.
extern "C" void WebCoreMseSpike(); // Wave 2 MSE-via-MF go/no-go probe (logs mse-spike: ...)
namespace WebCore { void GLContextEGLSetFixedWindowSize(int, int); }

// Minimal NotificationClient. ENABLE_NOTIFICATIONS is on (the Notification DOM API compiles),
// but WebCore's Document::notificationClient() unconditionally dereferences the page's
// NotificationController supplement — which only exists if provideNotification() was called.
// Without it, ANY page JS reading Notification.permission (very common) null-derefs and crashes.
// We report permission "denied" (we don't surface system notifications), which is safe + honest.
class PortNotificationClient final : public WebCore::NotificationClient {
public:
    bool show(WebCore::Notification&) final { return false; }
    void cancel(WebCore::Notification&) final { }
    void notificationObjectDestroyed(WebCore::Notification&) final { }
    void notificationControllerDestroyed() final { delete this; }
    void requestPermission(WebCore::ScriptExecutionContext&, PermissionHandler&& handler) final { handler(Permission::Denied); }
    Permission checkPermission(WebCore::ScriptExecutionContext*) final { return Permission::Denied; }
};

// Minimal GeolocationClient — same story: ENABLE_GEOLOCATION is on, but Geolocation.cpp
// dereferences GeolocationController::from(page) unconditionally, which is null unless
// provideGeolocationTo() was called. We deny permission (no location backend on W10M).
class PortGeolocationClient final : public WebCore::GeolocationClient {
public:
    void geolocationDestroyed() final { delete this; }
    void startUpdating(const String&, bool) final { }
    void stopUpdating() final { }
    void setEnableHighAccuracy(bool) final { }
    std::optional<WebCore::GeolocationPositionData> lastPosition() final { return std::nullopt; }
    void requestPermission(WebCore::Geolocation& geolocation) final { geolocation.setIsAllowed(false, String()); }
    void cancelPermissionRequest(WebCore::Geolocation&) final { }
};

// Revenant XAML/CX shell entry: render the engine into a XAML SwapChainPanel. The panel
// (passed as its IInspectable*) has no intrinsic pixel size, so the shell supplies the target
// backbuffer size in PHYSICAL pixels; we pin it via the GLContextEGL fixed-size hook, then
// reuse the normal init — createContextForWindow builds a fixed-size ANGLE surface on the
// panel and defaultFrameBufferSize() then reports pxW x pxH.
extern "C" int WebCoreBrowserInit(void* window, const char* url, double deviceScale);
int g_forcedWinW = 0, g_forcedWinH = 0; // shell-supplied panel size; overrides defaultFrameBufferSize

// Shell-supplied writable data directory (ApplicationData::Current->LocalFolder->Path). Used for
// persistent cookies (<path>\cookies.db) and persistent localStorage (<path>\localstorage). The
// shell MUST call WebCoreBrowserSetDataPath() before WebCoreBrowserInit so the cookie session and
// storage provider open the on-disk databases instead of falling back to in-memory.
static std::string g_dataPath;
extern "C" void WebCoreBrowserSetDataPath(const char* p) { g_dataPath = p ? p : ""; }
namespace WebCorePort { const std::string& portDataPath() { return g_dataPath; } }

extern "C" int WebCoreBrowserInitPanel(void* swapChainPanel, int pxW, int pxH, double deviceScale, const char* url)
{
    // The shell passes a configured IPropertySet (panel + EGLRenderSurfaceSizeProperty) as the
    // native window, so ANGLE renders at that explicit size and applies the compensating matrix
    // transform itself — we must NOT also force EGL_FIXED_SIZE (that path skips the transform →
    // zoomed). Clear the fixed-size hint. defaultFrameBufferSize() is unreliable for this
    // surface (returned 0 → 480x800 fallback → black), so force the engine window size from the
    // physical px the shell computed.
    WebCore::GLContextEGLSetFixedWindowSize(0, 0);
    g_forcedWinW = pxW;
    g_forcedWinH = pxH;
    return WebCoreBrowserInit(swapChainPanel, url, deviceScale);
}

// ANGLE debug callback (GL_KHR_debug). Fires SYNCHRONOUSLY at the offending GL call, so when the
// TextureMapper draw of a page's layer tree faults deep inside ANGLE, the log line immediately
// before the crash names the real cause (e.g. "program has not been successfully linked",
// "framebuffer incomplete", "invalid operation"). GL_APIENTRY matches ANGLE's calling convention.
static void GL_APIENTRY portGLDebugCallback(GLenum, GLenum type, GLuint id, GLenum severity,
    GLsizei, const GLchar* message, const void*)
{
    // Log actual GL ERRORS only (0x824C = GL_DEBUG_TYPE_ERROR); skip notifications/perf/other so the
    // log carries signal, not noise. These can fire every frame from the compositor, so DEDUP
    // consecutive identical messages and tag each with the current site for attribution.
    if (type != 0x824C)
        return;
    const char* msg = message ? message : "";
    static char s_last[248] = { 0 };
    if (!std::strncmp(s_last, msg, sizeof s_last - 1))
        return; // same error as last time — already reported
    std::strncpy(s_last, msg, sizeof s_last - 1);
    s_last[sizeof s_last - 1] = 0;
    std::string site = "-";
    if (g_page) {
        if (auto* doc = g_page->mainFrame().document())
            site = doc->url().string().left(100).utf8().data();
    }
    char b[400];
    snprintf(b, sizeof b, "GLERR [%s] id=%u: %.240s", site.c_str(), id, msg);
    WebCorePort::portLog(b);
}

extern "C" int WebCoreBrowserInit(void* coreWindow, const char* url, double deviceScale)
{
    using namespace WebCore;
    ensureInit();

    portLog("watchdog: JS runaway-script CPU time limit armed (10s) via Options");
    startMainStallDetector(); // logs where the main thread is stuck if a native (non-JS) freeze hits

    WebCoreMseSpike(); // one-shot: is MF's built-in MSE reachable from this AppContainer?

    g_glContext = GLContext::createContextForWindow(reinterpret_cast<GLNativeWindowType>(coreWindow), &PlatformDisplay::sharedDisplayForCompositing());
    if (!g_glContext || !g_glContext->makeContextCurrent()) {
        portLog("browser: window GL context creation failed");
        return -1;
    }
    // Turn on ANGLE's debug reporting so GL errors are logged at their source (see callback above).
    glEnable(GL_DEBUG_OUTPUT_KHR);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
    glDebugMessageCallbackKHR(portGLDebugCallback, nullptr);
    portLog("gl: KHR_debug callback installed");
    IntSize size = g_glContext->defaultFrameBufferSize();
    // Panel path: trust the shell-supplied physical size (defaultFrameBufferSize is unreliable
    // for the SwapChainPanel property-set surface). CoreWindow path: use the queried size.
    extern int g_forcedWinW, g_forcedWinH;
    g_winW = g_forcedWinW > 0 ? g_forcedWinW : (size.width() > 0 ? size.width() : 480);
    g_winH = g_forcedWinH > 0 ? g_forcedWinH : (size.height() > 0 ? size.height() : 800);
    g_deviceScale = deviceScale > 0 ? deviceScale : 1.0;
    // Round the CSS viewport UP (ceil), not to-nearest. The page is painted back at
    // g_deviceScale, so cssW*scale must be >= the physical window or a thin uncovered strip
    // on the right/bottom shows the white clear color (the "white border"). ceil guarantees
    // the content fully covers the surface (the sub-pixel overscan is clipped by glViewport).
    g_cssW = static_cast<int>(std::ceil(g_winW / g_deviceScale));
    g_cssH = static_cast<int>(std::ceil(g_winH / g_deviceScale));
    portLog((std::string("browser: window ") + std::to_string(g_winW) + "x" + std::to_string(g_winH)
        + " renderer=" + reinterpret_cast<const char*>(glGetString(GL_RENDERER))).c_str());
    g_textureMapper = TextureMapperGL::create();

    auto config = pageConfigurationWithEmptyClients(PAL::SessionID::defaultSessionID());
    // Real editor client: the empty one blocks all editing (typing inserts nothing). This one
    // permits editing and turns key events into text insertion / editing commands.
    config.editorClient = WebCorePort::createPortEditorClient();
    auto loaderRef = makeUniqueRef<PortFrameLoaderClient>();
    g_loaderClient = loaderRef.ptr();
    config.loaderClientForMainFrame = WTFMove(loaderRef);
    auto chromeRef = makeUnique<PortChromeClient>();
    g_chrome = chromeRef.release(); // leaked intentionally: lives for the app's lifetime
    config.chromeClient = g_chrome;
    // Real DOM storage + cookie jar. The EmptyClients defaults silently drop all
    // localStorage writes and all cookies, which stalls modern SPAs (they store state
    // in localStorage/cookies and loop when reads come back empty).
    config.storageNamespaceProvider = WebCorePort::createPortStorageNamespaceProvider();
    config.cookieJar = WebCore::CookieJar::create(WebCorePort::createPortStorageSessionProvider());
    // Real CacheStorage (window.caches). EmptyClients installs a dummy connection whose
    // methods never call back (promises hang); this provides a working in-memory one.
    config.cacheStorageProvider = WebCorePort::createPortCacheStorageProvider();

    // Expose window.caches: it's gated by the CacheAPIEnabled runtime flag (off by default,
    // hence "ReferenceError: caches"), and a SecureContext check that hides it on non-https
    // pages. Both live in the process-global RuntimeEnabledFeatures.
    WebCore::RuntimeEnabledFeatures::sharedFeatures().setCacheAPIEnabled(true);
    WebCore::RuntimeEnabledFeatures::sharedFeatures().setSecureContextChecksEnabled(false);
    // ServiceWorker runtime feature — enabled so PWAs (bsky/charavault/google) can register their
    // worker. The infra was built but only exercised at runtime once this flag flipped, exposing a
    // hang in the register/install/fetch path (see SW-DIAG logs in PortServiceWorker.cpp).
    WebCore::RuntimeEnabledFeatures::sharedFeatures().setServiceWorkerEnabled(true);
    WebCore::RuntimeEnabledFeatures::sharedFeatures().setPushAPIEnabled(true);

    g_page = makeUnique<Page>(WTFMove(config));

    // Install feature-controller supplements so their WebCore accessors don't null-deref when
    // page JS touches the (enabled) DOM API — same missing-provider pattern for each.
    WebCore::provideNotification(g_page.get(), new PortNotificationClient);
    WebCore::provideGeolocationTo(g_page.get(), *new PortGeolocationClient);
    g_page->settings().setScriptEnabled(true);
    // Enable image loading explicitly. Without these, the CachedResourceLoader treats
    // every image as DeferredUntilVisible; <img> gets revealed by the visibility path,
    // but CSS mask-image/background-image resources never do and stay unloaded (blank
    // icons + missing logo). Setting these syncs the loader and kicks deferred loads.
    g_page->settings().setImagesEnabled(true);
    g_page->settings().setLoadsImagesAutomatically(true);
    // Modern SPAs assume localStorage exists and works; WebKit gates it off by default.
    // With our real StorageNamespaceProvider above, turn it on. (sessionStorage has no
    // separate enable flag in 2.36.8 — it works once the namespace provider supplies it.)
    g_page->settings().setLocalStorageEnabled(true);
    // AC ON (re-enabling, 2026-07-03): AC was disabled early as a workaround for "blank
    // in-page images", but that was later root-caused to the canRender/size + storage +
    // loadsImagesAutomatically bugs (all since fixed) — not compositing. AC is the proper
    // path and is required for zero-copy accelerated <video> (TextureMapperPlatformLayerProxy).
    g_page->settings().setAcceleratedCompositingEnabled(true);
    // NON-HEADLESS: force full compositing so WebCore's RenderLayerCompositor builds ONE GPU layer
    // tree for the ENTIRE document — its root content layer paints the non-composited content and
    // composited elements are its children. We then just composite that tree; no flat FrameView layer,
    // no manual dirty tracking. (This is the WebKit2 drawing-area model.) See NONHEADLESS_RENDER_SPEC.
    g_page->settings().setForceCompositingMode(true);
    // Hand DPR to WebCore: layout in CSS px, backing stores in device px. Replaces the manual
    // scale-wrapper. WebCore now scales CSS->device internally; media (resolution), image srcset and
    // hit-testing are correct. Input arrives in device px and is converted to CSS px before dispatch.
    g_page->setDeviceScaleFactor(g_deviceScale);
    g_page->settings().setShouldAllowUserInstalledFonts(false);
    // window.requestIdleCallback is implemented in 2.36.8 but gated off by default
    // (EnabledBySetting=requestIdleCallbackEnabled) → "ReferenceError: requestIdleCallback".
    // Modern cooperative schedulers (YouTube c3, React) defer non-critical render/build work
    // to it; without it that work is either dropped or the scheduler's idle path throws.
    g_page->settings().setRequestIdleCallbackEnabled(true);

    // Core web-platform observers/APIs that WebKit ships default-OFF but every modern SPA
    // assumes exist. IntersectionObserver in particular is how feed/list UIs (YouTube, most
    // infinite-scroll sites) lazy-mount their items — with it disabled `new IntersectionObserver`
    // is absent and the feed never builds (permanent skeleton). ResizeObserver is used by
    // responsive components. All of these are pure-WebCore (no platform backend) so enabling
    // the flag makes them fully functional.
    g_page->settings().setIntersectionObserverEnabled(true);
    g_page->settings().setResizeObserverEnabled(true);
    g_page->settings().setVisualViewportAPIEnabled(true);
    g_page->settings().setLazyImageLoadingEnabled(true);
    g_page->settings().setLazyIframeLoadingEnabled(true);
    g_page->settings().setBeaconAPIEnabled(true);          // navigator.sendBeacon (analytics/logging)
    g_page->settings().setPermissionsAPIEnabled(true);     // navigator.permissions (feature-detected)
    g_page->settings().setWebGLEnabled(true);              // WebGL1 (WebGL2 already on); ANGLE backs it
    // HTML form controls that render native pickers.
    g_page->settings().setInputTypeDateEnabled(true);
    g_page->settings().setInputTypeColorEnabled(true);
    g_page->settings().setInputTypeTimeEnabled(true);
    g_page->settings().setInputTypeMonthEnabled(true);
    g_page->settings().setInputTypeWeekEnabled(true);
    g_page->settings().setInputTypeDateTimeLocalEnabled(true);
    g_page->settings().setInertAttributeEnabled(true);     // the `inert` attribute (modals/dialogs)
    // OffscreenCanvas main-thread + in-workers (both ENABLE flags now on).
    WebCore::RuntimeEnabledFeatures::sharedFeatures().setOffscreenCanvasEnabled(true);
    WebCore::RuntimeEnabledFeatures::sharedFeatures().setOffscreenCanvasInWorkersEnabled(true);
    // NOTE: WebKit's *internal* WebM demuxer + Opus/Vorbis decoders (ENABLE(WEBM)/VORBIS/OPUS)
    // stay off — we don't use WebKit's software decoders. WebM/VP9/Opus/Vorbis decoding is
    // done by Media Foundation via the installed Store media extensions, advertised by the MF
    // backend's getSupportedTypes()/supportsType() (see MediaPlayerPrivateMediaFoundation).

    Frame& frame = g_page->mainFrame();
    g_loaderClient->setFrame(&frame);
    frame.setView(FrameView::create(frame));
    frame.init();

    // Mark the page fully VISIBLE + ACTIVE + FOCUSED + IN-WINDOW, now that the main frame has a view
    // and an initial document (setIsVisible walks frames/documents — calling it before frame.init()
    // fast-fails). Without this WebCore treats the page as hidden and THROTTLES/pauses everything
    // that makes a page feel "live": animated GIFs, CSS animations, requestAnimationFrame, DOM timers
    // ("GIFs don't animate until you scroll"). Full activity state runs them at full rate.
    g_page->setActivityState({ ActivityState::WindowIsActive, ActivityState::IsFocused,
        ActivityState::IsVisible, ActivityState::IsVisibleOrOccluded, ActivityState::IsInWindow });

    FrameView* view = frame.view();
    view->resize(g_cssW, g_cssH); // CSS-px viewport => mobile layout width
    view->setBaseBackgroundColor(Color::white);
    // Mark the view on-screen. ScrollView::isVisible() = selfVisible && parentVisible, both default
    // false; without this ScrollView::isOffscreen() is true, so RenderElement::isVisibleIgnoringGeometry()
    // reports EVERY element as not-visible-in-viewport — which pauses all image (GIF) animations
    // (they only twitch back on scroll). We have no platformWidget/HWND, so set both flags directly.
    view->setParentVisible(true);
    view->show(); // sets selfVisible; with parentVisible above, isVisible() -> true -> on-screen
    portLog((std::string("browser: css viewport ") + std::to_string(g_cssW) + "x" + std::to_string(g_cssH)
        + " scale=" + std::to_string(g_deviceScale)).c_str());
    // Tell WebCore the real screen size (CSS px) so window.screen + @media (device-width) match the
    // device instead of the hardcoded 360x640 default. Fullscreen mobile => screen ~= viewport.
    setPlatformScreenBounds(FloatRect(0, 0, g_cssW, g_cssH));
    PortChromeClient::setViewportSize(IntSize(g_cssW, g_cssH));
    WebCoreBrowserKeepCompositing(90); // brief bootstrap; content invalidations drive compositing during load
                                       // (a long force-window wastes ~2.5s compositing an idle post-load page)
    // No flat content layer / scale wrapper anymore: with forceCompositingMode + setDeviceScaleFactor,
    // WebCore's RenderLayerCompositor owns the whole tree (device-px). We composite its
    // rootGraphicsLayer() directly each vsync (see WebCoreBrowserRenderFrame).

    URL u { URL(), String::fromUTF8(url) };
    if (!u.isValid())
        return -10;
    ResourceRequest request { u };
    FrameLoadRequest flr { frame, request, SubstituteData() };
    frame.loader().load(WTFMove(flr));
    portLog("browser: load started");
    return 0;
}

// volatile: g_renderStage has internal linkage, so without volatile the optimizer may hoist a
// "stage" write past an opaque GL call (glClear can't observe a static var → no ordering dep),
// making the SEH crash report name the WRONG sub-step. volatile forces the writes to stay ordered
// relative to the GL calls so the reported stage is the actual faulting call.
static const char* volatile g_renderStage = "start"; // last render sub-step, for the SEH crash report

// Main-thread heartbeat + native stall detector. A hard freeze with the JS watchdog silent means a
// NATIVE hang/loop on the main thread (JS watchdog only sees JS CPU time). This bumps every render
// tick; a background thread logs the last render stage when the heartbeat stops advancing — telling
// us WHERE the main thread is stuck (e.g. "a1:ctx" = inside RunLoop::iterate = a timer/microtask/curl
// storm; "u:layout" = a layout loop; etc.) without needing SuspendThread (desktop-only under UWP).
extern "C" char __ImageBase;
static std::atomic<unsigned long long> g_mainHeartbeat { 0 };
static HANDLE g_mainThreadHandle = nullptr;

// Read up to n bytes from a (possibly partly-unmapped) address, in 512B chunks, stopping at the first
// fault. Used to walk a deadlocked thread's stack without risking an AV in the detector thread.
static size_t safeReadStack(const void* addr, void* buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        size_t chunk = (n - done < 512) ? (n - done) : 512;
        __try {
            memcpy(static_cast<unsigned char*>(buf) + done, static_cast<const unsigned char*>(addr) + done, chunk);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        done += chunk;
    }
    return done;
}
static void startMainStallDetector()
{
    static bool started = false;
    if (started)
        return;
    started = true;
    // Grab a real handle to THIS (main) thread so the detector can suspend+sample it on a stall.
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
        &g_mainThreadHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::thread([]() {
        unsigned long long last = 0;
        int stalledSecs = 0;
        unsigned long long lastCpu = 0;
        for (;;) {
            ::Sleep(1000);
            unsigned long long hb = g_mainHeartbeat.load();
            if (hb == last && hb != 0) {
                stalledSecs++;
                if (stalledSecs == 3 || stalledSecs == 6 || (stalledSecs > 6 && stalledSecs % 15 == 0)) {
                    // CPU burned since last check -> loop (burning) vs deadlock (idle).
                    unsigned long long cpu = 0;
                    FILETIME ct, et, kt, ut;
                    if (g_mainThreadHandle && GetThreadTimes(g_mainThreadHandle, &ct, &et, &kt, &ut))
                        cpu = ((unsigned long long)kt.dwHighDateTime << 32 | kt.dwLowDateTime)
                            + ((unsigned long long)ut.dwHighDateTime << 32 | ut.dwLowDateTime);
                    unsigned long long cpuDeltaMs = (cpu - lastCpu) / 10000;
                    lastCpu = cpu;
                    const char* kind = cpuDeltaMs > 400 ? "BURNING-CPU(native loop)" : "IDLE(deadlock/blocked)";
                    char b[200];
                    snprintf(b, sizeof b, "MAIN-STALL ~%ds stage=%s hb=%llu cpuDelta=%llums %s",
                        stalledSecs, g_renderStage, hb, cpuDeltaMs, kind);
                    portLog(b);

                    // Sample the main thread's PC/LR (symbolize the rva against WebCoreRenderShell.map).
                    // Read the registers while suspended, but ONLY log AFTER resuming (portLog takes a
                    // mutex the main thread might hold -> would deadlock if we logged while it's frozen).
                    if (g_mainThreadHandle && SuspendThread(g_mainThreadHandle) != (DWORD)-1) {
                        CONTEXT ctx;
                        memset(&ctx, 0, sizeof ctx);
                        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                        bool ok = GetThreadContext(g_mainThreadHandle, &ctx);
                        unsigned long pc = ctx.Pc, lr = ctx.Lr, sp = ctx.Sp;
                        ResumeThread(g_mainThreadHandle);
                        if (ok) {
                            uintptr_t base = reinterpret_cast<uintptr_t>(&__ImageBase);
                            uintptr_t modEnd = base + 0x4000000; // exe is ~55MB; generous upper bound
                            char c[200];
                            snprintf(c, sizeof c, "MAIN-STALL sample pc=0x%08lx rva=0x%08lx lr=0x%08lx lrRva=0x%08lx sp=0x%08lx",
                                pc, (unsigned long)(pc - base), lr, (unsigned long)(lr - base), sp);
                            portLog(c);
                            // Walk the stack (the deadlocked thread's stack is stable). SEH-read a few KB
                            // above sp and log return addresses that fall in OUR module -> the WebCore
                            // call chain that led into the kernel wait. Symbolize rvas via the .map.
                            static unsigned char s_stackbuf[8192];
                            size_t got = safeReadStack(reinterpret_cast<const void*>((uintptr_t)sp), s_stackbuf, sizeof s_stackbuf);
                            int printed = 0;
                            for (size_t off = 0; off + 4 <= got && printed < 24; off += 4) {
                                unsigned long w;
                                memcpy(&w, s_stackbuf + off, 4);
                                if (w >= base && w < modEnd && (w & 1)) {
                                    char f[128];
                                    snprintf(f, sizeof f, "  st rva=0x%08lx (sp+0x%04zx)", (unsigned long)((w & ~1UL) - base), off);
                                    portLog(f);
                                    printed++;
                                }
                            }
                        }
                    }
                }
            } else {
                last = hb;
                stalledSecs = 0;
            }
        }
    }).detach();
}

// The HTML "update the rendering" step — WebCore-owned cadence. This is NOT called every
// frame by the driver; it is called by WebCore's own RenderingUpdateScheduler through
// PortChromeClient::triggerRenderingUpdate(). The scheduler (a WTF Timer at the page's
// preferred interval, fired by RunLoop::iterate() in RenderFrame) decides WHEN a rendering
// update is needed — rAF pending, style/layout dirty, image decoded, IntersectionObserver,
// animations — and throttles to idle when nothing changes. That is what makes scrolling and
// animation smooth: we no longer re-run layout/style/paint on every vsync; WebCore runs it
// only when the page actually changed, and RenderFrame just composites + presents the result.
//
// updateRendering() MUST be paired with finalizeRenderingUpdate() (Page.h): the former pushes
// a step-set onto Page::m_renderingUpdateRemainingSteps, the latter pops it. Without the pair
// the stack never drains and every later updateRendering hits the re-entrancy guard and skips
// rAF / IntersectionObserver / ResizeObserver — the classic SPA-stuck-at-skeleton bug.
static bool g_inRenderingUpdate = false;
extern "C" void WebCoreDriverRunRenderingUpdate()
{
    using namespace WebCore;
    if (!g_page || g_inRenderingUpdate)
        return;
    Frame& frame = g_page->mainFrame();
    FrameView* view = frame.view();
    if (!view)
        return;
    g_inRenderingUpdate = true;
    const char* savedStage = g_renderStage;
    g_renderStage = "u:updateRendering";
    g_page->updateRendering();
    g_renderStage = "u:finalize";
    g_page->finalizeRenderingUpdate({ FinalizeRenderingUpdateFlags::ApplyScrollingTreeLayerPositions });
    g_renderStage = "u:layout";
    // updateRendering() above already runs style + layout as part of the rendering update, so this
    // forced full synchronous layout was REDUNDANT — and it ran on EVERY animation frame, making it
    // the dominant per-frame CPU cost on animating pages. Only force it when layout is actually still
    // pending (e.g. updateRendering deferred it behind pending stylesheets); otherwise skip.
    if (view->needsLayout()) {
        if (RefPtr document = frame.document())
            document->updateLayoutIgnorePendingStylesheets();
    }
    // Keep animated-image + rAF/timer visibility current. Our port CACHES painted content (only
    // repaints on change), so WebCore never sees an animating GIF get re-painted and PAUSES it when a
    // frame advances (CachedImage::imageFrameAvailable finds no visible client → animation stops).
    // viewportContentsChanged() re-establishes each in-viewport image's visible state
    // (updateVisibleViewportRect) and resumes it — WebKit's sanctioned mechanism for a content-caching
    // drawing model (see Page::resumeAnimatingImages). Cheap (visibility checks only, no layout). This
    // is why GIFs "only animate on scroll": scrolling was the only thing calling this.
    g_renderStage = "u:viewportVisibility";
    view->viewportContentsChanged();
    g_renderStage = "u:flushCompositing";
    view->flushCompositingStateIncludingSubframes();
    g_renderStage = savedStage;
    g_inRenderingUpdate = false;
}

// GPU-offload gate state: skip the expensive per-frame composite when nothing changed. On a static
// page this hands the ARM core back to JS/loading instead of re-uploading + re-painting + swapping
// the whole layer tree at 60Hz for an unchanged image. (See the gate in WebCoreBrowserRenderFrame.)
static unsigned g_composeFrame = 0;      // increments once per render frame
static unsigned g_forceComposeUntil = 0; // composite unconditionally while g_composeFrame < this
static unsigned g_composited300 = 0, g_skipped300 = 0; // rolling 300-frame gate stats (proves savings)
static double g_msJS = 0, g_msComposite = 0;           // rolling 300-frame CPU split: JS/RunLoop vs composite

// Called by content whose pacing is decoupled from WebCore's invalidations (e.g. a playing <video>,
// whose frames arrive at composite time via OnVideoStreamTick, not per-invalidation). Keeps the
// composite alive for a window of frames so playback stays smooth; expires when the source stops.
extern "C" void WebCoreBrowserKeepCompositing(unsigned frames)
{
    unsigned until = g_composeFrame + frames;
    if (until > g_forceComposeUntil)
        g_forceComposeUntil = until;
}

// Pump WebCore (async load + timers), lay out, and present the current page state to
// the CoreWindow surface. Called once per frame by the shell's event loop.
extern "C" void WebCoreBrowserRenderFrame()
{
    using namespace WebCore;
    if (!g_page || !g_glContext)
        return;
    // Per-frame STAGE trace across a window of frames to localize a main-thread hang to an exact
    // sub-step: whichever "rf N <stage>" line is LAST before the log goes quiet is where the main
    // thread stopped returning. Windowed to avoid spamming every frame for the whole session.
    static unsigned g_rfFrame = 0;
    ++g_rfFrame;
    ++g_composeFrame;
    if (g_composeFrame % 300 == 0) {
        char b[140]; snprintf(b, sizeof b, "gate: last300 composited=%u skipped=%u | CPU JS=%.0fms composite=%.0fms",
            g_composited300, g_skipped300, g_msJS, g_msComposite);
        portLog(b); g_composited300 = 0; g_skipped300 = 0; g_msJS = 0; g_msComposite = 0;
    }
    g_mainHeartbeat.fetch_add(1, std::memory_order_relaxed); // stall detector watches this
    bool trc = false; // per-frame stage trace DISABLED for perf; g_renderStage is still set (cheap)
                      // so the SEH crash handler can still report the faulting sub-step.
    // Always record the current sub-step (cheap) so the SEH crash wrapper can report where an
    // access violation happened; additionally log it during the diagnostic window.
    auto stage = [&](const char* s){ g_renderStage = s; if (trc) { char b[64]; snprintf(b, sizeof b, "rf %u %s", g_rfFrame, s); WebCorePort::portLog(b); } };
    stage("a:enter");

    g_glContext->makeContextCurrent();
    stage("a1:ctx");
    MonotonicTime tJS0 = MonotonicTime::now();
    RunLoop::current().iterate(); // deliver curl completions, run timers (runs page JS)
    stage("a2:runloop");

    Frame& frame = g_page->mainFrame();
    FrameView* view = frame.view();
    if (!view)
        return;
    stage("a3:frameview");

    // Drain the microtask queue every tick. Modern SPAs (YouTube et al.) hydrate through an
    // ES-module load→evaluate chain and fetch().then() promise cascades — all microtask-driven.
    // Under our manually-pumped headless loop, WebCore's normal "run a task then checkpoint"
    // cadence doesn't fire on its own after the loader goes idle, so the promise/module graph
    // stalls mid-hydration and the page freezes at its loading skeleton (the embedded feed data
    // is present but never rendered). Explicitly checkpointing here advances that graph so the
    // SPA actually mounts. (Apotheosis does the same; it's the difference between a painted feed
    // and a permanent skeleton.)
    if (RefPtr doc = frame.document())
        doc->eventLoop().performMicrotaskCheckpoint();
    g_msJS += (MonotonicTime::now() - tJS0).milliseconds(); // RunLoop (curl completions + page JS) + microtasks
    stage("b:post-microtask");

    // DOM/PRIM hydration probes DISABLED for perf: they ran JavaScript (executeScriptIgnoringException)
    // on the main thread every frame-window to diagnose SPA hydration stalls. That's now understood;
    // running JS per tick is pure overhead. Re-enable via #if if a hydration stall needs probing again.
#if 0
    {
        static unsigned s_probeTick = 0;
        ++s_probeTick;
        if (s_probeTick == 60) {
            frame.script().executeScriptIgnoringException(String::fromUTF8(
                "try{Promise.resolve().then(function(){console.log('PRIM promise FIRED');});"
                "console.log('PRIM perfNow='+performance.now()+' dateNow='+Date.now());}catch(e){console.log('PRIM-ERR '+e);}"), false);
        }
        if (s_probeTick == 40 || s_probeTick == 120 || s_probeTick == 400 || s_probeTick == 1200 || s_probeTick == 3000) {
            frame.script().executeScriptIgnoringException(String::fromUTF8(
                "try{console.log('DOM-PROBE els='+document.getElementsByTagName('*').length"
                "+' bodyTxt='+(document.body?document.body.innerText.length:0));}catch(e){console.log('DOM-PROBE-ERR '+e);}"), false);
        }
    }
#endif
    // Deliver one REAL vsync tick to WebCore's display link (PortDisplayRefreshMonitor). This drives
    // the RenderingUpdateScheduler -> "update the rendering" step (layout, CSS/GIF animations, rAF,
    // IntersectionObserver, compositing flush) ONLY when WebCore has work, and idle-throttles itself.
    // WebCore owns the timing now — we do NOT hand-pump updateRendering().
    WebCoreBrowserVsyncTick();
    stage("c:vsync");

    // Composite WebCore's compositing layer tree directly. With forceCompositingMode +
    // setDeviceScaleFactor, rootGraphicsLayer() is a SINGLE tree that paints the ENTIRE document
    // (its root content layer paints the non-composited content; composited elements are children),
    // already at device px. No flat FrameView layer, no scale wrapper, no manual dirty tracking.
    stage("c1:flush");
    // Sync WebCore's pending compositing changes into the GraphicsLayer/TextureMapper tree. The
    // scheduler's updateRendering does this when the scene changes; calling it here is cheap when
    // already flushed and covers the first frame after the root layer is attached.
    view->flushCompositingStateIncludingSubframes();

    // GPU-OFFLOAD GATE: only pay for the clear/backing-upload/paint/swap when WebCore actually produced
    // a visual change this frame. Signals: a requested compositing flush or a dirty region (set by
    // animations, layout, invalidations, video, etc.), or an explicit keep-alive window (scroll/tap/
    // nav/resize/video). If none, skip — the last-swapped frame stays on the front buffer. On a static
    // page this drops the per-frame GPU+CPU cost to ~0, handing the ARM core back to JS/loading.
    bool needComposite = true;
    if (g_chrome) {
        bool sceneChanged = g_chrome->needsCompositingFlush() || g_chrome->hasDirtyRegion();
        g_chrome->clearCompositingFlush();
        g_chrome->takeDirtyRegion();
        needComposite = sceneChanged || (g_composeFrame < g_forceComposeUntil);
    }
    if (!needComposite) {
        ++g_skipped300;
        stage("f:skip-idle");
        return;
    }
    MonotonicTime tComposite0 = MonotonicTime::now();
    ++g_composited300;

    stage("c2:clear");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_winW, g_winH);
    glClearColor(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // Device-loss probe (Adreno TDR / D3D device-removed under heavy pages): a lost context makes
    // every later GL call deref a null D3D resource -> the recurring ANGLE crash + permanent wedge.
    if (EGLint e = eglGetError(); e != EGL_SUCCESS) {
        char b[64]; snprintf(b, sizeof b, "gl: eglGetError after clear = 0x%04x", e); portLog(b);
    }

    GraphicsLayer* root = g_chrome ? g_chrome->rootGraphicsLayer() : nullptr;
    if (root) {
        auto& tmRoot = downcast<GraphicsLayerTextureMapper>(*root);
        stage("c3:updateBacking");
        tmRoot.updateBackingStoreIncludingSubLayers(*g_textureMapper);
        stage("c4:beginPaint");
        g_textureMapper->beginPainting();
        // Apply device scale (DPR) to the whole composited tree. WebCore lays out in CSS px and its
        // GraphicsLayers are CSS-px sized, but our framebuffer is device px (cssW*DPR x cssH*DPR). A
        // hand-driven TextureMapper composite has no LayerTreeHost to apply the device-scale matrix,
        // so we set it on the root layer here — post-flush, every frame, since the c1 flush resets
        // the root to WebCore's identity transform. Without this the tree paints at 1x into the
        // top-left of the device framebuffer (content stuck in the corner). TMPAINT tsx100 -> ~175.
        tmRoot.layer().setTransform(TransformationMatrix().scale(g_deviceScale));
        stage("c5:layerPaint");
        tmRoot.layer().paint(*g_textureMapper);
        stage("c6:endPaint");
        g_textureMapper->endPainting();
    }
    stage("c7:swap");
    g_glContext->swapBuffers();
    g_msComposite += (MonotonicTime::now() - tComposite0).milliseconds(); // clear+backing-upload+paint+swap
    stage("f:present-done");
}

// SEH-guarded frame pump the shell actually calls. Some pages (heavy SPAs) still trip an
// access violation deep in layout/JS/paint; catching it here keeps the browser alive (skips
// the frame) instead of hard-crashing the whole app, and logs the sub-step it died in so the
// underlying bug can be found. Isolated from C++ unwinding objects (required for __try).
static unsigned long g_crashCode = 0;
static void* g_crashAt = nullptr;    // faulting INSTRUCTION address (is it in JIT'd code?)
static void* g_crashFault = nullptr; // memory address accessed (null? wild pointer?)
static int rfCrashFilter(EXCEPTION_POINTERS* ep)
{
    g_crashCode = ep->ExceptionRecord->ExceptionCode;
    g_crashAt = ep->ExceptionRecord->ExceptionAddress;
    g_crashFault = (ep->ExceptionRecord->NumberParameters >= 2)
        ? reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]) : nullptr;
    return EXCEPTION_EXECUTE_HANDLER;
}

extern "C" char __ImageBase; // linker-provided; its address IS this module's load base

extern "C" void WebCoreBrowserRenderFrameSafe()
{
    __try {
        WebCoreBrowserRenderFrame();
    } __except (rfCrashFilter(GetExceptionInformation())) {
        static unsigned s_crashes = 0;
        if (++s_crashes <= 20) {
            uintptr_t base = reinterpret_cast<uintptr_t>(&__ImageBase);
            uintptr_t rva = reinterpret_cast<uintptr_t>(g_crashAt) - base;
            char b[224];
            snprintf(b, sizeof b, "RENDER-CRASH #%u stage=%s code=0x%08lx at=%p base=0x%08zx rva=0x%08zx fault=%p",
                s_crashes, g_renderStage, g_crashCode, g_crashAt, (size_t)base, (size_t)rva, g_crashFault);
            WebCorePort::portLog(b);
        }
        // If the crash was in the GPU layer-tree paint (stage c5*), the composited tree of the
        // current page can't be drawn on this GPU without faulting. After a few, switch this page
        // to flat-only rendering (detach the composited root) so the browser stops re-faulting
        // every frame and doesn't stay wedged. Reset on navigation (WebCoreBrowserNavigate).
        if (g_renderStage && g_renderStage[0] == 'c' && g_renderStage[1] == '5') {
            if (++g_gpuTreeCrashes == 3) {
                g_flatOnlyFallback = true;
                g_needsRepaint = true; // force a full flat repaint so the whole page re-renders flattened
                WebCorePort::portLog("gl: GPU layer-tree paint keeps crashing -> flat CPU render (FlattenCompositingLayers) for this page");
            }
        }
    }
}

// Scroll the page by (dx, dy) device pixels (touch drag).
extern "C" void WebCoreBrowserScrollBy(int dx, int dy)
{
    using namespace WebCore;
    if (!g_page)
        return;
    if (FrameView* view = g_page->mainFrame().view()) {
        view->setScrollPosition(view->scrollPosition() + IntSize(dx, dy));
        WebCoreBrowserKeepCompositing(8); // ensure the scroll paints even if invalidation lags
    }
}

// A tap at (x, y) device pixels: a synthetic left click (focuses fields, follows links).
extern "C" void WebCoreBrowserTap(int x, int y)
{
    using namespace WebCore;
    if (!g_page)
        return;
    Frame& frame = g_page->mainFrame();
    IntPoint p(x, y);

    // Diagnostic: log the tap + what element the hit-test lands on at these CSS coords. If the
    // tag/href is empty or wrong, the coordinate mapping (DIP->CSS) is off; if it names the
    // expected control, the dispatch/handler side is the issue.
    {
        constexpr OptionSet<HitTestRequest::Type> ht { HitTestRequest::Type::ReadOnly, HitTestRequest::Type::Active, HitTestRequest::Type::AllowChildFrameContent };
        HitTestResult r { p };
        if (RefPtr doc = frame.document())
            doc->hitTest(ht, r);
        RefPtr node = r.innerNode();
        String tag = node ? node->nodeName() : String("<none>");
        String txt = node ? node->textContent().left(40) : String();
        portLog((std::string("tap: x=") + std::to_string(x) + " y=" + std::to_string(y)
            + " cssVp=" + std::to_string(g_cssW) + "x" + std::to_string(g_cssH)
            + " hit=" + tag.utf8().data() + " txt=" + txt.utf8().data()).c_str());
    }

    auto now = WallTime::now();
    PlatformMouseEvent down(p, p, LeftButton, PlatformEvent::MousePressed, 1, false, false, false, false, now, 0.0, NoTap);
    PlatformMouseEvent up(p, p, LeftButton, PlatformEvent::MouseReleased, 1, false, false, false, false, now, 0.0, NoTap);
    frame.eventHandler().handleMousePressEvent(down);
    frame.eventHandler().handleMouseReleaseEvent(up);

    // (Re)show or hide the on-screen keyboard based on what's focused AFTER the tap. WebCore only
    // fires elementDidFocus on a focus CHANGE, so re-tapping an already-focused field (e.g. after
    // the keyboard was dismissed on Enter/navigation) wouldn't bring the SIP back without this.
    // Tapping a non-editable spot hides it.
    if (RefPtr doc = frame.document()) {
        RefPtr fe = doc->focusedElement();
        bool editable = fe && (fe->isTextField() || fe->isContentEditable() || fe->hasEditableStyle());
        PortShowKeyboard(editable ? 1 : 0);
    }
    WebCoreBrowserKeepCompositing(8); // paint tap feedback / focus changes
}

// DOM key name for the common non-printable keys sites check via KeyboardEvent.key. Printable
// characters pass their own text as the key; this covers the navigation/editing keys.
static WTF::String domKeyForVirtualKey(int vk)
{
    switch (vk) {
    case 0x08: return "Backspace"_s;
    case 0x09: return "Tab"_s;
    case 0x0D: return "Enter"_s;
    case 0x1B: return "Escape"_s;
    case 0x20: return " "_s;
    case 0x25: return "ArrowLeft"_s;
    case 0x26: return "ArrowUp"_s;
    case 0x27: return "ArrowRight"_s;
    case 0x28: return "ArrowDown"_s;
    case 0x2E: return "Delete"_s;
    case 0x24: return "Home"_s;
    case 0x23: return "End"_s;
    default: return WTF::String();
    }
}

static WTF::OptionSet<WebCore::PlatformEvent::Modifier> keyModifiers(int shift, int ctrl, int alt)
{
    using namespace WebCore;
    WTF::OptionSet<PlatformEvent::Modifier> mods;
    if (shift) mods.add(PlatformEvent::Modifier::ShiftKey);
    if (ctrl)  mods.add(PlatformEvent::Modifier::ControlKey);
    if (alt)   mods.add(PlatformEvent::Modifier::AltKey);
    return mods;
}

// The key dispatch runs DOM/editing JS. It is invoked from the shell's CoreWindow key handlers,
// which fire between/within the message pump (RunLoop::iterate calls PeekMessage) — i.e. NOT inside
// the render pump's SEH guard. A fault there (bad focus state, editing-during-layout reentrancy)
// would hard-kill the whole app, so the two entrypoints below are SEH-wrapped exactly like the
// render pump: survive + log the fault address so the underlying bug can be found. The *Impl
// bodies hold the C++ objects (String/PlatformKeyboardEvent) that __try cannot coexist with.
static void keyDownUpImpl(int windowsVirtualKeyCode, int isDown, int shift, int ctrl, int alt)
{
    using namespace WebCore;
    if (!g_page)
        return;
    Frame& frame = g_page->focusController().focusedOrMainFrame();
    String key = domKeyForVirtualKey(windowsVirtualKeyCode);
    PlatformKeyboardEvent evt(isDown ? PlatformEvent::RawKeyDown : PlatformEvent::KeyUp,
        String(), String(), key, String(), String(), windowsVirtualKeyCode, false, false, false,
        keyModifiers(shift, ctrl, alt), WallTime::now());
    frame.eventHandler().keyEvent(evt);
    WebCoreBrowserKeepCompositing(6); // paint caret / editing result
}

static void charImpl(unsigned codepoint, int shift, int ctrl, int alt)
{
    using namespace WebCore;
    if (!g_page || !codepoint)
        return;
    Frame& frame = g_page->focusController().focusedOrMainFrame();
    String text;
    UChar32 c = static_cast<UChar32>(codepoint);
    if (U_IS_BMP(c)) {
        UChar u = static_cast<UChar>(c);
        text = String(&u, 1);
    } else {
        UChar u[2] = { U16_LEAD(c), U16_TRAIL(c) };
        text = String(u, 2);
    }
    PlatformKeyboardEvent evt(PlatformEvent::Char, text, text, text, String(), String(),
        0, false, false, false, keyModifiers(shift, ctrl, alt), WallTime::now());
    frame.eventHandler().keyEvent(evt);
}

static void logKeyCrash(const char* which)
{
    uintptr_t base = reinterpret_cast<uintptr_t>(&__ImageBase);
    uintptr_t rva = reinterpret_cast<uintptr_t>(g_crashAt) - base;
    char b[192];
    snprintf(b, sizeof b, "KEY-CRASH %s code=0x%08lx at=%p base=0x%08zx rva=0x%08zx fault=%p",
        which, g_crashCode, g_crashAt, (size_t)base, (size_t)rva, g_crashFault);
    WebCorePort::portLog(b);
}

extern "C" void WebCoreBrowserKey(int windowsVirtualKeyCode, int isDown, int shift, int ctrl, int alt)
{
    __try { keyDownUpImpl(windowsVirtualKeyCode, isDown, shift, ctrl, alt); }
    __except (rfCrashFilter(GetExceptionInformation())) { logKeyCrash("keydn"); }
}

extern "C" void WebCoreBrowserChar(unsigned codepoint, int shift, int ctrl, int alt)
{
    __try { charImpl(codepoint, shift, ctrl, alt); }
    __except (rfCrashFilter(GetExceptionInformation())) { logKeyCrash("char"); }
}

// ---- Navigation / chrome C ABI (driven by the Revenant XAML shell) --------------------
extern "C" void WebCoreBrowserNavigate(const char* url)
{
    using namespace WebCore;
    if (!g_page || !url || !*url)
        return;
    URL u { URL(), String::fromUTF8(url) };
    // Address-bar convenience: if it doesn't parse as an absolute URL, assume https://.
    if (!u.isValid() || u.protocol().isEmpty()) {
        std::string withScheme = std::string("https://") + url;
        u = URL { URL(), String::fromUTF8(withScheme.c_str()) };
    }
    if (!u.isValid())
        return;
    Frame& frame = g_page->mainFrame();
    ResourceRequest request { u };
    FrameLoadRequest flr { frame, request, SubstituteData() };
    frame.loader().load(WTFMove(flr));
    WebCoreBrowserKeepCompositing(60); // brief bootstrap; content invalidations drive compositing during load
    portLog((std::string("browser: navigate ") + u.string().utf8().data()).c_str());
}

extern "C" void WebCoreBrowserGoBack() { if (g_page) g_page->backForward().goBackOrForward(-1); }
extern "C" void WebCoreBrowserGoForward() { if (g_page) g_page->backForward().goBackOrForward(1); }
extern "C" int WebCoreBrowserCanGoBack() { return (g_page && g_page->backForward().canGoBackOrForward(-1)) ? 1 : 0; }
extern "C" int WebCoreBrowserCanGoForward() { return (g_page && g_page->backForward().canGoBackOrForward(1)) ? 1 : 0; }
extern "C" void WebCoreBrowserReload() { if (g_page) g_page->mainFrame().loader().reload(); }
extern "C" void WebCoreBrowserStop() { if (g_page) { g_page->mainFrame().loader().stopAllLoaders(); WebCoreBrowserKeepCompositing(8); } }
extern "C" int WebCoreBrowserIsLoading() { return (g_loaderClient && !g_loaderClient->loadComplete()) ? 1 : 0; }
extern "C" double WebCoreBrowserProgress() { return g_page ? g_page->progress().estimatedProgress() : 0.0; }

extern "C" const char* WebCoreBrowserCurrentURL()
{
    using namespace WebCore;
    static std::string s;
    s.clear();
    if (g_page) {
        if (RefPtr doc = g_page->mainFrame().document())
            s = doc->url().string().utf8().data();
    }
    return s.c_str();
}

// Reflow the page into a new render area (the SwapChainPanel resizes when our chrome bar is
// added/removed, the OS soft-key bar hides/shows, or the device rotates). This resizes the
// FrameView (CSS layout) + the composited content layer + the GL viewport. NOTE: the ANGLE
// swapchain itself is bound to the surface created at init; a growing area is handled here,
// but a full swapchain resize (new surface) is a follow-up if dynamic size changes look wrong.
extern "C" void WebCoreBrowserResize(int pxW, int pxH, double deviceScale)
{
    using namespace WebCore;
    if (!g_page || pxW <= 0 || pxH <= 0)
        return;
    g_forcedWinW = pxW;
    g_forcedWinH = pxH;
    g_winW = pxW;
    g_winH = pxH;
    if (deviceScale > 0)
        g_deviceScale = deviceScale;
    g_cssW = static_cast<int>(std::ceil(g_winW / g_deviceScale));
    g_cssH = static_cast<int>(std::ceil(g_winH / g_deviceScale));
    if (FrameView* view = g_page->mainFrame().view())
        view->resize(g_cssW, g_cssH);
    setPlatformScreenBounds(FloatRect(0, 0, g_cssW, g_cssH));
    PortChromeClient::setViewportSize(IntSize(g_cssW, g_cssH));
    WebCoreBrowserKeepCompositing(30); // relayout after resize needs a repaint
    portLog((std::string("browser: resize px=") + std::to_string(pxW) + "x" + std::to_string(pxH)
        + " css=" + std::to_string(g_cssW) + "x" + std::to_string(g_cssH)).c_str());
}

extern "C" int WebCoreRenderHtml(const char* utf8Html, int w, int h, uint8_t* outRGBA)
{
    if (!utf8Html || w <= 0 || h <= 0 || !outRGBA)
        return -1;
    ensureInit();

    auto pageConfiguration = pageConfigurationWithEmptyClients(PAL::SessionID::defaultSessionID());
    auto page = makeUnique<Page>(WTFMove(pageConfiguration));
    page->settings().setScriptEnabled(true);
    page->settings().setAcceleratedCompositingEnabled(false);
    page->settings().setShouldAllowUserInstalledFonts(false);

    Frame& frame = page->mainFrame();
    frame.setView(FrameView::create(frame));
    frame.init();

    FrameLoader& loader = frame.loader();
    if (!loader.activeDocumentLoader())
        return -3;
    auto& writer = loader.activeDocumentLoader()->writer();
    writer.setMIMEType("text/html");
    writer.begin(URL());
    auto buffer = SharedBuffer::create(utf8Html, strlen(utf8Html));
    writer.addData(buffer);
    writer.end();

    FrameView* view = frame.view();
    if (!view)
        return -4;
    RefPtr document = frame.document();
    if (!document)
        return -6;

    view->resize(w, h);
    view->setBaseBackgroundColor(Color::white);
    document->updateLayoutIgnorePendingStylesheets();

    return paintViewToRGBA(view, w, h, outRGBA);
}

extern "C" int WebCoreRenderUrl(const char* utf8Url, int w, int h, uint8_t* outRGBA)
{
    if (!utf8Url || w <= 0 || h <= 0 || !outRGBA)
        return -1;
    ensureInit();
    portLog("driver: WebCoreRenderUrl entry");

    auto pageConfiguration = pageConfigurationWithEmptyClients(PAL::SessionID::defaultSessionID());
    // Swap in our real FrameLoaderClient (policy -> Use, stops the run loop on
    // load completion). Keep a raw pointer to query load state afterwards.
    auto clientRef = makeUniqueRef<PortFrameLoaderClient>();
    PortFrameLoaderClient* client = clientRef.ptr();
    pageConfiguration.loaderClientForMainFrame = WTFMove(clientRef);

    // Real ChromeClient: captures the root GraphicsLayer when accelerated compositing
    // turns on. Declared before `page` so it outlives it.
    PortChromeClient portChrome;
    pageConfiguration.chromeClient = &portChrome;

    auto page = makeUnique<Page>(WTFMove(pageConfiguration));
    page->settings().setScriptEnabled(true);
    page->settings().setAcceleratedCompositingEnabled(true);
    page->settings().setShouldAllowUserInstalledFonts(false);

    Frame& frame = page->mainFrame();
    client->setFrame(&frame); // so the networking context is valid (non-null frame)
    frame.setView(FrameView::create(frame));
    frame.init();

    FrameView* view = frame.view();
    if (!view)
        return -4;
    view->resize(w, h);
    view->setBaseBackgroundColor(Color::white);

    URL url { URL(), String::fromUTF8(utf8Url) };
    if (!url.isValid()) {
        portLog("driver: URL invalid");
        return -10;
    }
    portLog("driver: page+frame ready, starting load");

    portLog((std::string("driver: isMainThread=") + (isMainThread() ? "1" : "0")
        + " current==main=" + ((&RunLoop::current() == &RunLoop::main()) ? "1" : "0")).c_str());

    ResourceRequest request { url };
    FrameLoadRequest frameLoadRequest { frame, request, SubstituteData() };
    frame.loader().load(WTFMove(frameLoadRequest));
    portLog("driver: FrameLoader::load() returned, entering run loop");

    // Heartbeat: proves whether the run loop is pumping scheduled work at all.
    // Reschedules itself each second (stops after the timeout window).
    static int s_beats = 0;
    s_beats = 0;
    static void (*beat)() = [] {
        portLog(("driver: heartbeat " + std::to_string(++s_beats)).c_str());
        if (s_beats < 30)
            RunLoop::current().dispatchAfter(Seconds(1), beat);
    };
    RunLoop::current().dispatchAfter(Seconds(1), beat);

    // Safety timeout: stop the run loop after 30s even if the load never resolves.
    RunLoop::current().dispatchAfter(Seconds(30), [] {
        portLog("driver: 30s timeout fired");
        RunLoop::main().stop();
    });

    // Pump until PortFrameLoaderClient stops us (didFinish / didFail) or timeout.
    RunLoop::current().run();
    portLog(client->loadComplete() ? "driver: run loop exited, loadComplete=1"
                                   : "driver: run loop exited, loadComplete=0 (timeout)");

    // Cancel any still-in-flight loaders BEFORE the page is destroyed (a live
    // ResourceLoader/CurlRequest torn down mid-flight crashes on teardown).
    frame.loader().stopAllLoaders();
    portLog("driver: stopAllLoaders done");

    bool complete = client->loadComplete();
    bool failed = client->loadFailed();
    if (!complete)
        return -11; // timed out
    if (failed)
        return -12;

    RefPtr document = frame.document();
    if (!document)
        return -6;
    document->updateLayoutIgnorePendingStylesheets();
    portLog("driver: layout done, painting");

    // Accelerated compositing: flush the layer tree; if a root GraphicsLayer was
    // captured, GPU-composite it via TextureMapperGL on the Adreno, else software paint.
    view->flushCompositingStateIncludingSubframes();
    int prc;
    if (portChrome.rootGraphicsLayer()) {
        portLog("comp: root GraphicsLayer CAPTURED -> GPU compositing");
        prc = compositeToRGBA(portChrome.rootGraphicsLayer(), view, w, h, outRGBA);
        if (prc != 0) {
            portLog("comp: GPU path failed, falling back to software paint");
            prc = paintViewToRGBA(view, w, h, outRGBA);
        }
    } else {
        portLog("comp: no root GraphicsLayer -> software paint");
        prc = paintViewToRGBA(view, w, h, outRGBA);
    }
    portLog("driver: paint done");
    return prc;
}
