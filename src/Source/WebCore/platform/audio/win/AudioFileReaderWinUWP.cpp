/*
 * AudioFileReader for the Revenant WinCairo/UWP (ARM32) port — decodeAudioData
 * via Media Foundation's IMFSourceReader.
 *
 * The compressed bytes are staged to the app's per-app temp folder and decoded
 * with MFCreateSourceReaderFromURL (the UWP-clean path — no custom async
 * IMFByteStream), configured to emit 32-bit float PCM. The interleaved output is
 * deinterleaved into a planar AudioBus, then mixed/resampled to the requested
 * format with WebKit's own AudioBus::createBySampleRateConverting.
 */

#include "config.h"

#if ENABLE(WEB_AUDIO)

#include "AudioBus.h"
#include "AudioFileReader.h"
#include <vector>
#include <wtf/RefPtr.h>

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

namespace WebCore {

namespace {

// Minimal RAII for COM interfaces used here (the port already carries COMPtr, but
// keeping this file self-contained avoids header coupling).
template<typename T> struct MFPtr {
    T* p { nullptr };
    ~MFPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

bool writeTempFile(const void* data, size_t dataSize, std::wstring& outPath)
{
    wchar_t tempDir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tempDir);
    if (!n || n > MAX_PATH)
        return false;

    static LONG s_counter = 0;
    LONG id = InterlockedIncrement(&s_counter);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%sreva_decode_%lu_%lu_%llu.bin", tempDir,
        static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned long>(id),
        static_cast<unsigned long long>(GetTickCount64()));

    HANDLE h = CreateFile2(path, GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    bool ok = true;
    const BYTE* cursor = static_cast<const BYTE*>(data);
    size_t remaining = dataSize;
    while (remaining) {
        DWORD chunk = static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
        DWORD written = 0;
        if (!WriteFile(h, cursor, chunk, &written, nullptr) || !written) {
            ok = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }
    CloseHandle(h);
    if (ok)
        outPath = path;
    else
        DeleteFileW(path);
    return ok;
}

} // anonymous namespace

static RefPtr<AudioBus> decodeFile(const wchar_t* path, bool mixToMono, float sampleRate)
{
    MFPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path, nullptr, &reader)) || !reader.p)
        return nullptr;

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);

    // Ask the reader to decode to uncompressed 32-bit float PCM.
    {
        MFPtr<IMFMediaType> partialType;
        if (FAILED(MFCreateMediaType(&partialType)))
            return nullptr;
        partialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        partialType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, partialType)))
            return nullptr;
    }

    UINT32 channels = 0;
    UINT32 fileRate = 0;
    {
        MFPtr<IMFMediaType> fullType;
        if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &fullType)))
            return nullptr;
        fullType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        fullType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &fileRate);
    }
    if (!channels || !fileRate)
        return nullptr;

    // Pull every sample, accumulating interleaved float PCM.
    std::vector<float> interleaved;
    for (;;) {
        DWORD flags = 0;
        MFPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample)))
            break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!sample.p)
            continue;

        MFPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer.p)
            continue;

        BYTE* bytes = nullptr;
        DWORD currentLength = 0;
        if (FAILED(buffer->Lock(&bytes, nullptr, &currentLength)))
            continue;

        size_t sampleCount = currentLength / sizeof(float);
        const float* floats = reinterpret_cast<const float*>(bytes);
        interleaved.insert(interleaved.end(), floats, floats + sampleCount);
        buffer->Unlock();
    }

    size_t totalFrames = interleaved.size() / channels;
    if (!totalFrames)
        return nullptr;

    auto decoded = AudioBus::create(channels, totalFrames);
    if (!decoded)
        return nullptr;
    decoded->setSampleRate(static_cast<float>(fileRate));

    for (unsigned c = 0; c < channels; ++c) {
        float* dst = decoded->channel(c)->mutableData();
        for (size_t f = 0; f < totalFrames; ++f)
            dst[f] = interleaved[f * channels + c];
    }

    double targetRate = sampleRate ? sampleRate : static_cast<double>(fileRate);
    if (targetRate != static_cast<double>(fileRate))
        return AudioBus::createBySampleRateConverting(decoded.get(), mixToMono, targetRate);
    if (mixToMono && channels > 1)
        return AudioBus::createByMixingToMono(decoded.get());
    return decoded;
}

RefPtr<AudioBus> createBusFromInMemoryAudioFile(const void* data, size_t dataSize, bool mixToMono, float sampleRate)
{
    if (!data || !dataSize)
        return nullptr;

    std::wstring tempPath;
    if (!writeTempFile(data, dataSize, tempPath))
        return nullptr;

    bool mfStarted = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    RefPtr<AudioBus> result = decodeFile(tempPath.c_str(), mixToMono, sampleRate);
    if (mfStarted)
        MFShutdown();
    DeleteFileW(tempPath.c_str());
    return result;
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
