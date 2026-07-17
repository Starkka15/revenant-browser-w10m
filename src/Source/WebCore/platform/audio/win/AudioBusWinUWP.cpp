/*
 * AudioBus::loadPlatformResource for the Revenant WinCairo/UWP port.
 *
 * The HRTF impulse-response set (platform/audio/resources/*.wav) is copied into
 * the appx under an "audio\" folder beside the executable at package time; this
 * loads the named resource from the install directory and decodes it through the
 * Media Foundation reader (createBusFromInMemoryAudioFile).
 */

#include "config.h"

#if ENABLE(WEB_AUDIO)

#include "AudioBus.h"
#include "AudioFileReader.h"
#include <algorithm>
#include <string>
#include <vector>
#include <wtf/RefPtr.h>

#include <windows.h>

namespace WebCore {

RefPtr<AudioBus> AudioBus::loadPlatformResource(const char* name, float sampleRate)
{
    wchar_t modulePath[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return nullptr;

    // Strip the executable filename, keeping the trailing separator.
    for (DWORD i = length; i > 0; --i) {
        if (modulePath[i - 1] == L'\\' || modulePath[i - 1] == L'/') {
            modulePath[i] = L'\0';
            break;
        }
    }

    // <installdir>\audio\<name>.wav
    std::wstring path = modulePath;
    path += L"audio\\";
    for (const char* c = name; *c; ++c)
        path += static_cast<wchar_t>(*c);
    path += L".wav";

    HANDLE h = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return nullptr;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64 * 1024 * 1024)) {
        CloseHandle(h);
        return nullptr;
    }

    std::vector<BYTE> buffer(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    bool ok = true;
    while (offset < buffer.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(buffer.size() - offset, 1u << 20));
        DWORD read = 0;
        if (!ReadFile(h, buffer.data() + offset, chunk, &read, nullptr) || !read) {
            ok = false;
            break;
        }
        offset += read;
    }
    CloseHandle(h);
    if (!ok)
        return nullptr;

    return createBusFromInMemoryAudioFile(buffer.data(), buffer.size(), false, sampleRate);
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
