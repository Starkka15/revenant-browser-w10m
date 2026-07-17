/*
 * AudioDestination for the Revenant WinCairo/UWP (ARM32) port.
 *
 * Real hardware output via WASAPI shared-mode, event-driven rendering. The audio
 * endpoint is obtained the UWP-sanctioned way (no desktop IMMDeviceEnumerator):
 * IMediaDeviceStatics::GetDefaultAudioRenderId + ActivateAudioInterfaceAsync.
 *
 * Structure mirrors AudioDestinationCocoa: a PushPullFIFO bridges the 128-frame
 * WebAudio render quantum and the device's (larger, variable) buffer period, and
 * an optional MultiChannelResampler converts the AudioContext sample-rate to the
 * hardware rate.
 */

#pragma once

#if ENABLE(WEB_AUDIO)

#include "AudioDestination.h"
#include <atomic>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/RefPtr.h>
#include <wtf/Threading.h>
#include <wtf/UniqueRef.h>

struct IAudioClient;
struct IAudioRenderClient;
typedef struct tWAVEFORMATEX WAVEFORMATEX;

namespace WebCore {

class AudioBus;
class MultiChannelResampler;
class PushPullFIFO;

class AudioDestinationWinUWP final : public AudioDestination {
public:
    AudioDestinationWinUWP(AudioIOCallback&, unsigned numberOfOutputChannels, float sampleRate);
    virtual ~AudioDestinationWinUWP();

    // Queries the default render endpoint's mix format once and caches it.
    static bool queryDeviceFormat(float& sampleRate, unsigned& channels);

private:
    void start(Function<void(Function<void()>&&)>&& dispatchToRenderThread, CompletionHandler<void(bool)>&&) final;
    void stop(CompletionHandler<void(bool)>&&) final;
    bool isPlaying() final { return m_isPlaying; }
    float sampleRate() const final { return m_contextSampleRate; }
    unsigned framesPerBuffer() const final;

    bool initializeDevice();
    void teardownDevice();
    void renderThreadEntry();
    void renderQuantaIntoFIFO(size_t framesToRender);
    void interleaveOutputBus(void* deviceBuffer, unsigned frames);

    // Bridges the 128-frame WebAudio quantum and the device buffer period.
    Ref<AudioBus> m_outputBus;   // pulled from FIFO, then interleaved to the device
    Ref<AudioBus> m_renderBus;   // one render quantum pushed into the FIFO

    Lock m_fifoLock;
    UniqueRef<PushPullFIFO> m_fifo WTF_GUARDED_BY_LOCK(m_fifoLock);

    std::unique_ptr<MultiChannelResampler> m_resampler;

    Lock m_dispatchToRenderThreadLock;
    Function<void(Function<void()>&&)> m_dispatchToRenderThread WTF_GUARDED_BY_LOCK(m_dispatchToRenderThreadLock);

    float m_contextSampleRate;
    float m_hardwareSampleRate { 48000 };
    unsigned m_numberOfOutputChannels;

    // WASAPI device objects (raw COM; addref/release handled manually).
    IAudioClient* m_audioClient { nullptr };
    IAudioRenderClient* m_renderClient { nullptr };
    WAVEFORMATEX* m_mixFormat { nullptr };
    unsigned m_deviceBufferFrames { 0 };
    bool m_deviceIsFloat { true };
    unsigned m_deviceBytesPerSample { 4 };

    void* m_sampleReadyEvent { nullptr }; // HANDLE
    RefPtr<Thread> m_renderThread;
    std::atomic<bool> m_isPlaying { false };
    std::atomic<bool> m_stopRequested { false };
};

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
