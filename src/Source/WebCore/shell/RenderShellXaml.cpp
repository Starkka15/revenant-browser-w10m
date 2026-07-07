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
#include <string>
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
extern "C" void WebCoreBrowserScrollBy(int dx, int dy);
extern "C" void WebCoreBrowserNavigate(const char* url);
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
    RevenantApp() { }

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
            auto props = makeSizedPanel(pxW, pxH);
            void* insp = reinterpret_cast<void*>(props);
            int rc = WebCoreBrowserInitPanel(insp, pxW, pxH, raw, "https://www.google.com/");
            char b[160];
            snprintf(b, sizeof b, "revenant: InitPanel rc=%d px=%dx%d scale=%.3f", rc, pxW, pxH, raw);
            WebCorePort::portLog(b);

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

    // Build the ANGLE configured property set (panel + explicit physical render size).
    Windows::Foundation::Collections::PropertySet^ makeSizedPanel(int pxW, int pxH)
    {
        auto props = ref new Windows::Foundation::Collections::PropertySet();
        props->Insert(L"EGLNativeWindowTypeProperty", m_panel);
        props->Insert(L"EGLRenderSurfaceSizeProperty",
            PropertyValue::CreateSize(Size(static_cast<float>(pxW), static_cast<float>(pxH))));
        return props;
    }

    void onRendering(Object^, Object^)
    {
        WebCoreBrowserRenderFrameSafe();

        // Periodic memory-usage sample (every ~120 frames ≈ 2s). Uses the UWP app-memory API so we
        // can see the trend leading into a freeze/slowdown (climbing usage → GC thrash / hitting the
        // app cap). AppMemoryUsageLevel: 0=Low 1=Medium 2=High 3=OverLimit.
        if ((++m_memSampleFrame % 120) == 0) {
            uint64 used = Windows::System::MemoryManager::AppMemoryUsage;
            uint64 limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
            int level = (int)Windows::System::MemoryManager::AppMemoryUsageLevel;
            char b[192];
            sprintf_s(b, sizeof b, "mem: appUsage=%lluMB / limit=%lluMB (%llu%%) level=%d",
                (unsigned long long)(used / (1024 * 1024)),
                (unsigned long long)(limit / (1024 * 1024)),
                limit ? (unsigned long long)(used * 100 / limit) : 0ULL, level);
            WebCoreBrowserLog(b);
        }

        // Reflect load state in the chrome (blue bar + refresh/stop + URL).
        bool loading = WebCoreBrowserIsLoading() != 0;
        if (loading != m_lastLoading) {
            m_lastLoading = loading;
            m_progress->Visibility = loading ? ::Visibility::Visible : ::Visibility::Collapsed;
            reloadIcon()->Glyph = loading ? L"\uE711" : L"\uE72C"; // stop (X) vs refresh
        }
        if (loading)
            m_progress->Value = WebCoreBrowserProgress();

        // Keep the address bar in sync with the committed URL (unless the user is editing it).
        if (!m_urlEditing) {
            const char* u = WebCoreBrowserCurrentURL();
            if (u && *u) {
                auto s = ref new String(std::wstring(u, u + strlen(u)).c_str());
                if (m_urlBox->Text != s)
                    m_urlBox->Text = s;
            }
        }
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
    void onPointerPressed(Object^, PointerRoutedEventArgs^ e)
    {
        toDip(e, m_pressX, m_pressY);
        m_lastX = m_pressX; m_lastY = m_pressY;
        m_pressed = true; m_moved = false;
        // Tapping the web view moves interaction to the page: stop treating keys as address-bar
        // input so typed characters reach web inputs (the URL box may still hold XAML focus).
        m_urlFocused = false;
    }
    void onPointerMoved(Object^, PointerRoutedEventArgs^ e)
    {
        if (!m_pressed) return;
        int x = m_lastX, y = m_lastY;
        toDip(e, x, y);
        int dx = x - m_lastX, dy = y - m_lastY;
        if (dx || dy) {
            if (std::abs(x - m_pressX) > 8 || std::abs(y - m_pressY) > 8)
                m_moved = true;
            WebCoreBrowserScrollBy(m_lastX - x, m_lastY - y);
            m_lastX = x; m_lastY = y;
        }
    }
    void onPointerReleased(Object^, PointerRoutedEventArgs^)
    {
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

[Platform::MTAThread]
int main(Array<String^>^)
{
    Application::Start(ref new ApplicationInitializationCallback([](ApplicationInitializationCallbackParams^) {
        ref new RevenantApp();
    }));
    return 0;
}
