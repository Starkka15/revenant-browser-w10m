/*
 * Revenant Tab Manager — memory-frugal multi-tab for W10M (tier-0 = 1GB).
 *
 * Core principle (see REVENANT-TABS-SPEC.md): exactly ONE live WebCore::Page (the active tab). Every
 * other tab is a lightweight record { url, title, scrollY, thumbnail } — NO Page, NO DOM, NO JS heap.
 * Switching to a background tab creates a Page and navigates to its url (fresh). Prior pages are never
 * retained; only the address survives. This is the single biggest memory lever on the 340MB ceiling.
 *
 * The live Page is produced by createConfiguredPage() (WebCoreDriver.cpp) — the same fully-wired-Page
 * factory used for the first tab and every window.open()-spawned tab.
 */
#pragma once

#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

typedef struct _cairo_surface cairo_surface_t;

namespace WebCore { class Page; }

namespace WebCorePort {

struct TabRecord {
    uint32_t id { 0 };
    WTF::String url;            // current address — the ONLY history a suspended tab keeps
    WTF::String title;
    int scrollY { 0 };          // best-effort restore on resume
    cairo_surface_t* thumb { nullptr }; // small snapshot for the switcher UI (owned; refcounted)
    bool live { false };        // true only for the active tab on tier-0
};

// Single process-wide manager. The live Page lives here as "the active tab's Page".
class TabManager {
    WTF_MAKE_NONCOPYABLE(TabManager);
public:
    static TabManager& singleton();

    // Lifecycle. background=true opens without switching (rare on tier-0; still suspends to record).
    uint32_t createTab(const WTF::String& url, bool background, uint32_t openerId);
    void closeTab(uint32_t id);
    void switchTo(uint32_t id);

    size_t count() const { return m_tabs.size(); }
    const WTF::Vector<TabRecord>& tabs() const { return m_tabs; }
    TabRecord* activeTab();
    uint32_t activeId() const;
    WebCore::Page* activePage() const { return m_livePage; }

    // Called by PortFrameLoaderClient on the active tab's commit so the record + switcher label track.
    void onDidCommitLoad(const WTF::String& url, const WTF::String& title);
    // Called by createWindow(): make a live Page for the new tab, suspend the opener (tier-0).
    WebCore::Page* createWindowForTab(const WTF::String& url, uint32_t openerId);

private:
    TabManager() = default;

    // Bridges to WebCoreDriver.cpp (the Page factory + render/snapshot plumbing).
    WebCore::Page* buildLivePage(const WTF::String& url); // createConfiguredPage() + navigate
    void suspendActive();   // capture thumbnail + scroll, destroy the live Page, keep the record
    void resume(size_t index); // build a live Page for m_tabs[index] and navigate to its url

    WTF::Vector<TabRecord> m_tabs;
    size_t m_activeIndex { 0 };
    WebCore::Page* m_livePage { nullptr }; // owned via createConfiguredPage; == activeTab's Page
    uint32_t m_nextId { 1 };
};

} // namespace WebCorePort
