// ShellMse.cpp — WinRT-native MSE backing for Revenant, exposed to WebCore via a C ABI.
//
// Windows 10 Mobile has no reachable desktop IMFMediaSourceExtension (CreateMediaSourceExtension
// returns 0xc00d36e6 in our AppContainer), but it DOES ship the UWP-native MSE engine:
// Windows.Media.Core.MseStreamSource / MseSourceBuffer (available since 10.0.10240). It demuxes +
// decodes appended fMP4/WebM segments internally using the installed Store codecs (VP9/H.264/AAC/…),
// so we need no demuxer. This file drives those WinRT objects (trivial in C++/CX) and bridges them
// to WebCore's MediaSourcePrivate/SourceBufferPrivate through the extern "C" surface below.
//
// The MseStreamSource implements IMediaSource, which via IMFGetService(MF_MEDIASOURCE_SERVICE)
// yields an IMFMediaSource — that is what our existing IMFMediaEngine (decode-to-texture +
// accelerated compositing) plays, unchanged, via the source-resolver bridge in WebCore.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <mfapi.h>
#include <mfidl.h>
#include <robuffer.h>       // IBufferByteAccess
#include <wrl/client.h>
#include <d3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h> // CreateDirect3D11SurfaceFromDXGISurface

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Media::Core;
using namespace Windows::Media::Playback;
using namespace Windows::Graphics::DirectX::Direct3D11;
using namespace Windows::Storage::Streams;

// Implemented in WebCore (SourceBufferPrivateMediaFoundation.cpp / MediaSourcePrivateMediaFoundation.cpp).
// Marshalled back on the UI thread (WinRT events fire there, same thread as the render pump).
extern "C" void WebCoreMseSbUpdateEnded(void* sbCtx);
extern "C" void WebCoreMseSbErrored(void* sbCtx, int hr);
extern "C" void WebCoreMseSourceOpened(void* srcCtx);
extern "C" void WebCoreMseSourceEnded(void* srcCtx);
extern "C" void WebCoreMsePlayerFrame(void* playerCtx, const uint8_t* bgra, int width, int height, int stride);
extern "C" void WebCoreMsePlayerStateChanged(void* playerCtx, int state);
extern "C" void WebCoreMsePlayerError(void* playerCtx, int hr);
extern "C" void WebCoreMsePlayerDurationChanged(void* playerCtx, double seconds);
extern "C" void WebCoreMsePlayerSizeChanged(void* playerCtx, int width, int height);
extern "C" void WebCoreMsePlayerTimeUpdate(void* playerCtx, double seconds);
extern "C" void WebCoreBrowserKeepCompositing(unsigned frames); // GPU-offload gate: keep compositing N frames
extern void PortImgLog(const char*);

namespace {

struct MseSource {
    MseStreamSource^ source;
    void* srcCtx { nullptr };
    // Frame-server playback (Option B): MediaPlayer plays the MseStreamSource; audio goes to the
    // default endpoint automatically, video frames arrive on VideoFrameAvailable.
    Windows::Media::Core::MediaSource^ mediaSource;
    Windows::Media::Playback::MediaPlayer^ player;
    void* playerCtx { nullptr };
    bool closing { false }; // set before teardown so in-flight WinRT events stop calling into freed WebCore
    Microsoft::WRL::ComPtr<ID3D11Device> d3d;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dCtx;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt;      // BGRA render target CopyFrameToVideoSurface writes into
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging; // CPU-readable copy we hand to WebCore
    UINT texW { 0 };
    UINT texH { 0 };
};

struct MseBuffer {
    MseSourceBuffer^ buffer;
    void* sbCtx { nullptr };
};

static String^ toPlatformString(const char* utf8)
{
    if (!utf8)
        return ref new String(L"");
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1)
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], wlen);
    return ref new String(w.c_str());
}

// Build an IBuffer that OWNS a copy of the bytes (the WinRT append is async; the caller's buffer
// may be gone by the time MF reads it). A DataWriter-backed buffer copies + owns.
static IBuffer^ makeBuffer(const uint8_t* data, int len)
{
    auto writer = ref new DataWriter();
    writer->WriteBytes(ArrayReference<unsigned char>(const_cast<unsigned char*>(data), len));
    return writer->DetachBuffer();
}

// (Re)create the render-target + staging textures the frame-server copies each decoded frame into.
static void ensurePlayerTextures(MseSource* h, UINT w, UINT hgt)
{
    if (h->rt && h->texW == w && h->texH == hgt)
        return;
    h->rt.Reset();
    h->staging.Reset();
    if (!h->d3d) {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL fl;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                D3D11_SDK_VERSION, &h->d3d, &fl, &h->d3dCtx))) {
            PortImgLog("mse: player D3D11CreateDevice FAILED");
            return;
        }
        Microsoft::WRL::ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(h->d3d.As(&mt)))
            mt->SetMultithreadProtected(TRUE);
    }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = hgt; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(h->d3d->CreateTexture2D(&d, nullptr, &h->rt))) { PortImgLog("mse: player rt CreateTexture2D FAILED"); return; }
    D3D11_TEXTURE2D_DESC s = d;
    s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(h->d3d->CreateTexture2D(&s, nullptr, &h->staging))) { PortImgLog("mse: player staging CreateTexture2D FAILED"); return; }
    h->texW = w; h->texH = hgt;
}

// VideoFrameAvailable handler (Media Foundation worker thread): pull the current decoded frame into
// our texture, read it back, and hand the BGRA bytes to WebCore (software paint path). Guarded —
// any uncaught C++/CX exception here would terminate the app.
static void onVideoFrame(MseSource* h)
{
    try {
        if (h->closing || !h->player || !h->playerCtx)
            return;
        static bool s_loggedFirst = false;
        if (!s_loggedFirst) { s_loggedFirst = true; PortImgLog("mse: onVideoFrame first frame"); }
        auto sess = h->player->PlaybackSession;
        UINT w = sess->NaturalVideoWidth, hgt = sess->NaturalVideoHeight;
        if (!w || !hgt)
            return;
        ensurePlayerTextures(h, w, hgt);
        if (!h->rt || !h->staging)
            return;
        Microsoft::WRL::ComPtr<IDXGISurface> dxgi;
        if (FAILED(h->rt.As(&dxgi)))
            return;
        Microsoft::WRL::ComPtr<IInspectable> insp;
        if (FAILED(CreateDirect3D11SurfaceFromDXGISurface(dxgi.Get(), &insp)) || !insp)
            return;
        auto surf = reinterpret_cast<IDirect3DSurface^>(insp.Get());
        h->player->CopyFrameToVideoSurface(surf);
        h->d3dCtx->CopyResource(h->staging.Get(), h->rt.Get());
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (SUCCEEDED(h->d3dCtx->Map(h->staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            if (!h->closing && h->playerCtx)
                WebCoreMsePlayerFrame(h->playerCtx, static_cast<const uint8_t*>(m.pData), static_cast<int>(w), static_cast<int>(hgt), static_cast<int>(m.RowPitch));
            h->d3dCtx->Unmap(h->staging.Get(), 0);
        }
    } catch (...) { PortImgLog("mse: onVideoFrame EXCEPTION (swallowed)"); }
}

} // namespace

extern "C" {

// Create an MseStreamSource. srcCtx is the WebCore MediaSourcePrivate pointer for callbacks.
void* PortMseCreate(void* srcCtx)
{
    auto h = new MseSource();
    h->source = ref new MseStreamSource();
    h->srcCtx = srcCtx;
    h->source->Opened += ref new TypedEventHandler<MseStreamSource^, Object^>(
        [srcCtx](MseStreamSource^, Object^) { WebCoreMseSourceOpened(srcCtx); });
    h->source->Ended += ref new TypedEventHandler<MseStreamSource^, Object^>(
        [srcCtx](MseStreamSource^, Object^) { WebCoreMseSourceEnded(srcCtx); });
    return h;
}

void PortMseDestroy(void* srcH)
{
    auto h = static_cast<MseSource*>(srcH);
    h->closing = true;
    h->playerCtx = nullptr;
    // `delete` on a C++/CX hat invokes IClosable::Close() (stops playback + frees the media pipeline)
    // before the ref is dropped, so late VideoFrameAvailable events can't touch a freed MseSource.
    if (h->player) { try { delete h->player; } catch (...) { } h->player = nullptr; }
    PortImgLog("mse: source destroyed");
    delete h;
}

int PortMseIsTypeSupported(void* srcH, const char* type)
{
    auto h = static_cast<MseSource*>(srcH);
    try { return h->source->IsContentTypeSupported(toPlatformString(type)) ? 1 : 0; }
    catch (...) { return 0; }
}

// Add a source buffer. sbCtx is the WebCore SourceBufferPrivate pointer for callbacks.
void* PortMseAddSourceBuffer(void* srcH, const char* type, void* sbCtx)
{
    auto h = static_cast<MseSource*>(srcH);
    MseSourceBuffer^ sb = nullptr;
    try { sb = h->source->AddSourceBuffer(toPlatformString(type)); }
    catch (...) { return nullptr; }
    if (!sb)
        return nullptr;
    auto b = new MseBuffer();
    b->buffer = sb;
    b->sbCtx = sbCtx;
    sb->UpdateEnded += ref new TypedEventHandler<MseSourceBuffer^, Object^>(
        [sbCtx](MseSourceBuffer^, Object^) { WebCoreMseSbUpdateEnded(sbCtx); });
    sb->ErrorOccurred += ref new TypedEventHandler<MseSourceBuffer^, Object^>(
        [sbCtx](MseSourceBuffer^, Object^) { WebCoreMseSbErrored(sbCtx, 0); });
    return b;
}

// Append a media segment. Async — completion arrives via WebCoreMseSbUpdateEnded(sbCtx).
void PortMseAppend(void* sbH, const uint8_t* data, int len)
{
    auto b = static_cast<MseBuffer*>(sbH);
    try { b->buffer->AppendBuffer(makeBuffer(data, len)); }
    catch (Platform::Exception^ ex) {
        char buf[96]; snprintf(buf, sizeof buf, "mse: AppendBuffer THREW hr=0x%08lx", (unsigned long)ex->HResult); PortImgLog(buf);
        WebCoreMseSbErrored(b->sbCtx, (int)ex->HResult);
    }
    catch (...) { PortImgLog("mse: AppendBuffer THREW (unknown)"); WebCoreMseSbErrored(b->sbCtx, -1); }
}

int PortMseIsUpdating(void* sbH)
{
    auto b = static_cast<MseBuffer*>(sbH);
    try { return b->buffer->IsUpdating ? 1 : 0; }
    catch (...) { return 0; }
}

void PortMseAbort(void* sbH)
{
    auto b = static_cast<MseBuffer*>(sbH);
    try { b->buffer->Abort(); } catch (...) { }
}

void PortMseSetTimestampOffset(void* sbH, double seconds)
{
    auto b = static_cast<MseBuffer*>(sbH);
    try { b->buffer->TimestampOffset = TimeSpan { static_cast<long long>(seconds * 1e7) }; } catch (...) { }
}

// Fill starts/ends (seconds) with the buffered ranges; returns the count (<= maxN).
int PortMseGetBuffered(void* sbH, double* starts, double* ends, int maxN)
{
    auto b = static_cast<MseBuffer*>(sbH);
    int n = 0;
    try {
        auto ranges = b->buffer->Buffered; // IVectorView<MseTimeRange>-like
        unsigned count = ranges->Size;
        for (unsigned i = 0; i < count && n < maxN; ++i) {
            auto r = ranges->GetAt(i);
            starts[n] = r.Start.Duration / 1e7;
            ends[n] = r.End.Duration / 1e7;
            ++n;
        }
    } catch (...) { }
    return n;
}

void PortMseSetDuration(void* srcH, double seconds)
{
    auto h = static_cast<MseSource*>(srcH);
    try { h->source->Duration = TimeSpan { static_cast<long long>(seconds * 1e7) }; } catch (...) { }
}

// status: 0 = success, 1 = network error, 2 = decode error.
void PortMseEndOfStream(void* srcH, int status)
{
    auto h = static_cast<MseSource*>(srcH);
    MseEndOfStreamStatus s = MseEndOfStreamStatus::Success;
    if (status == 1) s = MseEndOfStreamStatus::NetworkError;
    else if (status == 2) s = MseEndOfStreamStatus::DecodeError;
    try { h->source->EndOfStream(s); } catch (...) { }
}

// Extract the IMFMediaSource the IMFMediaEngine will play. Returns a ref'd IMFMediaSource* (the
// caller releases). MseStreamSource implements IMediaSource -> IMFGetService(MF_MEDIASOURCE_SERVICE).
void* PortMseGetMFMediaSource(void* srcH)
{
    auto h = static_cast<MseSource*>(srcH);
    Microsoft::WRL::ComPtr<IMFGetService> getService;
    IInspectable* insp = reinterpret_cast<IInspectable*>(h->source);
    HRESULT hrQI = insp->QueryInterface(IID_PPV_ARGS(&getService));
    if (FAILED(hrQI) || !getService) {
        char b[96]; snprintf(b, sizeof b, "mse: getMFSource FAIL - MseStreamSource has no IMFGetService hr=0x%08lx", (unsigned long)hrQI); PortImgLog(b);
        return nullptr;
    }
    IMFMediaSource* source = nullptr;
    HRESULT hrGS = getService->GetService(MF_MEDIASOURCE_SERVICE, IID_PPV_ARGS(&source));
    if (FAILED(hrGS)) {
        char b[96]; snprintf(b, sizeof b, "mse: getMFSource FAIL - GetService(MF_MEDIASOURCE_SERVICE) hr=0x%08lx", (unsigned long)hrGS); PortImgLog(b);
        return nullptr;
    }
    PortImgLog("mse: getMFSource OK - IMFMediaSource obtained");
    return source;
}

// ---- WinRT MediaPlayer frame-server playback (Option B) ----
void PortMsePlayerStart(void* srcH, void* playerCtx)
{
    auto h = static_cast<MseSource*>(srcH);
    h->playerCtx = playerCtx;
    h->closing = false;
    // All handlers capture `h` and read h->playerCtx live (guarded by h->closing) so a WinRT event
    // that fires during/after teardown can't call into a freed WebCore MediaPlayerPrivate.
    try {
        h->mediaSource = MediaSource::CreateFromMseStreamSource(h->source);
        h->player = ref new MediaPlayer();
        h->player->IsVideoFrameServerEnabled = true;
        h->player->AutoPlay = true;
        h->player->Source = h->mediaSource;
        h->player->VideoFrameAvailable += ref new TypedEventHandler<MediaPlayer^, Object^>(
            [h](MediaPlayer^, Object^) { onVideoFrame(h); });
        h->player->MediaEnded += ref new TypedEventHandler<MediaPlayer^, Object^>(
            [h](MediaPlayer^, Object^) { try { if (!h->closing && h->playerCtx) WebCoreMsePlayerStateChanged(h->playerCtx, 5); } catch (...) { } });
        h->player->MediaFailed += ref new TypedEventHandler<MediaPlayer^, MediaPlayerFailedEventArgs^>(
            [h](MediaPlayer^, MediaPlayerFailedEventArgs^ e) { try { if (!h->closing && h->playerCtx) { int hr = e ? (int)e->ExtendedErrorCode.Value : -1; char b[80]; snprintf(b, sizeof b, "mse: MediaFailed hr=0x%08x", (unsigned)hr); PortImgLog(b); WebCoreMsePlayerError(h->playerCtx, hr); } } catch (...) { } });
        auto sess = h->player->PlaybackSession;
        sess->PlaybackStateChanged += ref new TypedEventHandler<MediaPlaybackSession^, Object^>(
            [h](MediaPlaybackSession^ s, Object^) { try { if (!h->closing && h->playerCtx) WebCoreMsePlayerStateChanged(h->playerCtx, (int)s->PlaybackState); } catch (...) { } });
        sess->NaturalDurationChanged += ref new TypedEventHandler<MediaPlaybackSession^, Object^>(
            [h](MediaPlaybackSession^ s, Object^) { try { if (!h->closing && h->playerCtx) WebCoreMsePlayerDurationChanged(h->playerCtx, s->NaturalDuration.Duration / 1e7); } catch (...) { } });
        sess->NaturalVideoSizeChanged += ref new TypedEventHandler<MediaPlaybackSession^, Object^>(
            [h](MediaPlaybackSession^ s, Object^) { try { if (!h->closing && h->playerCtx) WebCoreMsePlayerSizeChanged(h->playerCtx, (int)s->NaturalVideoWidth, (int)s->NaturalVideoHeight); } catch (...) { } });
        sess->BufferingStarted += ref new TypedEventHandler<MediaPlaybackSession^, Object^>(
            [h](MediaPlaybackSession^, Object^) { if (!h->closing) PortImgLog("mse: player BufferingStarted"); });
        sess->PositionChanged += ref new TypedEventHandler<MediaPlaybackSession^, Object^>(
            [h](MediaPlaybackSession^ s, Object^) { try { if (!h->closing && h->playerCtx) WebCoreMsePlayerTimeUpdate(h->playerCtx, s->Position.Duration / 1e7); } catch (...) { } });
        PortImgLog("mse: MediaPlayer frame-server started (AutoPlay)");
    } catch (Platform::Exception^ ex) {
        char b[128]; snprintf(b, sizeof b, "mse: MediaPlayer start FAILED hr=0x%08lx", (unsigned long)ex->HResult); PortImgLog(b);
        WebCoreMsePlayerError(playerCtx, ex->HResult);
    }
}

// Detach + close the player synchronously. Called from WebCore's MediaPlayerPrivate destructor
// BEFORE the object (playerCtx) is freed, so no WinRT event can fire into a dangling pointer.
void PortMsePlayerStop(void* srcH)
{
    auto h = static_cast<MseSource*>(srcH);
    h->closing = true;
    h->playerCtx = nullptr;
    if (h->player) {
        try { h->player->Pause(); } catch (...) { }
        try { delete h->player; } catch (...) { } // IClosable::Close — tears down the pipeline + stops events
        h->player = nullptr;
    }
    PortImgLog("mse: player stopped");
}

void PortMsePlayerPlay(void* srcH)  { auto h = static_cast<MseSource*>(srcH); try { if (h->player) h->player->Play();  } catch (...) { } }
void PortMsePlayerPause(void* srcH) { auto h = static_cast<MseSource*>(srcH); try { if (h->player) h->player->Pause(); } catch (...) { } }
void PortMsePlayerSetRate(void* srcH, double rate)   { auto h = static_cast<MseSource*>(srcH); try { if (h->player) h->player->PlaybackSession->PlaybackRate = rate; } catch (...) { } }
void PortMsePlayerSetVolume(void* srcH, double vol)  { auto h = static_cast<MseSource*>(srcH); try { if (h->player) h->player->Volume = vol; } catch (...) { } }
void PortMsePlayerSetMuted(void* srcH, int muted)    { auto h = static_cast<MseSource*>(srcH); try { if (h->player) h->player->IsMuted = muted != 0; } catch (...) { } }
void PortMsePlayerSeek(void* srcH, double seconds)   { auto h = static_cast<MseSource*>(srcH); try { if (h->player) h->player->PlaybackSession->Position = TimeSpan { static_cast<long long>(seconds * 1e7) }; } catch (...) { } }
double PortMsePlayerPosition(void* srcH) { auto h = static_cast<MseSource*>(srcH); try { if (h->player) return h->player->PlaybackSession->Position.Duration / 1e7; } catch (...) { } return 0; }
double PortMsePlayerDuration(void* srcH) { auto h = static_cast<MseSource*>(srcH); try { if (h->player) return h->player->PlaybackSession->NaturalDuration.Duration / 1e7; } catch (...) { } return 0; }

// Native decoded video size (compositor thread; WebCore sizes its ANGLE texture from this).
void PortMsePlayerNativeSize(void* srcH, int* w, int* h_)
{
    if (w) *w = 0;
    if (h_) *h_ = 0;
    auto h = static_cast<MseSource*>(srcH);
    if (h->closing || !h->player)
        return;
    try {
        auto s = h->player->PlaybackSession;
        if (w) *w = static_cast<int>(s->NaturalVideoWidth);
        if (h_) *h_ = static_cast<int>(s->NaturalVideoHeight);
    } catch (...) { }
}

// Zero-copy: copy the current decoded frame straight into WebCore's ANGLE-device texture (which is
// then GL-composited with no readback). Called on the compositor thread with the GL/D3D device
// current; the device is multithread-protected so this is safe alongside ANGLE. Returns 1 on copy.
int PortMsePlayerCopyFrame(void* srcH, void* d3dTexture)
{
    auto h = static_cast<MseSource*>(srcH);
    if (h->closing || !h->player || !d3dTexture)
        return 0;
    try {
        auto* tex = static_cast<ID3D11Texture2D*>(d3dTexture);
        Microsoft::WRL::ComPtr<IDXGISurface> dxgi;
        if (FAILED(tex->QueryInterface(IID_PPV_ARGS(&dxgi))) || !dxgi)
            return 0;
        Microsoft::WRL::ComPtr<IInspectable> insp;
        if (FAILED(CreateDirect3D11SurfaceFromDXGISurface(dxgi.Get(), &insp)) || !insp)
            return 0;
        auto surf = reinterpret_cast<IDirect3DSurface^>(insp.Get());
        h->player->CopyFrameToVideoSurface(surf);
        return 1;
    } catch (...) { return 0; }
}

} // extern "C"
