// ShellMseBridge.h — C ABI between WebCore's MSE platform classes and the shell's WinRT MSE
// (ShellMse.cpp). WebCore drives MseStreamSource/MseSourceBuffer through these; the shell calls the
// WebCoreMse* callbacks back (on the UI thread) as WinRT events fire. See ShellMse.cpp.
#pragma once
#include <cstdint>

extern "C" {

// --- WebCore -> shell (WinRT MseStreamSource / MseSourceBuffer) ---
void* PortMseCreate(void* srcCtx);
void PortMseDestroy(void* srcH);
int PortMseIsTypeSupported(void* srcH, const char* type);
void* PortMseAddSourceBuffer(void* srcH, const char* type, void* sbCtx);
void PortMseAppend(void* sbH, const uint8_t* data, int len);
int PortMseIsUpdating(void* sbH);
void PortMseAbort(void* sbH);
void PortMseSetTimestampOffset(void* sbH, double seconds);
int PortMseGetBuffered(void* sbH, double* starts, double* ends, int maxN);
void PortMseSetDuration(void* srcH, double seconds);
void PortMseEndOfStream(void* srcH, int status);
void* PortMseGetMFMediaSource(void* srcH); // returns ref'd IMFMediaSource* (caller releases)

// --- WinRT MediaPlayer frame-server playback (Option B) ---
// MseStreamSource can't be played by IMFMediaEngine on W10M (it exposes IMFMediaSourceExtension,
// not IMFMediaSource). The supported path is Windows.Media.Playback.MediaPlayer in frame-server
// mode: MediaSource.CreateFromMseStreamSource -> MediaPlayer(IsVideoFrameServerEnabled) -> audio
// plays automatically, video frames arrive via VideoFrameAvailable -> CopyFrameToVideoSurface,
// which we read back and hand to WebCore as BGRA. playerCtx is the MediaPlayerPrivate pointer.
void PortMsePlayerStart(void* srcH, void* playerCtx);
void PortMsePlayerStop(void* srcH); // detach + close the player synchronously (call before playerCtx is freed)
void PortMsePlayerPlay(void* srcH);
void PortMsePlayerPause(void* srcH);
void PortMsePlayerSetRate(void* srcH, double rate);
void PortMsePlayerSetVolume(void* srcH, double volume);
void PortMsePlayerSetMuted(void* srcH, int muted);
void PortMsePlayerSeek(void* srcH, double seconds);
double PortMsePlayerPosition(void* srcH);
double PortMsePlayerDuration(void* srcH);
void PortMsePlayerNativeSize(void* srcH, int* w, int* h);   // decoded video size (compositor thread)
int PortMsePlayerCopyFrame(void* srcH, void* d3dTexture);   // zero-copy current frame -> ANGLE texture

// --- shell -> WebCore (WinRT events, on the UI thread) ---
void WebCoreMseSbUpdateEnded(void* sbCtx);
void WebCoreMseSbErrored(void* sbCtx, int hr);
void WebCoreMseSourceOpened(void* srcCtx);
void WebCoreMseSourceEnded(void* srcCtx);

// --- shell -> WebCore (MediaPlayer frame-server events; may fire off the main thread) ---
void WebCoreMsePlayerFrame(void* playerCtx, const uint8_t* bgra, int width, int height, int stride);
void WebCoreMsePlayerStateChanged(void* playerCtx, int state); // 0 none/closed,1 opening,2 buffering,3 playing,4 paused,5 ended
void WebCoreMsePlayerError(void* playerCtx, int hr);
void WebCoreMsePlayerDurationChanged(void* playerCtx, double seconds);
void WebCoreMsePlayerSizeChanged(void* playerCtx, int width, int height);
void WebCoreMsePlayerTimeUpdate(void* playerCtx, double seconds);

}
