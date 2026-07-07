# Revenant Browser — Feature Parity Spec

Goal: close the gap between the W10M WebKit 2.36.8 port and a mainstream browser (Chrome/Firefox) so that "lots of sites" work correctly, not just simple pages. This spec is the output of a 5-dimension audit (runtime features, compile-time flags, stubbed platform backends, network layer, captcha requirements). Every gap below was verified in-tree with a file:line reference — this is not speculation.

Paths are relative to `/mnt/ssd-raid/vm-shared/w10m-webengine/`. Engine tree: `WebKit-2.36.8/Source/WebCore/`. Port glue: `Source/WebCore/port/`. Built config: `build-wincairo/cmakeconfig.h`. curl config: `deps/curl/build-armuwp/lib/curl_config.h`.

---

## 0. What already works (do NOT re-audit)

Confirmed present + correctly wired, so these are OFF the table:
- **Network core**: brotli + gzip/deflate (`curl_config.h:375/372`, `CurlContext.cpp:550`), redirects w/ security (`ResourceHandleCurl.cpp:466-509`), CORS + OPTIONS preflight (`loader/CrossOriginPreflightChecker.cpp`), chunked/streaming/keep-alive, proxy, HTTP auth, multipart FormData, TLS/OpenSSL w/ in-memory CA bundle for AppContainer (`CurlContext.cpp:357`).
- **Storage**: persistent cookies (SQLite `cookies.db`, DELETE-journal) and localStorage (`localstorage.db`), CacheStorage, sessionStorage. Verified round-trip on device.
- **Graphics/JS**: WebGL1/2 (ANGLE D3D11), Canvas2D, OffscreenCanvas (+in workers), WebCrypto (`crypto.subtle`, `getRandomValues`), performance.now(), WebAssembly.
- **Captcha primitives (except iframes)**: blob URLs (`PortBlobRegistry`), MessageChannel/postMessage, canvas/WebGL fingerprint surface, `navigator.webdriver === false` (ENABLE_WEBDRIVER=0 — good, avoids bot detection), realistic iOS Safari UA.
- **Renderer**: composited layer tree scaled CSS→physical (scale-wrapper), flat cairo paint for non-composited content, WebCore-scheduler-driven rendering, GPU-crash → CPU-flatten fallback.

---

## 1. Gap inventory (verified) — ranked

### TIER 1 — CRITICAL (breaks a large fraction of sites; do first)

**T1.1 — iframes don't render** · task #22
- Evidence: `port/PortFrameLoaderClient.cpp:131-134` — `createFrame() { return nullptr; }`.
- Breaks: **every captcha** (Turnstile/reCAPTCHA/hCaptcha all load in a 3rd-party iframe), embeds (YouTube/maps/widgets), OAuth-in-iframe, ad/analytics frames, many SPAs.
- Fix: implement real subframe creation — construct a child `Frame` + its own `PortFrameLoaderClient`, attach to the frame tree via `HTMLFrameOwnerElement`, initialize the loader, and ensure the child's render/composite reaches the paint tree (subframe layers under the owner's layer). Mirror WebKitLegacy's `WebFrameLoaderClient::createFrame`.
- Effort: **Large**. This is the single highest-leverage fix.
- Verify: load a page with a captcha; a YouTube embed; a site with an `<iframe>` (e.g. codepen result, disqus comments).

**T1.2 — WebSocket missing** · task #23
- Evidence: `curl_config.h` — `/* #undef USE_WEBSOCKETS */`; no `platform/network/curl/WebSocket*` files. WebCore `Modules/websockets/` exists but has no curl platform impl.
- Breaks: Discord, Slack, Teams, live tickers, multiplayer, most realtime SPAs, many chat/notification features.
- Fix (pick one): (a) rebuild libcurl for ARM32 UWP with WebSocket support and wire a `SocketStreamHandle` over curl's ws API; or (b) implement `SocketStreamHandleImpl` directly on Winsock/StreamSocket (UWP) — cleaner control of the handshake + framing. (b) is likely more robust in AppContainer.
- Effort: **Large**.
- Verify: a site that opens `new WebSocket(...)` (e.g. a chat demo, or `websocket.org/echo`).

**T1.3 — window.open / popups dead** · task #24
- Evidence: `port/PortChromeClient.h:53` — `createWindow(...) { return nullptr; }`.
- Breaks: OAuth popup logins (Google/MS/etc.), `target="_blank"`, payment popups.
- Fix: return a real `Page` — open a new shell tab/view (ties to UI task #12) or a managed popup surface; support `window.opener`/`postMessage` back to the opener.
- Effort: **Medium** (engine) + shell tab coordination.
- Verify: an OAuth "Sign in with Google" popup flow; a `target="_blank"` link.

**T1.4 — file upload dead** · task #25
- Evidence: `port/PortChromeClient.cpp:39` — `runOpenPanel(...) { }` (empty).
- Breaks: every `<input type=file>` — avatars, attachments, document upload.
- Fix: UWP `FileOpenPicker` in the C++/CX shell; return chosen file(s) to the `FileChooser`. AppContainer-safe (broker-mediated). Handle `multiple` + `accept` filters.
- Effort: **Medium**.
- Verify: upload an avatar on any site; attach a file in a webmail/compose form.

### TIER 2 — HIGH (common features; strong site impact)

**T2.1 — HTTP/2 missing** · task #23 (same curl rebuild)
- Evidence: `curl_config.h` — `/* #undef USE_NGHTTP2 */`. Code path exists (`CurlContext.cpp:487`) but runtime cap absent → always HTTP/1.1.
- Breaks (soft): connection-limit stalls + latency on modern multi-request sites; a few APIs prefer H2.
- Fix: rebuild libcurl with nghttp2 (ARM32 UWP). Bundle with the WebSocket curl rebuild.
- Effort: **Medium** (dep build).

**T2.2 — Touch events off** · task #27
- Evidence: `cmakeconfig.h` ENABLE_TOUCH_EVENTS=0; shell synthesizes mouse clicks only.
- Breaks: sites detect no-touch → serve desktop UI; swipe/pinch/carousels/drawer gestures dead. We are a touchscreen phone — high UX impact.
- Fix: enable flag + feed C++/CX pointer input to WebCore `PlatformTouchEvent` (touchstart/move/end) with proper identifiers, alongside the click synthesis for compatibility.
- Effort: **Medium**.

**T2.3 — Web Audio off** · task #28
- Evidence: `cmakeconfig.h` ENABLE_WEB_AUDIO=0.
- Breaks: games, synths, some players, in-page notification sounds, WebRTC audio path.
- Fix: enable flag + implement an `AudioDestination` output backend on UWP AudioGraph or WASAPI (render WebCore's audio bus to the device).
- Effort: **Large** (real-time audio backend).

**T2.4 — ServiceWorker/Workers runtime flags never set** · task #29
- Evidence: `WebCoreDriver.cpp` — no `setServiceWorkerEnabled`/`setWorkersEnabled` call, despite ENABLE_SERVICE_WORKER=1 and the built SW infra.
- Risk: if the runtime-feature default is false, SW (offline/PWA/push) and DedicatedWorkers (captcha PoW, heavy SPA compute) are OFF at runtime regardless of the infra.
- Fix: confirm defaults; enable explicitly; wire a DedicatedWorker provider path if the generic one isn't auto-installed.
- Effort: **Small** to verify/enable; **Medium** if a worker provider must be authored.

**T2.5 — JS modal dialogs no-op** · task #30
- Evidence: `PortChromeClient` runJavaScriptAlert/Confirm/Prompt → no-op/false; `canRunBeforeUnloadConfirmPanel → false`.
- Breaks: form validation + flows that gate on `confirm()`; "unsaved changes" prompts.
- Fix: XAML `ContentDialog` in the shell, synchronously bridged to the ChromeClient callbacks.
- Effort: **Medium**.

### TIER 3 — MEDIUM (polish, specific features)

**T3.1 — compile flag flips** · task #26 — SMOOTH_SCROLLING, ASYNC_SCROLLING, VARIATION_FONTS, XSLT, DOWNLOAD_ATTRIBUTE, AUTOCAPITALIZE, SPELLCHECK, CONTENT_EXTENSIONS, LAYER_BASED_SVG_ENGINE, MHTML, POINTER_LOCK, ITP, SERVER_PRECONNECT, NETWORK_CACHE_STALE_WHILE_REVALIDATE, NETWORK_CACHE_SPECULATIVE_REVALIDATION. (GAMEPAD stays OFF per decision.) Mostly flag flips; some pull code needing porting — fix per build error. VARIATION_FONTS needs DirectWrite variation support; ASYNC_SCROLLING needs a scrolling coordinator (riskier, can defer).

**T3.2 — platform UI backends** · task #30 — geolocation (UWP Geolocator; currently always-denied), clipboard (UWP Clipboard + `createPasteboardStrategy`, currently nullptr → JS copy/paste dead), color/date pickers (XAML), notifications (WinRT Toast; currently denied), Web Share (`DataTransferManager`; currently false), `sendBeacon` ping loads (`startPingLoad` no-op → analytics lost).

**T3.3 — runtime feature enables** — fetch keepalive (`setFetchAPIKeepAliveEnabled`), readable byte streams (`setReadableByteStreamAPIEnabled`), datalist runtime flag, paint-timing, CSS Highlight/attr() where sites use them. Review `setSecureContextChecksEnabled(false)` at `WebCoreDriver.cpp:577` — intentional for HTTP testing but re-confirm it's not masking issues.

**T3.4 — SW fetch downloads** — `PortServiceWorker.cpp:790` cancels SW-intercepted downloads instead of serving; fix for offline-PWA download patterns.

---

## 2. Suggested execution order

Phasing balances impact vs. effort and groups shared infrastructure:

1. **Phase A — curl rebuild** (T1.2 WebSocket + T2.1 HTTP/2 together — one dep build). Unlocks realtime + perf with no engine-porting risk.
2. **Phase B — iframes** (T1.1). Highest site-coverage win; unblocks captchas + embeds. Largest single item — do it deliberately.
3. **Phase C — shell integrations batch** (T1.3 window.open, T1.4 file upload, T2.5 dialogs, T3.2 UI backends). All are C++/CX shell ↔ ChromeClient bridges; do them together while in that code.
4. **Phase D — input + media** (T2.2 touch events, T2.3 Web Audio). Touch is high-UX; Web Audio is a real-time backend.
5. **Phase E — flag flips + runtime enables** (T3.1, T3.3). Cheap wins + fix fallout per build.
6. **Phase F — worker runtime verify** (T2.4) — can slot earlier if captchas need it; check as part of Phase B captcha testing.

## 3. Verification matrix (test sites per feature)

- iframes/captcha → a Cloudflare-gated site (rule34.xxx), a reCAPTCHA login, a YouTube embed.
- WebSocket → a chat/echo demo; Discord web.
- window.open → OAuth "Sign in with Google".
- file upload → set an avatar anywhere.
- touch → a swipe carousel / image gallery.
- Web Audio → an in-browser synth or a game with sound.
- HTTP/2 → large multi-asset site (any news homepage) — smoke test for stalls.

## 4. Non-goals / deferred

- GAMEPAD (user decision — off).
- WebRTC full stack, EME/DRM, PiP — later (Wave 2, task #13).
- x.com GPU compositing crash — separate; CPU-flatten fallback holds (task #19).
- The ANGLE FL9_3 draw fault is a driver limitation, not a parity item.

---

Tasks: #22 iframes · #23 WebSocket+HTTP/2 · #24 window.open · #25 file upload · #26 flag flips · #27 touch · #28 Web Audio · #29 worker runtime · #30 dialogs+UI backends.
