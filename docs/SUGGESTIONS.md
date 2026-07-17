# Revenant — Suggestions Box

Community feature requests for the Revenant browser (W10M ARM32 UWP, WebKit 2.36.8).

**Working order (why things wait):** the base has to be *stable* before feature work. Current priorities,
in order: (1) fix the web-page crashes, (2) memory management on 1GB devices, then (3) requested features.
A suggestion landing here is *accepted onto the list* — not a promise of "now." Small ones may get folded in
opportunistically when they touch code already being worked.

Status legend: 🔵 logged · 🟡 planned/next · 🟢 in progress · ✅ shipped · ⚪ won't do (with reason)

---

## Open

### S-001 · Custom URI scheme / deep-link hand-off 🔵
**Requested by:** Computershik (Telegram) — 2026-07-16
**What:** When a page navigates/redirects to a non-`http(s)` scheme (e.g. `app://link_with_token`), Revenant
silently does nothing — the redirect is dropped. Requested behavior, in order of preference:
1. Hand the URI to the OS (open the target app) — the UWP analog of Qt's `QDesktopServices::openUrl(link)`.
2. Failing that, at least show a small **"unsupported link"** dialog surfacing the URL so the user isn't left
   staring at a dead page.

**Use case:** logging into a shop service through Revenant instead of a webview — the web login page completes
auth and redirects to a custom-scheme deep link (`app://…?token=…`) to bounce back into a native app. Right now
that final redirect goes nowhere, so the login can't complete.

**Implementation note (for when it's picked up):** WebKit's `FrameLoaderClient`/policy-decision path already sees
the navigation to the unknown scheme — it currently falls through to "ignore." Intercept there: if the target
scheme isn't `http`/`https`/`data`/`blob`/`file`, call **`Windows::System::Launcher::LaunchUriAsync(uri)`**
(async, fire-and-forget, wrap in the usual `create_task(...).then(...)` so a missing handler just no-ops) instead
of dropping it. The "unsupported link" dialog is the fallback when `LaunchUriAsync` reports no handler installed.
Small — a handful of lines at the policy-decision site. Blocked on: stable base (crashes + memory), not on effort.

---

## Shipped
_(none yet)_

## Won't do
_(none yet)_

---

_~20+ additional asked features exist outside this file (Telegram/DMs). Migrate them in as they're triaged —
one entry per request, newest at the top of **Open**._
