// ============================================================================
// RenderShell.cpp — interactive UWP CoreWindow app (WRL IFrameworkView). Creates an
// ANGLE EGL surface bound to the CoreWindow (D3D11 swapchain on the Adreno) and
// presents. Step 1 = an animated clear color (proves windowing + ANGLE present on
// device); the WebCore engine render is wired in next.
// ============================================================================

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <roapi.h>
#include <windows.applicationmodel.h>
#include <windows.applicationmodel.core.h>
#include <windows.ui.core.h>
#include <windows.foundation.h>
#include <windows.graphics.display.h>
#include <windows.storage.h>
#include <windows.ui.input.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

using namespace ABI::Windows::ApplicationModel::Core;
using namespace ABI::Windows::UI::Core;
using namespace ABI::Windows::Foundation;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

extern "C" void PortSetDebugLogPathW(const wchar_t* widePath);
extern "C" void PortSetCACertData(const uint8_t* data, unsigned size);
extern "C" int WebCoreBrowserInit(void* coreWindow, const char* url, double deviceScale);
extern "C" void WebCoreBrowserRenderFrame();
extern "C" void WebCoreBrowserScrollBy(int dx, int dy);
extern "C" void WebCoreBrowserTap(int x, int y);
namespace WebCorePort { void portLog(const char*); }
using WebCorePort::portLog;

// MSVC lacks the GCC byte-swap builtins that some prebuilt deps (webp) reference.
extern "C" {
unsigned short __builtin_bswap16(unsigned short x) { return _byteswap_ushort(x); }
unsigned int __builtin_bswap32(unsigned int x) { return _byteswap_ulong(x); }
unsigned long long __builtin_bswap64(unsigned long long x) { return _byteswap_uint64(x); }
}

// LocalState path (for the diagnostic log), via WRL (no C++/WinRT in this SDK).
static std::wstring localStatePath()
{
    using namespace ABI::Windows::Storage;
    ComPtr<IApplicationDataStatics> statics;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Storage_ApplicationData).Get(),
        __uuidof(IApplicationDataStatics), &statics)))
        return L"";
    ComPtr<IApplicationData> data;
    if (FAILED(statics->get_Current(&data)))
        return L"";
    ComPtr<IStorageFolder> folder;
    if (FAILED(data->get_LocalFolder(&folder)))
        return L"";
    ComPtr<IStorageItem> item;
    if (FAILED(folder.As(&item)))
        return L"";
    HString path;
    if (FAILED(item->get_Path(path.GetAddressOf())))
        return L"";
    UINT32 len = 0;
    const wchar_t* raw = path.GetRawBuffer(&len);
    return std::wstring(raw, len);
}

// Appx install dir (holds the bundled cacert.pem), via WRL.
static std::wstring installedLocationPath()
{
    using namespace ABI::Windows::ApplicationModel;
    using namespace ABI::Windows::Storage;
    ComPtr<IPackageStatics> statics;
    if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_ApplicationModel_Package).Get(),
        __uuidof(IPackageStatics), &statics)))
        return L"";
    ComPtr<IPackage> package;
    if (FAILED(statics->get_Current(&package)))
        return L"";
    ComPtr<IStorageFolder> folder;
    if (FAILED(package->get_InstalledLocation(&folder)))
        return L"";
    ComPtr<IStorageItem> item;
    if (FAILED(folder.As(&item)))
        return L"";
    HString path;
    if (FAILED(item->get_Path(path.GetAddressOf())))
        return L"";
    UINT32 len = 0;
    const wchar_t* raw = path.GetRawBuffer(&len);
    return std::wstring(raw, len);
}

// Read the bundled cacert.pem into memory and hand it to the engine (HTTPS trust roots;
// OpenSSL can't read files in the AppContainer, so it's loaded in-memory).
static void loadCACertBundle()
{
    const std::wstring install = installedLocationPath();
    if (install.empty())
        return;
    FILE* in = _wfopen((install + L"\\cacert.pem").c_str(), L"rb");
    if (!in)
        return;
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (sz > 0) {
        std::vector<uint8_t> buf(static_cast<size_t>(sz));
        size_t got = fread(buf.data(), 1, buf.size(), in);
        PortSetCACertData(buf.data(), static_cast<unsigned>(got));
    }
    fclose(in);
}

class ViewProvider : public RuntimeClass<RuntimeClassFlags<WinRtClassicComMix>, IFrameworkViewSource, IFrameworkView> {
public:
    // IFrameworkViewSource
    HRESULT STDMETHODCALLTYPE CreateView(IFrameworkView** view) override { return QueryInterface(IID_PPV_ARGS(view)); }

    // IFrameworkView
    HRESULT STDMETHODCALLTYPE Initialize(ICoreApplicationView*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetWindow(ICoreWindow* window) override { m_window = window; return S_OK; }
    HRESULT STDMETHODCALLTYPE Load(HSTRING) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Run() override { return runLoop(); }
    HRESULT STDMETHODCALLTYPE Uninitialize() override { return S_OK; }

private:
    // DIP -> physical pixel scale (pointer positions are DIPs; the ANGLE surface + the
    // WebCore viewport are physical pixels).
    double displayScale()
    {
        using namespace ABI::Windows::Graphics::Display;
        ComPtr<IDisplayInformationStatics> statics;
        if (FAILED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Graphics_Display_DisplayInformation).Get(),
            __uuidof(IDisplayInformationStatics), &statics)))
            return 1.0;
        ComPtr<IDisplayInformation> info;
        if (FAILED(statics->GetForCurrentView(&info)))
            return 1.0;
        FLOAT dpi = 96.0f;
        info->get_LogicalDpi(&dpi);
        return dpi > 0 ? dpi / 96.0 : 1.0;
    }

    // CoreWindow pointer positions are DIPs, which equal WebCore CSS pixels (the DPR is
    // applied inside the engine), so pass them through unscaled.
    void toCss(IPointerEventArgs* args, int& x, int& y)
    {
        using namespace ABI::Windows::UI::Input;
        ComPtr<IPointerPoint> pt;
        ABI::Windows::Foundation::Point pos {};
        if (args && SUCCEEDED(args->get_CurrentPoint(&pt)) && pt && SUCCEEDED(pt->get_Position(&pos))) {
            x = static_cast<int>(pos.X + 0.5f);
            y = static_cast<int>(pos.Y + 0.5f);
        }
    }

    HRESULT onPointerPressed(ICoreWindow*, IPointerEventArgs* args)
    {
        toCss(args, m_pressX, m_pressY);
        m_lastX = m_pressX; m_lastY = m_pressY;
        m_pressed = true; m_moved = false;
        return S_OK;
    }
    HRESULT onPointerMoved(ICoreWindow*, IPointerEventArgs* args)
    {
        if (!m_pressed)
            return S_OK;
        int x = m_lastX, y = m_lastY;
        toCss(args, x, y);
        int dx = x - m_lastX, dy = y - m_lastY;
        if (dx || dy) {
            if (std::abs(x - m_pressX) > 8 || std::abs(y - m_pressY) > 8)
                m_moved = true;
            WebCoreBrowserScrollBy(m_lastX - x, m_lastY - y); // natural touch scroll
            m_lastX = x; m_lastY = y;
        }
        return S_OK;
    }
    HRESULT onPointerReleased(ICoreWindow*, IPointerEventArgs* args)
    {
        int x = m_pressX, y = m_pressY;
        toCss(args, x, y);
        portLog((std::string("ptr: released press=(") + std::to_string(m_pressX) + "," + std::to_string(m_pressY)
            + ") rel=(" + std::to_string(x) + "," + std::to_string(y) + ") moved=" + std::to_string(m_moved)).c_str());
        if (!m_moved)
            WebCoreBrowserTap(m_pressX, m_pressY); // a tap, not a drag
        m_pressed = false;
        return S_OK;
    }

    void registerInput()
    {
        using namespace ABI::Windows::Foundation;
        using PtrHandler = ITypedEventHandler<CoreWindow*, PointerEventArgs*>;
        EventRegistrationToken tok;
        m_window->add_PointerPressed(Callback<PtrHandler>(this, &ViewProvider::onPointerPressed).Get(), &tok);
        m_window->add_PointerMoved(Callback<PtrHandler>(this, &ViewProvider::onPointerMoved).Get(), &tok);
        m_window->add_PointerReleased(Callback<PtrHandler>(this, &ViewProvider::onPointerReleased).Get(), &tok);
    }

    HRESULT runLoop()
    {
        m_window->Activate();
        ComPtr<ICoreDispatcher> dispatcher;
        m_window->get_Dispatcher(&dispatcher);
        m_scale = displayScale();
        registerInput();

        // The driver creates the ANGLE GL context on the CoreWindow, builds the page,
        // and starts loading. Then each frame: pump UWP events + pump/present WebCore.
        int rc = WebCoreBrowserInit(reinterpret_cast<void*>(m_window.Get()), "https://www.google.com/", m_scale);
        portLog((std::string("win: WebCoreBrowserInit rc=") + std::to_string(rc) + " scale=" + std::to_string(m_scale)).c_str());
        portLog("win: entering run loop");

        while (true) {
            if (dispatcher)
                dispatcher->ProcessEvents(CoreProcessEventsOption_ProcessAllIfPresent);
            WebCoreBrowserRenderFrame();
        }
        return S_OK;
    }

    ComPtr<ICoreWindow> m_window;
    double m_scale = 1.0;
    bool m_pressed = false, m_moved = false;
    int m_pressX = 0, m_pressY = 0, m_lastX = 0, m_lastY = 0;
};

int __cdecl main(int, char**)
{
    RoInitializeWrapper roInit(RO_INIT_MULTITHREADED);

    const std::wstring base = localStatePath();
    if (!base.empty())
        PortSetDebugLogPathW((base + L"\\debug.log").c_str());
    portLog("win: app main start");
    loadCACertBundle(); // HTTPS trust roots (in-memory)

    ComPtr<ICoreApplication> coreApp;
    if (SUCCEEDED(RoGetActivationFactory(
            HStringReference(RuntimeClass_Windows_ApplicationModel_Core_CoreApplication).Get(),
            __uuidof(ICoreApplication), &coreApp))) {
        auto view = Make<ViewProvider>();
        coreApp->Run(view.Get());
    } else
        portLog("win: CoreApplication factory failed");
    return 0;
}
