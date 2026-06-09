// Child half of the browser ProvSQL Playground (paired with shell-boot.js).
//
// This document (ui.html) runs the UNMODIFIED Studio frontend (app.js +
// circuit.js) inside an <iframe> owned by the shell (app.html). The backend
// lives in the shell, so here we only:
//   - apply the forwarded ?mode / ?q (the shell consumed ?db),
//   - bridge the frontend's /api/* fetches to the shell over postMessage,
//   - inject app.js, then add the WASM-only UI affordances.
// A mode switch sets window.location='?mode=…', reloading just this iframe
// (≈140 KB of JS); the shell's PGlite + Pyodide stay warm. No JSPI is needed
// here.
const asset = (p) => new URL(p, import.meta.url)
const params = new URLSearchParams(location.search)
const mode = ['where', 'notebook', 'contributions'].includes(params.get('mode')) ? params.get('mode') : 'circuit'
document.body.className = 'mode-' + mode
// A linked query is handed to app.js through the same sessionStorage channel
// its mode-switch "carry" uses: ps.sql fills the box, ps.sql.ran replays it.
// Only on an initial deep link (a ?q in the URL); a later mode-switch reload
// has no ?q, so app.js's own carry survives.
const linkedQuery = params.get('q')
if (linkedQuery != null) {
  sessionStorage.setItem('ps.sql', linkedQuery)
  sessionStorage.setItem('ps.sql.ran', '1')
}

// Hide UI that cannot apply to a single in-page database:
//  - Config rows for GUCs that only govern external tools the browser cannot
//    spawn: tool_search_path (a $PATH for subprocesses) and fallback_compiler
//    (an external compiler tried after the in-process routes).
//  - The DSN editor (the connection-edit button + its panel): there is one
//    in-page PGlite, so connecting to an arbitrary DSN is impossible. The DB
//    switcher stays.
// Elements stay in the DOM so app.js's load/save paths remain null-safe.
{
  const st = document.createElement('style')
  st.textContent =
    '.wp-config__row:has(#cfg-tool-search-path),' +
    '.wp-config__row:has(#cfg-fallback-compiler),' +
    '#conn-dot,#dsn-panel{display:none}'
  document.head.appendChild(st)
}

// Bridge /api/* to the shell over postMessage; everything else (static assets)
// uses the real fetch. The shell always replies (status 500 on a backend
// exception), so a request never hangs.
const realFetch = window.fetch.bind(window)
// Per-load epoch: request ids restart at 0 on every iframe reload, so a reply
// to a request the *previous* child fired (still in flight when a mode switch
// reloaded us) could carry a live id here and resolve the wrong request. Tag
// every message with this load's epoch and drop replies from any other.
const EPOCH = Math.random().toString(36).slice(2)
let _seq = 0
const _pending = new Map()
window.addEventListener('message', (ev) => {
  if (ev.source !== window.parent) return
  const d = ev.data
  if (d && d.type === 'api-reply' && d.epoch === EPOCH && _pending.has(d.id)) {
    const resolve = _pending.get(d.id)
    _pending.delete(d.id)
    resolve(d)
  }
})
const callShell = (method, path, body) => new Promise((resolve) => {
  const id = ++_seq
  _pending.set(id, resolve)
  window.parent.postMessage({ type: 'api', id, epoch: EPOCH, method, path, body }, '*')
})
window.fetch = async (input, init = {}) => {
  const url = typeof input === 'string' ? input : input.url
  const path = url.replace(location.origin, '')
  if (path.startsWith('/api/')) {
    const method = (init.method || (typeof input !== 'string' && input.method) || 'GET').toUpperCase()
    const body = init.body != null ? String(init.body) : ''
    const out = await callShell(method, path, body)
    // Null-body statuses (204 No Content -- e.g. the kernel DELETE -- 205,
    // 304) reject any body, even '': Response construction throws otherwise.
    const noBody = out.status === 204 || out.status === 205 || out.status === 304
    return new Response(noBody ? null : out.body,
                        { status: out.status, headers: { 'Content-Type': out.ctype } })
  }
  return realFetch(input, init)
}
// The notebook's pagehide kernel-close uses navigator.sendBeacon (a POST that
// survives page teardown); a real beacon would hit the static host and 404,
// leaking the kernel against MAX_KERNELS. Route /api/* beacons through the
// same postMessage bridge, fire-and-forget -- the shell outlives this iframe,
// so the request completes even though the reply goes unread.
const realBeacon = navigator.sendBeacon ? navigator.sendBeacon.bind(navigator) : null
navigator.sendBeacon = (url, data) => {
  const path = String(url).replace(location.origin, '')
  if (path.startsWith('/api/')) {
    callShell('POST', path, data != null ? String(data) : '')
    return true
  }
  return realBeacon ? realBeacon(url, data) : false
}

// Now load the unmodified frontend; it runs against the bridged backend.
await new Promise((resolve) => {
  const s = document.createElement('script')
  s.src = asset('./app.js'); s.onload = resolve; document.body.appendChild(s)
})

// External knowledge-compiler tools (d4, c2d…) need subprocesses the browser
// cannot run, so drop the tool-registry UI rather than offer an editor for
// tools that can never be available. The eval-strip already hides tool-backed
// methods on its own (every tool reports available:false).
document.getElementById('tools-btn')?.remove()
document.getElementById('tools-panel')?.remove()

// WASM-only: a Copy-link button. Builds a shareable URL capturing the current
// database, mode and query box. The link targets the shell (the top frame's
// app.html), not this inner UI document, so it reopens the Playground.
{
  const nav = document.querySelector('.wp-nav__meta')
  if (nav) {
    const link = document.createElement('button')
    link.type = 'button'
    link.id = 'share-link-btn'
    link.className = 'wp-nav__link'
    link.title = 'Copy a shareable link to this database, mode and query'
    const idle = '<i class="fas fa-link"></i> Link'
    link.innerHTML = idle
    link.addEventListener('click', async () => {
      let base = location
      try { if (window.top && window.top.location.href) base = window.top.location } catch (_e) { /* cross-origin top (file://): fall back */ }
      const u = new URL(base.origin + base.pathname)
      u.searchParams.set('mode',
        document.body.classList.contains('mode-where') ? 'where'
        : document.body.classList.contains('mode-contributions') ? 'contributions'
        : document.body.classList.contains('mode-notebook') ? 'notebook' : 'circuit')
      let db = null
      try { db = (await (await fetch('/api/conn')).json()).database } catch (_e) { /* ignore */ }
      if (db) u.searchParams.set('db', db)
      const sql = (document.getElementById('request')?.value || '').trim()
      if (sql) u.searchParams.set('q', sql)
      const href = u.toString()
      try {
        await navigator.clipboard.writeText(href)
        link.innerHTML = '<i class="fas fa-check"></i> Copied'
        setTimeout(() => { link.innerHTML = idle }, 1500)
      } catch (_e) {
        window.prompt('Copy this link:', href)   // e.g. clipboard blocked on plain http
      }
    })
    nav.appendChild(link)
  }
}

// WASM-only: a Reset button (the browser build persists every edit to
// IndexedDB, so without this the only way back to the pristine data is the
// browser's "clear site data"). The shell owns PGlite, so we ask it to drop and
// re-seed; it reloads this iframe when done.
{
  const nav = document.querySelector('.wp-nav__meta')
  if (nav) {
    const btn = document.createElement('button')
    btn.type = 'button'
    btn.id = 'reset-data-btn'
    btn.className = 'wp-nav__link'
    btn.title = 'Reset every database to its original tutorial / case-study data (discards your changes)'
    btn.innerHTML = '<i class="fas fa-eraser"></i> Reset'
    btn.addEventListener('click', () => {
      if (!confirm('Reset all databases to their original demo data? This discards every change you have made in the browser.')) return
      btn.disabled = true
      window.parent.postMessage({ type: 'reset' }, '*')
    })
    nav.appendChild(btn)
  }
}

// WASM-only: a Licenses link to the bundled third-party notices, at the left
// of the site footer (the browser build redistributes those components).
{
  const footer = document.getElementById('provsql-site-footer')
  if (footer) {
    const a = document.createElement('a')
    a.href = asset('./THIRD-PARTY.html').href
    a.title = 'Third-party components and their licenses'
    a.textContent = 'Licenses'
    const sep = document.createElement('span')
    sep.className = 'sep'
    sep.innerHTML = '&middot;'
    footer.prepend(a, sep)
  }
}

// Tell the shell the UI is up so it hides the boot status bar.
window.parent.postMessage({ type: 'ready' }, '*')
