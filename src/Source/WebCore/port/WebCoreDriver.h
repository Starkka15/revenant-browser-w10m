// ============================================================================
// WebCoreDriver.h  —  C ABI for the W10M/UWP WebCore headless render driver.
// Renders a UTF-8 HTML string into a caller-provided w*h RGBA8888 buffer
// (>= w*h*4 bytes). Returns 0 on success, negative on failure.
// ============================================================================
#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

int WebCoreRenderHtml(const char* utf8Html, int w, int h, uint8_t* outRGBA);

// Loads an http(s):// URL (real network fetch via curl), waits for the main load
// to finish (pumps the run loop, with a timeout), lays out and paints into a
// w*h RGBA8888 buffer. Returns 0 on success, negative on failure.
int WebCoreRenderUrl(const char* utf8Url, int w, int h, uint8_t* outRGBA);

// Point the driver's diagnostic log at a writable file (LocalState). Pass null to disable.
void PortSetDebugLogPathW(const wchar_t* widePath);

// GPU bring-up probe: initialize ANGLE EGL(D3D11) + an offscreen GL context, logging
// the EGL/GL vendor+renderer. Returns 0 on success, negative on failure.
int WebCoreGlSelfTest();

// Live interactive browser: create a GL context on the CoreWindow (IInspectable*),
// build a Page with accelerated compositing at the given device-pixels-per-CSS-pixel
// scale (mobile DPR), and start loading `url`. Returns 0 ok.
int WebCoreBrowserInit(void* coreWindow, const char* url, double deviceScale);
// Pump WebCore + present the current page state to the CoreWindow (call per frame).
void WebCoreBrowserRenderFrame();
// Input: scroll by device pixels (touch drag); tap = synthetic click at device pixels.
void WebCoreBrowserScrollBy(int dx, int dy, int x, int y);
void WebCoreBrowserTap(int x, int y);

#ifdef __cplusplus
}
#endif
