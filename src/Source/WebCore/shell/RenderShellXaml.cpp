// ============================================================================
// RenderShellXaml.cpp — Revenant browser shell. C++/CX (/ZW) UWP XAML app.
//
// Layout: a root Grid with two rows — row 0 (star) is a full-bleed SwapChainPanel that hosts
// the WebCore engine (rendered via ANGLE), row 1 (auto) is the bottom command bar (stock
// W10M-Edge chrome). Because the web panel is a star row above the auto chrome row, XAML sizes
// it to "screen minus chrome" automatically; the OS soft-key bar hiding/showing (or rotation)
// re-fires the panel's SizeChanged and we reflow. A thin blue ProgressBar rides the top edge of
// the chrome during loads.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <roapi.h>  // RoInitialize for the background memory-watcher thread
#include <string>
#include <thread>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Display;
using namespace Windows::Storage;
using namespace Windows::UI;
using namespace Windows::UI::Core;
using namespace Windows::UI::Text;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::ViewManagement;

extern "C" int WebCoreBrowserInitPanel(void* swapChainPanel, int pxW, int pxH, double deviceScale, const char* url);
extern "C" void WebCoreBrowserRenderFrame();
extern "C" void WebCoreBrowserRenderFrameSafe();
extern "C" void WebCoreBrowserLog(const char*);
extern "C" void WebCoreBrowserResize(int pxW, int pxH, double deviceScale);
extern "C" void WebCoreBrowserTap(int x, int y);
extern "C" void WebCoreSetMemoryBudgetFromLimit(unsigned long long appLimitBytes);
extern "C" void WebCoreBrowserTouch(int identifier, int phase, int x, int y); // 0=down 1=move 2=up 3=cancel
extern "C" void WebCoreBrowserScrollBy(int dx, int dy, int x, int y);
extern "C" void WebCoreBrowserNavigate(const char* url);
extern "C" void WebCoreBrowserKeepCompositing(unsigned frames);
extern "C" void WebCoreBrowserReleaseMemory(int critical); // release WebKit caches on memory pressure
extern "C" void WebCoreBrowserMemEmergency(); // background watcher -> frame pump: release at first opportunity
extern "C" void PortMseEmergencyTrim();       // off-main-thread MSE buffered-media trim (ShellMse.cpp)
extern "C" void WebCoreBrowserSetMemStats(unsigned long long usedBytes, unsigned long long limitBytes,
    unsigned long long pct, int level); // publish app memory to WebKit (footprint + gate line)
extern "C" void WebCoreBrowserForceRepaint();
extern "C" void WebCoreBrowserStage(const char*); // stall-detector breadcrumb (string literals only)
extern "C" void WebCoreBrowserGoBack();
extern "C" void WebCoreBrowserReload();
extern "C" void WebCoreBrowserStop();
extern "C" int WebCoreBrowserCanGoBack();
extern "C" int WebCoreBrowserIsLoading();
extern "C" double WebCoreBrowserProgress();
extern "C" const char* WebCoreBrowserCurrentURL();
extern "C" void PortSetDebugLogPathW(const wchar_t*);
extern "C" void PortSetCACertData(const uint8_t*, unsigned);
extern "C" void WebCoreBrowserSetDataPath(const char*);
extern "C" void WebCoreBrowserKey(int windowsVirtualKeyCode, int isDown, int shift, int ctrl, int alt);
extern "C" void WebCoreBrowserChar(unsigned codepoint, int shift, int ctrl, int alt);
namespace WebCorePort { void portLog(const char*); }

extern "C" {
unsigned short __builtin_bswap16(unsigned short x) { return _byteswap_ushort(x); }
unsigned int __builtin_bswap32(unsigned int x) { return _byteswap_ulong(x); }
unsigned long long __builtin_bswap64(unsigned long long x) { return _byteswap_uint64(x); }
}

// Stock W10M-Edge dark chrome colors.
static SolidColorBrush^ brush(unsigned char r, unsigned char g, unsigned char b, unsigned char a = 255)
{
    Windows::UI::Color c; c.R = r; c.G = g; c.B = b; c.A = a;
    return ref new SolidColorBrush(c);
}

ref class RevenantApp sealed : public Application {
public:
    RevenantApp()
    {
        // XAML's own exception + background hooks. UnhandledErrorDetected (installed in main) covers
        // WinRT errors that fail-fast; this covers exceptions XAML dispatches and swallows, and the
        // EnteredBackground transition that precedes a shell-driven kill.
        UnhandledException += ref new UnhandledExceptionEventHandler(
            [](Platform::Object^, UnhandledExceptionEventArgs^ e) {
                char b[192];
                snprintf(b, sizeof b, "app: XAML-UNHANDLED hr=0x%08X msg=%ls",
                    static_cast<unsigned>(e->Exception.Value),
                    e->Message ? e->Message->Data() : L"");
                WebCorePort::portLog(b);
            });
        EnteredBackground += ref new Windows::UI::Xaml::EnteredBackgroundEventHandler(
            [](Platform::Object^, Windows::ApplicationModel::EnteredBackgroundEventArgs^) {
                WebCorePort::portLog("app: ENTERED-BACKGROUND");
            });
        LeavingBackground += ref new Windows::UI::Xaml::LeavingBackgroundEventHandler(
            [](Platform::Object^, Windows::ApplicationModel::LeavingBackgroundEventArgs^) {
                WebCorePort::portLog("app: LEAVING-BACKGROUND");
            });
        Suspending += ref new Windows::UI::Xaml::SuspendingEventHandler(
            [](Platform::Object^, Windows::ApplicationModel::SuspendingEventArgs^) {
                // SHRINK BEFORE WE SLEEP. A suspended app is kept in memory only if the OS can afford
                // it; W10M terminates the fat ones to reclaim RAM. We were being suspended holding
                // 206MB and getting reclaimed -- the log shows ENTERED-BACKGROUND -> SUSPENDING ->
                // (no RESUMING) -> a cold relaunch. The suspends where we held less all resumed fine.
                // This hook existed and did nothing; dropping the caches here is the entire point of it.
                WebCorePort::portLog("app: XAML-SUSPENDING -> releaseMemory(critical) before sleep");
                WebCoreBrowserReleaseMemory(1);
            });
    }

protected:
    virtual void OnLaunched(LaunchActivatedEventArgs^) override
    {
        WebCorePort::portLog("revenant: OnLaunched");
        setupLog();
        loadCACert();

        // Keep our UI inside the VISIBLE bounds — above the OS soft-key bar (back/Windows/
        // search) and below the status bar — rather than full-bleed under them. When the
        // soft-key bar hides, the visible bounds grow and the panel's SizeChanged reflows.
        try {
            auto av = ApplicationView::GetForCurrentView();
            av->SetDesiredBoundsMode(ApplicationViewBoundsMode::UseVisible);
        } catch (...) { }

        // Root: row0 web (star), row1 chrome (auto).
        m_root = ref new Grid();
        auto root = m_root;
        auto rWeb = ref new RowDefinition(); rWeb->Height = GridLength(1, GridUnitType::Star);
        auto rBar = ref new RowDefinition(); rBar->Height = GridLength(1, GridUnitType::Auto);
        root->RowDefinitions->Append(rWeb);
        root->RowDefinitions->Append(rBar);

        m_panel = ref new SwapChainPanel();
        Grid::SetRow(m_panel, 0);
        m_panel->PointerPressed += ref new PointerEventHandler(this, &RevenantApp::onPointerPressed);
        m_panel->PointerMoved += ref new PointerEventHandler(this, &RevenantApp::onPointerMoved);
        m_panel->PointerReleased += ref new PointerEventHandler(this, &RevenantApp::onPointerReleased);
        m_panel->SizeChanged += ref new SizeChangedEventHandler(this, &RevenantApp::onSizeChanged);
        root->Children->Append(m_panel);

        auto chrome = buildChrome();
        Grid::SetRow(chrome, 1);
        root->Children->Append(chrome);

        // Hardware Back -> browser back.
        SystemNavigationManager::GetForCurrentView()->BackRequested +=
            ref new EventHandler<BackRequestedEventArgs^>(this, &RevenantApp::onBackRequested);

        Window::Current->Content = root;
        Window::Current->Activate();

        // Resume-from-background repaint. The GPU-offload gate skips the clear/paint/swap on any
        // frame WebCore reports as unchanged. When the app is backgrounded (Home / task switch) the
        // OS invalidates the swapchain backbuffers, but a now-static page produces no scene change,
        // so every post-resume frame is skipped and the black backbuffer is never redrawn (app looks
        // dead even though navigation still works). On becoming visible again, force a window of full
        // composites so the layer tree is re-painted and re-presented into the fresh buffers.
        Window::Current->VisibilityChanged +=
            ref new WindowVisibilityChangedEventHandler(this, &RevenantApp::onVisibilityChanged);

        // Keyboard for WEB PAGE inputs. The URL TextBox handles its own keys; for text fields
        // inside the page (rendered in the SwapChainPanel) there is no native control, so route
        // CoreWindow key/character events into WebCore — but only when the URL box does NOT have
        // focus (else we'd double-insert into the address bar). CharacterReceived carries typed
        // text; KeyDown/KeyUp carry navigation/editing keys.
        auto coreWin = Window::Current->CoreWindow;
        coreWin->CharacterReceived += ref new TypedEventHandler<CoreWindow^, CharacterReceivedEventArgs^>(this, &RevenantApp::onCharacterReceived);
        coreWin->KeyDown += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(this, &RevenantApp::onCoreKeyDown);
        coreWin->KeyUp += ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(this, &RevenantApp::onCoreKeyUp);

        // Inset our whole UI to the un-occluded VisibleBounds so the chrome sits ABOVE the OS
        // soft-key bar (and below the status bar), and re-inset whenever that changes (the bar
        // hiding/showing, rotation). UseVisible bounds mode alone didn't keep us clear of it.
        applyInsets();
        try {
            ApplicationView::GetForCurrentView()->VisibleBoundsChanged +=
                ref new TypedEventHandler<ApplicationView^, Object^>(this, &RevenantApp::onVisibleBoundsChanged);
        } catch (...) { }
    }

    void onVisibleBoundsChanged(ApplicationView^, Object^) { applyInsets(); }

    void applyInsets()
    {
        if (!m_root) return;
        try {
            auto av = ApplicationView::GetForCurrentView();
            Rect vb = av->VisibleBounds;         // un-occluded region (DIPs)
            Rect wb = Window::Current->Bounds;   // full window (DIPs)
            double top = vb.Y - wb.Y;                         if (top < 0) top = 0;
            double bottom = (wb.Y + wb.Height) - (vb.Y + vb.Height); if (bottom < 0) bottom = 0;
            double left = vb.X - wb.X;                        if (left < 0) left = 0;
            double right = (wb.X + wb.Width) - (vb.X + vb.Width);    if (right < 0) right = 0;
            m_root->Margin = Thickness(left, top, right, bottom);
            char b[128];
            snprintf(b, sizeof b, "revenant: insets t=%d b=%d l=%d r=%d", (int)top, (int)bottom, (int)left, (int)right);
            WebCorePort::portLog(b);
        } catch (...) { }
    }

private:
    FrameworkElement^ buildChrome()
    {
        // Dark bar; a thin blue progress line rides its top edge during loads.
        auto barGrid = ref new Grid();
        barGrid->Background = brush(0x1F, 0x1F, 0x1F);
        barGrid->Height = 48;

        // Columns: tabs | sep | reload/stop | (lock+URL, fills) | reading | sep | more
        auto cols = barGrid->ColumnDefinitions;
        auto addCol = [&](GridLength len) { auto c = ref new ColumnDefinition(); c->Width = len; cols->Append(c); };
        addCol(GridLength(1, GridUnitType::Auto));   // 0 tabs
        addCol(GridLength(1, GridUnitType::Auto));   // 1 sep
        addCol(GridLength(1, GridUnitType::Auto));   // 2 reload/stop
        addCol(GridLength(1, GridUnitType::Star));   // 3 lock + url
        addCol(GridLength(1, GridUnitType::Auto));   // 4 reading
        addCol(GridLength(1, GridUnitType::Auto));   // 5 sep
        addCol(GridLength(1, GridUnitType::Auto));   // 6 more

        auto place = [&](FrameworkElement^ e, int col) { Grid::SetColumn(e, col); barGrid->Children->Append(e); };

        place(iconButton(L"\uE7C4", ref new RoutedEventHandler(this, &RevenantApp::onTabsClick)), 0);   // tabs
        place(separator(), 1);
        m_reloadBtn = iconButton(L"\uE72C", ref new RoutedEventHandler(this, &RevenantApp::onReloadClick)); // refresh
        place(m_reloadBtn, 2);

        // Lock + URL textbox (fills).
        auto urlPanel = ref new StackPanel();
        urlPanel->Orientation = Orientation::Horizontal;
        urlPanel->VerticalAlignment = VerticalAlignment::Center;
        auto lock = ref new FontIcon();
        lock->FontFamily = ref new Media::FontFamily(L"Segoe MDL2 Assets");
        lock->Glyph = L"\uE72E";
        lock->FontSize = 14;
        lock->Foreground = brush(0x9A, 0x9A, 0x9A);
        lock->Margin = Thickness(10, 0, 6, 0);
        urlPanel->Children->Append(lock);
        m_urlBox = ref new TextBox();
        m_urlBox->BorderThickness = Thickness(0);
        m_urlBox->Background = brush(0, 0, 0, 0);
        m_urlBox->Foreground = brush(0xE6, 0xE6, 0xE6);
        m_urlBox->FontSize = 15;
        m_urlBox->VerticalAlignment = VerticalAlignment::Center;
        m_urlBox->Width = 220;
        m_urlBox->InputScope = makeUrlScope();
        m_urlBox->KeyDown += ref new KeyEventHandler(this, &RevenantApp::onUrlKeyDown);
        // Track URL-box focus so the CoreWindow key handlers don't also feed the web page while
        // the user is typing in the address bar.
        m_urlBox->GotFocus += ref new RoutedEventHandler(this, &RevenantApp::onUrlGotFocus);
        m_urlBox->LostFocus += ref new RoutedEventHandler(this, &RevenantApp::onUrlLostFocus);
        urlPanel->Children->Append(m_urlBox);
        place(urlPanel, 3);

        place(iconButton(L"\uE736", ref new RoutedEventHandler(this, &RevenantApp::onReadingClick)), 4); // reading view
        place(separator(), 5);
        place(iconButton(L"\uE10C", ref new RoutedEventHandler(this, &RevenantApp::onMoreClick)), 6);     // more (...)

        // Blue loading bar on the top edge of the chrome.
        m_progress = ref new ProgressBar();
        m_progress->Minimum = 0; m_progress->Maximum = 1;
        m_progress->Height = 2.5;
        m_progress->VerticalAlignment = VerticalAlignment::Top;
        m_progress->Foreground = brush(0x2A, 0x72, 0xC8);   // W10M accent blue
        m_progress->Background = brush(0, 0, 0, 0);
        m_progress->Visibility = ::Visibility::Collapsed;
        Grid::SetColumnSpan(m_progress, 7);
        barGrid->Children->Append(m_progress);

        return barGrid;
    }

    Button^ iconButton(String^ glyph, RoutedEventHandler^ handler)
    {
        auto b = ref new Button();
        b->Background = brush(0, 0, 0, 0);
        b->BorderThickness = Thickness(0);
        b->Padding = Thickness(14, 0, 14, 0);
        b->Height = 48;
        auto icon = ref new FontIcon();
        icon->FontFamily = ref new Media::FontFamily(L"Segoe MDL2 Assets");
        icon->Glyph = glyph;
        icon->FontSize = 18;
        icon->Foreground = brush(0xE6, 0xE6, 0xE6);
        b->Content = icon;
        b->Click += handler;
        return b;
    }

    Border^ separator()
    {
        auto s = ref new Border();
        s->Width = 1;
        s->Height = 24;
        s->Background = brush(0x40, 0x40, 0x40);
        s->VerticalAlignment = VerticalAlignment::Center;
        return s;
    }

    InputScope^ makeUrlScope()
    {
        auto scope = ref new InputScope();
        auto name = ref new InputScopeName();
        name->NameValue = InputScopeNameValue::Url;
        scope->Names->Append(name);
        return scope;
    }

    void setupLog()
    {
        try {
            auto path = ApplicationData::Current->LocalFolder->Path + L"\\debug.log";
            PortSetDebugLogPathW(path->Data());
        } catch (...) { }
    }
    void loadCACert()
    {
        try {
            auto path = Package::Current->InstalledLocation->Path + L"\\cacert.pem";
            FILE* f = _wfopen(path->Data(), L"rb");
            if (!f) return;
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                std::vector<uint8_t> buf(static_cast<size_t>(sz));
                size_t got = fread(buf.data(), 1, buf.size(), f);
                PortSetCACertData(buf.data(), static_cast<unsigned>(got));
            }
            fclose(f);
        } catch (...) { }
    }

    void onVisibilityChanged(Object^, Windows::UI::Core::VisibilityChangedEventArgs^ e)
    {
        if (e->Visible) {
            // Full re-raster, not just re-composite: suspend discards the GPU backing textures, so
            // re-presenting them shows black. ForceRepaint marks the RenderView + composited layers
            // dirty so the content is re-rastered into fresh textures, then holds the compose window.
            WebCoreBrowserForceRepaint();
            WebCorePort::portLog("revenant: visible -> force repaint (resume)");
        }
    }

    void onSizeChanged(Object^, SizeChangedEventArgs^)
    {
        if (m_panel->ActualWidth < 1.0 || m_panel->ActualHeight < 1.0)
            return;
        double raw = 1.0;
        try { raw = DisplayInformation::GetForCurrentView()->RawPixelsPerViewPixel; } catch (...) { }
        if (raw <= 0.0) raw = 1.0;
        int pxW = static_cast<int>(m_panel->ActualWidth * raw + 0.5);
        int pxH = static_cast<int>(m_panel->ActualHeight * raw + 0.5);

        if (!m_inited) {
            // Persistent cookies + localStorage live under the app's writable LocalFolder. Must be
            // set BEFORE InitPanel so the cookie session and storage provider open the on-disk DBs.
            try {
                auto data = toUtf8(ApplicationData::Current->LocalFolder->Path);
                WebCoreBrowserSetDataPath(data.c_str());
            } catch (...) { }
            // Hold the property set in a ^ local across the InitPanel call — a bare temporary
            // would be released before ANGLE finishes using it (freed native window -> crash).
            auto props = makeScaledPanel(raw);
            void* insp = reinterpret_cast<void*>(props);
            int rc = WebCoreBrowserInitPanel(insp, pxW, pxH, raw, "https://www.google.com/");
            char b[160];
            snprintf(b, sizeof b, "revenant: InitPanel rc=%d px=%dx%d scale=%.3f", rc, pxW, pxH, raw);
            WebCorePort::portLog(b);

            // BACKGROUND memory watcher. The UI-thread sampler above goes blind exactly when it matters:
            // a 3s+ main-thread stall (heavy decode/CSS at video open) stops both the readings and any
            // releaseMemory, and the run-4 death shows memory racing from 78% past the 390MB cap with
            // zero log lines. This thread keeps watching (and logging) through main-thread stalls:
            //  >=88%: flag the frame pump for an immediate critical release at its next iteration.
            //  >=92%: emergency-trim MSE buffered media directly on the WinRT buffers (thread-safe,
            //         MF owns the frames) -- the one big lever that does not need the main thread.
            //
            // Per-device memory tiering: read THIS device's real AppMemoryUsageLimit (1GB->390MB,
            // 2GB->900MB) and set the budget NOW, on the main thread (the resource-cache resize it does
            // is not safe from the watcher thread). Every lever below — release thresholds, MSE
            // keep-behind, cache size — then derives from the resulting tier instead of 1GB-hardcoded
            // constants. The watcher only READS the derived effective ceiling (pure computation).
            try {
                uint64 startupLimit = Windows::System::MemoryManager::AppMemoryUsageLimit;
                WebCoreSetMemoryBudgetFromLimit(startupLimit);
            } catch (...) { }
            std::thread([] {
                RoInitialize(RO_INIT_MULTITHREADED);
                unsigned long long lastLoggedPct = 0, lastLimit = 0;
                int lastLevel = -1;
                ULONGLONG lastEmergency = 0, lastTrim = 0;
                for (;;) {
                    Sleep(250);
                    unsigned long long used = 0, limit = 0;
                    int level = 0;
                    try {
                        used = Windows::System::MemoryManager::AppMemoryUsage;
                        limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
                        level = (int)Windows::System::MemoryManager::AppMemoryUsageLevel;
                    } catch (...) { continue; }
                    if (!limit)
                        continue;
                    // The OS LOWERS the limit dynamically under system commit pressure (observed
                    // 390MB -> 195MB mid-session, level jumping to 3=OverLimit at an unchanged
                    // usage). Log every change — it is the reaper announcing itself.
                    if (limit != lastLimit) {
                        if (lastLimit) {
                            char lb[112];
                            snprintf(lb, sizeof lb, "memwatch: LIMIT CHANGED %lluMB -> %lluMB",
                                lastLimit / (1024 * 1024), limit / (1024 * 1024));
                            WebCoreBrowserLog(lb);
                        }
                        lastLimit = limit;
                    }
                    unsigned long long pct = used * 100 / limit;
                    // Log transitions only (level change, or pct moved >=2 points) so a stable page
                    // stays quiet but a spike leaves a trail even while the UI thread is stalled.
                    if (level != lastLevel || (pct > lastLoggedPct ? pct - lastLoggedPct : lastLoggedPct - pct) >= 2) {
                        char wb[128];
                        snprintf(wb, sizeof wb, "memwatch: %lluMB (%llu%%) level=%d",
                            used / (1024 * 1024), pct, level);
                        WebCoreBrowserLog(wb);
                        lastLoggedPct = pct;
                        lastLevel = level;
                    }
                    ULONGLONG now = GetTickCount64();
                    // Thresholds derive from the EFFECTIVE kill line, PER DEVICE, not a hardcoded pct of
                    // the raw app cap. On ~1GB devices the SYSTEM commit limit binds ~50MB below the app
                    // cap (PC-side systemperf caught the reap at ~340MB while the app pct read a "safe"
                    // 80%); on >=2GB devices commit is not binding so the cap itself (minus a small
                    // margin) is the ceiling. Computed inline from the CURRENT limit so it also tracks
                    // the OS dynamically lowering the limit under system pressure. Emergency at 88% of
                    // effective, MSE trim at 93%. For the 640XL (390MB cap -> 340 effective) that is
                    // ~300MB / ~316MB — matching the old hand-tuned gates; for the 1520 (900 -> 855) it
                    // scales up to ~752MB / ~795MB instead of firing needlessly at 300MB.
                    // 15s cadence, not 5s: when the OS halves the limit (pct jumps to ~150%), a 5s
                    // cadence produced 19 critical wipes in ~90s — each re-decodes/re-JITs the page
                    // (the video stutter) while barely moving the footprint. One release per episode.
                    unsigned long long limitMB = limit / (1024 * 1024);
                    unsigned long long effMB = (limitMB < 512)
                        ? (limitMB > 60 ? limitMB - 50 : limitMB)
                        : (limitMB - limitMB / 20);
                    unsigned long long usedMB = used / (1024 * 1024);
                    if (usedMB >= effMB * 88 / 100 && now - lastEmergency >= 15000) {
                        lastEmergency = now;
                        WebCoreBrowserMemEmergency();
                    }
                    if (usedMB >= effMB * 93 / 100 && now - lastTrim >= 10000) {
                        lastTrim = now;
                        PortMseEmergencyTrim();
                    }
                }
            }).detach();

            // MSE go/no-go: does the WinRT-native Windows.Media.Core.MseStreamSource activate in OUR
            // AppContainer, and does it accept YouTube's VP9/Opus + H.264/AAC types? This is the path
            // that replaces the desktop-only IMFMediaSourceExtension (which returned 0xc00d36e6). If
            // this logs OK with vp9=1, MSE-via-MseStreamSource is green to build.
            try {
                auto mse = ref new Windows::Media::Core::MseStreamSource();
                bool vp9 = mse->IsContentTypeSupported(ref new Platform::String(L"video/webm; codecs=\"vp9,opus\""));
                bool h264 = mse->IsContentTypeSupported(ref new Platform::String(L"video/mp4; codecs=\"avc1.42E01E,mp4a.40.2\""));
                char mb[160]; snprintf(mb, sizeof mb, "mse-probe: MseStreamSource OK vp9=%d h264=%d", vp9 ? 1 : 0, h264 ? 1 : 0);
                WebCorePort::portLog(mb);
            } catch (Platform::Exception^ e) {
                char mb[160]; snprintf(mb, sizeof mb, "mse-probe: MseStreamSource FAILED hr=0x%08x", (unsigned)e->HResult);
                WebCorePort::portLog(mb);
            } catch (...) {
                WebCorePort::portLog("mse-probe: MseStreamSource FAILED (unknown exception)");
            }

            m_inited = true;
            m_deviceScale = raw;
            CompositionTarget::Rendering += ref new EventHandler<Object^>(this, &RevenantApp::onRendering);
        } else {
            // The render area changed (soft-key bar toggled, rotation): reflow the page.
            WebCoreBrowserResize(pxW, pxH, raw);
        }
    }

    // Build the ANGLE configured property set (panel + render RESOLUTION SCALE).
    //
    // We deliberately pass EGLRenderResolutionScaleProperty (a scale) and NOT
    // EGLRenderSurfaceSizeProperty (a fixed size). A fixed size pins ANGLE's backbuffer to the
    // initial (portrait) dimensions forever: on rotation ANGLE's SwapChainPanelNativeWindow takes
    // the size-specified branch of setNewClientSize, which only sets a stretch matrix
    // (scaleSwapChain) and never flags mClientRectChanged — so the portrait backbuffer is just
    // distorted to fill the landscape panel (the squished, black-barred landscape we saw).
    // With a scale instead, mSwapChainSizeSpecified is false, so every SizeChanged (rotation)
    // recomputes clientRect = panelSize x scale and ResizeBuffers the backbuffer to the new
    // orientation, at full physical resolution. The two properties are mutually exclusive.
    // Init render is unchanged: the composite matrix is 1/scale either way.
    Windows::Foundation::Collections::PropertySet^ makeScaledPanel(double scale)
    {
        auto props = ref new Windows::Foundation::Collections::PropertySet();
        props->Insert(L"EGLNativeWindowTypeProperty", m_panel);
        props->Insert(L"EGLRenderResolutionScaleProperty",
            PropertyValue::CreateSingle(static_cast<float>(scale <= 0.0 ? 1.0 : scale)));
        return props;
    }

    void onRendering(Object^, Object^)
    {
        WebCoreBrowserRenderFrameSafe();
        WebCoreBrowserStage("x1:shell-mem");

        // Periodic memory-usage sample (every ~120 frames ≈ 2s). Uses the UWP app-memory API so we
        // can see the trend leading into a freeze/slowdown (climbing usage → GC thrash / hitting the
        // app cap). AppMemoryUsageLevel: 0=Low 1=Medium 2=High 3=OverLimit.
        // Check the AppContainer memory level EVERY frame (cheap WinRT property read) so we can release
        // WebKit caches the moment the main thread is free after a memory-ballooning burst — a heavy page
        // can jump 80MB -> 290MB fast, and the OverLimit cap triggers the memory-manager thrash (60s hang).
        // level: 0=Low 1=Medium 2=High 3=OverLimit. Release runs HERE on the main thread (WebCore-safe).
        // Throttle so we don't call releaseMemory every frame while sitting at Medium: act on a rise, or
        // at most every ~45 frames if still elevated.
        {
            // Read the REAL numbers every frame, not just the coarse level. This whole block used to
            // throttle on a FRAME counter (every 120 frames): fine at 60fps, useless at the 1fps the
            // heavy pages collapse to, where 120 frames is two minutes. The last appUsage reading
            // before the app died was 97MB, taken ages earlier -- so the climb into the OS kill was
            // completely unmeasured. Everything here is now wall-clock based.
            uint64 used = Windows::System::MemoryManager::AppMemoryUsage;
            uint64 limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
            int level = (int)Windows::System::MemoryManager::AppMemoryUsageLevel;
            unsigned long long pct = limit ? (unsigned long long)(used * 100 / limit) : 0ULL;

            // Publish to the driver. This is not just for the log line: WebKit's memoryFootprint()
            // is a hard 0 on this port (PSAPI is desktop-only), so this call is the ONLY way the
            // engine learns how much memory it is using -- and without it RenderLayerCompositor never
            // leaves CompositingPolicy::Normal and promotes layers until the OS kills us. Bytes, not
            // MB: the pressure thresholds are computed from it.
            WebCoreBrowserSetMemStats((unsigned long long)used, (unsigned long long)limit, pct, level);

            ULONGLONG now = GetTickCount64();
            if (!m_lastMemLogTick || now - m_lastMemLogTick >= 2000) {
                m_lastMemLogTick = now;
                char b[192];
                sprintf_s(b, sizeof b, "mem: appUsage=%lluMB / limit=%lluMB (%llu%%) level=%d",
                    (unsigned long long)(used / (1024 * 1024)),
                    (unsigned long long)(limit / (1024 * 1024)), pct, level);
                WebCoreBrowserLog(b);
            }

            // Release on a level RISE immediately. Do NOT re-release on a sustained Medium: a heavy
            // page sits at Medium for its whole life under the 390MB cap, and a synchronous full GC +
            // cache wipe every couple of seconds is jank, not relief.
            //
            // But the LEVEL is too coarse to keep us alive on its own -- it has read Medium at 89% of
            // the cap while the OS was about to kill us. The percentage is the real guard, and it must
            // be checked on WALL TIME: it lived inside the 120-frame block, so on the pages that
            // actually approach the cap (1fps) it never ran at all.
            // 85% was too late AND too slow. chaturbate (a React SPA whose JSC heap alone hits 45MB)
            // climbed 73% -> 76% -> 81% -> KILLED in about six seconds, stepping straight over the
            // threshold without ever tripping it -- and the coarse AppMemoryUsageLevel sat at Medium
            // the whole way, so `rose` never fired either. Guard at 70%, and re-release every 5s while
            // we stay above it, so a fast climber gets shrunk before it reaches the cap.
            bool rose = level > m_lastMemLevel;
            bool sustainedCritical = level >= 2 && (!m_lastMemReleaseTick || now - m_lastMemReleaseTick >= 5000);
            bool overPct = pct >= 70 && (!m_lastMemReleaseTick || now - m_lastMemReleaseTick >= 5000);
            if ((level >= 1 && (rose || sustainedCritical)) || overPct) {
                char rb[96];
                sprintf_s(rb, sizeof rb, "mem: pressure level=%d pct=%llu%% -> releaseMemory(%s)",
                    level, pct, (level >= 2 || pct >= 85) ? "critical" : "normal");
                WebCoreBrowserLog(rb);
                // Match the decision to the log line above: critical (full wipe: memory cache, decoded
                // images, fonts, JIT-linked code) ONLY at High/OverLimit or >=85%. The 70% guard fires a
                // NORMAL release. Passing critical at 70% made every 5s tick a full wipe on pages that
                // live at 80% (YouTube: 25 critical releases in one session, usage never dropped -- the
                // wipes were pure re-decode/re-JIT jank with zero relief).
                WebCoreBrowserReleaseMemory((level >= 2 || pct >= 85) ? 1 : 0);
                m_lastMemReleaseTick = GetTickCount64();
            }
            m_lastMemLevel = level;
        }

        WebCoreBrowserStage("x2:shell-chrome");
        // Reflect load state in the chrome (blue bar + refresh/stop + URL).
        bool loading = WebCoreBrowserIsLoading() != 0;
        if (loading != m_lastLoading) {
            m_lastLoading = loading;
            m_progress->Visibility = loading ? ::Visibility::Visible : ::Visibility::Collapsed;
            reloadIcon()->Glyph = loading ? L"\uE711" : L"\uE72C"; // stop (X) vs refresh
        }
        if (loading)
            m_progress->Value = WebCoreBrowserProgress();

        WebCoreBrowserStage("x3:shell-url");
        // Keep the address bar in sync with the committed URL (unless the user is editing it).
        if (!m_urlEditing) {
            const char* u = WebCoreBrowserCurrentURL();
            if (u && *u) {
                auto s = ref new String(std::wstring(u, u + strlen(u)).c_str());
                if (m_urlBox->Text != s)
                    m_urlBox->Text = s;
            }
        }
        // Last marker before returning to XAML: a stall showing x9 means the freeze is in XAML's own
        // frame commit / DWM present, not in any of our code.
        WebCoreBrowserStage("x9:xaml-commit");
    }

    FontIcon^ reloadIcon() { return safe_cast<FontIcon^>(m_reloadBtn->Content); }

    void onBackRequested(Object^, BackRequestedEventArgs^ e)
    {
        if (WebCoreBrowserCanGoBack()) {
            WebCoreBrowserGoBack();
            e->Handled = true;
        }
    }

    void onUrlKeyDown(Object^, KeyRoutedEventArgs^ e)
    {
        if (e->Key == Windows::System::VirtualKey::Enter) {
            auto t = m_urlBox->Text;
            if (t && !t->IsEmpty()) {
                auto utf8 = toUtf8(t);
                WebCoreBrowserNavigate(utf8.c_str());
            }
            m_urlEditing = false;
            e->Handled = true;
        } else
            m_urlEditing = true;
    }

    // --- Web-page keyboard input (routes CoreWindow keys to WebCore when the address bar is not
    // focused; the URL TextBox handles its own input natively). ---
    void onUrlGotFocus(Object^, RoutedEventArgs^) { m_urlFocused = true; }
    void onUrlLostFocus(Object^, RoutedEventArgs^) { m_urlFocused = false; }

    static void keyMods(Windows::UI::Core::CoreWindow^ w, int& shift, int& ctrl, int& alt)
    {
        using Windows::UI::Core::CoreVirtualKeyStates;
        auto down = [w](Windows::System::VirtualKey k) {
            return (w->GetKeyState(k) & CoreVirtualKeyStates::Down) == CoreVirtualKeyStates::Down;
        };
        shift = down(Windows::System::VirtualKey::Shift) ? 1 : 0;
        ctrl  = down(Windows::System::VirtualKey::Control) ? 1 : 0;
        alt   = down(Windows::System::VirtualKey::Menu) ? 1 : 0;
    }

    void onCharacterReceived(Windows::UI::Core::CoreWindow^ w, Windows::UI::Core::CharacterReceivedEventArgs^ e)
    {
        unsigned cp = e->KeyCode;
        { char b[80]; snprintf(b, sizeof b, "INPUT: CharacterReceived cp=%u url=%d", cp, m_urlFocused ? 1 : 0); WebCorePort::portLog(b); }
        if (m_urlFocused) return;                    // address bar handles its own text
        if (cp < 0x20 || cp == 0x7F) return;         // control chars are handled via KeyDown
        int s, c, a; keyMods(w, s, c, a);
        WebCoreBrowserChar(cp, s, c, a);
    }
    void onCoreKeyDown(Windows::UI::Core::CoreWindow^ w, Windows::UI::Core::KeyEventArgs^ e)
    {
        { char b[80]; snprintf(b, sizeof b, "INPUT: KeyDown vk=%d url=%d", static_cast<int>(e->VirtualKey), m_urlFocused ? 1 : 0); WebCorePort::portLog(b); }
        if (m_urlFocused) return;
        int s, c, a; keyMods(w, s, c, a);
        WebCoreBrowserKey(static_cast<int>(e->VirtualKey), 1, s, c, a);
    }
    void onCoreKeyUp(Windows::UI::Core::CoreWindow^ w, Windows::UI::Core::KeyEventArgs^ e)
    {
        if (m_urlFocused) return;
        int s, c, a; keyMods(w, s, c, a);
        WebCoreBrowserKey(static_cast<int>(e->VirtualKey), 0, s, c, a);
    }

    void onReloadClick(Object^, RoutedEventArgs^)
    {
        if (WebCoreBrowserIsLoading()) WebCoreBrowserStop();
        else WebCoreBrowserReload();
    }
    void onTabsClick(Object^, RoutedEventArgs^) { /* TODO Step 3: tabs view */ }
    void onReadingClick(Object^, RoutedEventArgs^) { /* TODO: reading view */ }
    void onMoreClick(Object^, RoutedEventArgs^) { /* TODO Step 3: ... menu sheet */ }

    // UTF-16 (Platform::String) -> UTF-8, self-contained (no Win32 <windows.h> dependency).
    static std::string toUtf8(String^ s)
    {
        std::string out;
        if (!s) return out;
        const wchar_t* w = s->Data();
        unsigned len = s->Length();
        for (unsigned i = 0; i < len; ++i) {
            unsigned int cp = static_cast<unsigned>(w[i]);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len) {
                unsigned int lo = static_cast<unsigned>(w[i + 1]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); ++i; }
            }
            if (cp < 0x80) out += static_cast<char>(cp);
            else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        }
        return out;
    }

    void toDip(PointerRoutedEventArgs^ e, int& x, int& y)
    {
        auto pt = e->GetCurrentPoint(m_panel);
        x = static_cast<int>(pt->Position.X + 0.5f);
        y = static_cast<int>(pt->Position.Y + 0.5f);
    }
    static bool isTouch(PointerRoutedEventArgs^ e)
    {
        return e->Pointer->PointerDeviceType == Windows::Devices::Input::PointerDeviceType::Touch;
    }
    static int pointerId(PointerRoutedEventArgs^ e)
    {
        return static_cast<int>(e->Pointer->PointerId);
    }
    void onPointerPressed(Object^, PointerRoutedEventArgs^ e)
    {
        toDip(e, m_pressX, m_pressY);
        m_lastX = m_pressX; m_lastY = m_pressY;
        m_pressed = true; m_moved = false;
        // Tapping the web view moves interaction to the page: stop treating keys as address-bar
        // input so typed characters reach web inputs (the URL box may still hold XAML focus).
        m_urlFocused = false;
        // Real touch: fire a genuine touchstart so touch-driven sites + fingerprinters (Cloudflare)
        // see actual touch behind navigator.maxTouchPoints=5. Mouse tap/scroll below still runs.
        if (isTouch(e)) WebCoreBrowserTouch(pointerId(e), 0, m_pressX, m_pressY);
    }
    void onPointerMoved(Object^, PointerRoutedEventArgs^ e)
    {
        if (!m_pressed) return;
        int x = m_lastX, y = m_lastY;
        toDip(e, x, y);
        if (isTouch(e)) WebCoreBrowserTouch(pointerId(e), 1, x, y); // touchmove (each finger, per id)
        int dx = x - m_lastX, dy = y - m_lastY;
        if (dx || dy) {
            if (std::abs(x - m_pressX) > 8 || std::abs(y - m_pressY) > 8)
                m_moved = true;
            WebCoreBrowserScrollBy(m_lastX - x, m_lastY - y, x, y); // scroll element under the finger
            m_lastX = x; m_lastY = y;
        }
    }
    void onPointerReleased(Object^, PointerRoutedEventArgs^ e)
    {
        if (isTouch(e)) {
            int x = m_lastX, y = m_lastY;
            toDip(e, x, y);
            WebCoreBrowserTouch(pointerId(e), 2, x, y); // touchend
        }
        if (!m_moved) WebCoreBrowserTap(m_pressX, m_pressY);
        m_pressed = false;
    }

    Grid^ m_root;
    SwapChainPanel^ m_panel;
    TextBox^ m_urlBox;
    Button^ m_reloadBtn;
    ProgressBar^ m_progress;
    bool m_inited = false;
    bool m_urlFocused = false;
    bool m_lastLoading = false;
    unsigned m_memSampleFrame = 0;
    int m_lastMemLevel = 0;            // last AppMemoryUsageLevel seen (release on a rise)
    unsigned m_lastMemReleaseFrame = 0; // frame of last memory release (throttle)
    unsigned long long m_lastMemLogTick = 0;     // wall-clock throttles: the frame-counted
    unsigned long long m_lastMemReleaseTick = 0; // ones never fired at the 1fps that matters
    bool m_urlEditing = false;
    double m_deviceScale = 1.0;
    bool m_pressed = false, m_moved = false;
    int m_pressX = 0, m_pressY = 0, m_lastX = 0, m_lastY = 0;
};

// Called by PortChromeClient (WebCore) when a web-page text input gains/loses focus. Shows or
// hides the on-screen touch keyboard via the Windows InputPane. Runs on the UI thread (invoked
// from the render pump), so InputPane::GetForCurrentView() is valid here.
extern "C" void PortShowKeyboard(int show)
{
    try {
        auto pane = Windows::UI::ViewManagement::InputPane::GetForCurrentView();
        if (!pane)
            return;
        if (show)
            pane->TryShow();
        else
            pane->TryHide();
    } catch (...) { }
}

extern "C" void RevenantProbeMark(const char*);

[Platform::MTAThread]
int main(Array<String^>^)
{
    RevenantProbeMark("=== main() entered ===");
    // Revenant ARM32-UWP diag: the debug-log path is normally set deep inside OnLaunched
    // (PortSetDebugLogPathW), so any startup failure BEFORE that point left debug.log empty
    // and masqueraded as a pre-main death. Set it here first so the app's own portLog trail
    // through OnLaunched persists. Also hook CoreApplication::UnhandledErrorDetected: a C++/CX
    // unhandled WinRT exception fails-fast PAST std::set_terminate / SetUnhandledExceptionFilter,
    // which is why the crash probe's handlers never fired.
    try {
        auto path = Windows::Storage::ApplicationData::Current->LocalFolder->Path + L"\\debug.log";
        PortSetDebugLogPathW(path->Data());
        WebCorePort::portLog("main: log path set; installing UnhandledErrorDetected hook");
    } catch (...) { }

    Windows::ApplicationModel::Core::CoreApplication::UnhandledErrorDetected +=
        ref new Windows::Foundation::EventHandler<Windows::ApplicationModel::Core::UnhandledErrorDetectedEventArgs^>(
            [](Platform::Object^, Windows::ApplicationModel::Core::UnhandledErrorDetectedEventArgs^ e) {
                try {
                    e->UnhandledError->Propagate();
                } catch (Platform::Exception^ pe) {
                    char b[128];
                    snprintf(b, sizeof b, "UNHANDLED WinRT error HRESULT=0x%08X", static_cast<unsigned>(pe->HResult));
                    WebCorePort::portLog(b);
                } catch (...) {
                    WebCorePort::portLog("UNHANDLED startup error (non-WinRT)");
                }
            });

    // HOW DOES THE PROCESS END? Every "the app just closes" log so far simply STOPS: no fault, no
    // unhandled exception, no OOM, memory at 81MB of a 390MB cap. That leaves the platform ending us,
    // and the platform announces itself before it does -- but nothing was listening. Hook the whole
    // Process Lifetime Management surface so the log states the exit path instead of implying it:
    //
    //   app: SUSPENDING          -> PLM is suspending us (deferral has ~5s; if the UI thread is pegged
    //                               at 1 fps we will blow that deadline and get terminated)
    //   app: EXITING             -> orderly CoreApplication::Exiting
    //   app: ENTERED-BACKGROUND  -> we lost the foreground (screen off / shell took over)
    //   app: XAML-UNHANDLED      -> a XAML-dispatched exception nobody caught
    //
    // If the next crash log ends with NONE of these, the process was hard-killed with no notification
    // at all, and that is itself the answer (resource-policy kill / fail-fast), not a gap in the log.
    try {
        Windows::ApplicationModel::Core::CoreApplication::Suspending +=
            ref new Windows::Foundation::EventHandler<Windows::ApplicationModel::SuspendingEventArgs^>(
                [](Platform::Object^, Windows::ApplicationModel::SuspendingEventArgs^) {
                    WebCorePort::portLog("app: SUSPENDING (PLM suspend requested)");
                });
        Windows::ApplicationModel::Core::CoreApplication::Resuming +=
            ref new Windows::Foundation::EventHandler<Platform::Object^>(
                [](Platform::Object^, Platform::Object^) {
                    WebCorePort::portLog("app: RESUMING");
                });
        Windows::ApplicationModel::Core::CoreApplication::Exiting +=
            ref new Windows::Foundation::EventHandler<Platform::Object^>(
                [](Platform::Object^, Platform::Object^) {
                    WebCorePort::portLog("app: EXITING (CoreApplication::Exiting)");
                });
    } catch (...) {
        WebCorePort::portLog("app: PLM hook install FAILED");
    }

    WebCorePort::portLog("main: entering Application::Start");
    Application::Start(ref new ApplicationInitializationCallback([](ApplicationInitializationCallbackParams^) {
        ref new RevenantApp();
    }));
    WebCorePort::portLog("main: Application::Start returned");
    return 0;
}
