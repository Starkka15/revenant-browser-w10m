# Revenant — Tab Manager SPEC

Goal: real multi-tab browsing that fixes the "new-tab hijack" (a `window.open` / `target=_blank`
replacing the current page because `PortChromeClient::createWindow` returns `nullptr`), while staying
inside the tier-0 memory ceiling (1GB device, ~340MB app limit).

## Core principle — ONE live Page

The single biggest memory lever: **only the active tab holds a live `WebCore::Page`.** Everything else
is lightweight metadata. Prior pages are NOT retained — only their address.

- **Active tab** = the one live `Page`. Uses WebCore's **native** back/forward with the back/forward
  **cache DISABLED** (`BackForwardCache` capacity 0 on tier-0). That means Back re-navigates from the
  stored history URL (re-fetch) instead of restoring a cached in-RAM page — correct for SPA/pushState
  and POST, and holds no prior page in memory.
- **Background tabs** = `{ id, url, title, scrollY, thumbnail }` only. No Page, no DOM, no JS heap.
  Switching to a background tab creates a Page and navigates to its `url` (fresh load). Its deep
  back-history is intentionally dropped on suspend — a backgrounded tab remembers just its address.

Steady-state live memory = 1 Page + N small tab records + N small thumbnails.

Tier-awareness: tier-0 (1GB) keeps exactly 1 live Page. tier-1 (2GB) MAY keep the opener live across a
`createWindow` (so a script can still touch its popup) and MAY keep 2 live Pages; controlled by
`WebCoreMemBudgetTier()`. Default everywhere: 1 live Page, upgrade only if tier-1 proves stable.

## Data model (new: port/TabManager.{h,cpp})

```
struct Tab {
    uint32_t id;
    String   url;          // current address (the ONLY history a background tab keeps)
    String   title;
    int      scrollY;      // restored on resume (best-effort)
    RefPtr<cairo_surface_t> thumb; // small snapshot for the switcher (downscaled, ~ tab-cell size)
    bool     live;         // true only for the active tab (tier-0)
};
class TabManager {
    Vector<Tab> m_tabs; size_t m_active;
    // createTab(url, background), closeTab(id), switchTo(id), activeTab()
    // onDidCommitLoad(url,title) -> update active tab record
    // captureThumbnail() -> snapshot active Page framebuffer before suspend
};
```

The live `Page` (today `g_page`) becomes "the active tab's Page," owned/swapped by the manager.

## Page lifecycle

- **create**: factor today's inline Page construction (`WebCoreBrowserInit`, WebCoreDriver.cpp ~966-1150)
  into `createConfiguredPage()` returning a fully-wired `Page` (its own PortChromeClient +
  PortFrameLoaderClient + shared storage/cookies/cache providers + all settings). Called for the first
  tab AND every new tab. `g_loaderClient`/`g_chrome` singletons become per-Page (looked up via the
  Page, or stored on the manager's active entry).
- **suspend(tab)**: `captureThumbnail()` → save `url/title/scrollY` → destroy the Page (frees DOM/JS/
  backing). MSE teardown already async (`PortMseDestroy` off-thread).
- **resume(tab)**: `createConfiguredPage()` → navigate to `tab.url` → restore scroll on first paint.
- **close(tab)**: destroy Page if live; drop record; if it was active, activate a neighbor (its opener
  if set, else the previous tab).

## createWindow (the hijack fix) — PortChromeClient

`createWindow(Frame& opener, const WindowFeatures&, const NavigationAction& action)`:
1. `TabManager::createTab(action.url(), background=false)` → builds a new live Page via the factory.
2. Return that new Page (satisfies `window.open`; the opener's script gets a real Window back).
3. Post a task to **suspend the opener tab** (can't tear it down synchronously mid-`window.open`); on
   tier-1 keep the opener live instead.
`show()` no-ops (we present via the active-tab swap). Popup blockers: allow (user-gesture navigations).

## Navigation & history

- Active tab uses native WebCore back/forward, `BackForwardCache::setMaxSize(0)` on tier-0.
- `PortFrameLoaderClient::dispatchDidCommitLoad` → `TabManager::onDidCommitLoad(url,title)` to keep the
  active tab record + switcher label current, and to snapshot address for suspend.
- Address bar reflects active tab; navigate replaces active tab's Page load.

## Rendering integration (WebCoreDriver)

- The render loop drives the active tab's Page (rename the `g_page` accesses to `activePage()`).
- On `switchTo`: swap the live Page, force full repaint (`WebCoreBrowserForceRepaint` + KeepCompositing).
- Thumbnail capture = read back the current ANGLE framebuffer (or paint the FrameView to a scaled
  cairo surface) right before suspend.

## Shell UI (RenderShellXaml)

- Chrome gets a **tab-count button** (shows N). Tap → full-screen **tab switcher** overlay: grid of
  thumbnails w/ title + close (X); a **+** cell for a new tab. Tap a thumbnail → `switchTo`.
- New-tab from `createWindow` auto-switches to the new tab and toasts "opened in new tab".
- Phone-sized: switcher is a scrollable 2-col grid, not a cramped top strip.

## Back button (W10M hardware) — the other bug

Hardware Back "just closes the app": `SystemNavigationManager::BackRequested` alone doesn't reliably
catch the **physical** button on W10M. Fix:
- Also subscribe `Windows::Phone::UI::Input::HardwareButtons::BackPressed` (guarded by
  `ApiInformation::IsTypePresent`, needs the Mobile Extension SDK winmd referenced in the vcxproj/CMake).
- Handler order: if the tab switcher is open → close it; else if `WebCoreBrowserCanGoBack()` → GoBack;
  else if >1 tab → close current tab; else leave unhandled (OS exits app). Set `e->Handled` when consumed.
- Instrument both back handlers (`back: src=hw/nav canGoBack=%d tabs=%d`) so the next log proves which
  event fires + why it did/didn't consume.

## Phases (each ends at a clean build + on-device check)

1. **Engine core**: factor `createConfiguredPage()`; `TabManager` with 1-live-Page lifecycle;
   `createWindow` → new tab (opener suspended); bfcache off; history hooks. Address bar follows active
   tab. (No switcher UI yet — new tab just becomes active; back button recovers.)
2. **Back button**: wire `HardwareButtons.BackPressed` + switcher/close-tab/goBack precedence +
   instrumentation. (Requires Mobile Extension SDK reference — verify it compiles.)
3. **Shell switcher UI**: tab-count button + thumbnail grid overlay + new/close/switch.
4. **Polish**: scroll restore, thumbnail quality, tier-1 keep-opener-live, toasts.

## Known v1 limitations (accepted)

- Suspended tabs lose deep back-history (keep only current address) — by design, for memory.
- Suspending a tab drops in-page JS/form state; resume is a fresh GET of the URL.
- LL-HLS live video (chaturbate) still won't play in any tab — that's a separate MSE fix
  (strip LL-HLS tags → AdaptiveMediaSource), tracked outside this SPEC.
