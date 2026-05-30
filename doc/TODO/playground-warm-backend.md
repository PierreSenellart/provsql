# Keep the Playground's WASM backend warm across mode / database switches

## Problem

In the browser Playground the "server" lives inside the page: PGlite
(PostgreSQL + provsql, WASM) and Pyodide (the unmodified `provsql_studio`
Python, via a `psycopg`→PGlite shim and a `fetch`→Flask-`test_client`
bridge) are all instantiated by `studio/web/studio-boot.js` in the same
document as the Studio frontend.

The unmodified Studio frontend is **reload-driven**:

- mode switch (where ↔ circuit) does `window.location.href = '?mode=…'`
  (`studio/provsql_studio/static/app.js`, the canonical
  `window.location.href = '/circuit'`, path-rewritten by `build.sh`);
- database switch does POST `/api/conn` then `window.location.reload()`
  (`app.js` around the conn editor and the DB picker).

On a real server a reload is cheap. In the Playground a reload tears down
the whole JS realm and rebuilds the entire backend: Pyodide core, the
`micropip` wheel install, the PGlite instance + provsql, and the Flask
app. That is the ~2–5 s "reloading everything" the user sees on every
mode/DB switch. The bytes are HTTP-cached; the cost is **re-initialisation**,
not re-download.

The frontend exposes no in-page "re-render mode X" / "reconnect DB" hook,
so intercepting the click and swapping views in place would mean
re-implementing `app.js`'s bootstrap from the glue layer — tightly coupled
to Studio internals and brittle across Studio releases. That is rejected
(see Alternatives).

A page reload destroys the realm, so nothing in the reloading document can
survive it. The only way to stop re-initialising the backend is to make
the backend's lifetime **independent of the document that reloads**.

## Design: shell page (warm backend) + Studio UI in an iframe

Split the single document into two same-origin documents:

```
┌─ app.html  (SHELL — never reloads) ───────────────────────────────┐
│  shell-boot.js:                                                    │
│    PGlite cluster + active DB   (switchDb in place)                │
│    Pyodide + wheels + psycopg shim + Flask test_client `handle`    │
│    graphvizDot, enqueue/callBackend  (JSPI lives here, top frame)  │
│    postMessage server  ◄──────────────┐                           │
│    <iframe src="ui.html?mode=&db=&q="> │ (reloads, JS-only)        │
│  ┌─ ui.html (CHILD — the Studio UI) ───┴──────────────────────┐    │
│  │  unmodified app.js + circuit.js + Studio HTML/CSS          │    │
│  │  child-boot.js:                                            │    │
│  │    window.fetch('/api/*')  ── postMessage(req) ──► shell   │    │
│  │                            ◄─ postMessage(resp) ──         │    │
│  │    UI mutations: remove tools, Reset, Copy-link, hide GUCs │    │
│  └────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────┘
```

- **Mode switch** navigates the **iframe** (`app.js` sets
  `location.href='?mode=…'` inside the child), reloading ~140 KB of JS in
  a few hundred ms. The shell's Pyodide + PGlite are untouched.
- **Database switch**: the child POSTs `/api/conn`; the shell handles it by
  calling `switchDb(target)` **in place** (close + reopen the PGlite
  connection — cheap next to Pyodide), keeps Pyodide/Flask warm, then tells
  the child to reload its iframe. As a bonus this **removes the IndexedDB
  flush-race workaround**: today the conn handler defers the switch to the
  reload because an in-place switch immediately followed by a *whole-page*
  reload can abort a half-written seed (`studio-boot.js`, the `/api/conn`
  POST branch). In the shell model the shell never tears down, so the
  reopen completes (awaited) before the child reloads — the race is gone,
  and `switchDb` can do the work directly.
- **JSPI requirement is unchanged**: JSPI (`WebAssembly.Suspending` +
  `run_sync` + `handle.callPromising`) runs only in the **top frame**,
  which is the shell. The iframe needs no JSPI — it only does
  `fetch`→`postMessage`. The landing-page JSPI gate stays on
  landing → shell.

### Cross-frame `/api/*` bridge

Today `studio-boot.js` overrides `window.fetch`, serialises calls through
`enqueue` (one shared PGlite connection, so requests must not interleave),
and calls `handle.callPromising` (`studio-boot.js:226–266`). In the split:

- **child**: `window.fetch('/api/*')` posts `{id, method, path, body}` to
  the shell and returns a `Response` built from the correlated reply. Plain
  static assets (`static/…`, `pkg/…`) keep using the real `fetch`.
- **shell**: a `message` listener dispatches by `id`. `/api/databases` and
  `/api/conn` keep their JS-layer special cases (manifest list; in-place
  `switchDb`); everything else goes through the existing
  `enqueue(() => handle.callPromising(...))`. The reply posts back
  `{id, status, body, ctype}`.

Responses are all text/JSON (circuit JSON, semiring scalars, DOT-rendered
SVG strings, notices carried in the JSON body — `handle` already returns a
JSON envelope at `studio-boot.js:207–214`), so they are structured-clone
safe; no binary crosses the boundary. The shell **always** replies (even on
a Python exception → status 500), so the child never hangs.

### Readiness handshake

The shell shows the boot status bar (the one `build.sh` injects) until both
(a) the backend is up and (b) the child posted `ready`; then it reveals the
iframe. Deep-link params (`?mode/db/q`) flow shell → iframe `src`.

## Files to change

- **`studio/web/studio-boot.js`** → split into:
  - `shell-boot.js` (parent): everything backend — `ensureCluster` /
    `switchDb`, Pyodide + wheels + shim + `handle`, `graphvizDot`,
    `enqueue`/`callBackend`, the `/api/databases` + `/api/conn` special
    cases, the deep-link parse, and the new `postMessage` server + iframe
    creation. Reset (drop DBs) moves here too (the child relays a `reset`
    message).
  - `child-boot.js` (iframe): the `fetch`→`postMessage` client, plus the UI
    mutations currently at `studio-boot.js:268–319` (remove `tools-btn` /
    `tools-panel`, inject Reset and Copy-link, hide GUC/DSN rows, Licenses
    footer). **Copy-link must build the shareable URL from the shell's
    top-frame location** (`window.top.location`, same-origin), not the
    iframe URL, so the link still points at `app.html?mode=&db=&q=`.
- **`studio/web/build.sh`** → generate **two** pages instead of one:
  - `app.html` (shell): status bar + `<iframe>` + `<script
    type="module" src="shell-boot.js">`; **no** `app.js`.
  - `ui.html` (child): today's `app.html` body (canonical Studio UI) but
    with `child-boot.js` in place of `studio-boot.js`. The existing
    path-portability rewrite of `app.js` (`/static`→`static`,
    `/circuit`→`?mode=circuit`, `build.sh:71–82`) is unchanged.
  - `landing.html`→`index.html` still forwards deep links to `app.html`.
- **`studio/tests/web/`** (the 12 Playwright e2e): the Studio DOM now lives
  inside the iframe, so `conftest.py`'s `_boot` / `open_studio` must return
  the **frame** and tests must target it (`page.frame_locator('iframe')` /
  `page.frame(...)`). This is the largest mechanical chunk. Add one test
  that asserts the backend stays warm across a mode switch (e.g. a marker
  set on `window` in the shell survives an iframe-only navigation).
- **`serve.py` / `.htaccess`**: no change (still pure static; same origin,
  no sandbox attribute, no CSP — `window.top` access works).

`app.js` / `circuit.js` stay **unmodified** — they still just `fetch` and
navigate; only which frame they run in changes. The "no parallel port,
unmodified Studio" principle holds.

## Risks / watch-list

- **e2e iframe refactor** — every test reaches into the iframe; mechanical
  but touches all 12. Biggest single cost.
- **`sessionStorage` carrying** — `app.js` carries the textarea + a preload
  UUID across mode switches via `sessionStorage`
  (`app.js` `ps.preloadCircuit` / `ps.sql`). A nested browsing context
  keeps its `sessionStorage` across its own navigations, so this should
  keep working when only the iframe reloads — **verify in a real browser**;
  fallback is to thread the state through the iframe `src` instead.
- **Request correlation / timeouts** — monotonic `id`s; shell always
  replies so the child cannot hang.
- **Deep links + Copy-link** — params must round-trip shell → iframe, and
  Copy-link must emit the shell URL (`window.top`), not the iframe's.
- **First-paint handshake** — reveal the iframe only when backend-ready AND
  child-ready, else the UI fires `/api/*` before the bridge exists.
- **PGlite single connection** — unchanged; the shell still serialises via
  `enqueue`.

## Effort and payoff

Medium, ~one focused session: split the boot module, two-page `build.sh`,
e2e iframe refactor (the bulk), and manual browser verification of mode +
DB switch warmth, deep links, Reset, and Copy-link. Payoff: mode switch
drops from full WASM re-init (~2–5 s) to an iframe JS reload
(~100–300 ms); DB switch drops to a PGlite reopen with Pyodide/Flask warm;
and the conn flush-race workaround is retired.

This composes with the already-shipped cheap win (skip the bootstrap PGlite
open on later boots via a manifest-signature flag, `studio-boot.js`
`ensureCluster`): that trims the reload, this removes the reload of the
heavy backend entirely.

## Alternatives considered (and why the shell wins)

- **SPA-intercept the navigations** (no iframe): keep the UI in the top
  frame and re-render in place on mode/DB change. Rejected: the frontend
  has no re-render entry point, so the glue would have to re-run `app.js`'s
  load-time bootstrap — tight coupling to Studio internals, brittle across
  releases, and it forks the "unmodified Studio" contract.
- **Backend in a SharedWorker** (survives top-frame reload, UI stays top
  frame): the cleanest *conceptually*, but PGlite + Pyodide + the
  `psycopg`/`subprocess` shims would move into a DOM-less worker reached by
  message passing, and **JSPI inside a SharedWorker is uncertain**. Large
  rewrite, high risk. The iframe shell gets the same "backend outlives the
  reloading UI" property while keeping Pyodide + JSPI in an ordinary top
  frame and `app.js` unmodified.
- **Backend in a child iframe, UI in the top frame**: fails — the top frame
  is the one that reloads on a mode/DB switch, taking its child iframe down
  with it. The persistent document must be the parent; hence the UI is the
  child.
