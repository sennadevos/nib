# nib

A surf-like minimal browser on a **Chromium** backend.

No toolbar, no menus, no buttons — one hidden command bar and vim-style
navigation. The engine is Chromium via QtWebEngine: Blink, V8, Chromium's GPU
compositor and its scroll animator. About 2000 lines of C++.

## Why QtWebEngine

The point of this project is *not* to be surf. surf uses WebKitGTK, and
WebKitGTK is where the sluggishness comes from. So:

- **Gecko** cannot be embedded. Mozilla removed the embedding API, so "Firefox
  as a base" only exists as a skinned Firefox profile, not an app you compile.
- **Upstream CEF** is the truest Chromium, but means a ~1 GB SDK and building
  the wrapper. Worth revisiting if QtWebEngine's Chromium lag ever bites.
- **QtWebEngine** *is* Chromium — same Blink, same V8, same compositor and
  scroll physics as Vivaldi or Chrome — behind a real embedding API, and
  packaged in Fedora at a version matching the host's Qt.

Confirmed at runtime, not assumed: loading a page spawns Chromium's own process
tree (zygote, GPU, renderer, utility) out of the bundled runtime.

## Build and install

The host is a Fedora atomic image, so the build happens in a toolbox and the
result is copied out. Nothing lands on the base system.

```sh
toolbox run -c fedora-toolbox-43 sudo dnf install -y \
  gcc-c++ make qt6-qtbase-devel qt6-qtwebengine-devel \
  qt6-qtdeclarative-devel qt6-qtwayland

toolbox run -c fedora-toolbox-43 make            # build
toolbox run -c fedora-toolbox-43 make bundle     # copy the Qt/Chromium runtime
make install                                     # → ~/.local/bin/nib
```

`make bundle` fills `~/.local/lib/nib` (~323 MB — Chromium is most of it) with
the Qt libraries, the `QtWebEngineProcess` helper, Chromium's `.pak` resources,
and the Qt plugin/QML modules. Qt libraries are bundled unconditionally so a
host Qt update cannot break nib; the graphics and driver stack still comes from
the host.

Two details that are easy to get wrong:

- The binary is linked with `--disable-new-dtags` so it carries `DT_RPATH`, not
  `DT_RUNPATH`. glibc applies `DT_RUNPATH` only to an object's *own* direct
  dependencies, so the bundled Qt libraries' dependencies would not resolve.
- The Chromium helper is `exec`'d as a separate process and does not inherit
  the rpath, so `main()` exports `LD_LIBRARY_PATH` before Qt starts.

## Use

```sh
nib                       # NIB_HOME
nib example.com           # bare host → https://
nib ./page.html           # path → file://
nib 'rust lifetimes'      # not a URL → search
nib a.com b.com c.com     # one tab each
```

### vim keys

Active **only when the page has nothing focused that takes typing.** An
injected script reports the focused element over a QWebChannel, so a focused
`<input>`, `<textarea>`, `contenteditable`, or `<iframe>` hands every key
straight to the page. If the script has not reported yet, bare keys stay off —
the safe direction is always "let the page have it".

| key | action | key | action |
| --- | --- | --- | --- |
| `h` `j` `k` `l` | scroll left/down/up/right | `d` / `u` | half page down / up |
| `gg` / `G` | top / bottom | `H` / `L` | back / forward |
| `f` / `F` | hint links: follow / new tab | `gi` | hint text fields, focus one |
| `yy` / `yf` | yank page URL / a link's URL | `v` | caret mode (below) |
| `/` | find | `n` / `N` | next / previous match |
| `o` | open URL bar | `5j` `3d` … | counts work on scroll keys |

Scrolling prefers the document, and falls back to the largest scrollable box on
screen, so app-shell layouts still respond to `j`/`k`.

### Hints: acting on page elements

`f` overlays home-row labels (`a s d f …`) on every link, button and field in
view; type a label to click it. `F` opens the link in a new tab instead (in app
mode it loads in place, scope rules included), `yf` copies the link's URL, and
`gi` labels only text fields and focuses the one you pick. Backspace edits a
half-typed label, Escape cancels, and a key that matches nothing dismisses the
hints. The status line says which mode you are in throughout.

Getting text *into* a page is the same grammar: `gi` to focus the field, then
paste with `C-v` — the field owns the keys from that point, exactly like the
focus gate above. Escape blurs the field when you are done: insert mode out,
normal mode back.

### Caret mode: yanking text off a page

`v` drops a text cursor on the page — the visible one-character selection is
the caret, vim-style. `h j k l` move by character and line, `w` / `b` / `e` by
word, `0` / `$` to the line edges. `v` again starts extending (visual mode),
`y` copies the selection to the clipboard and leaves, Escape leaves without
copying. The caret starts from the current selection when there is one — a
`/`-find match, say — otherwise from the text nearest mid-viewport.

### Always available

| key | action | key | action |
| --- | --- | --- | --- |
| `C-g` | URL bar (prefilled) | `C-t` / `C-w` | new / close tab |
| `C-f` | find | `C-Tab` / `C-S-Tab` | next / prev tab |
| `C-j` / `C-k` | full page down / up | `M-1`…`M-9` | nth tab |
| `C-h` / `C-l` | back / forward | `C-[` / `C-]` | back / forward |
| `C-y` | yank URL to clipboard | `M-Left` / `M-Right` | back / forward |
| `C-p` | open URL from clipboard | `C-r` / `C-S-r` | reload / bypass cache |
| `C-+` / `C--` / `C-0` | zoom | `C-S-p` | print to PDF |
| `Escape` | blur a field / stop loading / dismiss the bar | `C-q` | quit |

`C-g` took over the URL bar from `C-l`, which now means "forward". Find-next
moved off `C-g` accordingly: use `n` / `N`, or Enter in the find bar.

While the command bar has focus it keeps **every** key including Ctrl combos, so
`C-h`, `C-w` and `C-u` do line editing there rather than navigating.

`C-y` and `C-p` need the window focused — Wayland only lets the focused client
touch the clipboard. That is always true when you press the key yourself.

Tabs appear only past the first one. The URL bar, the 2px progress line and the
status line stay hidden unless they have something to say.

## App mode (PWA-style)

Pin a window to one site, like Chromium's `--app=`:

```sh
nib --app=https://app.slack.com/client                  # run it
nib --install-app https://app.slack.com/client \
    --name Slack --profile work                         # make a launcher
```

`--install-app` writes `~/.local/share/applications/nib-<slug>.desktop` and
prints what it did. In app mode:

- **One tab, always.** `C-t` is refused with a status message; `C-w` quits.
  A `target=_blank` link loads in place instead of opening a tab.
- **Confined to a domain.** Main-frame navigation outside the scope is refused
  and handed to the system browser (`$NIB_EXTERNAL`, default `xdg-open`).
  Iframes are untouched — they are part of the page, not navigation.
- **Its own launcher identity.** The Wayland `app_id` becomes `nib-<slug>`, so
  the compositor groups the window and shows the app's own icon. The favicon is
  saved to `~/.local/share/icons/hicolor/256x256/apps/nib-<slug>.png` on first
  run, at the name the desktop entry already points to — so the icon appears
  from the second launch, with no desktop-file rewriting.
- **Optionally its own session.** `--profile NAME` gives a separate cookie jar
  under `~/.local/share/nib/profiles/NAME`, for a second login.

Both `--opt value` and `--opt=value` work.

### Scope rules

The default scope is the registrable domain of the `--app` URL, and subdomains
are included. `--scope a.com,b.com` overrides it.

```
$ nib --app=https://app.slack.com/client --check-scope \
      https://slack.com/signin https://evil.com/x http://slack.com.evil.com/y
scope slack.com
  ALLOW   https://slack.com/signin
  HANDOFF https://evil.com/x
  HANDOFF http://slack.com.evil.com/y
```

`--check-scope` prints decisions and exits without opening a window — use it to
confirm confinement before wiring up a launcher.

Three limits worth knowing before you rely on this:

- **Not a security boundary.** It governs navigation, not the renderer. A page
  can still fetch, XHR and embed across origins exactly as in any browser.
- **Ports are not part of the scope.** An app on `localhost:8090` may navigate
  to `localhost:3000`. Distinct local apps are not isolated from each other.
- **Third-party logins get handed off.** "Sign in with Google" on an
  `example.com` app sends you to `accounts.google.com`, which is out of scope
  and opens externally, breaking the flow. Add the identity provider to the
  scope: `--scope example.com,accounts.google.com`.

The registrable-domain guess uses a short list of two-part suffixes (`co.uk`,
`com.au`, …) rather than the full public suffix list. It is right for common
cases; use `--scope` when it is not.

### Environment

| var | default | meaning |
| --- | --- | --- |
| `NIB_HOME` | `https://duckduckgo.com` | start page |
| `NIB_SEARCH` | DuckDuckGo | search URL with a `%s` slot |
| `NIB_UA` | Chromium default | user agent override |
| `NIB_DEBUG` | unset | `1` traces focus reports and self-tests scrolling; `keys`/`hint`/`caret` etc. drive synthetic keys through the real filter path (see test.sh) |

Profile in `~/.local/share/nib`, cache in `~/.cache/nib`.

## Deliberate defaults

Persistent cookies, disk cache, popups from script blocked (user-initiated
`target=_blank` still opens a tab), all page permission requests denied
outright, autoplay requires a gesture, bad TLS certificates rejected with a
status message. Downloads go to `~/Downloads` without prompting.

Clipboard is one-way: a page may **write** it (so "copy" buttons work) but
never read it — `JavascriptCanPaste` stays off and clipboard-read permission
requests are denied, so no site can sniff what you copied elsewhere.

No bookmarks, no history UI, no session restore, no adblock. Hints and the
caret run in the top frame only — elements inside iframes are part of the
page, not nib's.

## PoC status

Verified: builds warning-clean; renders local and HTTPS pages; Chromium process
tree and bundled runtime confirmed live; `DT_RPATH` resolves with nothing
missing on the host; `--help` / `--version` exit without opening a window.

The vim gate is verified end to end with `NIB_DEBUG=keys`, which posts real
`QKeyEvent`s through `QApplication::notify` — the same path platform keys take:

| page | result |
| --- | --- |
| plain tall page | 3×`j` → scrollTop 192, exactly 3 × 64 |
| app shell with inner scroller | target resolved to the inner `DIV`, scrolled 192 |
| page with autofocused `<input>` | scrollTop stayed 0 — keys went to the field |

Scroll limits, with `NIB_DEBUG=bounds` — six keys past each end, then six smooth
page-downs queued while already pinned at the bottom:

| page | past bottom | past top | settled at 1.2s / 2.2s |
| --- | --- | --- | --- |
| plain | 3152.5 / 3153 | 0 | unchanged, unchanged |
| app shell | 4152.5 / 4153 | 0 | unchanged, unchanged |

The half-pixel gap is fractional display scaling, not drift — the value is
identical at both samples. `NIB_DEBUG=ctrl` confirms `C-j` moves exactly one
page (799.2 px of an expected 799.0).

Hint and caret mode are verified through the same synthetic-key path:

| sequence | result |
| --- | --- |
| `f` on a page with 2 links + 1 field | 3 labelled hints raised |
| `f` `a` | first link followed (`location.hash` = `#target`) |
| `gi` `a` | the `<input>` focused, keys handed to the page |
| `v` `w` `v` `w` `y` on "alpha beta …" | clipboard holds `beta` |

The caret/clipboard check is automatable precisely because the offscreen
platform has a private clipboard — the Wayland caveat below does not apply.

`C-y` and `C-p` were verified once by hand with the window focused, then dropped
from the automated suite: on Wayland the result depends on window focus, and
taking clipboard ownership and then exiting **destroys whatever was on the
clipboard**. Do not re-add them to a test that kills the process.

App mode is verified offscreen: single tab held at 1 after `C-t`, a scripted
off-scope navigation refused with the URL unchanged, the handoff command invoked
with the right URL, and `--profile` writing a separate store while the main
profile stayed untouched. The generated desktop entry passes
`desktop-file-validate`.

Not exercised: downloads, print-to-PDF, tab teardown under load, the TLS-error
path, the favicon capture, and the remaining Ctrl bindings individually. They share the event-filter
path proven above, but the individual handlers are unverified.

### Testing without hijacking your session

```sh
make test          # 23 checks, offscreen, no window ever appears
```

`test.sh` sets `QT_QPA_PLATFORM=offscreen` and points `XDG_DATA_HOME` /
`XDG_CACHE_HOME` at a tempdir, so the suite neither churns your real profile
nor collides with a running nib holding the profile lock. The engine runs for
real — Blink loads pages, JS executes, synthetic keys traverse the actual
event filter — but nothing is mapped, so it is safe on a session someone is
working in. Covered: the scroll steps, the inner-scroller heuristic, the focus
gate, both scroll limits, `C-j`, hint follow / `gi`, a caret-mode yank,
app-mode single-tab and off-scope handoff, and the scope rules.

Do **not** run the GUI against a desktop you are using. Windows steal focus,
and on Wayland only the focused client may read the clipboard — so `C-y`/`C-p`
report failures that are really just focus having moved. Those two are excluded
from the suite for that reason, and because taking clipboard ownership and then
exiting destroys whatever was on the clipboard.

`attic/nib-webkit.c` is the original WebKitGTK implementation, kept only for
reference.
