/*
 * AudioDestination for the Revenant WinCairo/UWP (ARM32) port — real WASAPI output.
 * See AudioDestinationWinUWP.h for the design overview.
 */

#include "config.h"
#include "AudioDestinationWinUWP.h"

#if ENABLE(WEB_AUDIO)

#include "AudioBus.h"
#include "AudioIOCallback.h"
#include "AudioUtilities.h"
#include "MultiChannelResampler.h"
#include "PushPullFIFO.h"
#include <wtf/MainThread.h>
#include <wtf/MathExtras.h>

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <roapi.h>
#include <winstring.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.media.devices.h>

namespace WebCore {

constexpr size_t fifoSize = 96 * AudioUtilities::renderQuantumSize;

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT — spelled out so we don't depend on <ksmedia.h>
// being reachable under the UWP header partition.
static const GUID kFloatSubFormat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static bool isFloatFormat(const WAVEFORMATEX* f)
{
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        return IsEqualGUID(ext->SubFormat, kFloatSubFormat);
    }
    return false;
}

// ---- Async activation of the default render endpoint (UWP-sanctioned path) ----

namespace {

class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit ActivateHandler(HANDLE doneEvent)
        : m_doneEvent(doneEvent) { }

    HRESULT m_activateHr { E_FAIL };
    IUnknown* m_result { nullptr };

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r)
            delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
    {
        HRESULT hrActivate = E_FAIL;
        IUnknown* punk = nullptr;
        HRESULT hr = operation->GetActivateResult(&hrActivate, &punk);
        m_activateHr = SUCCEEDED(hr) ? hrActivate : hr;
        m_result = punk; // keeps the reference; caller releases
        SetEvent(m_doneEvent);
        return S_OK;
    }

private:
    LONG m_ref { 1 };
    HANDLE m_doneEvent;
};

IAudioClient* activateDefaultRenderClient()
{
    using namespace Microsoft::WRL;
    using namespace Microsoft::WRL::Wrappers;

    ComPtr<ABI::Windows::Media::Devices::IMediaDeviceStatics> statics;
    HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Media_Devices_MediaDevice).Get(), IID_PPV_ARGS(&statics));
    if (FAILED(hr) || !statics)
        return nullptr;

    HString deviceId;
    hr = statics->GetDefaultAudioRenderId(ABI::Windows::Media::Devices::AudioDeviceRole_Default, deviceId.GetAddressOf());
    if (FAILED(hr))
        return nullptr;

    UINT32 idLen = 0;
    PCWSTR rawId = WindowsGetStringRawBuffer(deviceId.Get(), &idLen);
    if (!rawId || !idLen)
        return nullptr;

    HANDLE doneEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS | SYNCHRONIZE);
    if (!doneEvent)
        return nullptr;

    ActivateHandler* handler = new ActivateHandler(doneEvent);
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    hr = ActivateAudioInterfaceAsync(rawId, __uuidof(IAudioClient), nullptr, handler, &operation);
    if (FAILED(hr)) {
        handler->Release();
        CloseHandle(doneEvent);
        return nullptr;
    }

    WaitForSingleObjectEx(doneEvent, INFINITE, FALSE);

    IAudioClient* client = nullptr;
    if (SUCCEEDED(handler->m_activateHr) && handler->m_result)
        handler->m_result->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client));

    if (handler->m_result)
        handler->m_result->Release();
    if (operation)
        operation->Release();
    handler->Release();
    CloseHandle(doneEvent);
    return client;
}

} // anonymous namespace

// ---- AudioDestination static factory + hardware queries ----

Ref<AudioDestination> AudioDestination::create(AudioIOCallback& callback, const String&, unsigned numberOfInputChannels, unsigned numberOfOutputChannels, float sampleRate)
{
    if (numberOfInputChannels)
        WTFLogAlways("AudioDestinationWinUWP: live input (%u ch) not supported", numberOfInputChannels);
    return adoptRef(*new AudioDestinationWinUWP(callback, numberOfOutputChannels, sampleRate));
}

bool AudioDestinationWinUWP::queryDeviceFormat(float& sampleRate, unsigned& channels)
{
    static Lock lock;
    static bool cached = false;
    static bool ok = false;
    static float cachedRate = 48000;
    static unsigned cachedChannels = 2;

    Locker locker { lock };
    if (!cached) {
        cached = true;
        if (IAudioClient* client = activateDefaultRenderClient()) {
            WAVEFORMATEX* fmt = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&fmt)) && fmt) {
                cachedRate = static_cast<float>(fmt->nSamplesPerSec);
                cachedChannels = fmt->nChannels;
                ok = true;
                CoTaskMemFree(fmt);
            }
            client->Release();
        }
    }
    sampleRate = cachedRate;
    channels = cachedChannels;
    return ok;
}

float AudioDestination::hardwareSampleRate()
{
    float rate;
    unsigned channels;
    if (AudioDestinationWinUWP::queryDeviceFormat(rate, channels))
        return rate;
    return 48000;
}

unsigned long AudioDestination::maxChannelCount()
{
    float rate;
    unsigned channels;
    if (AudioDestinationWinUWP::queryDeviceFormat(rate, channels))
        return channels;
    return 0;
}

// ---- Instance ----

AudioDestinationWinUWP::AudioDestinationWinUWP(AudioIOCallback& callback, unsigned numberOfOutputChannels, float sampleRate)
    : AudioDestination(callback)
    , m_outputBus(AudioBus::create(numberOfOutputChannels, AudioUtilities::renderQuantumSize).releaseNonNull())
    , m_renderBus(AudioBus::create(numberOfOutputChannels, AudioUtilities::renderQuantumSize).releaseNonNull())
    , m_fifo(makeUniqueRef<PushPullFIFO>(numberOfOutputChannels, fifoSize))
    , m_contextSampleRate(sampleRate)
    , m_numberOfOutputChannels(numberOfOutputChannels)
{
    float hwRate;
    unsigned hwChannels;
    if (queryDeviceFormat(hwRate, hwChannels))
        m_hardwareSampleRate = hwRate;

    if (sampleRate != m_hardwareSampleRate) {
        double scaleFactor = static_cast<double>(sampleRate) / m_hardwareSampleRate;
        m_resampler = makeUnique<MultiChannelResampler>(scaleFactor, numberOfOutputChannels, AudioUtilities::renderQuantumSize, [this](AudioBus* bus, size_t framesToProcess) {
            ASSERT_UNUSED(framesToProcess, framesToProcess == AudioUtilities::renderQuantumSize);
            callRenderCallback(nullptr, bus, AudioUtilities::renderQuantumSize, { });
        });
    }
}

AudioDestinationWinUWP::~AudioDestinationWinUWP()
{
    m_stopRequested = true;
    if (m_sampleReadyEvent)
        SetEvent(static_cast<HANDLE>(m_sampleReadyEvent));
    if (m_renderThread) {
        m_renderThread->waitForCompletion();
        m_renderThread = nullptr;
    }
    teardownDevice();
}

unsigned AudioDestinationWinUWP::framesPerBuffer() const
{
    return m_renderBus->length();
}

bool AudioDestinationWinUWP::initializeDevice()
{
    m_audioClient = activateDefaultRenderClient();
    if (!m_audioClient)
        return false;

    if (FAILED(m_audioClient->GetMixFormat(&m_mixFormat)) || !m_mixFormat)
        return false;

    m_hardwareSampleRate = static_cast<float>(m_mixFormat->nSamplesPerSec);
    m_deviceBytesPerSample = m_mixFormat->wBitsPerSample / 8;
    m_deviceIsFloat = isFloatFormat(m_mixFormat);

    REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
    m_audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);

    HRESULT hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, defaultPeriod, 0, m_mixFormat, nullptr);
    if (FAILED(hr))
        return false;

    if (FAILED(m_audioClient->GetBufferSize(&m_deviceBufferFrames)) || !m_deviceBufferFrames)
        return false;

    m_sampleReadyEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS | SYNCHRONIZE);
    if (!m_sampleReadyEvent)
        return false;
    if (FAILED(m_audioClient->SetEventHandle(static_cast<HANDLE>(m_sampleReadyEvent))))
        return false;

    if (FAILED(m_audioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&m_renderClient))))
        return false;

    // The device buffer period is larger than one render quantum; the FIFO pull fills
    // the whole period in one shot, so grow m_outputBus to match.
    m_outputBus = AudioBus::create(m_numberOfOutputChannels, m_deviceBufferFrames).releaseNonNull();
    return true;
}

void AudioDestinationWinUWP::teardownDevice()
{
    if (m_renderClient) {
        m_renderClient->Release();
        m_renderClient = nullptr;
    }
    if (m_audioClient) {
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    if (m_mixFormat) {
        CoTaskMemFree(m_mixFormat);
        m_mixFormat = nullptr;
    }
    if (m_sampleReadyEvent) {
        CloseHandle(static_cast<HANDLE>(m_sampleReadyEvent));
        m_sampleReadyEvent = nullptr;
    }
}

void AudioDestinationWinUWP::start(Function<void(Function<void()>&&)>&& dispatchToRenderThread, CompletionHandler<void(bool)>&& completionHandler)
{
    ASSERT(isMainThread());
    {
        Locker locker { m_dispatchToRenderThreadLock };
        m_dispatchToRenderThread = WTFMove(dispatchToRenderThread);
    }

    bool success = initializeDevice();
    if (success) {
        m_stopRequested = false;
        m_isPlaying = true;
        m_renderThread = Thread::create("WebAudio WASAPI Render", [this] {
            renderThreadEntry();
        });
        {
            Locker locker { m_callbackLock };
            if (m_callback)
                m_callback->isPlayingDidChange();
        }
    }

    callOnMainThread([completionHandler = WTFMove(completionHandler), success]() mutable {
        completionHandler(success);
    });
}

void AudioDestinationWinUWP::stop(CompletionHandler<void(bool)>&& completionHandler)
{
    ASSERT(isMainThread());

    m_stopRequested = true;
    if (m_sampleReadyEvent)
        SetEvent(static_cast<HANDLE>(m_sampleReadyEvent));
    if (m_renderThread) {
        m_renderThread->waitForCompletion();
        m_renderThread = nullptr;
    }

    bool wasPlaying = m_isPlaying.exchange(false);
    teardownDevice();

    {
        Locker locker { m_dispatchToRenderThreadLock };
        m_dispatchToRenderThread = nullptr;
    }

    if (wasPlaying) {
        Locker locker { m_callbackLock };
        if (m_callback)
            m_callback->isPlayingDidChange();
    }

    callOnMainThread([completionHandler = WTFMove(completionHandler)]() mutable {
        completionHandler(true);
    });
}

void AudioDestinationWinUWP::renderThreadEntry()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Prime the endpoint with one buffer of silence so the event callback starts.
    BYTE* primeBuffer = nullptr;
    if (SUCCEEDED(m_renderClient->GetBuffer(m_deviceBufferFrames, &primeBuffer)))
        m_renderClient->ReleaseBuffer(m_deviceBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);

    m_audioClient->Start();

    while (!m_stopRequested.load()) {
        DWORD wait = WaitForSingleObjectEx(static_cast<HANDLE>(m_sampleReadyEvent), 200, FALSE);
        if (m_stopRequested.load())
            break;
        if (wait != WAIT_OBJECT_0)
            continue;

        UINT32 padding = 0;
        if (FAILED(m_audioClient->GetCurrentPadding(&padding)))
            continue;
        UINT32 available = m_deviceBufferFrames - padding;
        if (!available)
            continue;

        BYTE* deviceBuffer = nullptr;
        if (FAILED(m_renderClient->GetBuffer(available, &deviceBuffer)))
            continue;

        size_t framesToRender;
        {
            Locker locker { m_fifoLock };
            framesToRender = m_fifo->pull(m_outputBus.ptr(), available);
        }

        interleaveOutputBus(deviceBuffer, available);
        m_renderClient->ReleaseBuffer(available, 0);

        // Refill the FIFO for the next device period.
        if (m_dispatchToRenderThreadLock.tryLock()) {
            Locker locker { AdoptLock, m_dispatchToRenderThreadLock };
            if (m_dispatchToRenderThread) {
                m_dispatchToRenderThread([protectedThis = Ref { *this }, framesToRender]() mutable {
                    if (protectedThis->m_isPlaying)
                        protectedThis->renderQuantaIntoFIFO(framesToRender);
                });
            } else if (m_isPlaying)
                renderQuantaIntoFIFO(framesToRender);
        }
    }

    m_audioClient->Stop();
    CoUninitialize();
}

void AudioDestinationWinUWP::renderQuantaIntoFIFO(size_t framesToRender)
{
    for (size_t pushedFrames = 0; pushedFrames < framesToRender; pushedFrames += AudioUtilities::renderQuantumSize) {
        if (m_resampler)
            m_resampler->process(m_renderBus.ptr(), AudioUtilities::renderQuantumSize);
        else
            callRenderCallback(nullptr, m_renderBus.ptr(), AudioUtilities::renderQuantumSize, { });

        Locker locker { m_fifoLock };
        m_fifo->push(m_renderBus.ptr());
    }
}

void AudioDestinationWinUWP::interleaveOutputBus(void* deviceBuffer, unsigned frames)
{
    unsigned deviceChannels = m_mixFormat->nChannels;
    unsigned busChannels = m_outputBus->numberOfChannels();

    if (m_deviceIsFloat) {
        float* out = static_cast<float*>(deviceBuffer);
        for (unsigned f = 0; f < frames; ++f) {
            for (unsigned c = 0; c < deviceChannels; ++c)
                out[f * deviceChannels + c] = (c < busChannels) ? m_outputBus->channel(c)->data()[f] : 0.0f;
        }
    } else if (m_deviceBytesPerSample == 2) {
        int16_t* out = static_cast<int16_t*>(deviceBuffer);
        for (unsigned f = 0; f < frames; ++f) {
            for (unsigned c = 0; c < deviceChannels; ++c) {
                float v = (c < busChannels) ? m_outputBus->channel(c)->data()[f] : 0.0f;
                v = clampTo(v, -1.0f, 1.0f);
                out[f * deviceChannels + c] = static_cast<int16_t>(v * 32767.0f);
            }
        }
    } else
        memset(deviceBuffer, 0, static_cast<size_t>(frames) * deviceChannels * m_deviceBytesPerSample);
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
