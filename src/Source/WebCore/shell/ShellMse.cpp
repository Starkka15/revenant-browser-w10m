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
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <mfapi.h>
#include <mfidl.h>
#include <robuffer.h>       // IBufferByteAccess
#include <wrl/client.h>
#include <d3d11.h>
#include <ppltasks.h>       // concurrency::create_task/create_async — progressive HTTP byte stream
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
extern "C" void WebCoreMseSbNoteEmergencyTrim(void* sbCtx); // post-emergency-trim range refresh
extern "C" unsigned WebCoreMemBudgetMseKeepBehindSec();     // per-device MSE keep-behind (memory tier)
extern "C" void WebCoreMseSourceOpened(void* srcCtx);
extern "C" void WebCoreMseSourceEnded(void* srcCtx);
extern "C" void WebCoreMsePlayerFrame(void* playerCtx, const uint8_t* bgra, int width, int height, int stride);
extern "C" void WebCoreMsePlayerStateChanged(void* playerCtx, int state);
extern "C" void WebCoreMsePlayerError(void* playerCtx, int hr);
extern "C" void WebCoreMsePlayerDurationChanged(void* playerCtx, double seconds);
extern "C" void WebCoreMsePlayerSizeChanged(void* playerCtx, int width, int height);
extern "C" void WebCoreMsePlayerTimeUpdate(void* playerCtx, double seconds);
extern "C" void WebCoreMsePlayerSeekCompleted(void* playerCtx);
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
    Windows::Foundation::EventRegistrationToken frameToken {}; // VideoFrameAvailable, unhooked at stop
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt;      // BGRA render target CopyFrameToVideoSurface writes into
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging; // CPU-readable copy we hand to WebCore
    std::vector<struct MseBuffer*> buffers;          // source buffers (for the emergency memory trim)
    UINT texW { 0 };
    UINT texH { 0 };
};

struct MseBuffer {
    MseSourceBuffer^ buffer;
    void* sbCtx { nullptr };
};

// Registry of live MSE sources so the shell's background memory watcher can emergency-trim buffered
// media OFF the main thread (main is exactly what's stalled when memory races to the OS kill; WinRT
// MF objects are agile, so Remove()/Buffered are safe from any thread). Guarded by s_mseRegistryLock;
// PortMseDestroy unregisters under the same lock, so the trimmer can never touch a freed source.
static std::mutex s_mseRegistryLock;
static std::vector<struct MseSource*> s_mseSources;

// Platform::String^ -> UTF-8 std::string (the C ABI into WebCore's curl fetcher speaks UTF-8).
static std::string toUtf8(Platform::String^ s)
{
    if (s == nullptr || s->IsEmpty())
        return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, s->Data(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return std::string();
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s->Data(), -1, &out[0], len, nullptr, nullptr);
    return out;
}

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
        [srcCtx](MseStreamSource^, Object^) { PortImgLog("mse: MseStreamSource Opened"); WebCoreMseSourceOpened(srcCtx); });
    h->source->Ended += ref new TypedEventHandler<MseStreamSource^, Object^>(
        [srcCtx](MseStreamSource^, Object^) { PortImgLog("mse: MseStreamSource Ended"); WebCoreMseSourceEnded(srcCtx); });
    {
        std::lock_guard<std::mutex> lock(s_mseRegistryLock);
        s_mseSources.push_back(h);
    }
    return h;
}

void PortMseDestroy(void* srcH)
{
    auto h = static_cast<MseSource*>(srcH);
    {
        std::lock_guard<std::mutex> lock(s_mseRegistryLock);
        s_mseSources.erase(std::remove(s_mseSources.begin(), s_mseSources.end(), h), s_mseSources.end());
    }
    h->closing = true;
    h->playerCtx = nullptr;
    // `delete` on a C++/CX hat invokes IClosable::Close() (stops playback + frees the media pipeline).
    // Doing that INLINE here froze the app hard: PortMseDestroy runs on the MAIN thread from
    // ~MediaPlayerPrivateMediaFoundation during Document::commonTeardown, and a wedged MF pipeline can
    // block Close() for 100+ seconds (measured: stage=a1:ctx IDLE(deadlock/blocked), 180s+, unkillable
    // even by WDP terminate). Close on a threadpool thread instead — same shape as PortMsePlayerStop:
    // detach the frame server + source first (makes Close trivial), then delete off the UI thread. The
    // captured `player` hat keeps it alive until the worker finishes; `h` is freed inline (its player
    // ref already transferred), so no late VideoFrameAvailable can reach it (playerCtx is null + the
    // handler is unhooked below).
    if (h->player) {
        MediaPlayer^ player = h->player;
        // Keep the media source + its backing byte stream ALIVE inside the close worker until AFTER
        // delete player. The progressive path hands MF an HttpRandomAccessStream (held by h->mediaSource);
        // `delete h` below releases h's ref and the worker's `Source = nullptr` drops the player's ref, so
        // WITHOUT these captures the stream is destructed BEFORE MF's source reader (RTWorkQ) finishes and
        // an in-flight Seek/Read hits the disposed stream -> Platform::ObjectDisposedException on the MF
        // worker thread -> crash (Rule34 video-select). Capturing extra refs ties their lifetime to Close.
        auto mediaSource = h->mediaSource;
        auto mseSource = h->source;
        h->player = nullptr;
        try { if (h->frameToken.Value) player->VideoFrameAvailable -= h->frameToken; } catch (...) { }
        try {
            Windows::System::Threading::ThreadPool::RunAsync(
                ref new Windows::System::Threading::WorkItemHandler(
                    [player, mediaSource, mseSource](Windows::Foundation::IAsyncAction^) {
                        try { player->Pause(); } catch (...) { }
                        try { player->Source = nullptr; } catch (...) { }
                        try { delete player; } catch (...) { } // Close synchronously drains MF's source reader
                        (void)mediaSource; (void)mseSource;     // released HERE, after Close -> no late Seek
                        PortImgLog("mse: player closed (destroy worker)");
                    }));
        } catch (...) {
            try { delete player; } catch (...) { } // fallback: close inline if dispatch fails
        }
    }
    PortImgLog("mse: source destroyed (async close)");
    delete h;
}

// Codecs the device can't decode fast enough at video res. Kept in sync with
// mseCodecsUndecodable() in MediaPlayerPrivateMediaFoundation.cpp; duplicated (not linked) because
// this is a C++/CX TU and the check is a trivial ASCII substring scan.
static bool mseTypeUndecodable(const char* type)
{
    if (!type) return false;
    std::string t(type);
    for (auto& ch : t) ch = (char)tolower((unsigned char)ch);
    static const char* const bad[] = { "av01", "av1.", "vp09", "vp9", "vp08", "vp8",
                                        "opus", "vorbis", "ac-3", "ec-3", "flac", "dts" };
    for (auto* b : bad)
        if (t.find(b) != std::string::npos) return true;
    return false;
}

int PortMseIsTypeSupported(void* srcH, const char* type)
{
    // Reject undecodable codecs up front so YouTube's ABR selects H.264/AAC (RC1). Log every query so
    // the on-device log shows exactly which types the page probes and which we gate.
    bool gated = mseTypeUndecodable(type);
    auto h = static_cast<MseSource*>(srcH);
    int ok = 0;
    if (!gated) {
        try { ok = h->source->IsContentTypeSupported(toPlatformString(type)) ? 1 : 0; }
        catch (...) { ok = 0; }
    }
    { char b[300]; snprintf(b, sizeof b, "mse: isTypeSupported type=%s gated=%d -> %d",
        type ? type : "(null)", gated ? 1 : 0, ok); PortImgLog(b); }
    return ok;
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
    {
        std::lock_guard<std::mutex> lock(s_mseRegistryLock);
        h->buffers.push_back(b);
    }
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

// Coded-frame removal, forwarded to the WinRT buffer (it owns the demuxed frames). Async: completion
// arrives as UpdateEnded on sbCtx. endSeconds < 0 = unbounded (to end of stream).
void PortMseRemove(void* sbH, double startSeconds, double endSeconds)
{
    auto b = static_cast<MseBuffer*>(sbH);
    try {
        TimeSpan start { static_cast<long long>(startSeconds * 1e7) };
        if (endSeconds < 0)
            b->buffer->Remove(start, nullptr);
        else
            b->buffer->Remove(start, ref new Platform::Box<TimeSpan>(TimeSpan { static_cast<long long>(endSeconds * 1e7) }));
    } catch (Platform::Exception^ ex) {
        char m[96]; snprintf(m, sizeof m, "mse: Remove THREW hr=0x%08lx", (unsigned long)ex->HResult); PortImgLog(m);
        WebCoreMseSbUpdateEnded(b->sbCtx); // still complete WebCore's removal (ranges just refresh as-is)
    } catch (...) {
        PortImgLog("mse: Remove THREW (unknown)");
        WebCoreMseSbUpdateEnded(b->sbCtx);
    }
}

// EMERGENCY memory trim, called from the shell's background memory watcher when app usage nears the
// OS kill cap (>=92%) while the main thread may be stalled. Removes buffered media BEHIND playback
// (keeps a 10s tail for instant rewind) directly on the WinRT buffers — MF frees the demuxed frames.
// Deliberately does NOT signal WebCore: the resulting UpdateEnded lands in onUpdateEnded() with no
// append/remove in flight and is ignored there ("spurious UpdateEnded" path preserves ranges); WebCore's
// buffered view goes stale until the next append, which is acceptable against a certain OS kill.
// Buffers mid-update are skipped (Remove would throw); a lost race just means that buffer is skipped.
void PortMseEmergencyTrim()
{
    std::lock_guard<std::mutex> lock(s_mseRegistryLock);
    for (auto* h : s_mseSources) {
        if (h->closing || !h->player)
            continue;
        double pos = 0;
        try { pos = h->player->PlaybackSession->Position.Duration / 1e7; } catch (...) { continue; }
        // Keep-behind is per device: 8s on a 1GB device (trim harder — memory is the scarce
        // resource that kills it on scroll+video), up to 30s on 3GB+. Set by the memory tier.
        double cutoff = pos - static_cast<double>(WebCoreMemBudgetMseKeepBehindSec());
        if (cutoff <= 0)
            continue;
        for (auto* b : h->buffers) {
            try {
                if (b->buffer->IsUpdating)
                    continue;
                auto ranges = b->buffer->Buffered;
                if (!ranges->Size || ranges->GetAt(0).Start.Duration / 1e7 >= cutoff)
                    continue; // nothing buffered behind the cutoff
                b->buffer->Remove(TimeSpan { 0 }, ref new Platform::Box<TimeSpan>(TimeSpan { static_cast<long long>(cutoff * 1e7) }));
                char m[96]; snprintf(m, sizeof m, "memwatch: MSE emergency trim [0, %.1fs) pos=%.1fs", cutoff, pos);
                PortImgLog(m);
                // Tell WebCore the ranges changed under it, or the page believes the removed span is
                // still buffered and playback wedges in "buffering" when the play head reaches it.
                WebCoreMseSbNoteEmergencyTrim(b->sbCtx);
            } catch (Platform::Exception^ ex) {
                char m[96]; snprintf(m, sizeof m, "memwatch: MSE trim THREW hr=0x%08lx (skipped)", (unsigned long)ex->HResult); PortImgLog(m);
            } catch (...) { PortImgLog("memwatch: MSE trim THREW (skipped)"); }
        }
    }
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
    try { h->source->Duration = TimeSpan { static_cast<long long>(seconds * 1e7) }; }
    catch (Platform::Exception^ ex) { char b[80]; snprintf(b, sizeof b, "mse: setDuration THREW hr=0x%08lx", (unsigned long)ex->HResult); PortImgLog(b); }
    catch (...) { }
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
// Shared player wiring for BOTH sources we play through the frame server: an MSE MseStreamSource
// and an HLS AdaptiveMediaSource (below). Expects h->mediaSource set; wires all session events.
// All handlers capture `h` and read h->playerCtx live (guarded by h->closing) so a WinRT event
// that fires during/after teardown can't call into a freed WebCore MediaPlayerPrivate.
static void startFrameServerPlayer(MseSource* h)
{
    {
        h->player = ref new MediaPlayer();
        h->player->IsVideoFrameServerEnabled = true;
        h->player->AutoPlay = true;
        h->player->Source = h->mediaSource;
        h->frameToken = h->player->VideoFrameAvailable += ref new TypedEventHandler<MediaPlayer^, Object^>(
            [h](MediaPlayer^, Object^) { onVideoFrame(h); });
        h->player->MediaOpened += ref new TypedEventHandler<MediaPlayer^, Object^>(
            [h](MediaPlayer^, Object^) { if (!h->closing) PortImgLog("mse: player MediaOpened"); });
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
        sess->BufferingEnded += ref new TypedEventHandler<MediaPlaybackSession^, Object^>(
            [h](MediaPlaybackSession^, Object^) { if (!h->closing) PortImgLog("mse: player BufferingEnded"); });
        sess->PositionChanged += ref new TypedEventHandler<MediaPlaybackSession^, Object^>(
            [h](MediaPlaybackSession^ s, Object^) { try { if (!h->closing && h->playerCtx) WebCoreMsePlayerTimeUpdate(h->playerCtx, s->Position.Duration / 1e7); } catch (...) { } });
        // Seek completion. Without this the port had no way to tell WebCore a seek had FINISHED, so
        // it used to just call timeChanged() straight out of seek() -- which re-entered
        // HTMLMediaElement mid-seek and recursed until the render thread's 16MB stack was gone.
        sess->SeekCompleted += ref new TypedEventHandler<MediaPlaybackSession^, Object^>(
            [h](MediaPlaybackSession^, Object^) {
                try {
                    if (h->closing || !h->playerCtx)
                        return;
                    PortImgLog("mse: player SeekCompleted");
                    WebCoreMsePlayerSeekCompleted(h->playerCtx);
                } catch (...) { }
            });
        PortImgLog("mse: MediaPlayer frame-server started (AutoPlay)");
    }
}

void PortMsePlayerStart(void* srcH, void* playerCtx)
{
    auto h = static_cast<MseSource*>(srcH);
    h->playerCtx = playerCtx;
    h->closing = false;
    try {
        h->mediaSource = MediaSource::CreateFromMseStreamSource(h->source);
        startFrameServerPlayer(h);
    } catch (Platform::Exception^ ex) {
        char b[128]; snprintf(b, sizeof b, "mse: MediaPlayer start FAILED hr=0x%08lx", (unsigned long)ex->HResult); PortImgLog(b);
        WebCoreMsePlayerError(playerCtx, ex->HResult);
    }
}

// ---- HLS playback via the platform's adaptive-streaming stack ----
// W10M's IMFMediaEngine cannot play an HLS manifest from this AppContainer (SetSource succeeds,
// LOADSTART fires, then nothing — no metadata, no error). The platform-blessed HLS path on UWP is
// Windows.Media.Streaming.Adaptive.AdaptiveMediaSource -> MediaSource -> MediaPlayer, which is the
// SAME frame-server pipeline the MSE path already uses (audio auto-routes, video frames arrive on
// VideoFrameAvailable). We present as iOS Safari, so sites (YouTube) hand us native HLS constantly —
// this is a first-class path, not an edge case. Creation is async; playerCtx callbacks report state.
void* PortHlsPlayerStart(const char* url, const char* userAgent, void* playerCtx)
{
    auto h = new MseSource(); // same handle type: player wiring + teardown are shared
    h->playerCtx = playerCtx;
    try {
        auto uri = ref new Windows::Foundation::Uri(toPlatformString(url));
        // Fetch the manifest/segments with the BROWSER'S identity. AMS's default HTTP stack sends a
        // Windows media UA; googlevideo (and most CDNs doing UA-based gating) then never serves the
        // playlist — AMS retried silently forever (no state change, no MediaFailed, 90s of nothing).
        // A browser fetches media subresources with its own UA; give AMS an HttpClient that does too.
        auto httpClient = ref new Windows::Web::Http::HttpClient();
        if (userAgent && *userAgent) {
            if (!httpClient->DefaultRequestHeaders->UserAgent->TryParseAdd(toPlatformString(userAgent)))
                PortImgLog("hls: UA TryParseAdd rejected (sending default UA)");
        }
        auto op = Windows::Media::Streaming::Adaptive::AdaptiveMediaSource::CreateFromUriAsync(uri, httpClient);
        op->Completed = ref new AsyncOperationCompletedHandler<Windows::Media::Streaming::Adaptive::AdaptiveMediaSourceCreationResult^>(
            [h](IAsyncOperation<Windows::Media::Streaming::Adaptive::AdaptiveMediaSourceCreationResult^>^ a, AsyncStatus s) {
                try {
                    if (h->closing)
                        return;
                    if (s != AsyncStatus::Completed) {
                        PortImgLog("hls: CreateFromUriAsync did not complete");
                        if (h->playerCtx) WebCoreMsePlayerError(h->playerCtx, -1);
                        return;
                    }
                    auto res = a->GetResults();
                    if (res->Status != Windows::Media::Streaming::Adaptive::AdaptiveMediaSourceCreationStatus::Success) {
                        char b[112]; snprintf(b, sizeof b, "hls: AMS create FAILED status=%d hr=0x%08x",
                            (int)res->Status, (unsigned)res->ExtendedError.Value); PortImgLog(b);
                        if (h->playerCtx) WebCoreMsePlayerError(h->playerCtx, (int)res->ExtendedError.Value);
                        return;
                    }
                    auto ams = res->MediaSource;
                    // Failures inside AMS are otherwise INVISIBLE (it retries internally; the player
                    // just never leaves Opening). Log every failed fetch with type + HTTP status.
                    ams->DownloadFailed += ref new TypedEventHandler<Windows::Media::Streaming::Adaptive::AdaptiveMediaSource^, Windows::Media::Streaming::Adaptive::AdaptiveMediaSourceDownloadFailedEventArgs^>(
                        [h](Windows::Media::Streaming::Adaptive::AdaptiveMediaSource^, Windows::Media::Streaming::Adaptive::AdaptiveMediaSourceDownloadFailedEventArgs^ e) {
                            try {
                                int status = 0;
                                if (e->HttpResponseMessage)
                                    status = (int)e->HttpResponseMessage->StatusCode;
                                std::wstring wu = e->ResourceUri ? std::wstring(e->ResourceUri->RawUri->Data()) : L"(null)";
                                std::string uu(wu.begin(), wu.end());
                                char b[400]; snprintf(b, sizeof b, "hls: DownloadFailed type=%d http=%d extErr=0x%08x uri=%.280s",
                                    (int)e->ResourceType, status, (unsigned)e->ExtendedError.Value, uu.c_str());
                                PortImgLog(b);
                                // FairPlay (skd:// key URI): unsatisfiable off Apple hardware. AMS
                                // retries the key fetch forever, burning CPU behind a black player.
                                // Fail the element once so the page can react (and we stop spinning).
                                if (uu.rfind("skd://", 0) == 0 && !h->closing && h->playerCtx) {
                                    PortImgLog("hls: FairPlay DRM key required (skd://) - UNPLAYABLE, failing element");
                                    void* ctx = h->playerCtx;
                                    h->playerCtx = nullptr; // report once
                                    h->closing = true;      // stop the retry storm from reaching us
                                    WebCoreMsePlayerError(ctx, (int)0x8004025E /* DRM not supported */);
                                }
                            } catch (...) { PortImgLog("hls: DownloadFailed (no detail)"); }
                        });
                    // Log the first requests AMS issues (type + URI) so a refused scheme or a
                    // mis-resolved relative URI is visible without guessing.
                    ams->DownloadRequested += ref new TypedEventHandler<Windows::Media::Streaming::Adaptive::AdaptiveMediaSource^, Windows::Media::Streaming::Adaptive::AdaptiveMediaSourceDownloadRequestedEventArgs^>(
                        [](Windows::Media::Streaming::Adaptive::AdaptiveMediaSource^, Windows::Media::Streaming::Adaptive::AdaptiveMediaSourceDownloadRequestedEventArgs^ e) {
                            try {
                                static int s_reqLogged = 0;
                                if (s_reqLogged >= 16 && (int)e->ResourceType != 3)
                                    return;
                                if (s_reqLogged < 40) {
                                    ++s_reqLogged;
                                    std::wstring wu = e->ResourceUri ? std::wstring(e->ResourceUri->RawUri->Data()) : L"(null)";
                                    std::string uu(wu.begin(), wu.end());
                                    char b[400]; snprintf(b, sizeof b, "hls: DownloadRequested type=%d uri=%.300s",
                                        (int)e->ResourceType, uu.c_str());
                                    PortImgLog(b);
                                }
                            } catch (...) { }
                        });
                    // Constrained device (390MB AppContainer cap, 720x1280 panel): start on the
                    // LOWEST rung and cap the ladder. Uncapped, AMS buffers the top variant
                    // (1080p+) and its download+decode buffers OOM-killed the app the moment the
                    // content pipeline spun up on top of a ~340MB heavy YouTube page.
                    try {
                        unsigned lowest = 0;
                        auto rates = ams->AvailableBitrates;
                        for (unsigned i = 0; i < rates->Size; ++i) {
                            unsigned r = rates->GetAt(i);
                            if (!lowest || r < lowest)
                                lowest = r;
                        }
                        if (lowest)
                            ams->InitialBitrate = lowest;
                        ams->DesiredMaxBitrate = ref new Platform::Box<unsigned int>(1500000u); // ~480-720p H.264
                        char b[96]; snprintf(b, sizeof b, "hls: bitrate initial=%u max=1500000 (of %u rungs)",
                            lowest, rates->Size); PortImgLog(b);
                    } catch (...) { PortImgLog("hls: bitrate caps not applied"); }
                    h->mediaSource = MediaSource::CreateFromAdaptiveMediaSource(ams);
                    startFrameServerPlayer(h);
                    PortImgLog("hls: AdaptiveMediaSource OK -> frame-server started");
                } catch (Platform::Exception^ ex) {
                    char b[96]; snprintf(b, sizeof b, "hls: start THREW hr=0x%08lx", (unsigned long)ex->HResult); PortImgLog(b);
                    if (!h->closing && h->playerCtx) WebCoreMsePlayerError(h->playerCtx, ex->HResult);
                } catch (...) { PortImgLog("hls: start THREW (unknown)"); }
            });
        PortImgLog("hls: AdaptiveMediaSource create requested");
    } catch (Platform::Exception^ ex) {
        char b[96]; snprintf(b, sizeof b, "hls: create THREW hr=0x%08lx", (unsigned long)ex->HResult); PortImgLog(b);
        WebCoreMsePlayerError(playerCtx, ex->HResult);
    }
    return h;
}

// Tear down an HLS player handle (stop first via PortMsePlayerStop, which is shared).
void PortHlsPlayerDestroy(void* srcH)
{
    PortMseDestroy(srcH); // shared teardown: closing flag, player Close, handle free (source is null)
}

// ============================ PROGRESSIVE (<video src="...mp4">) ============================
//
// IMFMediaEngine::SetSource(url) makes the ENGINE fetch the URL with the legacy Windows Media network
// source: its own UA, its own cookie jar (empty), no Referer. SetSource returns S_OK and then the
// engine fails asynchronously -- exactly what the log shows on nearly every CDN:
//   mf: SetSource hr=0x00000000 -> mf: EVENT_ERROR code=4 (SRC_NOT_SUPPORTED) extHr=0xc00d001a / 0xc00d0035
// on ew.phncdn.com, ht-cdn.trafficjunky.net, ht-cdn2.adtng.com ... while a plain unguarded MP4 from
// evtubescms.phncdn.com plays fine. Same container, same codecs => it is the FETCH being refused, not
// the media. And even when it does play, IMFMediaEngine gives us no per-frame callback, so the video
// only repainted on TIMEUPDATE (~4/sec) -- the choppiness.
//
// Fix both by owning the transport: read the URL through Windows.Web.Http with the BROWSER'S identity,
// hand MF a stream instead of a URL, and play it on the SAME frame-server MediaPlayer that MSE/HLS use
// (VideoFrameAvailable per decoded frame).
// Ranged fetch over the browser's own curl stack (DoH resolver + our TLS profile + Referer/UA).
// Blocking; only ever called from worker threads. See port/PortMediaFetch.cpp for why this cannot
// go through Windows.Web.Http: the OS resolver cannot resolve these media hosts (0x80072ee7), curl can.
extern "C" int WebCoreMediaFetchRange(const char* url, const char* userAgent, const char* referer,
    unsigned long long start, unsigned long long count,
    uint8_t* out, unsigned long long* outRead,
    unsigned long long* outTotal, char* outType, int outTypeLen, int* outStatus);

namespace RevenantMedia {

// Raw bytes behind an IBuffer, so a range response can be written straight into it.
static uint8_t* bufferBytes(IBuffer^ buffer)
{
    Microsoft::WRL::ComPtr<IInspectable> insp(reinterpret_cast<IInspectable*>(buffer));
    Microsoft::WRL::ComPtr<Windows::Storage::Streams::IBufferByteAccess> access;
    if (FAILED(insp.As(&access)))
        return nullptr;
    uint8_t* bytes = nullptr;
    if (FAILED(access->Buffer(&bytes)))
        return nullptr;
    return bytes;
}

// A seekable byte stream MF can demux from, served by ranged curl GETs -- so the whole file is never
// buffered in memory (a 390MB-cap device cannot afford that) and seeking works.
private ref class HttpRandomAccessStream sealed : public IRandomAccessStreamWithContentType {
internal:
    HttpRandomAccessStream(Platform::String^ url, Platform::String^ userAgent, Platform::String^ referer,
        unsigned long long size, Platform::String^ contentType)
        : m_url(url), m_userAgent(userAgent), m_referer(referer), m_size(size), m_contentType(contentType) { }

public:
    virtual Windows::Foundation::IAsyncOperationWithProgress<IBuffer^, unsigned int>^ ReadAsync(
        IBuffer^ buffer, unsigned int count, InputStreamOptions options)
    {
        (void)buffer; (void)options;
        std::string url = toUtf8(m_url);
        std::string ua = toUtf8(m_userAgent);
        std::string rf = toUtf8(m_referer);
        unsigned long long start = m_pos;
        unsigned long long size = m_size;
        auto self = this;
        return concurrency::create_async(
            [url, ua, rf, start, count, size, self](concurrency::progress_reporter<unsigned int> reporter)
                -> IBuffer^ {
                unsigned long long want = count;
                if (size && start >= size)
                    return ref new Windows::Storage::Streams::Buffer(0);
                if (size && start + want > size)
                    want = size - start;

                auto data = ref new Windows::Storage::Streams::Buffer(static_cast<unsigned int>(want));
                uint8_t* dst = bufferBytes(data);
                if (!dst)
                    return ref new Windows::Storage::Streams::Buffer(0);

                unsigned long long got = 0;
                int status = 0;
                int rc = WebCoreMediaFetchRange(url.c_str(), ua.c_str(), rf.c_str(), start, want,
                    dst, &got, nullptr, nullptr, 0, &status);
                if (rc != 0)
                    got = 0;

                data->Length = static_cast<unsigned int>(got);
                self->m_pos = start + got;
                reporter.report(static_cast<unsigned int>(got));
                return data;
            });
    }

    virtual Windows::Foundation::IAsyncOperationWithProgress<unsigned int, unsigned int>^ WriteAsync(IBuffer^)
    {
        throw ref new Platform::NotImplementedException(); // read-only stream
    }
    virtual Windows::Foundation::IAsyncOperation<bool>^ FlushAsync()
    {
        throw ref new Platform::NotImplementedException();
    }
    virtual IInputStream^ GetInputStreamAt(unsigned long long position)
    {
        auto s = ref new HttpRandomAccessStream(m_url, m_userAgent, m_referer, m_size, m_contentType);
        s->m_pos = position;
        return s;
    }
    virtual IOutputStream^ GetOutputStreamAt(unsigned long long)
    {
        throw ref new Platform::NotImplementedException();
    }
    virtual void Seek(unsigned long long position) { m_pos = position; }
    virtual IRandomAccessStream^ CloneStream()
    {
        auto s = ref new HttpRandomAccessStream(m_url, m_userAgent, m_referer, m_size, m_contentType);
        s->m_pos = m_pos;
        return s;
    }
    // C++/CX projects IClosable::Close as Platform::IDisposable::Dispose, and the way to implement it
    // is a public destructor -- a Close() method does NOT satisfy the interface. Nothing to release:
    // the HttpClient and Uri are ref-counted and each range request owns its own response.
    virtual ~HttpRandomAccessStream() { }

    property bool CanRead { virtual bool get() { return true; } }
    property bool CanWrite { virtual bool get() { return false; } }
    property unsigned long long Position { virtual unsigned long long get() { return m_pos; } }
    property unsigned long long Size {
        virtual unsigned long long get() { return m_size; }
        virtual void set(unsigned long long v) { m_size = v; }
    }
    property Platform::String^ ContentType { virtual Platform::String^ get() { return m_contentType; } }

private:
    Platform::String^ m_url;
    Platform::String^ m_userAgent;
    Platform::String^ m_referer;
    unsigned long long m_pos { 0 };
    unsigned long long m_size { 0 };
    Platform::String^ m_contentType;
};

} // namespace RevenantMedia

// Probe the URL (range request for the first byte) to learn its length + content type, then play it
// off an HttpRandomAccessStream on the frame-server pipeline. Async: playerCtx callbacks report state.
void* PortProgressivePlayerStart(const char* url, const char* userAgent, const char* referer, void* playerCtx)
{
    auto h = new MseSource(); // same handle type: player wiring + teardown are shared with MSE/HLS
    h->playerCtx = playerCtx;
    std::string u = url ? url : "";
    std::string ua = userAgent ? userAgent : "";
    std::string rf = referer ? referer : "";
    try {
        // The probe is a blocking curl GET, so it must not run on the WebCore main thread.
        concurrency::create_task([h, u, ua, rf] {
            try {
                if (h->closing)
                    return;
                uint8_t first = 0;
                unsigned long long got = 0, total = 0;
                char ctype[128] = { 0 };
                int status = 0;
                int rc = WebCoreMediaFetchRange(u.c_str(), ua.c_str(), rf.c_str(), 0, 1,
                    &first, &got, &total, ctype, sizeof ctype, &status);
                {
                    char b[240];
                    snprintf(b, sizeof b, "prog: probe rc=%d http=%d len=%llu type=%s", rc, status, total, ctype);
                    PortImgLog(b);
                }
                if (h->closing)
                    return;
                if (rc != 0) {
                    // Names the refusal outright (403/404/curl code) instead of laundering it through
                    // MediaFoundation as "SRC_NOT_SUPPORTED".
                    if (h->playerCtx) WebCoreMsePlayerError(h->playerCtx, status ? status : -1);
                    return;
                }
                if (!total) {
                    PortImgLog("prog: no content length -> cannot seek, failing element");
                    if (h->playerCtx) WebCoreMsePlayerError(h->playerCtx, -1);
                    return;
                }

                auto ctypeStr = toPlatformString(ctype);
                auto stream = ref new RevenantMedia::HttpRandomAccessStream(
                    toPlatformString(u.c_str()), toPlatformString(ua.c_str()), toPlatformString(rf.c_str()),
                    total, ctypeStr);
                h->mediaSource = MediaSource::CreateFromStream(stream, ctypeStr);
                startFrameServerPlayer(h);
                PortImgLog("prog: stream OK -> frame-server started");
            } catch (Platform::Exception^ ex) {
                char b[112];
                snprintf(b, sizeof b, "prog: probe THREW hr=0x%08lx", (unsigned long)ex->HResult);
                PortImgLog(b);
                if (!h->closing && h->playerCtx) WebCoreMsePlayerError(h->playerCtx, ex->HResult);
            } catch (...) {
                PortImgLog("prog: probe THREW (unknown)");
                if (!h->closing && h->playerCtx) WebCoreMsePlayerError(h->playerCtx, -1);
            }
        });
        PortImgLog("prog: probe requested (curl/DoH)");
    } catch (Platform::Exception^ ex) {
        char b[96];
        snprintf(b, sizeof b, "prog: create THREW hr=0x%08lx", (unsigned long)ex->HResult);
        PortImgLog(b);
        WebCoreMsePlayerError(playerCtx, ex->HResult);
    }
    return h;
}

void PortProgressivePlayerDestroy(void* srcH)
{
    PortMseDestroy(srcH); // shared teardown
}

// Detach + close the player synchronously. Called from WebCore's MediaPlayerPrivate destructor
// BEFORE the object (playerCtx) is freed, so no WinRT event can fire into a dangling pointer.
void PortMsePlayerStop(void* srcH)
{
    auto h = static_cast<MseSource*>(srcH);
    h->closing = true;
    h->playerCtx = nullptr;
    if (h->player) {
        // Take ownership so we can null h->player immediately (no dangling events) and outlive `h`.
        MediaPlayer^ player = h->player;
        // Keep the media source + its backing byte stream (progressive: HttpRandomAccessStream) alive in
        // the worker until AFTER delete player, or an in-flight MF source-reader Seek (RTWorkQ) hits the
        // disposed stream -> Platform::ObjectDisposedException -> crash (Rule34 video-select). See
        // PortMseDestroy for the full note.
        auto mediaSource = h->mediaSource;
        auto mseSource = h->source;
        h->player = nullptr;
        // Stop the frame server FIRST: unhook VideoFrameAvailable so no CopyFrameToVideoSurface can
        // run against a player mid-teardown (an in-flight handler racing Close is a deadlock vector).
        try { if (h->frameToken.Value) player->VideoFrameAvailable -= h->frameToken; } catch (...) { }
        // Tear down ENTIRELY on a threadpool thread, in stages: Pause -> detach the source (this is
        // what actually stops the MF decode pipeline + frame server, synchronously but off the UI
        // thread) -> Close. The previous shape (Pause on the UI thread, one-shot Close on the pool)
        // still froze the UI thread: right after "player stop" the XAML frame commit blocked forever
        // in a system wait (stage=f:present-done, zero WebCore frames on the stack, cpuDelta=0) and
        // the W10M watchdog killed the app ~60s later. Detaching Source before Close makes Close
        // trivial instead of a full-pipeline teardown racing DWM composition.
        try {
            Windows::System::Threading::ThreadPool::RunAsync(
                ref new Windows::System::Threading::WorkItemHandler(
                    [player, mediaSource, mseSource](Windows::Foundation::IAsyncAction^) {
                        try { player->Pause(); } catch (...) { }
                        try { player->Source = nullptr; } catch (...) { }
                        try { delete player; } catch (...) { }
                        (void)mediaSource; (void)mseSource; // released after Close -> no late Seek crash
                        PortImgLog("mse: player closed (worker)");
                    }));
        } catch (...) {
            try { delete player; } catch (...) { } // fallback: close inline if dispatch fails
        }
    }
    PortImgLog("mse: player stop (async close)");
}

// Play/Pause used to swallow every exception silently, so a refused Play() was invisible in the log.
void PortMsePlayerPlay(void* srcH)
{
    auto h = static_cast<MseSource*>(srcH);
    if (!h->player) { PortImgLog("mse: Play() SKIPPED - no player"); return; }
    try {
        int before = (int)h->player->PlaybackSession->PlaybackState;
        h->player->Play();
        int after = (int)h->player->PlaybackSession->PlaybackState;
        char b[80]; snprintf(b, sizeof b, "mse: Play() called (state %d -> %d)", before, after); PortImgLog(b);
    } catch (Platform::Exception^ ex) {
        char b[80]; snprintf(b, sizeof b, "mse: Play() THREW hr=0x%08lx", (unsigned long)ex->HResult); PortImgLog(b);
    } catch (...) { PortImgLog("mse: Play() THREW (unknown)"); }
}

void PortMsePlayerPause(void* srcH)
{
    auto h = static_cast<MseSource*>(srcH);
    if (!h->player) return;
    try { h->player->Pause(); PortImgLog("mse: Pause() called"); }
    catch (Platform::Exception^ ex) { char b[80]; snprintf(b, sizeof b, "mse: Pause() THREW hr=0x%08lx", (unsigned long)ex->HResult); PortImgLog(b); }
    catch (...) { }
}
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
