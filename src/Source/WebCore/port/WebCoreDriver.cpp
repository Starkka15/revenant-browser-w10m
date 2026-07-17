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
#include "RenderObject.h"
#include "RenderLayer.h"
#include "RenderLayerScrollableArea.h"
#include "RenderView.h"
#include "IdleCallbackController.h"
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
#include "PlatformWheelEvent.h"   // WebCoreBrowserScrollBy feeds EventHandler::handleWheelEvent
#include "BackForwardController.h"
#include "MemoryRelease.h"        // WebCore::releaseMemory (memory-pressure release)
#include "CommonVM.h"
#include "MemoryCache.h"
#include <JavaScriptCore/Heap.h>
#include <JavaScriptCore/VM.h>
#include "BackForwardCache.h"
#include <wtf/FastMalloc.h>
#include <wtf/MemoryFootprint.h>
#include <wtf/MemoryPressureHandler.h>
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
#if ENABLE(CONTENT_EXTENSIONS)
#include "UserContentController.h"
#endif
#include "PlatformMouseEvent.h"
#include "PlatformTouchEvent.h" // real multitouch dispatch (WebCoreBrowserTouch)
#include <map>                  // active touch-point set keyed by pointer id
#include "EditorClient.h"      // complete type needed for config.editorClient (UniqueRef) assignment
#include "PortChromeClient.h"
#include "PortDeviceSensorClients.h"
#include "PortEditorClient.h"
#include <wtf/DataLog.h> // WTF::setDataFile — route DFG disassembly to dfgdis.txt
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
#include <JavaScriptCore/ExecutableAllocator.h> // exported JIT telemetry: revJITAllocCount/Bytes(), revExecAllocValid()
#include <JavaScriptCore/Options.h>
#include <cairo.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <atomic>
#include <thread>
#include <pal/SessionID.h>
#include <string>
#include <vector>
#include <wtf/FileSystem.h>
#include <wtf/MainThread.h>
#include <wtf/NumberOfCores.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/Seconds.h>
#include <wtf/URL.h>

namespace WebCorePort { void installPortPlatformStrategies(); }
#if ENABLE(CONTENT_EXTENSIONS)
namespace WebCore { class UserContentController; }
namespace WebCorePort { unsigned installPortContentBlocker(WebCore::UserContentController&); }
#endif

// Exported instrumentation accessors: bytecode-cache stats live in JavaScriptCore.dll (SourceProvider.cpp),
// URL-parse stats in WTF.dll (URLParser.cpp). Plain extern decls link against the DLL import libs (function
// imports resolve through the .lib without needing dllimport).
namespace JSC { void bytecodeCacheStats(uint64_t out[6]); }
namespace WTF { void urlParseStats(uint64_t out[3]); }

using namespace WebCore;

// --- on-device diagnostic log (written to a path the shell points at LocalState) ---
static std::wstring g_logPath;
static std::mutex g_logMutex;
// Backlog for log lines emitted BEFORE the shell hands us the log path (PortSetDebugLogPathW).
// JSC's JIT-reservation diagnostics (jitdiag:) run during JSC::initialize(), which the GL self-test
// triggers before the data path is set — those lines would otherwise be silently dropped. Buffer
// them (bounded) and flush on the first real write.
static std::vector<std::string> g_logBacklog;

extern "C" void PortSetDebugLogPathW(const wchar_t* widePath)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logPath = widePath ? widePath : L"";
    // Route JSC's dataLog (dumpDFGDisassembly output) to LocalState\dfgdis.txt, a sibling
    // of debug.log, so the emitted ARM can be pulled via WDP. Set once, as soon as the shell
    // hands us the writable path (before any page JS / DFG compile).
    if (!g_logPath.empty()) {
        std::wstring disW = g_logPath;
        size_t slash = disW.find_last_of(L"\\/");
        disW = (slash == std::wstring::npos) ? std::wstring(L"dfgdis.txt") : disW.substr(0, slash + 1) + L"dfgdis.txt";
        std::string disN(disW.begin(), disW.end()); // LocalState paths are ASCII
        WTF::setDataFile(disN.c_str());
    }
}

namespace WebCorePort {
void portLog(const char* msg)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logPath.empty()) {
        // Path not set yet (early init, e.g. JSC jitdiag during the GL self-test). Stash, bounded.
        if (g_logBacklog.size() < 2048)
            g_logBacklog.emplace_back(msg ? msg : "");
        return;
    }
    // Keep the log file OPEN across the session instead of open+append+close on every call. The
    // per-call open/close (directory lookup + handle create) is a major perf drain on the phone's
    // flash when logging is frequent. (g_logPath is set once via WebCoreBrowserSetDataPath.)
    static std::ofstream s_logFile(g_logPath, std::ios::app);
    if (!s_logFile)
        return;

    // Drain any lines captured before the path was known (JSC jitdiag et al.), in order.
    if (!g_logBacklog.empty()) {
        for (const auto& line : g_logBacklog)
            s_logFile << line << '\n';
        g_logBacklog.clear();
        s_logFile.flush();
    }

    // DEDUP consecutive identical lines. Diagnostics on the PAINT path (img: INCOMPLETE / FILL2 /
    // SVGdraw, TMPAINT, ...) emit the SAME line hundreds of times per frame on an image-heavy page.
    // Each one was a formatted write + a flush to flash — on pornhub.com the stall profiler caught
    // 40 of 60 main-thread samples inside system DLLs (the file I/O), i.e. the logging itself was a
    // large part of the CPU burn that got the app watchdog-killed. Collapsing repeats keeps every
    // distinct diagnostic while removing the storm.
    static std::string s_lastLine;
    static unsigned s_repeatCount = 0;
    const char* line = msg ? msg : "";
    if (s_lastLine == line) {
        ++s_repeatCount;
        return;
    }
    if (s_repeatCount) {
        s_logFile << "  (previous line repeated " << s_repeatCount << " times)\n";
        s_repeatCount = 0;
    }
    s_lastLine = line;
    s_logFile << line << '\n';

    // Flush on a time budget rather than every line, EXCEPT for lines that must survive a hard kill
    // (crash/stall/media/memory/identity markers) — those still flush immediately, so a watchdog kill
    // or OS OOM-kill never truncates the evidence that explains it.
    auto mustFlush = [line]() {
        static const char* kCritical[] = { "MAIN-STALL", "RENDER-CRASH", "KEY-CRASH", "UNHANDLED",
            "GLERR", "TEXALLOC", "TEXFBO", "TEXDEL", "SHADER", "GLCAP", "mem:", "mse:", "mf:", "hls:", "prog:", "media:",
            "fullscreen:", "identity:", "browser:", "gate:", "SLOWFRAME", "jitcfg", "watchdog",
            "app:", "mempool:", "tmtile:", "compositing:", "cmplayer:", "CRAWL", "  prof", "  st rva",
            "  dstk", "bcache:", "  athr" };
        for (const char* p : kCritical) {
            if (!std::strncmp(line, p, std::strlen(p)))
                return true;
        }
        return false;
    };
    static ULONGLONG s_lastFlushTick = 0;
    ULONGLONG now = GetTickCount64();
    if (mustFlush() || now - s_lastFlushTick >= 250) {
        s_logFile.flush();
        s_lastFlushTick = now;
    }
}
}
using WebCorePort::portLog;

// Lets the C++/CX shell write into the same diagnostic log (e.g. memory-usage samples from
// Windows::System::MemoryManager, which is only reachable from the CX side).
extern "C" void WebCoreBrowserLog(const char* msg) { if (msg) portLog(msg); }

// Defined in GraphicsLayerTextureMapper.cpp: feeds the compositor the visible content band so it can
// clip full-page repaints of the huge CSS-px content layer to what's on screen (perf).
namespace WebCore { void setTexmapVisibleRect(int x, int y, int w, int h); }

// Install the JSC diag sink at WebCore.dll LOAD time (before any code runs, well before anything
// touches JSC::initialize). JSC's JIT-reservation diagnostics (jitdiag: in ExecutableAllocator/VM)
// route through this function pointer — the ONE cross-DLL export that reliably links. Setting it in
// ensureInit() was too late if JSC initializes first; a static initializer removes the race, and the
// portLog backlog above captures the lines emitted before the log path is known. g_watchdogTerminationLog
// is an exported JSC function pointer, so assigning it at load time links cleanly.
namespace {
struct JitDiagSinkInstaller {
    JitDiagSinkInstaller() { JSC::g_watchdogTerminationLog = [](const char* m) { portLog(m); }; }
};
static JitDiagSinkInstaller s_jitDiagSinkInstaller;
}

static void startMainStallDetector(); // defined below (near the render pump / g_renderStage)
static void installFaultHandler();    // defined below (vectored handler, all threads)
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

// Size-cap the on-disk bytecode cache: if the dir exceeds capBytes, delete oldest files (by mtime)
// until back under. Bytecode blobs are compact, so a modest cap holds many sites; this just keeps
// LocalState from growing without bound across visits. Best-effort — failures are non-fatal.
static void pruneBytecodeCacheDir(const WTF::String& dir, uint64_t capBytes)
{
    struct Ent { WTF::String path; uint64_t size; WTF::WallTime mtime; };
    WTF::Vector<Ent> ents;
    uint64_t total = 0;
    for (auto& name : FileSystem::listDirectory(dir)) {
        auto full = FileSystem::pathByAppendingComponent(dir, name);
        auto sz = FileSystem::fileSize(full);
        if (!sz)
            continue;
        auto mt = FileSystem::fileModificationTime(full);
        ents.append({ full, *sz, mt.value_or(WTF::WallTime::fromRawSeconds(0)) });
        total += *sz;
    }
    if (total <= capBytes)
        return;
    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) { return a.mtime < b.mtime; }); // oldest first
    for (auto& e : ents) {
        if (total <= capBytes)
            break;
        if (FileSystem::deleteFile(e.path))
            total -= e.size;
    }
}

// ---- Per-device memory budget --------------------------------------------------------------
// W10M gives each device a different AppMemoryUsageLimit (1GB Lumia -> 390MB, 2GB -> 900MB), and on
// the 1GB device the SYSTEM commit limit binds ~50MB BELOW that cap (the OS reaps us at ~340MB while
// the app-level pct still reads "safe"). A single hardcoded config can't serve both: what keeps the
// 640XL alive starves nothing on the 1520, and what the 1520 can afford kills the 640XL. So the shell
// reads the real AppMemoryUsageLimit at startup and calls WebCoreSetMemoryBudgetFromLimit(); every
// memory lever (resource cache size, MSE keep-behind, the watcher's release thresholds) derives from
// the resulting tier. Defaults below are the safe LOW (1GB) tier until the shell sets the real value.
struct MemoryBudget {
    int tier;                       // 0=LOW(~1GB) 1=MID(~2GB) 2=HIGH(>=3GB)
    unsigned appLimitMB;            // raw AppMemoryUsageLimit
    unsigned effectiveCeilingMB;    // real kill line (commit-limited on 1GB, ~cap on bigger RAM)
    unsigned memCacheTotalMB;       // WebCore MemoryCache total capacity
    unsigned mseKeepBehindSec;      // seconds of already-played video the emergency trim keeps
};
static MemoryBudget g_memBudget = { 0, 390, 340, 12, 8 };

static void applyMemoryCacheFromBudget()
{
    using namespace WebCore;
    unsigned total = g_memBudget.memCacheTotalMB * 1024u * 1024u;
    MemoryCache::singleton().setCapacities(0, total / 3, total); // minDead, maxDead, total
    // On the LOW (1GB) tier, drop dead decoded image data every 1s instead of 3s: while scrolling a
    // thumbnail feed, images that leave the viewport become "dead" and must free FAST or they pile up
    // and tip the ~340MB ceiling mid-scroll (the 640XL scroll-while-video crash). Bigger devices can
    // afford to keep them longer (fewer re-decodes when scrolling back up).
    MemoryCache::singleton().setDeadDecodedDataDeletionInterval(Seconds { g_memBudget.tier == 0 ? 1.0 : 3.0 });
}

extern "C" void WebCoreSetMemoryBudgetFromLimit(unsigned long long appLimitBytes)
{
    unsigned limitMB = static_cast<unsigned>(appLimitBytes / (1024 * 1024));
    if (!limitMB)
        return;
    MemoryBudget b {};
    b.appLimitMB = limitMB;
    if (limitMB < 512) {            // ~1GB device: commit binds ~50MB below the app cap
        b.tier = 0;
        b.effectiveCeilingMB = limitMB > 60 ? limitMB - 50 : limitMB;
        b.memCacheTotalMB = 12;
        b.mseKeepBehindSec = 8;
    } else if (limitMB < 1400) {    // ~2GB device: cap itself is the ceiling (commit not binding)
        b.tier = 1;
        b.effectiveCeilingMB = limitMB - limitMB / 20;
        b.memCacheTotalMB = 48;
        b.mseKeepBehindSec = 20;
    } else {                        // 3GB+
        b.tier = 2;
        b.effectiveCeilingMB = limitMB - limitMB / 20;
        b.memCacheTotalMB = 96;
        b.mseKeepBehindSec = 30;
    }
    g_memBudget = b;
    applyMemoryCacheFromBudget();
    char lg[220];
    snprintf(lg, sizeof lg,
        "membudget: tier=%d appLimit=%uMB effectiveCeiling=%uMB memCache=%uMB mseKeepBehind=%us",
        b.tier, b.appLimitMB, b.effectiveCeilingMB, b.memCacheTotalMB, b.mseKeepBehindSec);
    portLog(lg);
}

extern "C" unsigned WebCoreMemBudgetEffectiveCeilingMB() { return g_memBudget.effectiveCeilingMB; }
extern "C" int WebCoreMemBudgetTier() { return g_memBudget.tier; }
extern "C" unsigned WebCoreMemBudgetMseKeepBehindSec() { return g_memBudget.mseKeepBehindSec; }

static void ensureInit()
{
    static bool inited = false;
    if (inited)
        return;
    inited = true;
    // Set the cross-DLL log sink BEFORE JSC::initialize() so JSC's JIT-reservation diagnostics
    // (jitdiag: lines in ExecutableAllocator/VM) — which run *inside* initialize() via
    // computeCanUseJIT->enableAssembler — actually reach debug.log. Assigning this raw exported
    // pointer needs no prior init.
    JSC::g_watchdogTerminationLog = [](const char* m) { portLog(m); };
    // JIT pool 32MB -> 16MB. The UWP W^X path commits the WHOLE reservation RWX up front, and PC-side
    // systemperf polling (2026-07-15) showed the OS reaping us at ~340MB app commit — the 1GB device's
    // SYSTEM commit limit binds ~50MB below our 390MB cap, so standing commit is the scarcest resource
    // we have. Measured JIT usage on heavy pages is well under 16MB; if exec memory ever runs out, JSC
    // degrades to LLInt for the overflow (jitdiag logs it), not a crash. Must be set BEFORE
    // JSC::initialize(): the reservation happens inside it (computeCanUseJIT -> enableAssembler), long
    // before setOption() is usable — hence the env var, which Options::initialize reads via the
    // GetEnvironmentVariableA shim. Numeric option, so the thread_local-getenv-buffer trap
    // (OptionString keeping a raw pointer) does not apply.
    SetEnvironmentVariableA("JSC_jitMemoryReservationSize", "16777216");
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
    // MINI-VM MODE: temporarily REVERTED. Enabled alongside the content blocker in one build; that build
    // died silently (no AV, no dump) during YouTube video load at only 55% memory — a new failure mode.
    // Pulling mini mode to isolate: get a clean content-blocker-only run first, then re-A/B mini mode
    // alone if we still want the jsHeap win. (Option is valid: OptionsList.h forceMiniVMMode.)
    // JSC::Options::setOption("forceMiniVMMode=true");
    // DFG TIER (re-enabled 2026-07-12 to attack the Turnstile CPU wall): the DFG speculatively
    // optimizes hot functions ~3-10x over baseline. With DFG OFF, heavy Turnstile PoW JS runs baseline
    // forever (inModule=0 CPU burn, 90+ /new/ cycles) — the binding constraint on rule34. Baseline JIT
    // + LLInt + JIT W^X are all proven on-device now, so re-test DFG as the speedup lever.
    // HISTORY / RISK: DFG was originally disabled because JS-heavy pages (Google search) infinite-looped
    // in DFG'd code — a suspected miscompile on this custom MSVC-ARMv7 codegen. If that hang returns,
    // the real fix is the DFG ARM32 codegen bug, not re-disabling. Watchdog=60s + the jitcfg/MAIN-STALL
    // diagnostics stay in to catch it. (Toggle back to false to isolate if a regression appears.)
    JSC::Options::setOption("useDFGJIT=true");
    // CONCURRENCY / MULTICORE: use every core the device actually reports. JSC's stock defaults CAP the
    // worklist/DFG pool at computeNumberOfWorkerThreads(3,2) = min(cores,3) — so even a 4-core Snapdragon
    // 400 gets only 2 DFG compiler threads and never scales past 3. Page-load profiling (2026-07-14, PC
    // build) attributed the 6-12s load burst as: 69% inside JavaScriptCore.dll (parser / bytecode-gen /
    // runtime property access, only ~2% LLInt), 25% RUNNING JIT-compiled code at the 0x19600000 pool, 6%
    // WebCore CSS/layout. Not interpreter-bound — so tiering hot JS up sooner and OFF the main thread is a
    // real lever on the 25%. Detect cores (GetSystemInfo via WTF::numberOfProcessorCores) and raise the
    // caps to use them all: every core compiles concurrently while the main thread parses. useConcurrentJIT
    // stays on so DFG compilation never blocks the executing JS thread. Must be set here — after
    // JSC::initialize(), before the first VM/DFG-Worklist is created (which snapshots these). FTL is
    // compiled OUT on this port (ENABLE_FTL_JIT OFF) so numberOfFTLCompilerThreads is a no-op; DFG is the
    // only tier that threads today. The jitthreads: log line reports the RESOLVED count so we can see what
    // the AppContainer actually grants on-device (it may throttle below the physical core count).
    {
        int cores = WTF::numberOfProcessorCores();
        if (cores < 1) cores = 1;
        unsigned worklist = static_cast<unsigned>(cores);                        // all cores may compile
        unsigned dfgThreads = static_cast<unsigned>(cores > 1 ? cores - 1 : 1);  // leave 1 core for main-thread JS
        JSC::Options::useConcurrentJIT() = true;
        JSC::Options::numberOfWorklistThreads() = worklist;
        JSC::Options::numberOfDFGCompilerThreads() = dfgThreads;
        char cb[200];
        snprintf(cb, sizeof cb,
            "jitthreads: cores=%d -> worklistThreads=%u dfgCompilerThreads=%u concurrentJIT=1 ftlCompiled=0",
            cores, worklist, dfgThreads);
        portLog(cb);
    }
    // DFG DISASSEMBLY CAPTURE (debug, OFF by default): flip to true + load a minimal page to dump
    // the DFG's ARM as RAWCODE hex into LocalState\dfgdis.txt (routed in PortSetDebugLogPathW;
    // Disassembler.cpp emits raw bytes since Capstone is off). Used to root-cause the ARM32
    // hard-float ABI bug (2026-07-12). Left off — it bloats the log and slows JS.
    // JSC::Options::setOption("dumpDFGDisassembly=true");
    // NOTE: do NOT set jitMemoryReservationSize here. A large reservation (we tried 128MB) FAILS on
    // ARM32 UWP's constrained AppContainer address space, and when the JIT pool reservation fails JSC
    // auto-disables the JIT (Options::useJIT()=false) and falls back to the LLInt interpreter for ALL
    // JS — which is exactly what jitcfg caught (useJIT=0). The default reservation (32MB, the size the
    // working on-device JSC self-test uses) is what keeps the JIT enabled. Leave it at default.
    // (g_watchdogTerminationLog is set at the top of ensureInit, before JSC::initialize.)
    // DISK BYTECODE CACHE: point JSC's on-disk unlinked-bytecode cache at a LocalState subdir. The base
    // SourceProvider (patched) serializes each parsed script's UnlinkedCodeBlock tree here and, on the
    // next visit (or after an in-memory CodeCache wipe), decodes it instead of re-tokenizing — the
    // durable half of the re-parse fix. Must be set AFTER JSC::initialize() (Options ready) and BEFORE
    // the first compile. LocalState path arrives via PortSetDebugLogPathW (g_logPath); derive a sibling
    // "bytecode-cache" dir from it. If the shell hasn't handed us the path yet, the cache stays off.
    {
        std::wstring logDir;
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            logDir = g_logPath;
        }
        if (!logDir.empty()) {
            size_t slash = logDir.find_last_of(L"\\/");
            std::wstring cacheW = (slash == std::wstring::npos ? std::wstring() : logDir.substr(0, slash + 1)) + L"bytecode-cache";
            static std::string s_bytecodeCacheDir(cacheW.begin(), cacheW.end()); // LocalState paths are ASCII
            WTF::String cacheDirStr = WTF::String::fromUTF8(s_bytecodeCacheDir.c_str());
            FileSystem::makeAllDirectories(cacheDirStr);
            pruneBytecodeCacheDir(cacheDirStr, 48ull * 1024 * 1024); // 48MB cap, LRU by mtime
            JSC::Options::diskCachePath() = s_bytecodeCacheDir.c_str(); // stable pointer (static)
            char cb[300];
            snprintf(cb, sizeof cb, "bytecodecache: enabled dir=%s cap=48MB minSrc=2048B", s_bytecodeCacheDir.c_str());
            portLog(cb);
        } else
            portLog("bytecodecache: no LocalState path yet -> disk cache DISABLED this run");
    }

    WebCorePort::installPortPlatformStrategies();
    WebCore::ResourceHandle::registerBuiltinConstructor("blob"_s, createBlobResourceHandle);
#if ENABLE(SERVICE_WORKER)
    WebCorePort::installPortServiceWorkerProvider();
#endif

    // MEMORY: this AppContainer has a hard ~390MB cap; a heavy page (YouTube search/watch) balloons the
    // WebCore caches + decoded thumbnail images + JSC heap toward it, and W10M's memory manager then
    // thrashes the main thread (60s+ ntdll stalls = the "hangs"). Wire WebKit's OWN memory-pressure
    // machinery (not a hand-roll): the standard low-memory handler releases the MemoryCache, back/forward
    // cache, decoded image data, font/glyph caches, and GCs JSC. Driven from the shell's AppMemoryUsage
    // signal via WebCoreBrowserReleaseMemory(). (Same pattern as WebKitLegacy/win/WebView.cpp.)
    {
        auto& mph = MemoryPressureHandler::singleton();
        mph.setLowMemoryHandler([](Critical critical, Synchronous synchronous) {
            WebCore::releaseMemory(critical, synchronous);
        });
        mph.setShouldLogMemoryMemoryPressureEvents(true);
        mph.install();
    }
    // Keep retained-resource memory small on constrained devices: cap the resource cache (size scales
    // with the detected memory tier — see g_memBudget; the shell sets the real tier once it reads the
    // AppMemoryUsageLimit, this applies the safe LOW default until then) and disable the back/forward
    // page cache (retaining whole prior pages is unaffordable at 390MB). Dead decoded image data is
    // dropped promptly so offscreen thumbnails don't pin memory.
    applyMemoryCacheFromBudget();
    BackForwardCache::singleton().setMaxSize(0);
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
double g_deviceScale = 1.0;   // device pixels per CSS pixel (mobile DPR) — GEOMETRIC scale: drives
                              // cssW/cssH, the root composite transform (fills the physical panel), and
                              // input/clip px<->CSS conversion. Stays at the true device DPR.
double g_rasterScale = 1.0;   // RASTER/DPR scale = min(g_deviceScale, cap). On extreme-DPR panels
                              // (Lumia 950 @ 3.5, 1440p) every backing store is a 3.5x D3D texture and
                              // YouTube's watch page exhausts the GPU texture budget -> E_OUTOFMEMORY on
                              // Texture2D -> present throws on the XAML commit thread -> process killed
                              // (I-01). Tiles raster (and window.devicePixelRatio reports) at THIS capped
                              // value; the composite transform still uses g_deviceScale, so the GPU
                              // upscales the smaller tiles to fill the panel. Verified safe: contentsScale
                              // (=deviceScaleFactor) only sets texture resolution — on-screen size comes
                              // from the root transform via rectToRect normalization (contentsScale!=1
                              // already renders correctly). cssW + iOS-impersonation layout unchanged.
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
    // The shell passes a configured IPropertySet (panel + EGLRenderResolutionScaleProperty) as the
    // native window, so ANGLE sizes its backbuffer to panelSize x scale and applies the compensating
    // matrix transform itself — we must NOT also force EGL_FIXED_SIZE (that path skips the transform →
    // zoomed). The scale property (vs a fixed size) is what lets ANGLE resize the backbuffer on
    // rotation. Clear the fixed-size hint. defaultFrameBufferSize() is unreliable for this surface
    // (returned 0 → 480x800 fallback → black), so force the engine window size from the physical px
    // the shell computed (pxW/pxH == panelDIP x scale, so it matches ANGLE's backbuffer).
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
    // DEFINITIVE JIT-engagement check: log the browser VM's actual JIT config. If useJIT=0 the whole
    // runtime is LLInt-only (the self-test proving JIT works runs in a SEPARATE jsc harness, so it
    // does NOT establish that the browser VM enabled the JIT). Pair with the gate line's jitAllocs
    // counter (rising => baseline/DFG actually emitting code; flat => interpreter).
    {
        char jb[280];
        snprintf(jb, sizeof jb, "jitcfg: useJIT=%d baselineJIT=%d dfgJIT=%d llint=%d regexpJIT=%d "
            "concurrentJIT=%d worklistThreads=%u dfgThreads=%u cores=%d",
            JSC::Options::useJIT() ? 1 : 0, JSC::Options::useBaselineJIT() ? 1 : 0,
            JSC::Options::useDFGJIT() ? 1 : 0, JSC::Options::useLLInt() ? 1 : 0,
            JSC::Options::useRegExpJIT() ? 1 : 0,
            JSC::Options::useConcurrentJIT() ? 1 : 0, JSC::Options::numberOfWorklistThreads(),
            JSC::Options::numberOfDFGCompilerThreads(), WTF::numberOfProcessorCores());
        portLog(jb);
    }
    startMainStallDetector(); // logs where the main thread is stuck if a native (non-JS) freeze hits
    installFaultHandler();    // catch hardware faults on ANY thread, not just the render frame

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
    // Cap the RASTER scale on extreme-DPR panels so backing textures don't exhaust GPU memory (I-01).
    // 2.5x is still crisp on a 500+ppi screen; the composite transform (g_deviceScale) upscales to fill.
    g_rasterScale = g_deviceScale > 3.0 ? 2.5 : g_deviceScale;
    if (g_rasterScale != g_deviceScale)
        portLog((std::string("browser: raster scale capped ") + std::to_string(g_deviceScale)
            + " -> " + std::to_string(g_rasterScale) + " (GPU texture budget)").c_str());
    portLog((std::string("browser: window ") + std::to_string(g_winW) + "x" + std::to_string(g_winH)
        + " renderer=" + reinterpret_cast<const char*>(glGetString(GL_RENDERER))).c_str());
    // The shader budget of this GPU. D3D feature level 9_3 (Adreno 305) caps the FRAGMENT uniform
    // vectors hard -- and TextureMapper's rounded-rect-clip fragment shader declares 70 of them
    // (vec4[3*MAX_RECTS] + mat4[MAX_RECTS], MAX_RECTS=10 upstream). If the cap is below that the
    // program silently fails to link (WebKit never checks GL_LINK_STATUS), and every draw that uses
    // it then errors -- which is what wedged pornhub.com. Log the real numbers so the shader budget
    // is a measured fact, not an assumption.
    {
        GLint fragU = 0, vertU = 0, vary = 0, texUnits = 0, vertAttribs = 0;
        glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &fragU);
        glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &vertU);
        glGetIntegerv(GL_MAX_VARYING_VECTORS, &vary);
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &texUnits);
        glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &vertAttribs);
        char b[224];
        snprintf(b, sizeof b, "GLCAP fragUniformVecs=%d vertUniformVecs=%d varyingVecs=%d texUnits=%d vertAttribs=%d ctx=%p",
            fragU, vertU, vary, texUnits, vertAttribs, static_cast<void*>(GLContext::current()));
        portLog(b);
    }
    g_textureMapper = TextureMapperGL::create();

    auto config = pageConfigurationWithEmptyClients(PAL::SessionID::defaultSessionID());
    // Content blocker: replace the EmptyUserContentProvider (blocks nothing) with a real
    // UserContentController carrying our ad/tracker blocklist. ResourceLoader::willSendRequest runs the
    // ContentExtensions DFA pre-fetch, so blocked scripts never download/parse/JIT — the biggest memory
    // + CPU + network lever we have on this commit-starved device.
#if ENABLE(CONTENT_EXTENSIONS)
    {
        auto userContent = WebCore::UserContentController::create();
        WebCorePort::installPortContentBlocker(userContent.get());
        config.userContentProvider = WTFMove(userContent);
    }
#endif
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
#if ENABLE(DEVICE_ORIENTATION)
    // Real DeviceOrientation + DeviceMotion backed by the phone's Windows.Devices.Sensors
    // (Inclinometer / Accelerometer / Gyrometer) — see port/PortDeviceSensorClients.cpp.
    WebCorePort::provideDeviceSensorsTo(*g_page);
#endif
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
    g_page->setDeviceScaleFactor(g_rasterScale); // capped on extreme-DPR (I-01); geometry uses g_deviceScale
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

    // MEDIA POLICY: match the platform we present as. We impersonate iOS Safari (UA + TLS/JA3), so
    // sites hand us the MOBILE page -- a thumbnail grid where every tile is a real autoplaying
    // <video> preview. But these five settings are the DESKTOP defaults on this port (they only
    // default to the conservative values under PLATFORM(IOS_FAMILY), which we are not). So we took
    // iOS content and ran desktop media policy underneath it: every preview tile was allowed to load
    // AND autoplay, and each one spun up a full MediaFoundation decode pipeline. Measured on
    // pornhub's home page: 4 frame-server players alive, ~87MB, straight into the OS memory kill --
    // on a page where the user never pressed play.
    //
    // RequiresUserGestureToLoadVideo is the important one: without a gesture the media data is never
    // even fetched, so no MF player is constructed at all. The poster/thumbnail still renders, and a
    // tap (a user gesture) still plays the video normally.
    g_page->settings().setRequiresUserGestureToLoadVideo(true);      // iOS: true.  no tap -> no load
    g_page->settings().setMediaDataLoadsAutomatically(false);        // iOS: false. clamps preload to metadata
    g_page->settings().setVideoPlaybackRequiresUserGesture(true);    // iOS: true.  no autoplay
    g_page->settings().setAudioPlaybackRequiresUserGesture(true);    // iOS: true
    g_page->settings().setInvisibleAutoplayNotPermitted(true);       // offscreen video can't autoplay
    g_page->settings().setWebGLEnabled(true);              // WebGL1 (WebGL2 already on); ANGLE backs it
    // WebGL renderer/vendor strings: mask ON. Revenant now presents as iOS 15.4 Safari (see
    // PortFrameLoaderClient::userAgent + NavigatorBase::platform="iPhone"), and WebKit's mask returns
    // exactly what shipping iOS Safari returns — "Apple GPU" / "Apple Inc." (WebGLRenderingContextBase.cpp
    // UNMASKED_RENDERER/VENDOR_WEBGL). This is NOT a fabricated value: it is WebKit's own standard masking,
    // the same behavior a real iPhone exposes. The alternative — leaking the true ANGLE "Qualcomm Adreno
    // 305 Direct3D11" string under an iPhone UA — is an instant, glaring contradiction (no iOS device uses
    // ANGLE/D3D11/Adreno) that CF Turnstile bot-scoring flags immediately. Coherence with the chosen
    // identity requires the mask ON. (Was OFF for the abandoned Windows-Phone "honest hardware" approach.)
    g_page->settings().setMaskWebGLStringsEnabled(true);
    // HTML form controls that render native pickers.
    g_page->settings().setInputTypeDateEnabled(true);
    g_page->settings().setInputTypeColorEnabled(true);
    g_page->settings().setInputTypeTimeEnabled(true);
    g_page->settings().setInputTypeMonthEnabled(true);
    g_page->settings().setInputTypeWeekEnabled(true);
    g_page->settings().setInputTypeDateTimeLocalEnabled(true);
    g_page->settings().setInertAttributeEnabled(true);     // the `inert` attribute (modals/dialogs)
    // Web Audio (AudioContext/OfflineAudioContext). ENABLE(WEB_AUDIO) is on; the
    // real platform backends live in platform/audio/win (WASAPI output, self-
    // contained FFT, Media Foundation decodeAudioData, HRTF resource loader).
    g_page->settings().setWebAudioEnabled(true);
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

    // Wire the content client to the main frame so the per-frame render loop can read the visible
    // content rect and arm the TextureMapper viewport clip (setTexmapVisibleRect). Without this the
    // clip gate (`if (g_contentClient.frame)`) is always false, so s_texmapClipEnabled stays off and
    // EVERY dirty layer re-rasters the FULL page height. Latent on short pages; on a tall heavy page
    // (a 4722px rule34 video post) it means a ~1.9M-px cairo raster in one frame -> ~3s main-thread
    // stall -> the W10M process-lifetime watchdog kills the app. Arming the clip caps raster to the
    // visible band + half-screen margins.
    g_contentClient.frame = &frame;

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

// Shell-side stage breadcrumbs. The f:present-done watchdog kills showed the UI thread frozen with
// stage=f:present-done — i.e. AFTER WebCoreBrowserRenderFrame returned, somewhere in the shell's
// onRendering tail or XAML's own frame commit. These markers (passed as STRING LITERALS only; the
// pointer is stored, not copied) let the stall detector name that phase.
extern "C" void WebCoreBrowserStage(const char* s) { if (s) g_renderStage = s; }

// The __try around the render frame only covers the RENDER THREAD. A hardware fault on any other
// thread — a curl worker, a WinRT threadpool task, a MediaFoundation decode thread — takes the process
// down with NOTHING in the log: no RENDER-CRASH, memory nowhere near the cap, the log simply stops
// mid-line. That is exactly how the app has been dying. A vectored handler sees exceptions on EVERY
// thread, before any frame-based handler, so the fault cannot hide. It only REPORTS: it returns
// CONTINUE_SEARCH so normal handling is unchanged.
extern "C" char __ImageBase; // linker-provided; its address IS this module's load base

// The thread WebCoreBrowserRenderFrame runs on (i.e. the XAML UI thread). Recorded so a FAULT line
// can say whether the faulting thread is the one we gave a 16MB stack to, or some other thread that
// is still on the default 1MB.
static unsigned long g_renderThreadId = 0;

// Stack extent of an ARBITRARY address on some thread's stack: VirtualQuery gives the allocation
// base (low end of the reserved stack) and we can walk up to find the total reserve. This is the
// number that distinguishes "a worker thread with a 1MB stack legitimately ran out" from "something
// recursed away 16MB", which need opposite fixes.
static void stackExtentFor(void* sp, unsigned* reserveKB, unsigned* headroomKB)
{
    *reserveKB = 0;
    *headroomKB = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(sp, &mbi, sizeof mbi))
        return;
    auto allocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    auto spv = reinterpret_cast<uintptr_t>(sp);
    if (spv > allocBase)
        *headroomKB = static_cast<unsigned>((spv - allocBase) / 1024);
    // Walk the regions of this allocation to get the full reserved size.
    uintptr_t total = 0;
    for (uintptr_t addr = allocBase;;) {
        MEMORY_BASIC_INFORMATION r;
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &r, sizeof r))
            break;
        if (reinterpret_cast<uintptr_t>(r.AllocationBase) != allocBase)
            break;
        total += r.RegionSize;
        addr += r.RegionSize;
    }
    *reserveKB = static_cast<unsigned>(total / 1024);
}

static LONG CALLBACK portVectoredFaultHandler(EXCEPTION_POINTERS* info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // Do NOT filter to hardware faults. The app is dying with no FAULT line at all, which means it is
    // not faulting -- it is being TERMINATED. A UWP process dies that way from a fail-fast
    // (0xC0000409, which is how an unhandled C++/CX or WinRT exception ends up killing us) or an
    // unhandled C++ throw (0xE06D7363) on a thread with no handler. Filtering those out made the death
    // invisible. Log every exception, deduped per code and capped so a chatty C++ throw cannot flood.
    switch (code) {
    case 0x406D1388: // thread-name notification for the debugger
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        return EXCEPTION_CONTINUE_SEARCH;
    default:
        break;
    }
    {
        static std::atomic<unsigned> s_logged { 0 };
        static std::atomic<DWORD> s_lastCode { 0 };
        static std::atomic<unsigned> s_sameCode { 0 };
        if (s_lastCode.exchange(code) == code) {
            if (s_sameCode.fetch_add(1) > 6)
                return EXCEPTION_CONTINUE_SEARCH; // same code over and over: already reported
        } else
            s_sameCode.store(0);
        if (s_logged.fetch_add(1) > 200)
            return EXCEPTION_CONTINUE_SEARCH;
    }

    void* addr = info->ExceptionRecord->ExceptionAddress;
    auto base = reinterpret_cast<uintptr_t>(&__ImageBase); // linker-provided module load base
    auto pc = reinterpret_cast<uintptr_t>(addr);
    // ExceptionInformation[0] = 0 read / 1 write / 8 DEP; [1] = the address it touched.
    unsigned long op = 0;
    void* target = nullptr;
    if (info->ExceptionRecord->NumberParameters >= 2) {
        op = static_cast<unsigned long>(info->ExceptionRecord->ExceptionInformation[0]);
        target = reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]);
    }
    // Which thread, and how much stack did it actually have? A stack overflow (0xC00000FD) means
    // nothing until we know whether the thread had 1MB or the 16MB we gave the render thread.
    unsigned long tid = GetCurrentThreadId();
    char probe;
    unsigned reserveKB = 0, headroomKB = 0;
    stackExtentFor(&probe, &reserveKB, &headroomKB);

    char b[400];
    snprintf(b, sizeof b, "FAULT code=0x%08lx tid=%lu%s pc=%p rva=0x%08zx op=%lu addr=%p stage=%s inExe=%d | stack reserve=%uKB headroom=%uKB",
        static_cast<unsigned long>(code), tid,
        tid == g_renderThreadId ? "(RENDER)" : "(worker)", addr,
        static_cast<size_t>(pc - base), op, target,
        g_renderStage ? g_renderStage : "-", (pc >= base && pc - base < 0x4000000) ? 1 : 0,
        reserveKB, headroomKB);
    WebCorePort::portLog(b);
    return EXCEPTION_CONTINUE_SEARCH;
}

// AddVectoredExceptionHandler is outside the UWP app partition in the SDK headers, so it is not
// declared for this build — but the OS exports it and an AppContainer may call it. Declare it.
extern "C" __declspec(dllimport) void* __stdcall AddVectoredExceptionHandler(
    unsigned long first, LONG(CALLBACK* handler)(EXCEPTION_POINTERS*));

// Remaining stack on THIS thread, in KB, before the guard page. VirtualQuery on a stack address
// gives the allocation base = the low end of the thread's reserved stack; the distance from the
// current SP down to it is the headroom. Cheap (one syscall) and gives us the number that decides
// whether "the process vanished with no fault" was a stack overflow or not.
static unsigned stackHeadroomKB()
{
    MEMORY_BASIC_INFORMATION mbi;
    char probe;
    if (!VirtualQuery(&probe, &mbi, sizeof mbi))
        return 0;
    auto base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    auto sp = reinterpret_cast<uintptr_t>(&probe);
    return sp > base ? static_cast<unsigned>((sp - base) / 1024) : 0;
}

// SetThreadStackGuarantee reserves a slice of the stack that only the exception path may use, so a
// stack overflow can still DELIVER an exception instead of killing the process outright. Without it
// the guard-page hit has no room left to run portVectoredFaultHandler, which is precisely why every
// "the app just closes" log ends with no FAULT line at all. With it, an overflow reports
// FAULT code=0xC00000FD and we stop guessing. Not in the UWP app partition; declare it (same as
// AddVectoredExceptionHandler above).
extern "C" __declspec(dllimport) int __stdcall SetThreadStackGuarantee(unsigned long* stackSizeInBytes);

static void installFaultHandler()
{
    if (AddVectoredExceptionHandler(1 /* call first */, portVectoredFaultHandler))
        WebCorePort::portLog("watchdog: vectored fault handler armed (all threads)");
    else
        WebCorePort::portLog("watchdog: vectored fault handler FAILED to install");

    unsigned long guarantee = 64 * 1024;
    int ok = SetThreadStackGuarantee(&guarantee);
    g_renderThreadId = GetCurrentThreadId();
    char probe;
    unsigned reserveKB = 0, headroomKB = 0;
    stackExtentFor(&probe, &reserveKB, &headroomKB);
    char b[192];
    snprintf(b, sizeof b, "watchdog: stack guarantee %s (64KB) | render tid=%lu reserve=%uKB headroom=%uKB",
        ok ? "armed" : "FAILED", g_renderThreadId, reserveKB, headroomKB);
    WebCorePort::portLog(b);
}

// Main-thread heartbeat + native stall detector. A hard freeze with the JS watchdog silent means a
// NATIVE hang/loop on the main thread (JS watchdog only sees JS CPU time). This bumps every render
// tick; a background thread logs the last render stage when the heartbeat stops advancing — telling
// us WHERE the main thread is stuck (e.g. "a1:ctx" = inside RunLoop::iterate = a timer/microtask/curl
// storm; "u:layout" = a layout loop; etc.) without needing SuspendThread (desktop-only under UWP).
static std::atomic<unsigned long long> g_mainHeartbeat { 0 };
static HANDLE g_mainThreadHandle = nullptr;
static DWORD g_mainThreadId = 0;

// At an IDLE deadlock the main thread is parked in a kernel wait -- to name WHAT it waits on we must
// see the OTHER threads (the one holding the lock/mutex). CreateToolhelp32Snapshot isn't in the UWP
// app container, so enumerate with ntdll's NtGetNextThread (present on Win10 OneCore). For each thread
// (except the detector itself) suspend, read PC/LR + a few executable return addresses off the stack
// top, RESUME, then log (portLog takes a mutex a suspended thread might hold -> log only after resume).
// Offline: resolve pc/lr/st against the module at each base (JSC 0x54970000, WTF 0x77c40000, exe base,
// JIT pool). Best-effort: if the export is missing we still have the main-thread deep stack.
typedef LONG (NTAPI *PFN_NtGetNextThread)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);

// Get ntdll's base WITHOUT GetModuleHandle*: both GetModuleHandleW and GetModuleHandleExW are guarded
// into WINAPI_PARTITION_DESKTOP in this SDK, so neither compiles for the UWP app. GetProcAddress IS
// APP-available but needs a base. Walk the PEB loader list via NtCurrentTeb() (an intrinsic, present in
// all partitions). 32-bit/ARM32 field offsets: TEB->PEB @0x30, PEB->Ldr @0x0C, LDR->InMemoryOrderList
// @0x14; each list node is &entry.InMemoryOrderLinks (@0x08 within the entry). The loader memory is
// valid to read; we only touch documented, stable fields.
static void* ntdllBaseViaPeb()
{
    // UNICODE_STRING isn't declared in this TU (winnt's ntdef bits aren't pulled in for the UWP build);
    // inline the identical 8-byte (32-bit) layout so the field offsets still line up.
    struct MY_USTR { USHORT Length; USHORT MaximumLength; wchar_t* Buffer; };
    struct MY_LDR_ENTRY {
        LIST_ENTRY InLoadOrderLinks;            // 0x00
        LIST_ENTRY InMemoryOrderLinks;          // 0x08
        LIST_ENTRY InInitializationOrderLinks;  // 0x10
        void* DllBase;                          // 0x18
        void* EntryPoint;                       // 0x1C
        ULONG SizeOfImage;                      // 0x20
        MY_USTR FullDllName;                    // 0x24
        MY_USTR BaseDllName;                    // 0x2C
    };
    unsigned char* teb = reinterpret_cast<unsigned char*>(NtCurrentTeb());
    if (!teb) return nullptr;
    void* peb = *reinterpret_cast<void**>(teb + 0x30);
    if (!peb) return nullptr;
    void* ldr = *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(peb) + 0x0C);
    if (!ldr) return nullptr;
    LIST_ENTRY* head = reinterpret_cast<LIST_ENTRY*>(reinterpret_cast<unsigned char*>(ldr) + 0x14);
    for (LIST_ENTRY* node = head->Flink; node && node != head; node = node->Flink) {
        MY_LDR_ENTRY* e = reinterpret_cast<MY_LDR_ENTRY*>(reinterpret_cast<unsigned char*>(node) - 0x08);
        const wchar_t* nm = e->BaseDllName.Buffer;
        unsigned len = e->BaseDllName.Length / 2; // bytes -> wchars
        if (!nm || len < 9) continue;             // "ntdll.dll" == 9 chars
        // case-insensitive suffix match on "ntdll.dll" (some entries carry a path)
        const wchar_t* want = L"ntdll.dll";
        const wchar_t* tail = nm + (len - 9);
        bool eq = true;
        for (int i = 0; i < 9; ++i) {
            wchar_t a = tail[i]; if (a >= L'A' && a <= L'Z') a = wchar_t(a - L'A' + L'a');
            if (a != want[i]) { eq = false; break; }
        }
        if (eq) return e->DllBase;
    }
    return nullptr;
}

static void dumpAllThreadsAtStall()
{
    void* nt = ntdllBaseViaPeb();
    PFN_NtGetNextThread getNext = nt ? reinterpret_cast<PFN_NtGetNextThread>(GetProcAddress(reinterpret_cast<HMODULE>(nt), "NtGetNextThread")) : nullptr;
    if (!getNext) { portLog("  athr: NtGetNextThread unavailable (no thread enumeration)"); return; }
    const ACCESS_MASK acc = THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION;
    HANDLE prev = nullptr, cur = nullptr;
    int n = 0;
    while (n < 96 && getNext(GetCurrentProcess(), prev, acc, 0, 0, &cur) == 0 && cur) {
        if (prev) CloseHandle(prev);
        prev = cur;
        ++n;
        DWORD tid = GetThreadId(cur);
        if (tid == GetCurrentThreadId())
            continue; // the detector thread itself -- suspending self would hang
        unsigned long pc = 0, lr = 0, sp = 0; bool ok = false;
        unsigned long sw[6]; int nsw = 0;
        if (SuspendThread(cur) != (DWORD)-1) {
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            ok = GetThreadContext(cur, &c);
            if (ok) {
                pc = c.Pc; lr = c.Lr; sp = c.Sp;
                if (sp) {
                    MEMORY_BASIC_INFORMATION sm;
                    if (VirtualQuery(reinterpret_cast<void*>((uintptr_t)sp), &sm, sizeof sm) && sm.State == MEM_COMMIT) {
                        uintptr_t end = reinterpret_cast<uintptr_t>(sm.BaseAddress) + sm.RegionSize;
                        int mw = static_cast<int>((end - sp) / sizeof(unsigned long)); if (mw > 128) mw = 128;
                        unsigned long buf[128];
                        if (mw > 0) {
                            memcpy(buf, reinterpret_cast<void*>((uintptr_t)sp), mw * sizeof(unsigned long));
                            for (int i = 0; i < mw && nsw < 6; ++i) {
                                unsigned long w = buf[i];
                                if (w < 0x10000 || !(w & 1)) continue;
                                MEMORY_BASIC_INFORMATION cm;
                                if (VirtualQuery(reinterpret_cast<void*>((uintptr_t)(w & ~1UL)), &cm, sizeof cm)) {
                                    bool ex = (cm.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) != 0;
                                    if (ex && (cm.Type == MEM_IMAGE || cm.Type == MEM_PRIVATE)) sw[nsw++] = w;
                                }
                            }
                        }
                    }
                }
            }
            ResumeThread(cur);
        }
        if (ok) {
            char tb[300];
            int off = snprintf(tb, sizeof tb, "  athr tid=%lu%s pc=0x%08lx lr=0x%08lx sp=0x%08lx st=",
                (unsigned long)tid, tid == g_mainThreadId ? "*main" : "", pc, lr, sp);
            for (int i = 0; i < nsw && off < static_cast<int>(sizeof tb) - 12; ++i)
                off += snprintf(tb + off, sizeof tb - off, "%08lx ", sw[i]);
            portLog(tb);
        }
    }
    if (prev) CloseHandle(prev);
}

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
// Sample the (running) main thread ~60x over ~1.2s and histogram where its PC lands, bucketed by
// address range: in OUR module (WebCore/JSC C++), the JIT executable pool, or system DLLs. Emits
// RVAs to symbolize offline against WebCoreRenderShell.map. Shared by the frozen-stall path and the
// crawl path so both produce identical, comparable output. `tag` distinguishes them in the log.
static void profileBurstMainThread(unsigned long long hb, unsigned long long cpuDeltaMs, const char* tag)
{
    if (!g_mainThreadHandle)
        return;
    uintptr_t pbase = reinterpret_cast<uintptr_t>(&__ImageBase);
    uintptr_t pmodEnd = pbase + 0x4000000;
    struct ProfBucket { unsigned long rva; int count; };
    ProfBucket hist[48];
    ProfBucket callerHist[48];
    // FULL per-sample capture of every out-of-module PC (DLL + JIT) so the 69% JSC.dll bucket can be
    // resolved offline against the PDB's function RANGES (not just top-6 nearest-public, which misresolves
    // gaps to bogus symbols like _initterm_e). Store PC + caller LR + allocBase + kind for all 60 samples.
    unsigned long allOutPc[60]; unsigned long allOutLr[60]; unsigned long allOutBase[60]; char allOutKind[60];
    int nOut = 0;
    // DEEP STACK SCAN: PC+LR (1 frame) proved too thin to attribute the 26-min WTF::sleep hang, the
    // URLParser hotpath, and __rt_sdiv64 (LR resolved only 1 weak caller). For each sample we copy the
    // top of the suspended thread's stack, then (after resume) keep the words that point into executable
    // memory -- a heuristic call chain (no unwind info on Thumb-2). Offline we resolve each frame against
    // the module at its base (JSC 0x54970000 / WTF 0x77c40000 / exe 0x400000 / JIT pool). frame:base pairs.
    unsigned long stkPc[60][8]; unsigned long stkBase[60][8]; unsigned char stkN[60];
    for (int i = 0; i < 60; ++i) stkN[i] = 0;
    int nb = 0, ncall = 0, samples = 0;
    int inMod = 0, jitRange = 0, sysHi = 0, other = 0;
    for (int s = 0; s < 60; ++s) {
        if (SuspendThread(g_mainThreadHandle) == (DWORD)-1)
            break;
        CONTEXT pctx;
        memset(&pctx, 0, sizeof pctx);
        pctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        bool pok = GetThreadContext(g_mainThreadHandle, &pctx);
        unsigned long ppc = pok ? pctx.Pc : 0, plr = pok ? pctx.Lr : 0;
        // Copy the top of the stack WHILE suspended (fast memcpy, minimal suspend extension); classify
        // after resume since VirtualQuery doesn't need the target paused. Bound the copy to the committed
        // stack region so we never read past the guard page.
        unsigned long stackCopy[200]; int stackWords = 0;
        unsigned long psp = pok ? pctx.Sp : 0;
        if (pok && psp) {
            MEMORY_BASIC_INFORMATION sm;
            if (VirtualQuery(reinterpret_cast<void*>(psp), &sm, sizeof sm) && sm.State == MEM_COMMIT) {
                uintptr_t regionEnd = reinterpret_cast<uintptr_t>(sm.BaseAddress) + sm.RegionSize;
                int maxw = static_cast<int>((regionEnd - psp) / sizeof(unsigned long));
                stackWords = maxw < 200 ? maxw : 200;
                if (stackWords > 0)
                    memcpy(stackCopy, reinterpret_cast<void*>(psp), stackWords * sizeof(unsigned long));
                else
                    stackWords = 0;
            }
        }
        ResumeThread(g_mainThreadHandle);
        if (!pok) { ::Sleep(20); continue; }
        {
            int fcount = 0;
            for (int i = 0; i < stackWords && fcount < 8; ++i) {
                unsigned long val = stackCopy[i];
                if (val < 0x10000) continue;
                MEMORY_BASIC_INFORMATION cm;
                if (VirtualQuery(reinterpret_cast<void*>(val), &cm, sizeof cm)) {
                    bool ex = (cm.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) != 0;
                    if (ex && (cm.Type == MEM_IMAGE || cm.Type == MEM_PRIVATE)) {
                        stkPc[s][fcount] = val;
                        stkBase[s][fcount] = static_cast<unsigned long>(reinterpret_cast<uintptr_t>(cm.AllocationBase));
                        ++fcount;
                    }
                }
            }
            stkN[s] = static_cast<unsigned char>(fcount);
        }
        ++samples;
        if (ppc >= pbase && ppc < pmodEnd) {
            ++inMod;
            unsigned long rva = (unsigned long)((ppc & ~1UL) - pbase);
            int found = -1;
            for (int i = 0; i < nb; ++i) { unsigned long d = hist[i].rva>rva?hist[i].rva-rva:rva-hist[i].rva; if (d<64){found=i;break;} }
            if (found >= 0) hist[found].count++; else if (nb < 48) { hist[nb].rva=rva; hist[nb].count=1; ++nb; }
        } else {
            // Classify the out-of-module PC by what the OS actually mapped there -- NOT by a guessed
            // address range (the old 0x04-0x10M "jitRange" never matched: the JIT pool reserves
            // wherever the AppContainer gives it, e.g. 0x19D00000, and JSC commits more exec pages
            // elsewhere). VirtualQuery is ground truth:
            //   MEM_IMAGE                     -> inside a DLL (ANGLE / ICU / ntdll / combase)
            //   MEM_PRIVATE + executable prot -> JIT-COMPILED CODE (baseline/DFG output). THIS is the
            //                                    number that says whether hot JS runs compiled or in
            //                                    the LLInt interpreter (interpreter PCs land in-module).
            MEMORY_BASIC_INFORMATION mq;
            char kind = 'o'; unsigned long ab = 0;
            if (VirtualQuery(reinterpret_cast<void*>(ppc), &mq, sizeof mq)) {
                bool execProt = (mq.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) != 0;
                ab = (unsigned long)reinterpret_cast<uintptr_t>(mq.AllocationBase);
                if (mq.Type == MEM_IMAGE) { ++sysHi; kind = 'I'; }                    // a DLL
                else if (mq.Type == MEM_PRIVATE && execProt) { ++jitRange; kind = 'J'; } // JIT code (private exec pages)
                else ++other;
            } else
                ++other;
            if (nOut < 60) { allOutPc[nOut]=ppc; allOutLr[nOut]=plr; allOutBase[nOut]=ab; allOutKind[nOut]=kind; ++nOut; }
            if (plr >= pbase && plr < pmodEnd) {
                unsigned long lrva = (unsigned long)((plr & ~1UL) - pbase);
                int f=-1; for (int i=0;i<ncall;++i){ unsigned long d=callerHist[i].rva>lrva?callerHist[i].rva-lrva:lrva-callerHist[i].rva; if(d<64){f=i;break;} }
                if (f>=0) callerHist[f].count++; else if (ncall<48){ callerHist[ncall].rva=lrva; callerHist[ncall].count=1; ++ncall; }
            }
        }
        ::Sleep(20);
    }
    char ph[220];
    snprintf(ph, sizeof ph, "%s profile hb=%llu cpuDelta=%llums samples=%d | inModule(C++/LLInt)=%d JITcode=%d DLL=%d other=%d",
        tag, hb, cpuDeltaMs, samples, inMod, jitRange, sysHi, other);
    portLog(ph);
    // For each hot out-of-module address, report what the OS mapped there so the region is
    // IDENTIFIABLE in one pass: type (IMAGE=a DLL, PRIVATE=JIT/heap), the exec protection, and the
    // allocation base (a DLL's base is stable per-run; a JIT pool's base matches jitdiag's reservation
    // or a later JSC commit). This is what turns "other=44 at 0x54000000" into a named thing.
    // Dump EVERY out-of-module sample (kind I=DLL image, J=JIT private-exec, o=other), with caller LR
    // and allocBase, so offline resolution against the PDB gets the full ~60-sample/burst distribution
    // instead of a deduped top-6. kind+base disambiguate JSC.dll (base 0x54970000) from ANGLE/ntdll/etc.
    for (int i = 0; i < nOut; ++i) {
        char sp2[128]; snprintf(sp2, sizeof sp2, "  profs k=%c pc=0x%08lx lr=0x%08lx base=0x%08lx",
            allOutKind[i], allOutPc[i], allOutLr[i], allOutBase[i]); portLog(sp2);
    }
    // Deep stack per sample: "  dstk i=<sample> <frameAddr>:<moduleBase> ..." (up to 8 exec frames).
    // Offline: for each frame, base 0x54970000 -> jsc ranges, 0x77c40000 -> wtf ranges, 0x400000 -> exe
    // map (rva+0x400000), a 0x19xxxxxx-ish MEM_PRIVATE base -> JIT pool. The chain gives real callers.
    for (int s2 = 0; s2 < 60; ++s2) {
        if (!stkN[s2]) continue;
        char db[320]; int off = snprintf(db, sizeof db, "  dstk i=%d", s2);
        for (int f = 0; f < stkN[s2] && off < static_cast<int>(sizeof db) - 20; ++f)
            off += snprintf(db + off, sizeof db - off, " %08lx:%08lx", stkPc[s2][f], stkBase[s2][f]);
        portLog(db);
    }
    for (int k = 0; k < 8; ++k) { int bi=-1,bc=0; for(int i=0;i<nb;++i) if(hist[i].count>bc){bc=hist[i].count;bi=i;} if(bi<0||bc==0)break; char pl[96]; snprintf(pl,sizeof pl,"  prof pcRva=0x%08lx hits=%d",hist[bi].rva,bc); portLog(pl); hist[bi].count=0; }
    for (int k = 0; k < 10; ++k) { int bi=-1,bc=0; for(int i=0;i<ncall;++i) if(callerHist[i].count>bc){bc=callerHist[i].count;bi=i;} if(bi<0||bc==0)break; char pl[96]; snprintf(pl,sizeof pl,"  prof callerRva=0x%08lx hits=%d",callerHist[bi].rva,bc); portLog(pl); callerHist[bi].count=0; }
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
    g_mainThreadId = GetCurrentThreadId();
    std::thread([]() {
        unsigned long long last = 0;
        int stalledSecs = 0;
        unsigned long long lastCpu = 0;
        unsigned long long lastCrawlCpu = 0;
        int crawlProfiled = 0;
        for (;;) {
            ::Sleep(1000);
            unsigned long long hb = g_mainHeartbeat.load();
            // CRAWL PROFILE. The block below only fires when the heartbeat is FROZEN (hb==last) for 3s.
            // But a page-load burst is NOT frozen -- the render loop still turns at 1-4 fps -- so the
            // heartbeat advances a few ticks each second, hb==last is false, and the whole detector
            // resets without ever sampling the burst. That is why every load profile came back with a
            // handful of samples: we were only ever catching true 3s deadlocks, never the slow grind
            // that IS the slow load. Here: the heartbeat advanced, but by very little (<6 ticks/sec =
            // <6fps) AND the main thread burned real CPU that second => it is grinding, not idle.
            // Sample it the same way, but rate-limited so a long burst doesn't flood the log.
            unsigned long long hbDelta = hb >= last ? hb - last : 0;
            if (hb != last && hb != 0 && hbDelta > 0 && hbDelta < 6) {
                unsigned long long ccpu = 0; FILETIME c0,c1,c2,c3;
                if (g_mainThreadHandle && GetThreadTimes(g_mainThreadHandle,&c0,&c1,&c2,&c3))
                    ccpu = ((unsigned long long)c2.dwHighDateTime<<32|c2.dwLowDateTime)+((unsigned long long)c3.dwHighDateTime<<32|c3.dwLowDateTime);
                unsigned long long cDeltaMs = lastCrawlCpu ? (ccpu-lastCrawlCpu)/10000 : 0;
                lastCrawlCpu = ccpu;
                if (cDeltaMs > 600 && crawlProfiled < 40) {
                    ++crawlProfiled;
                    profileBurstMainThread(hb, cDeltaMs, "CRAWL");
                }
                last = hb; stalledSecs = 0;
                continue;
            }
            lastCrawlCpu = 0;
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

                    // BURST PROFILE the frozen stall (shared with the crawl path above).
                    // BURNING: burst-profile the spinning main thread (dstk shows the native loop).
                    // IDLE (deadlock): the main thread is parked -- one deep-stack pass shows the full
                    // frozen chain (does it end in WTF::Lock::lockSlow / Condition::wait -> waiting on
                    // another thread, or in ICU data-load -> self-contained?), and dumpAllThreadsAtStall
                    // names the thread that HOLDS whatever it waits on. Gate both to the first few stall
                    // reports so a long deadlock doesn't flood the log (nothing moves between samples).
                    static int s_stallProbes = 0;
                    if (cpuDeltaMs > 400)
                        profileBurstMainThread(hb, cpuDeltaMs, "MAIN-STALL");
                    else if (s_stallProbes < 3) {
                        profileBurstMainThread(hb, cpuDeltaMs, "MAIN-STALL-IDLE");
                        portLog("  athr: enumerating all threads at deadlock (holder = who parks the ICU/lock main waits on)");
                        dumpAllThreadsAtStall();
                        ++s_stallProbes;
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
// Ensures the "update the rendering" step runs exactly ONCE per render frame. Reset at the top of
// each WebCoreBrowserRenderFrame. The render loop calls WebCoreDriverRunRenderingUpdate()
// unconditionally every frame (the HTML event loop's rendering opportunity), and WebCore's own
// RenderingUpdateScheduler ALSO calls it via PortChromeClient::triggerRenderingUpdate() — whichever
// fires first this frame runs the update; the other is deduped here (no double rAF/animation tick).
static bool g_renderingUpdateDoneThisFrame = false;
extern "C" void WebCoreDriverRunRenderingUpdate()
{
    using namespace WebCore;
    if (!g_page || g_inRenderingUpdate || g_renderingUpdateDoneThisFrame)
        return;
    Frame& frame = g_page->mainFrame();
    FrameView* view = frame.view();
    if (!view)
        return;
    g_renderingUpdateDoneThisFrame = true;
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
// TRUE video throughput. The gate's old "fps" was the RENDER-LOOP iteration rate, which reads ~60
// even when every one of those iterations SKIPPED compositing (composited=0) -- i.e. it reported 60fps
// while painting nothing. These count what actually happened: frames the decoder handed us, and frames
// the compositor really uploaded and drew.
static std::atomic<unsigned> g_videoFramesDelivered { 0 };
static std::atomic<unsigned> g_videoFramesDrawn { 0 };
extern "C" void WebCoreBrowserNoteVideoFrameDelivered() { g_videoFramesDelivered.fetch_add(1, std::memory_order_relaxed); }
extern "C" void WebCoreBrowserNoteVideoFrameDrawn() { g_videoFramesDrawn.fetch_add(1, std::memory_order_relaxed); }

static unsigned g_composeFrame = 0;      // increments once per render frame
static unsigned g_forceComposeUntil = 0; // composite unconditionally while g_composeFrame < this
static unsigned g_lastActiveComposeFrame = 0; // last frame WebCore produced a visual change (dirty/flush)
static unsigned g_composited300 = 0, g_skipped300 = 0; // rolling 300-frame gate stats (proves savings)
static unsigned g_gateFrames = 0; // loop iterations in the CURRENT gate window (gate is time- OR frame-triggered)
static double g_msJS = 0, g_msComposite = 0;           // rolling 300-frame CPU split: JS/RunLoop vs composite
static double g_msBacking = 0, g_msPaint = 0, g_msSwap = 0; // composite sub-split: CPU raster+upload / GPU paint-submit / present-block
// The main thread's OTHER half. JS and composite were the only two timers, and on the pages that
// collapse to ~1 fps they account for barely a third of uiCPU (e.g. wall=2072 uiCPU=2062 JS=298
// composite=345 -> 1419ms measured by nothing). The gap is everything BETWEEN them: the HTML
// "update the rendering" step (style + layout + rAF + Intersection/ResizeObserver), which runs
// inside WebCoreBrowserVsyncTick via the RenderingUpdateScheduler; the idle period; and the
// compositing-state flush. Those run on EVERY loop iteration, including the ones the composite gate
// skips -- so a skipped frame is not a free frame, and nothing was reporting that.
static double g_msUpdate = 0, g_msIdle = 0, g_msFlush = 0;
// beginPainting/paint/endPainting split. "paint=1268ms" for three 412x232 layers is not GL submit
// cost; it is a block somewhere inside that trio, and lumping them hides which.
static double g_msPaintBegin = 0, g_msPaintTree = 0, g_msPaintEnd = 0;

// App memory, published by the shell every frame (WinRT MemoryManager is only reachable from there).
// W10M TERMINATES a process that crosses AppMemoryUsageLimit -- no exception, no PLM notification,
// nothing for a fault handler to catch. That is what "the app just closes" has been. The last log
// ends on "pressure HIGH -> releaseMemory(critical)" with no "done", and the reading before it was
// 97MB taken minutes earlier, because the sampler was frame-counted. Put it on every gate line.
static unsigned long long g_memUsedMB = 0, g_memLimitMB = 0, g_memPct = 0;
static int g_memLevel = 0;

extern "C" void WebCoreBrowserSetMemStats(unsigned long long usedBytes, unsigned long long limitBytes,
    unsigned long long pct, int level)
{
    g_memUsedMB = usedBytes / (1024 * 1024);
    g_memLimitMB = limitBytes / (1024 * 1024);
    g_memPct = pct;
    g_memLevel = level;

    // ARM WebKit's memory-conservative machinery. It was inert, and that is the whole bug:
    // PSAPI's QueryWorkingSet is desktop-only, so this build compiles MemoryFootprintGeneric.cpp,
    // whose memoryFootprint() returns 0. WebKit therefore believed the process used NO memory, kept
    // MemoryUsagePolicy at Unrestricted and RenderLayerCompositor at CompositingPolicy::Normal, and
    // never stopped promoting layers -- 263 composited layers / 214MB of GPU tiles, straight into the
    // W10M memory kill. Hand it the real figure the shell already has.
    WTF::setMemoryFootprintOverride(static_cast<size_t>(usedBytes));

    // And point the thresholds at the cap that actually kills us. The default baseThreshold is
    // min(3GB, ramSize()), so Conservative would arm around 500MB on this phone -- far past the
    // ~390MB AppContainer cap the OS terminates us at. Anchored to the real limit, Conservative
    // arms at 50% (~195MB) and Strict at 65% (~254MB), both comfortably below the kill.
    static bool s_configured = false;
    if (!s_configured && limitBytes) {
        s_configured = true;
        MemoryPressureHandler::Configuration config {
            static_cast<size_t>(limitBytes),
            0.5,            // conservative: compositor stops promoting speculative layers
            0.65,           // strict
            std::nullopt,   // no self-kill; the OS owns that
            Seconds { 5 }   // poll often -- this page goes 140MB -> 270MB in seconds
        };
        MemoryPressureHandler::singleton().setConfiguration(WTFMove(config));
        // install() ran at startup, before we knew the cap, so its monitor is on the DEFAULT 30s
        // poll. Restart it so it actually polls on our 5s interval -- chaturbate went 73% -> dead in
        // about six seconds, which a 30s poll would sleep straight through.
        MemoryPressureHandler::singleton().setShouldUsePeriodicMemoryMonitor(true);
        char b[160];
        snprintf(b, sizeof b, "mem: pressure thresholds anchored to cap: base=%lluMB conservative=%lluMB strict=%lluMB",
            limitBytes / (1024 * 1024), (limitBytes / 2) / (1024 * 1024),
            (unsigned long long)(limitBytes * 0.65) / (1024 * 1024));
        portLog(b);
    }
}

// GPU tile bytes currently held by the tiled backing stores (TextureMapperTiledBackingStore keeps
// this current). "appUsage went up 200MB" is not actionable on its own -- this splits the total into
// the three pools we can actually do something about.
namespace WebCore {
extern unsigned long long g_texmapTileBytes;
extern unsigned g_texmapStoreCount;      // live TextureMapperTiledBackingStore objects
extern unsigned g_texmapLayerCount;      // live GraphicsLayerTextureMapper objects
extern unsigned long long g_texmapPoolBytes; // BitmapTexturePool retained bytes
extern unsigned g_texmapPoolCount;
// bstat: backing-phase breakdown (defined in GraphicsLayerTextureMapper.cpp; see comment there).
extern unsigned g_bsVisited, g_bsPainted, g_bsNdspFull, g_bsClipped, g_bsSelfHeal;
extern unsigned g_bsInvFull, g_bsInvFullBig, g_bsInvRect, g_bsInvRectBig;
extern unsigned long long g_bsDirtyPx;
extern unsigned g_bsRetile, g_bsTilesCreated, g_bsTilesRecycled, g_bsTilesRemoved, g_bsScaleNuke;
extern float g_bsScaleMin, g_bsScaleMax;
extern double g_bsRasterMs; extern unsigned long long g_bsRasterPx;
extern double g_bsBackCreateMs, g_bsBackPaintMs, g_bsBackCopyMs; extern unsigned g_bsBackCalls;
extern double g_bsUploadMs; extern unsigned long long g_bsUploadKB;
extern unsigned g_bsTexRealloc, g_bsTexReuse, g_bsTexClamp;
extern unsigned g_bsPoolHit, g_bsPoolMiss;
// Raster attribution (defined in GraphicsContextCairo.cpp): splits g_bsRasterMs by cairo primitive type.
extern double g_bsCairoImageMs; extern unsigned g_bsCairoImageN;
extern double g_bsCairoGlyphMs; extern unsigned g_bsCairoGlyphN; extern unsigned g_bsCairoGlyphCount;
extern double g_bsCairoFillMs; extern unsigned g_bsCairoFillN;
extern double g_bsCairoGradMs; extern unsigned g_bsCairoGradN;
extern double g_bsCairoStrokeMs;
}

// Per-gate-window accumulation of the bstat counters (frame counters are snapshot+reset every frame in
// compositeTree; slow frames print their own snapshot, the window sums print beside the gate line).
struct BstatWindow {
    unsigned visited, painted, ndspFull, clipped, selfHeal;
    unsigned invFull, invFullBig, invRect, invRectBig;
    unsigned long long dirtyPx;
    unsigned retile, tilesCreated, tilesRecycled, tilesRemoved, scaleNuke;
    double rasterMs; unsigned long long rasterPx;
    double uploadMs; unsigned long long uploadKB;
    unsigned texRealloc, texReuse, texClamp, poolHit, poolMiss;
    double cImageMs, cGlyphMs, cFillMs, cGradMs, cStrokeMs;
    unsigned cImageN, cGlyphN, cGlyphCount, cFillN, cGradN;
    double backCreateMs, backPaintMs, backCopyMs; unsigned backCalls;
    float scaleMin, scaleMax; // range of contents scales that caused a nuke this frame
};
static BstatWindow g_bstatWindow = {};
static BstatWindow g_bstatFrame = {}; // last composited frame's counters (valid whenever backing ran)

// Snapshot the frame's counters into `out`, fold them into the window sums, and zero the live counters.
// Called once per composited frame, right after updateBackingStoreIncludingSubLayers.
static void bstatHarvestFrame(BstatWindow& out)
{
    using namespace WebCore;
    out = { g_bsVisited, g_bsPainted, g_bsNdspFull, g_bsClipped, g_bsSelfHeal,
            g_bsInvFull, g_bsInvFullBig, g_bsInvRect, g_bsInvRectBig, g_bsDirtyPx,
            g_bsRetile, g_bsTilesCreated, g_bsTilesRecycled, g_bsTilesRemoved, g_bsScaleNuke,
            g_bsRasterMs, g_bsRasterPx, g_bsUploadMs, g_bsUploadKB,
            g_bsTexRealloc, g_bsTexReuse, g_bsTexClamp, g_bsPoolHit, g_bsPoolMiss,
            g_bsCairoImageMs, g_bsCairoGlyphMs, g_bsCairoFillMs, g_bsCairoGradMs, g_bsCairoStrokeMs,
            g_bsCairoImageN, g_bsCairoGlyphN, g_bsCairoGlyphCount, g_bsCairoFillN, g_bsCairoGradN,
            g_bsBackCreateMs, g_bsBackPaintMs, g_bsBackCopyMs, g_bsBackCalls,
            g_bsScaleMin, g_bsScaleMax };
    g_bstatWindow.visited += out.visited; g_bstatWindow.painted += out.painted;
    g_bstatWindow.ndspFull += out.ndspFull; g_bstatWindow.clipped += out.clipped;
    g_bstatWindow.selfHeal += out.selfHeal;
    g_bstatWindow.invFull += out.invFull; g_bstatWindow.invFullBig += out.invFullBig;
    g_bstatWindow.invRect += out.invRect; g_bstatWindow.invRectBig += out.invRectBig;
    g_bstatWindow.dirtyPx += out.dirtyPx;
    g_bstatWindow.retile += out.retile; g_bstatWindow.tilesCreated += out.tilesCreated;
    g_bstatWindow.tilesRecycled += out.tilesRecycled; g_bstatWindow.tilesRemoved += out.tilesRemoved;
    g_bstatWindow.scaleNuke += out.scaleNuke;
    if (g_bstatWindow.scaleMin == 0.f || (out.scaleMin != 0.f && out.scaleMin < g_bstatWindow.scaleMin)) g_bstatWindow.scaleMin = out.scaleMin;
    if (out.scaleMax > g_bstatWindow.scaleMax) g_bstatWindow.scaleMax = out.scaleMax;
    g_bstatWindow.rasterMs += out.rasterMs; g_bstatWindow.rasterPx += out.rasterPx;
    g_bstatWindow.uploadMs += out.uploadMs; g_bstatWindow.uploadKB += out.uploadKB;
    g_bstatWindow.texRealloc += out.texRealloc; g_bstatWindow.texReuse += out.texReuse;
    g_bstatWindow.texClamp += out.texClamp;
    g_bstatWindow.poolHit += out.poolHit; g_bstatWindow.poolMiss += out.poolMiss;
    g_bstatWindow.cImageMs += out.cImageMs; g_bstatWindow.cGlyphMs += out.cGlyphMs;
    g_bstatWindow.cFillMs += out.cFillMs; g_bstatWindow.cGradMs += out.cGradMs;
    g_bstatWindow.cStrokeMs += out.cStrokeMs;
    g_bstatWindow.cImageN += out.cImageN; g_bstatWindow.cGlyphN += out.cGlyphN;
    g_bstatWindow.cGlyphCount += out.cGlyphCount; g_bstatWindow.cFillN += out.cFillN;
    g_bstatWindow.cGradN += out.cGradN;
    g_bstatWindow.backCreateMs += out.backCreateMs; g_bstatWindow.backPaintMs += out.backPaintMs;
    g_bstatWindow.backCopyMs += out.backCopyMs; g_bstatWindow.backCalls += out.backCalls;
    g_bsCairoImageMs = g_bsCairoGlyphMs = g_bsCairoFillMs = g_bsCairoGradMs = g_bsCairoStrokeMs = 0;
    g_bsCairoImageN = g_bsCairoGlyphN = g_bsCairoGlyphCount = g_bsCairoFillN = g_bsCairoGradN = 0;
    g_bsBackCreateMs = g_bsBackPaintMs = g_bsBackCopyMs = 0; g_bsBackCalls = 0;
    g_bsVisited = g_bsPainted = g_bsNdspFull = g_bsClipped = g_bsSelfHeal = 0;
    g_bsInvFull = g_bsInvFullBig = g_bsInvRect = g_bsInvRectBig = 0;
    g_bsDirtyPx = 0;
    g_bsRetile = g_bsTilesCreated = g_bsTilesRecycled = g_bsTilesRemoved = g_bsScaleNuke = 0;
    g_bsScaleMin = g_bsScaleMax = 0.f;
    g_bsRasterMs = 0; g_bsRasterPx = 0; g_bsUploadMs = 0; g_bsUploadKB = 0;
    g_bsTexRealloc = g_bsTexReuse = g_bsTexClamp = 0;
    g_bsPoolHit = g_bsPoolMiss = 0;
}

static void bstatFormat(char* buf, size_t bufSize, const char* tag, const BstatWindow& s)
{
    snprintf(buf, bufSize,
        "%s: vis=%u paint=%u ndspFull=%u clip=%u heal=%u | inv full=%u(big=%u) rect=%u(big=%u) dirtyMpx=%.1f | "
        "retile=%u tiles+%u/~%u/-%u scaleNuke=%u[%.3f..%.3f] | raster=%.0fms/%.1fMpx upload=%.0fms/%lluKB | "
        "tex realloc=%u reuse=%u clamp=%u pool=%u/%u | "
        "RASTER-BY-TYPE image=%.0fms/%u glyph=%.0fms/%u(%uglyphs) fill=%.0fms/%u grad=%.0fms/%u stroke=%.0fms | "
        "BACK-SPLIT create=%.0fms paint=%.0fms copy=%.0fms calls=%u",
        tag, s.visited, s.painted, s.ndspFull, s.clipped, s.selfHeal,
        s.invFull, s.invFullBig, s.invRect, s.invRectBig, s.dirtyPx / 1.0e6,
        s.retile, s.tilesCreated, s.tilesRecycled, s.tilesRemoved, s.scaleNuke, s.scaleMin, s.scaleMax,
        s.rasterMs, s.rasterPx / 1.0e6, s.uploadMs, s.uploadKB,
        s.texRealloc, s.texReuse, s.texClamp, s.poolHit, s.poolMiss,
        s.cImageMs, s.cImageN, s.cGlyphMs, s.cGlyphN, s.cGlyphCount, s.cFillMs, s.cFillN,
        s.cGradMs, s.cGradN, s.cStrokeMs,
        s.backCreateMs, s.backPaintMs, s.backCopyMs, s.backCalls);
}
static const unsigned GATE_N = 120;      // gate window size (frames); ~10-12fps under load => a line every ~10-12s
static MonotonicTime g_gateLastWall;      // wall baseline (seeded on frame 1 so the FIRST gate line is valid)
static bool g_gateSeeded = false;
static unsigned long long g_gateLastCpuMs = 0; // main-thread CPU baseline (kernel+user ms)
static unsigned long long readMainThreadCpuMs()
{
    FILETIME c, e, k, u;
    if (g_mainThreadHandle && GetThreadTimes(g_mainThreadHandle, &c, &e, &k, &u))
        return (((unsigned long long)k.dwHighDateTime << 32 | k.dwLowDateTime)
              + ((unsigned long long)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000;
    return 0;
}

// Called by content whose pacing is decoupled from WebCore's invalidations (e.g. a playing <video>,
// whose frames arrive at composite time via OnVideoStreamTick, not per-invalidation). Keeps the
// composite alive for a window of frames so playback stays smooth; expires when the source stops.
extern "C" void WebCoreBrowserKeepCompositing(unsigned frames)
{
    unsigned until = g_composeFrame + frames;
    if (until > g_forceComposeUntil)
        g_forceComposeUntil = until;
}

// Force a full re-raster + re-present of the whole page. Used on resume-from-background: the OS
// discards the GPU backing textures while suspended, but WebCore has no dirty regions on a static
// page, so simply re-compositing (KeepCompositing) re-presents the now-black textures. Marking the
// RenderView and every composited layer for repaint makes updateBackingStore actually re-raster the
// content into fresh textures before the next present.
extern "C" void WebCoreBrowserForceRepaint()
{
    using namespace WebCore;
    if (!g_page)
        return;
    if (FrameView* view = g_page->mainFrame().view()) {
        if (RenderView* renderView = view->renderView())
            renderView->repaintViewAndCompositedLayers();
    }
    WebCoreBrowserKeepCompositing(90); // present the re-rastered frames (gate would otherwise skip)
}

// Pump WebCore (async load + timers), lay out, and present the current page state to
// the CoreWindow surface. Called once per frame by the shell's event loop.
extern "C" void WebCoreBrowserReleaseMemory(int critical); // defined below

// Set by the shell's BACKGROUND memory watcher (any thread) when app usage nears the OS kill cap.
// The frame pump consumes it below, first thing on the main thread — the earliest legal moment for a
// WebCore releaseMemory() after a stall (the watcher can't call WebCore itself; wrong thread).
static std::atomic<int> g_memEmergencyFlag { 0 };
extern "C" void WebCoreBrowserMemEmergency()
{
    g_memEmergencyFlag.store(1, std::memory_order_relaxed);
}

extern "C" void WebCoreBrowserRenderFrame()
{
    using namespace WebCore;
    if (!g_page || !g_glContext)
        return;
    // Emergency release requested by the background watcher while we were busy/stalled. Do it before
    // any frame work — memory may be within a few MB of the kill cap by the time we get here.
    if (g_memEmergencyFlag.exchange(0, std::memory_order_relaxed)) {
        portLog("mem: EMERGENCY release (background watcher flagged near-cap)");
        WebCoreBrowserReleaseMemory(1);
    }
    // Per-frame STAGE trace across a window of frames to localize a main-thread hang to an exact
    // sub-step: whichever "rf N <stage>" line is LAST before the log goes quiet is where the main
    // thread stopped returning. Windowed to avoid spamming every frame for the whole session.
    static unsigned g_rfFrame = 0;
    ++g_rfFrame;
    ++g_composeFrame;
    // Seed the gate wall/CPU baseline on the very first frame so the FIRST gate line has a valid delta
    // (the prior build printed the first window as wall=0/util=0 because there was no baseline yet).
    if (!g_gateSeeded) {
        g_gateLastWall = MonotonicTime::now();
        g_gateLastCpuMs = readMainThreadCpuMs();
        g_gateSeeded = true;
    }
    // Fire on FRAMES *or* on TIME. Frame-counted only was a blind spot exactly where it mattered: when
    // the loop collapses to ~1 fps (heavy compositor paint), 120 frames take two minutes, so the gate
    // goes SILENT precisely during the pathological window. Every crash log ends with a long gap and no
    // gate line — not because nothing was happening, but because the meter stopped reporting.
    ++g_gateFrames;
    bool gateByTime = (MonotonicTime::now() - g_gateLastWall).milliseconds() >= 2000;
    if (g_gateFrames >= GATE_N || gateByTime) {
        // Wall time + REAL main-thread CPU time over this window => true utilization.
        // Resolves "work timers sum to ~Ns but nothing is pegged": the JS and swap timers wrap BLOCKING
        // calls (RunLoop::iterate waits on network completions; SwapBuffers blocks on vsync), so they
        // bill wait-time as if it were work. uiCPU (kernel+user thread time, same handle the stall
        // detector uses) is ground truth. util = uiCPU/wall: low util => the frame loop is WAIT-bound
        // (network waterfall / vsync); ~100% util => the main thread is genuinely compute-pegged.
        MonotonicTime nowG = MonotonicTime::now();
        double wallMs = (nowG - g_gateLastWall).milliseconds();
        unsigned long long cpuMs = readMainThreadCpuMs();
        unsigned long long cpuDeltaMs = cpuMs >= g_gateLastCpuMs ? cpuMs - g_gateLastCpuMs : 0;
        double fps = wallMs > 0 ? g_gateFrames * 1000.0 / wallMs : 0;        // loop iterations/sec
        double paintedFps = wallMs > 0 ? g_composited300 * 1000.0 / wallMs : 0; // frames ACTUALLY presented
        int util = wallMs > 0 ? (int)(cpuDeltaMs * 100 / (unsigned long long)wallMs) : 0;
        // Video: frames the decoder delivered vs frames we actually uploaded+drew this window. A gap
        // between them means we are DROPPING decoded frames (composite gate skipping, or slow paint).
        static unsigned s_lastDelivered = 0, s_lastDrawn = 0;
        unsigned deliveredNow = g_videoFramesDelivered.load(std::memory_order_relaxed);
        unsigned drawnNow = g_videoFramesDrawn.load(std::memory_order_relaxed);
        unsigned deliveredDelta = deliveredNow - s_lastDelivered;
        unsigned drawnDelta = drawnNow - s_lastDrawn;
        s_lastDelivered = deliveredNow; s_lastDrawn = drawnNow;
        double vDeliveredFps = wallMs > 0 ? deliveredDelta * 1000.0 / wallMs : 0;
        double vDrawnFps = wallMs > 0 ? drawnDelta * 1000.0 / wallMs : 0;
        g_gateLastWall = nowG; g_gateLastCpuMs = cpuMs;

        // JIT executable-memory telemetry: committed bytes of the (now 128MB) pool + whether JSC
        // considers itself under exec-memory pressure. If committed climbs toward the pool ceiling or
        // pressure=1, heavy-JS sites are thrashing the JIT (exec-mem GC + LLInt fallback) = the hang.
        // jitAllocs = cumulative baseline/DFG code emissions. If this RISES between windows while JS
        // runs, the JIT is compiling web JS; if it stays FLAT during heavy JS, we're LLInt-only.
        // Every phase of the loop is now billed, so the line must ADD UP to uiCPU. Whatever is left
        // over ("other") is XAML/shell overhead outside WebCoreBrowserRenderFrame.
        double accounted = g_msJS + g_msUpdate + g_msIdle + g_msFlush + g_msComposite;
        char b[600]; snprintf(b, sizeof b, "gate: last%u composited=%u skipped=%u | wall=%.0fms loopFps=%.1f PAINTEDfps=%.1f uiCPU=%llums util=%d%% | mem=%lluMB/%lluMB (%llu%%) lvl=%d | video: decoded=%.1ffps drawn=%.1ffps (%u/%u) | JS=%.0f update=%.0f idle=%.0f flush=%.0f composite=%.0f (backing=%.0f paint=%.0f[b=%.0f t=%.0f e=%.0f] swap=%.0f) | acct=%.0f other=%.0f",
            g_gateFrames, g_composited300, g_skipped300, wallMs, fps, paintedFps, cpuDeltaMs, util,
            g_memUsedMB, g_memLimitMB, g_memPct, g_memLevel,
            vDeliveredFps, vDrawnFps, drawnDelta, deliveredDelta,
            g_msJS, g_msUpdate, g_msIdle, g_msFlush, g_msComposite,
            g_msBacking, g_msPaint, g_msPaintBegin, g_msPaintTree, g_msPaintEnd, g_msSwap,
            accounted, (double)cpuDeltaMs - accounted);
        portLog(b);

        // WHAT is growing. appUsage alone can't be acted on; these are the three pools we control.
        // jsHeap = JSC's GC heap. resCache = WebCore MemoryCache (live+dead resource bytes, i.e.
        // decoded images/CSS/JS we are holding). gpuTiles = compositor textures.
        {
            unsigned long long jsHeapKB = 0;
            if (JSC::VM* vm = commonVMOrNull())
                jsHeapKB = static_cast<unsigned long long>(vm->heap.size()) / 1024;
            unsigned long long resCacheKB = MemoryCache::singleton().size() / 1024;
            // Killing the filter-triggered layer explosion dropped live tiles from 248MB to 18MB --
            // and the process still climbed to 259MB with only 30MB of it accounted for. So account
            // for the rest: the texture POOL (released textures WebKit retains for reuse) and the
            // fastMalloc heap (cairo surfaces, decoded image buffers, DOM, everything else).
            auto mallocStats = WTF::fastMallocStatistics();
            unsigned long long mallocKB = static_cast<unsigned long long>(mallocStats.committedVMBytes) / 1024;
            char mb[400];
            snprintf(mb, sizeof mb, "mempool: jsHeap=%lluKB resCache=%lluKB gpuTiles=%lluKB (stores=%u layers=%u) gpuPool=%lluKB (n=%u) malloc=%lluKB | appUsage=%lluMB (%llu%% of %lluMB) lvl=%d",
                jsHeapKB, resCacheKB, WebCore::g_texmapTileBytes / 1024,
                WebCore::g_texmapStoreCount, WebCore::g_texmapLayerCount,
                WebCore::g_texmapPoolBytes / 1024, WebCore::g_texmapPoolCount, mallocKB,
                g_memUsedMB, g_memPct, g_memLimitMB, g_memLevel);
            portLog(mb);

            // Backing-phase breakdown summed over the whole gate window (per-frame spikes get their own
            // bstat line on SLOWFRAMEs; this catches the steady-state cost even when no frame trips 250ms).
            {
                char bw[640];
                bstatFormat(bw, sizeof bw, "bstatw", g_bstatWindow);
                portLog(bw);
                g_bstatWindow = {};
            }

            // Bytecode-cache hit rate + URL-parse split (proof the disk cache is READ, not just written,
            // and whether URLs needlessly take the UTF-16 path). Stats come from JSC.dll / WTF.dll via
            // exported accessors (cross-DLL global import would need extra plumbing; functions are clean).
            {
                uint64_t bc[6] = {0,0,0,0,0,0}; uint64_t url[3] = {0,0,0};
                JSC::bytecodeCacheStats(bc);
                WTF::urlParseStats(url);
                unsigned long long lookups = bc[0] + bc[1];
                unsigned hitPct = lookups ? static_cast<unsigned>(bc[0] * 100 / lookups) : 0;
                char cb[300];
                snprintf(cb, sizeof cb,
                    "bcache: hit=%llu miss=%llu (%u%%) write=%llu wfail=%llu readKB=%llu writeKB=%llu | url: parses=%llu 8bit=%llu 16bit=%llu",
                    (unsigned long long)bc[0], (unsigned long long)bc[1], hitPct,
                    (unsigned long long)bc[2], (unsigned long long)bc[3],
                    (unsigned long long)(bc[4] / 1024), (unsigned long long)(bc[5] / 1024),
                    (unsigned long long)url[0], (unsigned long long)url[1], (unsigned long long)url[2]);
                portLog(cb);
            }
        }

        g_composited300 = 0; g_skipped300 = 0; g_gateFrames = 0; g_msJS = 0; g_msComposite = 0;
        g_msBacking = 0; g_msPaint = 0; g_msSwap = 0;
        g_msUpdate = 0; g_msIdle = 0; g_msFlush = 0;
        g_msPaintBegin = 0; g_msPaintTree = 0; g_msPaintEnd = 0;
    }
    g_mainHeartbeat.fetch_add(1, std::memory_order_relaxed); // stall detector watches this
    bool trc = false; // per-frame stage trace DISABLED for perf; g_renderStage is still set (cheap)
                      // so the SEH crash handler can still report the faulting sub-step.
    // Always record the current sub-step (cheap) so the SEH crash wrapper can report where an
    // access violation happened; additionally log it during the diagnostic window.
    auto stage = [&](const char* s){ g_renderStage = s; if (trc) { char b[64]; snprintf(b, sizeof b, "rf %u %s", g_rfFrame, s); WebCorePort::portLog(b); } };
    stage("a:enter");
    MonotonicTime tFrame0 = MonotonicTime::now();
    double frameUpdateMs = 0, frameIdleMs = 0, frameFlushMs = 0; // this frame's engine-side cost
    g_renderingUpdateDoneThisFrame = false; // one "update the rendering" per frame (see below)

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

    // Shorts SPA-stall probe: YouTube fetches the reel data (reel_item_watch = 200) but the reel DOM /
    // <video> never builds on a client-side route change. On a /shorts URL, periodically log the DOM
    // state (element count, #video + their readyState/paused, reel-item + player counts) so we can see
    // exactly where its client rendering stops. Throttled + URL-gated, so ~0 cost off /shorts.
    {
        static unsigned s_shortsProbeTick = 0;
        if ((++s_shortsProbeTick % 120) == 30) {
            if (RefPtr document = frame.document()) {
                if (document->url().string().contains("shorts")) {
                    frame.script().executeScriptIgnoringException(String::fromUTF8(
                        "try{"
                        "if(!window.__revAsync){window.__revAsync=1;window.__ric=-1;window.__raf=0;window.__to=0;window.__ro=-1;window.__mo=-1;"
                        "if(typeof requestIdleCallback!=='undefined'){window.__ric=0;requestIdleCallback(function(){window.__ric=1});}"
                        "requestAnimationFrame(function(){window.__raf=1});setTimeout(function(){window.__to=1},50);"
                        "try{if(typeof ResizeObserver!=='undefined'){window.__ro=0;var _ro=new ResizeObserver(function(){window.__ro=1});_ro.observe(document.body);}}catch(e){window.__ro=9;}"
                        "try{if(typeof MutationObserver!=='undefined'){window.__mo=0;var _mo=new MutationObserver(function(){window.__mo=1});_mo.observe(document.body,{childList:true,subtree:true});}}catch(e){window.__mo=9;}}"
                        "console.warn('ASYNCTEST hasRIC='+(typeof requestIdleCallback)+' ric='+window.__ric+' raf='+window.__raf+' to='+window.__to+' ro='+window.__ro+' mo='+window.__mo);"
                        "var v=document.querySelector('video');"
                        "var s='SHORTSDIAG vis='+document.visibilityState+' els='+document.getElementsByTagName('*').length"
                        "+' reels='+document.querySelectorAll('ytm-reel-item-renderer,[id^=reel-video]').length;"
                        "if(v){var r=v.getBoundingClientRect();s+=' vid{rdy='+v.readyState+' net='+v.networkState+' src='+(v.src?v.src.slice(0,24):'EMPTY')"
                        "+' rect='+Math.round(r.width)+'x'+Math.round(r.height)+'}';}else{s+=' NO-VIDEO';}"
                        "console.warn(s);}catch(e){console.warn('SHORTSDIAG err '+e);}"), false);
                }
            }
        }
    }
    // Deliver a vsync tick to WebCore's display link (drives rAF timestamps + the
    // RenderingUpdateScheduler's own trigger path).
    // This ONE call is the entire HTML "update the rendering" step (style resolve, layout, rAF
    // callbacks, Intersection/ResizeObserver) -- it reaches RenderingUpdateScheduler synchronously.
    // It was outside every timer.
    MonotonicTime tUpdate0 = MonotonicTime::now();
    WebCoreBrowserVsyncTick();
    frameUpdateMs = (MonotonicTime::now() - tUpdate0).milliseconds();
    g_msUpdate += frameUpdateMs;
    stage("c:vsync");

    // The HTML "update the rendering" step is ENGINE-DRIVEN — we do NOT hand-pump it every frame.
    // WebCore arms its RenderingUpdateScheduler (Page::scheduleRenderingUpdate) whenever a visual
    // update is needed: style/layout invalidation, requestAnimationFrame, Intersection/ResizeObserver,
    // media, and SPA DOM mutations. Our always-on PortDisplayRefreshMonitor delivers the vsync tick
    // above SYNCHRONOUSLY to that scheduler on the main thread:
    //   WebCoreBrowserVsyncTick -> PortDisplayRefreshMonitor::fireVsync -> DisplayRefreshMonitor::
    //   displayLinkFired -> displayDidRefresh -> RenderingUpdateScheduler::displayRefreshFired ->
    //   PortChromeClient::triggerRenderingUpdate -> WebCoreDriverRunRenderingUpdate().
    // So on any frame the engine scheduled an update, it ALREADY ran inside WebCoreBrowserVsyncTick().
    // The old unconditional per-frame call was only needed while the display monitor was idle-throttled
    // and failed to re-arm on client-side route changes; making the monitor always-on fixed that at the
    // source, so the hand-pump is gone. g_renderingUpdateDoneThisFrame tells us whether it ran.
    stage("c0:renderingUpdate");
    // ANIMATION FIX: the engine-driven rendering update (above, inside VsyncTick) advances CSS
    // animations/transitions, FIRES their animationend/transitionend events, and services rAF — but it
    // only runs on frames where WebCore armed its RenderingUpdateScheduler, and during CSS animations
    // that arming is unreliable on this port (measured: it ran on ~5% of frames). A CSS animation that
    // never gets a rendering update freezes mid-flight and NEVER fires its end event — so YouTube's
    // bottom-sheet close animation stalled and its dark scrim stayed composited forever (screen stuck
    // darkened, uninteractable). While the page is visually ACTIVE — a keep-alive window (tap/scroll/
    // nav/video) or WebCore produced a visual change within the last ~30 frames, i.e. an animation is in
    // progress and repainting — force the rendering update every frame so the animation actually runs to
    // completion and fires its end event. When the page goes idle (~30 frames with no change) this stops
    // and the engine-driven path resumes, preserving the static-page idle savings.
    bool recentlyActive = (g_composeFrame < g_forceComposeUntil)
        || (g_composeFrame - g_lastActiveComposeFrame) < 30;
    if (!g_renderingUpdateDoneThisFrame && recentlyActive)
        WebCoreDriverRunRenderingUpdate();
    { static unsigned s_rupdTick = 0;
      if ((++s_rupdTick % 120) == 5) {
          char b[112]; snprintf(b, sizeof b, "rupd: updateRendering this frame=%d active=%d (engine-armed + activity-forced)",
              g_renderingUpdateDoneThisFrame ? 1 : 0, recentlyActive ? 1 : 0);
          WebCorePort::portLog(b); } }

    // Idle period: run pending requestIdleCallback callbacks with a small per-frame budget — this is
    // the HTML event loop's idle period, which runs after a frame's rendering while there is time
    // before the next frame. Our headless port has no implicit idle period and the task-based idle
    // path starves under load (its 50ms deadline expires across frames; the timeout is unimplemented),
    // so idle callbacks otherwise NEVER fire. SPA frameworks that defer non-critical rendering to
    // requestIdleCallback — YouTube's Shorts/reel feed — never build their DOM without this.
    stage("c0b:idle");
    MonotonicTime tIdle0 = MonotonicTime::now();
    if (RefPtr document = frame.document()) {
        if (auto* idle = document->idleCallbackController()) {
            if (idle->runIdleCallbacksWithDeadline(MonotonicTime::now() + Seconds::fromMilliseconds(8))
                || idle->hasPendingIdleCallbacks())
                WebCoreBrowserKeepCompositing(4); // callbacks likely mutated the DOM -> keep painting
        }
    }
    frameIdleMs = (MonotonicTime::now() - tIdle0).milliseconds();
    g_msIdle += frameIdleMs;

    // Composite WebCore's compositing layer tree directly. With forceCompositingMode +
    // setDeviceScaleFactor, rootGraphicsLayer() is a SINGLE tree that paints the ENTIRE document
    // (its root content layer paints the non-composited content; composited elements are children),
    // already at device px. No flat FrameView layer, no scale wrapper, no manual dirty tracking.
    stage("c1:flush");
    // Sync WebCore's pending compositing changes into the GraphicsLayer/TextureMapper tree. The
    // scheduler's updateRendering does this when the scene changes; calling it here is cheap when
    // already flushed and covers the first frame after the root layer is attached.
    MonotonicTime tFlush0 = MonotonicTime::now();
    view->flushCompositingStateIncludingSubframes();
    frameFlushMs = (MonotonicTime::now() - tFlush0).milliseconds();
    g_msFlush += frameFlushMs;

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
        if (sceneChanged)
            g_lastActiveComposeFrame = g_composeFrame; // feeds the animation-fix "recentlyActive" window
        needComposite = sceneChanged || (g_composeFrame < g_forceComposeUntil);
    }
    if (!needComposite) {
        ++g_skipped300;
        // A skipped composite is NOT a free frame: style/layout/rAF still ran. Report it, or a loop
        // pegged entirely in layout looks idle (composited=0 => no SLOWFRAME under the old check).
        double frameMs = (MonotonicTime::now() - tFrame0).milliseconds();
        if (frameMs >= 250) {
            char b[240];
            snprintf(b, sizeof b, "SLOWFRAME frame=%u total=%.0fms SKIPPED-COMPOSITE | update=%.0fms idle=%.0fms flush=%.0fms",
                g_composeFrame, frameMs, frameUpdateMs, frameIdleMs, frameFlushMs);
            portLog(b);
        }
        stage("f:skip-idle");
        return;
    }
    MonotonicTime tComposite0 = MonotonicTime::now();
    ++g_composited300;
    double frameBackingMs = 0, framePaintMs = 0; // this frame's raster / GL-submit cost (SLOWFRAME)
    double framePaintBeginMs = 0, framePaintTreeMs = 0, framePaintEndMs = 0;

    // RE-BIND OUR CONTEXT. It was made current at a1:ctx -- but that was BEFORE the runloop ran page
    // JS. Any page that touches WebGL or an accelerated canvas goes through ANGLEContext, whose
    // makeContextCurrent() binds ITS context and never restores ours (and whose destructor unbinds
    // unconditionally). We would then composite against the WebGL context's namespace, where our
    // textures and FBOs do not exist: glIsTexture/glIsFramebuffer false, glCheckFramebufferStatus 0,
    // and no GL error at all -- precisely the "isFbo=0 isTex=0 err=0x0000 nerr=0" bursts in the log --
    // and the draw that follows eventually faults inside ANGLE (pornhub.com, which makes a WebGL
    // context for fingerprinting). Binding here costs nothing when it is already current.
    g_glContext->makeContextCurrent();

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
        // Viewport-clip the huge CSS-px content layer's paint to the visible band (+half-screen margin
        // each side for scroll buffer). WebCore hands the whole page as one 412x3677 layer and full-
        // repaints it on load/scroll -> cairo/pixman rasters the ENTIRE page = the multi-second stall.
        // Feed the compositor the current visible content rect (CSS px, matches the layer's coords);
        // the clip in updateBackingStoreIfNeeded only fires for layers whose width == the CSS viewport
        // width (this content layer) and much taller than the viewport, so the physical viewport layer
        // and small composited layers are untouched.
        if (g_contentClient.frame) {
            if (WebCore::FrameView* fv = g_contentClient.frame->view()) {
                WebCore::IntRect vis = fv->visibleContentRect();
                // Scroll-buffer margin. This was HALF A SCREEN each way, making the band 2x the viewport
                // -- and the band is what we software-raster in cairo on EVERY full-page invalidation
                // (480x1224 CSS at 1.75 DPR = ~1.8M px, which the stall profiler caught burning seconds
                // inside pixman on pornhub.com until the watchdog killed the app). A quarter-screen keeps
                // a usable scroll buffer at ~60% of the raster cost. (Tall pages invalidate as one united
                // bounding rect, so this raster runs even when the real change is small -- keeping the
                // band tight is the cheapest lever we have short of async/off-thread raster.)
                int margin = g_cssH / 4;
                int top = vis.y() - margin;
                if (top < 0) top = 0;
                // Horizontal band too. This used to hand over x=0, width=g_cssW unconditionally, which
                // was harmless while every page laid out at ~viewport width -- but a DESKTOP-width
                // layout (pornhub's video page: a 1922 CSS px wide layer) then got clipped vertically
                // and rastered/tiled across its FULL width, ~4.7x the pixels that can ever be on screen.
                // The clip now gates each axis, so it needs the real horizontal scroll offset + margin.
                int marginX = g_cssW / 4;
                int left = vis.x() - marginX;
                if (left < 0) left = 0;
                WebCore::setTexmapVisibleRect(left, top, g_cssW + marginX * 2, vis.height() + margin * 2);
            }
        }
        stage("c3:updateBacking");
        MonotonicTime tBacking0 = MonotonicTime::now();
        tmRoot.updateBackingStoreIncludingSubLayers(*g_textureMapper);
        frameBackingMs = (MonotonicTime::now() - tBacking0).milliseconds();
        g_msBacking += frameBackingMs; // CPU raster (cairo) + GL texture upload
        bstatHarvestFrame(g_bstatFrame); // snapshot+reset this frame's backing counters (printed on SLOWFRAME)
        stage("c4:beginPaint");
        MonotonicTime tPaint0 = MonotonicTime::now();
        g_textureMapper->beginPainting();
        double beginMs = (MonotonicTime::now() - tPaint0).milliseconds();
        g_msPaintBegin += beginMs;
        MonotonicTime tTree0 = MonotonicTime::now();
        // Apply device scale (DPR) to the whole composited tree. WebCore lays out in CSS px and its
        // GraphicsLayers are CSS-px sized, but our framebuffer is device px (cssW*DPR x cssH*DPR). A
        // hand-driven TextureMapper composite has no LayerTreeHost to apply the device-scale matrix,
        // so we set it on the root layer here — post-flush, every frame, since the c1 flush resets
        // the root to WebCore's identity transform. Without this the tree paints at 1x into the
        // top-left of the device framebuffer (content stuck in the corner). TMPAINT tsx100 -> ~175.
        tmRoot.layer().setTransform(TransformationMatrix().scale(g_deviceScale));
        stage("c5:layerPaint");
        tmRoot.layer().paint(*g_textureMapper);
        double treeMs = (MonotonicTime::now() - tTree0).milliseconds();
        g_msPaintTree += treeMs;
        stage("c6:endPaint");
        MonotonicTime tEnd0 = MonotonicTime::now();
        g_textureMapper->endPainting();
        double endMs = (MonotonicTime::now() - tEnd0).milliseconds();
        g_msPaintEnd += endMs;
        framePaintMs = (MonotonicTime::now() - tPaint0).milliseconds();
        g_msPaint += framePaintMs; // GPU paint-submit (TextureMapperGL draw calls)
        framePaintBeginMs = beginMs; framePaintTreeMs = treeMs; framePaintEndMs = endMs;
    }
    stage("c7:swap");
    MonotonicTime tSwap0 = MonotonicTime::now();
    g_glContext->swapBuffers();
    double swapMs = (MonotonicTime::now() - tSwap0).milliseconds();
    g_msSwap += swapMs; // present / eglSwapBuffers (absorbs GPU stall + vsync block)
    double compositeMs = (MonotonicTime::now() - tComposite0).milliseconds();
    g_msComposite += compositeMs; // clear+backing-upload+paint+swap

    // Name any frame that is slow ON ITS OWN. The loop collapses to ~1 fps on these pages, but each
    // frame stays just under the 3s MAIN-STALL threshold, so nothing was ever reported: the stall
    // detector stayed quiet and the frame-counted gate went silent. A per-frame line makes the
    // expensive phase (cairo raster vs GL submit vs present) impossible to miss.
    double frameMs = (MonotonicTime::now() - tFrame0).milliseconds();
    if (frameMs >= 250) {
        char b[320];
        snprintf(b, sizeof b, "SLOWFRAME frame=%u total=%.0fms | update=%.0f idle=%.0f flush=%.0f composite=%.0f (backing=%.0f paint=%.0f[begin=%.0f tree=%.0f end=%.0f] swap=%.0f)",
            g_composeFrame, frameMs, frameUpdateMs, frameIdleMs, frameFlushMs, compositeMs,
            frameBackingMs, framePaintMs, framePaintBeginMs, framePaintTreeMs, framePaintEndMs, swapMs);
        portLog(b);
        // When the backing phase is the slow part, break it down: invalidation source, raster vs upload,
        // tile/texture churn. g_bstatFrame was harvested right after updateBackingStoreIncludingSubLayers,
        // so it describes exactly this frame.
        if (frameBackingMs >= 250) {
            char bb[640];
            bstatFormat(bb, sizeof bb, "bstat", g_bstatFrame);
            portLog(bb);
        }
    }
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

// Scroll the page by (dx, dy) device pixels at finger position (x, y) (touch drag).
// We do NOT hand-roll scrolling: we synthesize a PlatformWheelEvent and feed it to WebCore's real
// EventHandler::handleWheelEvent — the same engine path GTK/WPE/Mac use. It hit-tests at the event
// position, resolves the target scrollable area, chains nested scrollers, and honors scroll-snap /
// overflow / iframes correctly. Async scrolling is OFF in this build, so it scrolls synchronously on
// the main thread. Wheel delta is REVERSED from scroll direction (ScrollAnimator: scrollDelta =
// pixelStep * -deltaY), so a drag that should increase scroll offset by dy uses deltaY = -dy.
// C++ objects (PlatformWheelEvent) live here — __try cannot coexist with values needing unwind, so
// the SEH wrapper is the separate extern "C" function below (same split as tapImpl/keyDownUpImpl).
static void scrollByImpl(int dx, int dy, int x, int y)
{
    using namespace WebCore;
    Frame& frame = g_page->mainFrame();
    IntPoint pos(x, y);
    PlatformWheelEvent wheel(pos, pos,
        static_cast<float>(-dx), static_cast<float>(-dy),   // deltas (px), reversed from scroll dir
        static_cast<float>(-dx) / 120.0f, static_cast<float>(-dy) / 120.0f, // wheel ticks
        ScrollByPixelWheelEvent,
        false, false, false, false);                        // shift/ctrl/alt/meta
    constexpr OptionSet<WheelEventProcessingSteps> steps {
        WheelEventProcessingSteps::MainThreadForScrolling,                 // perform the scroll here
        WheelEventProcessingSteps::MainThreadForBlockingDOMEventDispatch,  // fire wheel/scroll to JS (snap, infinite-scroll)
    };
    frame.eventHandler().handleWheelEvent(wheel, steps);
    WebCoreBrowserKeepCompositing(8); // ensure the scroll paints even if invalidation lags
}

extern "C" void WebCoreBrowserScrollBy(int dx, int dy, int x, int y)
{
    if (!g_page)
        return;
    // handleWheelEvent runs the synchronous scroll -> style/layout -> compositing-update path (the
    // cmplayer: layer-promotion churn). A fault or C++ throw down there (crashprobe showed a repeated
    // 0xE06D7363 from a WinRT graphics call during layer rebuild) had NO handler on this path and,
    // uncaught, escaped to the WinRT caller -> RoFailFast killed the app: the long-standing "closes on
    // scroll while a video plays". WebCore is built /EHs- (no C++ unwind), so a C++ try/catch can't
    // reliably catch it — use SEH like WebCoreBrowserRenderFrameSafe. rfCrashFilter catches every code
    // (incl. 0xE06D7363) and records it; we log + survive so the next frame retries instead of dying.
    __try {
        scrollByImpl(dx, dy, x, y);
    } __except (rfCrashFilter(GetExceptionInformation())) {
        static unsigned s_scrollCrashes = 0;
        if (++s_scrollCrashes <= 20) {
            uintptr_t base = reinterpret_cast<uintptr_t>(&__ImageBase);
            uintptr_t rva = reinterpret_cast<uintptr_t>(g_crashAt) - base;
            char b[224];
            snprintf(b, sizeof b, "SCROLL-CRASH #%u code=0x%08lx at=%p base=0x%08zx rva=0x%08zx fault=%p (survived; see crashprobe VEH type=)",
                s_scrollCrashes, g_crashCode, g_crashAt, (size_t)base, (size_t)rva, g_crashFault);
            WebCorePort::portLog(b);
        }
    }
}

// Release memory in response to the platform (W10M AppMemoryUsage) pressure signal. critical!=0 means
// we're near the AppContainer limit (High/OverLimit) — do the aggressive release + also let JSC know it
// is under pressure so it GCs harder and grows its heap less. Must run on the main thread (the shell's
// render/UI thread). This is the lever that stops the 290MB spike -> memory-manager thrash -> 60s hang.
extern "C" void WebCoreBrowserReleaseMemory(int critical)
{
    using namespace WebCore;
    // Direct, synchronous release (don't wait for a posted event — we may be seconds from the OOM cap).
    // releaseMemory() frees the MemoryCache, back/forward cache, decoded image data, font/glyph caches,
    // and GCs the JSC heap. critical=Yes releases more aggressively (drops even live-ish resources).
    //
    // BRACKETED: every recent death has the log stopping immediately after a "pressure ... ->
    // releaseMemory" line, at ~78MB of a 390MB cap -- i.e. nowhere near OOM. If "releaseMemory done"
    // never appears, the process is dying INSIDE this call (it tears down live GPU/graphics resources,
    // among other things) rather than being OOM-killed, and that is a completely different bug.
    portLog(critical ? "mem: releaseMemory begin (critical)" : "mem: releaseMemory begin");
    WebCore::releaseMemory(critical ? Critical::Yes : Critical::No, Synchronous::Yes);
    portLog("mem: releaseMemory done");
}

// A tap at (x, y) device pixels: a synthetic left click (focuses fields, follows links).
static void logKeyCrash(const char* which); // defined below; shared crash reporter for input entrypoints

// Body split out so the extern "C" entrypoint can SEH-wrap it: a <select>/dropdown tap dispatches into
// RenderMenuList::showPopup() and other layout-reentrant paths that can hard-fault, and unlike the render
// pump and key/char entrypoints this one was NOT guarded -- a fault here killed the whole app with no report.
// __try cannot coexist with C++ objects needing unwind (String/RefPtr/PlatformMouseEvent), so they live here.
static void tapImpl(int x, int y)
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
        // Describe an element as tag#id.class (first class only) so a fall-through to HTML/BODY vs a
        // real overlay backdrop is distinguishable, and the modal's structure is visible on repro.
        auto describe = [](Node* n) -> std::string {
            if (!is<Element>(n))
                return n ? std::string(n->nodeName().utf8().data()) : std::string("<none>");
            Element& e = downcast<Element>(*n);
            std::string s = e.tagName().utf8().data();
            const AtomString& id = e.getIdAttribute();
            if (!id.isEmpty()) { s += "#"; s += id.string().utf8().data(); }
            const AtomString& cls = e.getAttribute(HTMLNames::classAttr);
            if (!cls.isEmpty()) { s += "."; s += cls.string().left(40).utf8().data(); }
            return s;
        };
        // Ancestor chain from the hit node up to <html> — reveals whether the tap landed inside the
        // settings modal/backdrop or fell straight through to the document root.
        std::string chain;
        for (RefPtr a = node; a && chain.size() < 240; a = a->parentNode()) {
            if (!chain.empty()) chain += " < ";
            chain += describe(a.get());
        }
        portLog((std::string("tap: x=") + std::to_string(x) + " y=" + std::to_string(y)
            + " cssVp=" + std::to_string(g_cssW) + "x" + std::to_string(g_cssH)
            + " hit=" + tag.utf8().data() + " txt=" + txt.utf8().data()
            + " chain=" + chain).c_str());
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

extern "C" void WebCoreBrowserTap(int x, int y)
{
    __try { tapImpl(x, y); }
    __except (rfCrashFilter(GetExceptionInformation())) { logKeyCrash("tap"); }
}

#if ENABLE(TOUCH_EVENTS)
namespace {
// PlatformTouchPoint/PlatformTouchEvent have protected members and only default ctors; derive to
// set their fields. Standard WebKit pattern for ports synthesizing touch events without a native
// builder. The subclasses add no data members, so slicing to the base on copy is lossless.
class DriverTouchPoint final : public WebCore::PlatformTouchPoint {
public:
    DriverTouchPoint(unsigned identifier, State s, WebCore::IntPoint position)
    {
        m_id = identifier;
        m_state = s;
        m_pos = position;
        m_screenPos = position;
    }
};
class DriverTouchEvent final : public WebCore::PlatformTouchEvent {
public:
    DriverTouchEvent(WebCore::PlatformEvent::Type t, WTF::Vector<WebCore::PlatformTouchPoint>&& pts, WTF::WallTime ts)
    {
        m_type = t;
        m_timestamp = ts;
        m_touchPoints = WTFMove(pts);
    }
};
} // namespace
#endif

// Real multitouch from the shell. phase: 0=down 1=move 2=up 3=cancel; identifier = WinRT PointerId.
// Maintains the active touch set (keyed by id) and dispatches a PlatformTouchEvent carrying ALL
// current points — the changed one gets the phase state, the others TouchStationary — which is the
// model WebCore's touch handling expects. Backs navigator.maxTouchPoints=5 with genuine
// touchstart/touchmove/touchend so touch-driven sites AND fingerprinters (Cloudflare) see real
// touch behavior. The shell still runs mouse tap/scroll as a fallback for single-finger use.
extern "C" void WebCoreBrowserTouch(int identifier, int phase, int x, int y)
{
#if ENABLE(TOUCH_EVENTS)
    using namespace WebCore;
    if (!g_page)
        return;
    Frame& frame = g_page->mainFrame();

    static std::map<unsigned, std::pair<IntPoint, PlatformTouchPoint::State>> s_active;

    unsigned id = static_cast<unsigned>(identifier);
    IntPoint pos(x, y);
    PlatformEvent::Type evType;
    PlatformTouchPoint::State changedState;
    switch (phase) {
    case 0: evType = PlatformEvent::TouchStart;  changedState = PlatformTouchPoint::TouchPressed;    break;
    case 1: evType = PlatformEvent::TouchMove;   changedState = PlatformTouchPoint::TouchMoved;      break;
    case 2: evType = PlatformEvent::TouchEnd;    changedState = PlatformTouchPoint::TouchReleased;   break;
    default: evType = PlatformEvent::TouchCancel; changedState = PlatformTouchPoint::TouchCancelled; break;
    }
    s_active[id] = { pos, changedState };

    Vector<PlatformTouchPoint> points;
    points.reserveInitialCapacity(s_active.size());
    for (auto& kv : s_active) {
        PlatformTouchPoint::State st = (kv.first == id) ? changedState : PlatformTouchPoint::TouchStationary;
        points.append(DriverTouchPoint(kv.first, st, kv.second.first));
    }

    DriverTouchEvent evt(evType, WTFMove(points), WallTime::now());
    frame.eventHandler().handleTouchEvent(evt);

    // Drop released/cancelled points only AFTER they've been delivered in this event.
    if (phase == 2 || phase == 3)
        s_active.erase(id);

    WebCoreBrowserKeepCompositing(4);
#else
    UNUSED_PARAM(identifier); UNUSED_PARAM(phase); UNUSED_PARAM(x); UNUSED_PARAM(y);
#endif
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
    // Interrupt the current page immediately: cancel all in-flight loads before starting the new nav.
    // Without this, typing a new address while a heavy page is still loading left the old page's
    // fetches/scripts running (they keep pegging the one thread), so the switch felt stuck.
    // stopAllLoaders() aborts the main frame AND recurses into child frames.
    frame.loader().stopAllLoaders();
    // Fresh page, fresh attempt at GPU compositing. These were documented as "reset on navigation"
    // but never actually were: one page tripping 3 GPU-paint crashes silently locked EVERY later page
    // into the flat CPU render path (FlattenCompositingLayers full-page cairo) for the app's lifetime.
    g_gpuTreeCrashes = 0;
    g_flatOnlyFallback = false;
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
    double newRaster = g_deviceScale > 3.0 ? 2.5 : g_deviceScale; // keep the I-01 cap consistent on rotate
    if (newRaster != g_rasterScale) {
        g_rasterScale = newRaster;
        g_page->setDeviceScaleFactor(g_rasterScale);
    }
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
