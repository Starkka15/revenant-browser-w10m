// PortMediaFetch.cpp — ranged media fetch over the BROWSER'S network stack, exposed to the shell.
//
// Why this exists: neither IMFMediaEngine nor Windows.Web.Http can fetch media on this device. Both go
// through the OS resolver, and the log is unambiguous about what that costs:
//
//   prog: probe THREW hr=0x80072ee7          (WININET_E_NAME_NOT_RESOLVED)
//
// on ht-cdn.trafficjunky.net / ht-cdn2.adtng.com / ew.phncdn.com — while in the SAME session curl
// fetched images from those exact hosts with zero failures. Our curl resolves over DoH
// (CURLOPT_DOH_URL, CurlContext.cpp), so it sees hosts the OS resolver cannot. That is the whole story
// behind MediaFoundation's useless "SRC_NOT_SUPPORTED" (0xc00d001a / 0xc00d0035): they were DNS
// failures wearing a codec error's clothes.
//
// So media has to be fetched the way every other subresource on the page is fetched. A CurlHandle is
// constructed with our DoH resolver, our CA bundle and the iOS-Safari BoringSSL/JA3 profile already
// applied, so a range request through it looks exactly like the rest of the browser's traffic.
//
// These calls BLOCK (curl_easy_perform). Call them only from worker threads — the shell drives them
// from MF's frame-server read callbacks, which is precisely where blocking I/O belongs.

#include "config.h"

#include "CurlContext.h"
#include <wtf/URL.h>
#include <wtf/text/CString.h>
#include <wtf/text/StringView.h>
#include <wtf/text/WTFString.h>

#include <cstdio>
#include <cstring>

namespace WebCorePortMedia {

struct FetchSink {
    uint8_t* out { nullptr };
    size_t capacity { 0 };
    size_t written { 0 };

    unsigned long long total { 0 };   // full resource length, from Content-Range
    char contentType[128] { 0 };
};

static size_t writeBody(char* ptr, size_t size, size_t nmemb, void* userData)
{
    auto* sink = static_cast<FetchSink*>(userData);
    size_t bytes = size * nmemb;
    if (!sink->out)
        return bytes; // discard (probe)
    size_t room = sink->capacity > sink->written ? sink->capacity - sink->written : 0;
    size_t take = bytes < room ? bytes : room;
    if (take) {
        memcpy(sink->out + sink->written, ptr, take);
        sink->written += take;
    }
    return bytes; // always claim it all; over-delivery just gets dropped
}

static size_t writeHeader(char* ptr, size_t size, size_t nmemb, void* userData)
{
    auto* sink = static_cast<FetchSink*>(userData);
    size_t bytes = size * nmemb;
    std::string line(ptr, bytes);

    auto lower = line;
    for (auto& c : lower)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    // "Content-Range: bytes 0-0/12345" -> 12345 is the only trustworthy total for a ranged GET.
    if (!lower.compare(0, 14, "content-range:")) {
        auto slash = line.rfind('/');
        if (slash != std::string::npos)
            sink->total = strtoull(line.c_str() + slash + 1, nullptr, 10);
    } else if (!lower.compare(0, 13, "content-type:")) {
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string v = line.substr(colon + 1);
            // trim whitespace and any ";charset=..." parameter
            size_t b = v.find_first_not_of(" \t");
            if (b == std::string::npos)
                b = v.size();
            size_t e = v.find_first_of(";\r\n", b);
            if (e == std::string::npos)
                e = v.size();
            std::string mime = v.substr(b, e - b);
            snprintf(sink->contentType, sizeof sink->contentType, "%s", mime.c_str());
        }
    }
    return bytes;
}

} // namespace WebCorePortMedia

extern void PortImgLog(const char*);

// Fetch [start, start+count) of `url`. Blocking. Returns 0 on success.
//   outRead   - bytes actually copied into `out` (may be short at EOF)
//   outTotal  - full resource length (from Content-Range), 0 if the server did not say
//   outType   - response MIME type
//   outStatus - HTTP status code (so a 403 says "403", not "SRC_NOT_SUPPORTED")
extern "C" int WebCoreMediaFetchRange(const char* url, const char* userAgent, const char* referer,
    unsigned long long start, unsigned long long count,
    uint8_t* out, unsigned long long* outRead,
    unsigned long long* outTotal, char* outType, int outTypeLen, int* outStatus)
{
    using namespace WebCore;
    using namespace WebCorePortMedia;

    if (outRead)
        *outRead = 0;
    if (outStatus)
        *outStatus = 0;

    FetchSink sink;
    sink.out = out;
    sink.capacity = out ? static_cast<size_t>(count) : 0;

    CurlHandle handle; // ctor applies DoH, CA bundle and our TLS (JA3) profile
    handle.setUrl(URL(URL(), String::fromUTF8(url)));
    handle.enableHttpGetRequest();

    if (userAgent && *userAgent)
        handle.appendRequestHeader("User-Agent"_s, String::fromUTF8(userAgent));
    // Video/ad CDNs hotlink-gate on Referer; a browser sends the page URL, so we do too.
    if (referer && *referer)
        handle.appendRequestHeader("Referer"_s, String::fromUTF8(referer));

    char range[64];
    snprintf(range, sizeof range, "%llu-%llu", start, start + count - 1);
    curl_easy_setopt(handle.handle(), CURLOPT_RANGE, range);
    curl_easy_setopt(handle.handle(), CURLOPT_FOLLOWLOCATION, 1L); // CDNs redirect constantly

    handle.setWriteCallbackFunction(WebCorePortMedia::writeBody, &sink);
    handle.setHeaderCallbackFunction(WebCorePortMedia::writeHeader, &sink);

    CURLcode code = handle.perform();
    long status = 0;
    if (auto rc = handle.getResponseCode())
        status = *rc;

    if (outStatus)
        *outStatus = static_cast<int>(status);
    if (outRead)
        *outRead = sink.written;
    if (outTotal) {
        // No Content-Range (server ignored the range): fall back to the body length it did send.
        *outTotal = sink.total;
        if (!*outTotal) {
            if (auto len = handle.getContentLength())
                *outTotal = static_cast<unsigned long long>(*len);
        }
    }
    if (outType && outTypeLen > 0)
        snprintf(outType, outTypeLen, "%s", sink.contentType[0] ? sink.contentType : "video/mp4");

    if (code != CURLE_OK) {
        char b[200];
        snprintf(b, sizeof b, "prog: curl range %llu-%llu FAILED code=%d http=%ld", start,
            start + count - 1, static_cast<int>(code), status);
        PortImgLog(b);
        return -1;
    }
    if (status && (status < 200 || status >= 300)) {
        char b[160];
        snprintf(b, sizeof b, "prog: curl range %llu-%llu http=%ld (refused)", start,
            start + count - 1, status);
        PortImgLog(b);
        return -2;
    }
    return 0;
}
