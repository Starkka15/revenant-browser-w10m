# Reporting a bug

Revenant is a one-person hobby port of a browser engine to phones from ~2013. Good bug
reports are what make it possible to fix anything, so thank you for taking the time.

**Every bug report must include a `debug.log`.** Without it a crash or "page X is broken"
report is almost never actionable — the log is where the engine records what it was doing at
the moment it failed.

---

## 1. Get the `debug.log`

Revenant writes a rolling log to its own app storage on the phone:

```
LocalAppData\<Revenant package>\LocalState\debug.log
```

The easiest way to pull it off the phone is the **Windows Device Portal** (the same tool used
to sideload the app):

1. On the phone: **Settings → Update & security → For developers** → turn on **Device Portal**
   (and **Device discovery / pairing** if you sideload over USB).
2. On your PC, open the portal URL it shows (e.g. `https://192.168.x.x` or `http://127.0.0.1:10080`
   over USB) and pair if asked.
3. Go to **Apps → File explorer** (or **Apps manager → Revenant → Local storage**), browse into
   **`LocalState`**, and download **`debug.log`**.

If you can't use Device Portal, any method that can reach the app's `LocalState` folder works.

## 2. Scrub it BEFORE posting ⚠️

`debug.log` can contain things you may not want to publish. **Open it in a text editor and remove
private information before attaching it.** In particular:

- **The URLs / site names you visited** — if any are private, personal, or ones you'd rather not
  share, delete or replace those lines (e.g. `https://REDACTED`). Keep the line that shows the site
  where the bug happened if you're comfortable sharing it — otherwise just describe it.
- **Cookies, tokens, `Authorization` headers, `cf_clearance`, session IDs** — anything that looks
  like a long random string tied to your logged-in accounts.
- **Passwords or anything you typed into a form.**
- **Your home IP / LAN addresses and the phone's device ID (WMID / pairing tokens).**
- **Email addresses, usernames, real names.**

A quick "find" for `http`, `cookie`, `token`, `auth`, and `@` catches most of it. When in doubt,
delete the line — a slightly shorter log is fine; a leaked credential is not.

> GitHub issues are **public**. Anything you attach can be read (and cached/indexed) by anyone.

## 3. File the issue

Open a new issue and include:

```
**Device:**            Lumia 640 XL / 950 / 1520 / other  (RAM, if known)
**OS build:**          Settings → System → About → OS build (e.g. 10.0.15254.xxx)
**Revenant version:**  Settings tile / About, or the release you installed (e.g. 0.0.1.1-alpha)
**Site / page:**       URL where it happened, or "redacted — private site" + a description
**What you did:**      step by step (e.g. "searched, scrolled the grid, tapped a thumbnail")
**Expected:**          what you thought would happen
**Actual:**            what actually happened (crash? blank? frozen? wrong layout?)
**Screenshots:**       if visual (tiling, blank video, garbled text) — a photo of the screen is fine
```

Then **attach your scrubbed `debug.log`.**

### Good vs. not-useful

- 👍 "On the 950, rule34 search → scrolled → tapped a video, app closed. Log attached; the last
  lines are `c3:updateBacking`." → actionable.
- 👎 "video doesnt work" with no log, no device, no site → can't do anything with this.

## Known issues first

Please skim **[KNOWN_ISSUES.md](KNOWN_ISSUES.md)** and the existing open issues before filing —
video drawing, tiling artifacts, slow heavy pages, and OOM on very large pages are already tracked,
so a new report on those mainly helps if it adds a *log* or a clean repro we don't have yet.
