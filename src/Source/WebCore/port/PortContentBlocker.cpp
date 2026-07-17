// Revenant content blocker: compiles a curated ad/tracker/analytics blocklist into WebKit's native
// ContentExtensions DFA and installs it on the page's UserContentController. Blocked requests are
// dropped in ResourceLoader::willSendRequest BEFORE the fetch — so the ad/tracker script is never
// downloaded, never parsed, never JIT'd, never heap-resident. On the 1GB / ~340MB-effective-commit
// device this is the single biggest lever for JS CPU + memory + network across the whole web (it does
// NOT strip YouTube's own inline ads — those are same-origin — but kills the third-party ad/analytics
// load that dominates most pages). Uses only stock WebCore ContentExtensions APIs (ENABLE=ON in build).

#include "config.h"

#if ENABLE(CONTENT_EXTENSIONS)

#include "CompiledContentExtension.h"
#include "ContentExtensionParser.h"
#include "ContentExtensionCompiler.h"
#include "ContentExtensionsBackend.h"
#include "UserContentController.h"
#include "UserContentProvider.h"
#include <wtf/text/StringBuilder.h>

namespace WebCorePort {

void portLog(const char*);

using namespace WebCore::ContentExtensions;

namespace {

struct CompiledData {
    Vector<SerializedActionByte> actions;
    Vector<DFABytecode> urlFilters;
    Vector<DFABytecode> topURLFilters;
    Vector<DFABytecode> frameURLFilters;
};

// Collects the compiler's bytecode streams into CompiledData (the non-test analog of WebKit's
// InMemoryContentExtensionCompilationClient — same call order, no test asserts).
class PortCompilationClient final : public ContentExtensionCompilationClient {
public:
    explicit PortCompilationClient(CompiledData& data) : m_data(data) { }
private:
    void writeSource(String&&) final { }
    void writeActions(Vector<SerializedActionByte>&& a) final { m_data.actions.appendVector(a); }
    void writeURLFiltersBytecode(Vector<DFABytecode>&& b) final { m_data.urlFilters.appendVector(b); }
    void writeTopURLFiltersBytecode(Vector<DFABytecode>&& b) final { m_data.topURLFilters.appendVector(b); }
    void writeFrameURLFiltersBytecode(Vector<DFABytecode>&& b) final { m_data.frameURLFilters.appendVector(b); }
    void finalize() final { }
    CompiledData& m_data;
};

class PortCompiledContentExtension final : public CompiledContentExtension {
public:
    static Ref<PortCompiledContentExtension> create(CompiledData&& d) { return adoptRef(*new PortCompiledContentExtension(WTFMove(d))); }
private:
    explicit PortCompiledContentExtension(CompiledData&& d) : m_data(WTFMove(d)) { }
    Span<const uint8_t> serializedActions() const final { return { m_data.actions.data(), m_data.actions.size() }; }
    Span<const uint8_t> urlFiltersBytecode() const final { return { m_data.urlFilters.data(), m_data.urlFilters.size() }; }
    Span<const uint8_t> topURLFiltersBytecode() const final { return { m_data.topURLFilters.data(), m_data.topURLFilters.size() }; }
    Span<const uint8_t> frameURLFiltersBytecode() const final { return { m_data.frameURLFilters.data(), m_data.frameURLFilters.size() }; }
    CompiledData m_data;
};

// Curated blocklist. Kept deliberately small (~fast DFA, tiny bytecode) and focused on the high-volume
// third-party ad/tracker/analytics hosts that dominate script weight across the general web. A full
// EasyList (~70k rules) would be slow to compile and bloat the DFA on ARM32; this is the 80/20 set.
// BARE hostnames only — the JSON builder escapes the dots (a literal "\\." in the host would produce
// an invalid JSON escape "\." and fail parseRuleList; that was the v1 bug that left the blocker inert).
static const char* const s_blockHosts[] = {
    // Google ad/analytics stack
    "doubleclick.net", "googlesyndication.com", "googleadservices.com",
    "google-analytics.com", "googletagmanager.com", "googletagservices.com",
    "analytics.google.com",
    // Facebook/Meta tracking
    "connect.facebook.net",
    // Amazon ads
    "amazon-adsystem.com",
    // Major ad exchanges / SSPs
    "adnxs.com", "rubiconproject.com", "pubmatic.com", "openx.net",
    "criteo.com", "criteo.net", "casalemedia.com", "smartadserver.com",
    "advertising.com", "adform.net", "3lift.com", "sharethrough.com",
    "taboola.com", "outbrain.com", "moatads.com",
    "yieldmo.com", "sonobi.com", "gumgum.com", "districtm.io",
    // Analytics / tag managers / RUM
    "scorecardresearch.com", "quantserve.com", "quantcount.com",
    "hotjar.com", "mouseflow.com", "fullstory.com", "mixpanel.com",
    "amplitude.com", "heap.io",
    "newrelic.com", "nr-data.net", "bugsnag.com", "sentry.io",
    "chartbeat.com", "branch.io",
    // Social / misc trackers
    "clarity.ms",
    "ads-twitter.com", "analytics.twitter.com",
    "adroll.com", "crwdcntrl.net", "krxd.net", "demdex.net",
    "everesttech.net", "adsrvr.org", "bidswitch.net", "agkn.com",
};

static String buildRuleJSON()
{
    StringBuilder json;
    json.append('[');
    bool first = true;
    for (auto* host : s_blockHosts) {
        if (!first)
            json.append(',');
        first = false;
        // Anchor to scheme+host so we match the host label (and its subdomains) but not arbitrary
        // occurrences in a path/query. Block regardless of load-type (first- or third-party) — the
        // intent for these hosts. Each '.' in the host is emitted as the JSON sequence "\\." so the
        // decoded regex is a literal escaped dot.
        json.append("{\"action\":{\"type\":\"block\"},\"trigger\":{\"url-filter\":\"^https?://([^/]+\\\\.)?");
        for (const char* p = host; *p; ++p) {
            if (*p == '.')
                json.append("\\\\."); // JSON "\\." -> regex "\."
            else
                json.append(*p);
        }
        json.append("\"}}");
    }
    json.append(']');
    return json.toString();
}

} // namespace

// Compile the blocklist and install it on `controller`. Returns the number of rules on success, 0 on
// failure (feature simply stays inert — never fatal).
unsigned installPortContentBlocker(WebCore::UserContentController& controller)
{
    String ruleJSON = buildRuleJSON();

    auto parsed = parseRuleList(ruleJSON);
    if (!parsed.has_value()) {
        portLog("contentblocker: parse FAILED -> blocker inert");
        return 0;
    }

    CompiledData data;
    PortCompilationClient client(data);
    auto err = compileRuleList(client, WTFMove(ruleJSON), WTFMove(parsed.value()));
    if (err) {
        portLog("contentblocker: compile FAILED -> blocker inert");
        return 0;
    }

    auto compiled = PortCompiledContentExtension::create(WTFMove(data));
    // extensionBaseURL is only used for CSS-selector injection; these are pure block rules, so a
    // placeholder is fine. ShouldCompileCSS::No skips the (unused) stylesheet path entirely.
    // userContentExtensionBackend() is public on the UserContentProvider base but a private override on
    // UserContentController; access is checked at the static type, so call it through the base ref.
    static_cast<WebCore::UserContentProvider&>(controller).userContentExtensionBackend()
        .addContentExtension("revenant-blocklist"_s, WTFMove(compiled),
            URL { { }, "about:blank"_s }, ContentExtension::ShouldCompileCSS::No);

    unsigned count = sizeof(s_blockHosts) / sizeof(s_blockHosts[0]);
    char b[96];
    snprintf(b, sizeof b, "contentblocker: installed %u block rules (native ContentExtensions DFA)", count);
    portLog(b);
    return count;
}

} // namespace WebCorePort

#endif // ENABLE(CONTENT_EXTENSIONS)
