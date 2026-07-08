/* ProvSQL Studio: notebook mode (lazy-loaded by app.js, mirroring
   circuit.js). An ordered list of Markdown + SQL cells executed against
   a pinned, stateful database session (the "kernel": /api/nb/session +
   /api/nb/exec), with per-cell results rendered by the same block
   renderer as the shared result pane (app.js makeBlockRenderer).
   Persistence: Jupyter nbformat v4 (.ipynb) download / load, plus a
   localStorage autosave of the working draft. See
   the Notebook-mode section of doc/source/user/studio.rst. */
(function () {
  'use strict';

  let env = null;          // window.__provsqlStudio, passed by init()
  let kernel = null;       // {sessionId, pid, db} | null
  let kernelStarting = null;  // in-flight ensureKernel() promise
  let cells = [];          // [{id, type, source, outputs, count, htmlSnapshot}]
  let execCounter = 0;
  let running = null;      // {cellId, requestId} | null
  let mdLibs = null;       // promise for marked + DOMPurify
  let katexReady = null;   // always-resolving promise for the best-effort KaTeX chain
  let mdRenders = [];      // in-flight markdown renders of the current paint
  let lastFocused = null;  // last cell whose editor had focus
  let schemaCache = null;  // /api/schema payload for the sidebar
  let tabs = [];           // [{id, name, db, doc, kernel, resume}]
  let activeTabId = null;
  let connDb = null;       // current connection's database (from /api/conn)
  let connExtVersion = null;  // provsql extversion, null when not installed (from /api/conn)
  let nextTabId = 1;
  let selected = null;     // command-mode selected cell (Jupyter-style)
  let lastDeleted = null;  // {cell, index} for the `z` undo
  let lastDKey = 0;        // timestamp of the previous lone `d` press

  const byId = (id) => document.getElementById(id);
  const cellsEl = () => byId('nb-cells');

  let nextCellId = 1;
  function newCell(type, source) {
    return { id: 'c' + (nextCellId++), type, source: source || '',
             outputs: null, count: null, htmlSnapshot: null };
  }

  /* ──────── tabs as database bindings ──────── */
  /* Each tab is one notebook plus the database it is bound to. Both
     deployments share a single ACTIVE connection (the Playground's
     PGlite cannot even open two), so tabs multiplex serially: a tab
     whose binding differs from the live connection shows a banner
     offering to switch (or to create the database, or to rebind);
     activating it never switches silently. Loading an .ipynb opens a
     new tab; booting on a different database than the active tab's
     binding activates (or creates) a tab bound to it. Inactive tabs
     park their notebook as an ipynb doc; their kernels stay alive
     until the tab closes or the page unloads. */

  function currentTab() {
    return tabs.find((t) => t.id === activeTabId) || null;
  }

  function flushActiveTab() {
    const tab = currentTab();
    if (!tab) return;
    tab.doc = toIpynb();
    tab.kernel = kernel;
    tab.resume = {
      idx: selected ? cells.indexOf(selected) : -1,
      scrollY: window.scrollY,
    };
  }

  function persistTabs() {
    flushActiveTab();
    try {
      localStorage.setItem('ps.nb.tabs', JSON.stringify({
        active: activeTabId,
        tabs: tabs.map((t) => ({ id: t.id, name: t.name, db: t.db,
                                 doc: t.doc, resume: t.resume })),
      }));
    } catch (e) { /* quota / disabled */ }
  }

  function makeTab(name, db, doc) {
    return { id: 't' + (nextTabId++), name: name || 'Untitled',
             db: db || connDb || null, doc: doc || null,
             kernel: null, resume: null };
  }

  function loadTabIntoView(tab) {
    if (tab.doc) {
      loadNotebook(tab.doc);
    } else {
      defaultNotebook();
    }
    kernel = tab.kernel || null;
    if (kernel) setKernelChip('alive', `pid ${kernel.pid} · ${kernel.db}`);
    else setKernelChip('none', 'no kernel');
    if (tab.resume) {
      if (Number.isInteger(tab.resume.idx) && tab.resume.idx >= 0
          && tab.resume.idx < cells.length) {
        selectCell(cells[tab.resume.idx], { scroll: false });
      }
      const y = tab.resume.scrollY;
      if (Number.isFinite(y) && y > 0) {
        // Markdown / KaTeX cells render asynchronously and grow the page
        // after the first paint; wait for this paint's renders to settle
        // before restoring the saved offset, so returning (e.g. from
        // Circuit mode) lands on the same content rather than near the top.
        Promise.allSettled(mdRenders.slice()).then(() =>
          requestAnimationFrame(() => requestAnimationFrame(
            () => window.scrollTo(0, y))));
      }
    } else {
      if (cells.length) selectCell(cells[0], { scroll: false });
      window.scrollTo(0, 0);
    }
    renderTabBar();
    updateBindingBanner();
    updateProvsqlBanner();
  }

  function activateTab(id) {
    if (id === activeTabId) return;
    flushActiveTab();
    const tab = tabs.find((t) => t.id === id);
    if (!tab) return;
    activeTabId = id;
    loadTabIntoView(tab);
    persistTabs();
  }

  // A tab worth dropping silently: never ran anything (no kernel) and
  // holds no content (the freshly-booted Untitled tab).
  function tabIsPristine(tab) {
    if (!tab || tab.kernel) return false;
    const docCells = (tab.doc && tab.doc.cells) || [];
    return docCells.every((c) =>
      !String(Array.isArray(c.source) ? c.source.join('')
                                      : c.source || '').trim()
      && !(c.outputs && c.outputs.length));
  }

  function newTab(name, db, doc) {
    flushActiveTab();
    const prev = currentTab();
    const tab = makeTab(name, db, doc);
    tabs.push(tab);
    activeTabId = tab.id;
    // Opening a document (example / .ipynb load) from a pristine
    // Untitled tab replaces it rather than leaving an empty husk
    // behind. The + button (doc == null) keeps the old tab: making a
    // second blank tab is exactly its job.
    if (doc && prev && tabIsPristine(prev)) {
      tabs.splice(tabs.indexOf(prev), 1);
    }
    loadTabIntoView(tab);
    persistTabs();
    return tab;
  }

  // Does a tab hold anything worth a confirm-before-close? The active tab's
  // live cells, or an inactive tab's stored doc.
  function tabHasContent(tab) {
    return tab.doc
      ? (tab.doc.cells || []).some((c) => String(
          Array.isArray(c.source) ? c.source.join('') : c.source || '').trim())
      : (tab.id === activeTabId && cells.some((c) => (c.source || '').trim()));
  }

  function closeTab(id) {
    const idx = tabs.findIndex((t) => t.id === id);
    if (idx < 0) return;
    const tab = tabs[idx];
    if (tabHasContent(tab)
        && !window.confirm(`Close tab “${tabDisplayName(tab)}”?`)) return;
    if (tab.kernel) {
      fetch(`/api/nb/session/${encodeURIComponent(tab.kernel.sessionId)}`,
            { method: 'DELETE' }).catch(() => {});
    }
    tabs.splice(idx, 1);
    if (id === activeTabId) {
      activeTabId = null;
      kernel = null;
      if (tabs.length) activateTab(tabs[Math.max(0, idx - 1)].id);
      else newTab();
    } else {
      renderTabBar();
      persistTabs();
    }
  }

  // Drop every tab and reopen a single fresh one. One confirm covers the
  // lot when any tab holds content; each tab's kernel is released.
  function closeAllTabs() {
    if (!tabs.length) return;
    if (tabs.some(tabHasContent)
        && !window.confirm('Close all tabs and start fresh?')) return;
    for (const t of tabs) {
      if (t.kernel) {
        fetch(`/api/nb/session/${encodeURIComponent(t.kernel.sessionId)}`,
              { method: 'DELETE' }).catch(() => {});
      }
    }
    tabs = [];
    activeTabId = null;
    kernel = null;
    newTab();   // one fresh Untitled tab; renders + persists
  }

  // A tab's display name is the first level-1 Markdown heading in its
  // notebook (the document names itself, like a paper title); the
  // stored name (file stem / "Untitled") is only the fallback.
  function headingTabName(tab) {
    const list = tab.id === activeTabId
      ? cells.map((c) => ({ md: c.type === 'markdown', src: c.source || '' }))
      : ((tab.doc && tab.doc.cells) || []).map((c) => ({
          md: c.cell_type === 'markdown',
          src: Array.isArray(c.source) ? c.source.join('') : (c.source || ''),
        }));
    for (const c of list) {
      if (!c.md) continue;
      let inFence = false;
      for (const line of String(c.src).split('\n')) {
        if (/^\s*(```|~~~)/.test(line)) { inFence = !inFence; continue; }
        if (inFence) continue;
        const m = line.match(/^#\s+(.+?)\s*#*\s*$/);
        if (m) return m[1];
      }
    }
    return null;
  }

  function tabDisplayName(tab) {
    return headingTabName(tab) || tab.name;
  }

  function renderTabBar() {
    const bar = byId('nb-tabs');
    if (!bar) return;
    const esc = env.escapeHtml, escA = env.escapeAttr;
    bar.innerHTML = tabs.map((t) => {
      const active = t.id === activeTabId;
      const foreign = t.db && connDb && t.db !== connDb;
      const name = tabDisplayName(t);
      return `<span class="nb__tab${active ? ' nb__tab--active' : ''}"`
        + ` data-tab="${escA(t.id)}" title="${escA(name)}${t.db ? ' – ' + escA(t.db) : ''}">`
        + `<span class="nb__tab-name">${esc(name)}</span>`
        + (foreign ? `<span class="nb__tab-db">${esc(t.db)}</span>` : '')
        + `<button type="button" class="nb__tab-close" data-tab-close="${escA(t.id)}"`
        + ` title="Close tab" aria-label="Close tab">×</button>`
        + `</span>`;
    }).join('')
    + `<button type="button" class="nb__tab-add" id="nb-tab-add"`
    + ` title="New notebook tab (bound to the current database)">+</button>`
    // Discreet "close all" at the far end, only once a second tab exists.
    + (tabs.length > 1
        ? `<button type="button" class="nb__tab-closeall" id="nb-tab-closeall"`
          + ` title="Close all tabs and start fresh">close all</button>`
        : '');
  }

  function wireTabBar() {
    const bar = byId('nb-tabs');
    if (!bar) return;
    bar.addEventListener('click', (e) => {
      const close = e.target.closest('[data-tab-close]');
      if (close) {
        closeTab(close.dataset.tabClose);
        return;
      }
      if (e.target.closest('#nb-tab-add')) {
        newTab();
        return;
      }
      if (e.target.closest('#nb-tab-closeall')) {
        closeAllTabs();
        return;
      }
      const tabEl = e.target.closest('[data-tab]');
      if (tabEl) activateTab(tabEl.dataset.tab);
    });
  }

  /* Binding banner: the active tab's database vs the live connection.
     Offers exactly the action that makes sense: "Switch to X" when the
     bound database exists (is CONNECT-able), "Create X" when it does
     not -- offering both would be nonsense. */
  let dbListCache = null;   // GET /api/databases payload

  async function accessibleDatabases() {
    if (dbListCache) return dbListCache;
    try {
      const resp = await fetch('/api/databases');
      if (resp.ok) dbListCache = await resp.json();
    } catch (e) { /* fall through */ }
    return dbListCache || [];
  }

  async function updateBindingBanner() {
    const banner = byId('nb-binding-banner');
    if (!banner) return;
    const tab = currentTab();
    const foreign = tab && tab.db && connDb && tab.db !== connDb;
    banner.hidden = !foreign;
    if (!foreign) { banner.innerHTML = ''; return; }
    const exists = (await accessibleDatabases()).includes(tab.db);
    // The tab may have changed while the list was fetched.
    if (currentTab() !== tab) return;
    const esc = env.escapeHtml;
    banner.innerHTML =
      `<span class="nb__banner-msg"><i class="fas fa-database"></i> `
      + `This notebook is bound to <strong>${esc(tab.db)}</strong>`
      + `${exists ? '' : ' (which does not exist)'}; `
      + `you are connected to <strong>${esc(connDb)}</strong>.</span>`
      + (exists
        ? `<button type="button" class="wp-btn wp-btn--mini" id="nb-bind-switch">`
          + `Switch to ${esc(tab.db)}</button> `
        : `<button type="button" class="wp-btn wp-btn--mini" id="nb-bind-create">`
          + `Create ${esc(tab.db)}</button> `)
      + `<button type="button" class="wp-btn wp-btn--ghost wp-btn--mini" id="nb-bind-keep">`
      + `Rebind to ${esc(connDb)}</button>`;
    const sw = byId('nb-bind-switch');
    if (sw) sw.addEventListener('click', () => switchConnectionTo(tab.db));
    const cr = byId('nb-bind-create');
    if (cr) cr.addEventListener('click', async () => {
      const resp = await fetch('/api/databases', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: tab.db }),
      });
      const payload = await resp.json().catch(() => ({}));
      dbListCache = null;
      if (resp.status === 409) {
        // Created elsewhere since the list was cached: just switch.
        switchConnectionTo(tab.db);
        return;
      }
      if (!resp.ok) {
        window.alert(payload.error || `HTTP ${resp.status}`);
        return;
      }
      if (payload.warning) window.alert(payload.warning);
      switchConnectionTo(tab.db);
    });
    byId('nb-bind-keep').addEventListener('click', () => {
      tab.db = connDb;
      persistTabs();
      renderTabBar();
      updateBindingBanner();
    });
  }

  /* ProvSQL-missing banner: the live connection's database has no provsql
     extension, so every provenance query would fail one by one. Offer to
     install it (CREATE EXTENSION IF NOT EXISTS provsql CASCADE) in one
     click, rather than making the user hand-type it or leave the notebook.
     Orthogonal to the binding banner above (that is about which database
     the tab targets; this is about the connected database's readiness). */
  async function updateProvsqlBanner() {
    const banner = byId('nb-provsql-banner');
    if (!banner) return;
    // A set connDb with a falsy version means /api/conn resolved and
    // provsql is absent; a failed conn fetch leaves connDb null, so a
    // connectivity blip does not flash the banner.
    const missing = !!connDb && !connExtVersion;
    banner.hidden = !missing;
    if (!missing) { banner.innerHTML = ''; return; }
    const esc = env.escapeHtml;
    banner.innerHTML =
      `<span class="nb__banner-msg"><i class="fas fa-exclamation-triangle"></i> `
      + `ProvSQL is not installed on <strong>${esc(connDb)}</strong>; `
      + `provenance queries will not work here.</span>`
      + `<button type="button" class="wp-btn wp-btn--mini" id="nb-provsql-install">`
      + `Install ProvSQL</button>`;
    const btn = byId('nb-provsql-install');
    if (!btn) return;
    btn.addEventListener('click', async () => {
      btn.disabled = true;
      btn.textContent = 'Installing…';
      try {
        const resp = await fetch('/api/install-provsql', { method: 'POST' });
        const payload = await resp.json().catch(() => ({}));
        if (!resp.ok) {
          window.alert(payload.error || `HTTP ${resp.status}`);
          btn.disabled = false;
          btn.textContent = 'Install ProvSQL';
          return;
        }
        // Committed DDL is visible to every kernel session immediately, so
        // no reload is needed; just drop the banner and let the chrome's
        // /api/conn poll pick the version up for its chip.
        connExtVersion = payload.version || 'installed';
        updateProvsqlBanner();
      } catch (e) {
        window.alert(String((e && e.message) || e));
        btn.disabled = false;
        btn.textContent = 'Install ProvSQL';
      }
    });
  }

  async function switchConnectionTo(dbname) {
    // Persist first: the connection switch reloads the page (and kills
    // every kernel server-side); on boot the active tab matches the new
    // database, so the reconcile below leaves it in place.
    persistTabs();
    try {
      const resp = await fetch('/api/conn', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ database: dbname }),
      });
      if (!resp.ok) {
        const payload = await resp.json().catch(() => ({}));
        window.alert(payload.error || `HTTP ${resp.status}`);
        return;
      }
    } catch (e) {
      window.alert(`Network error: ${e.message}`);
      return;
    }
    window.location.reload();
  }

  /* ──────── kernel client ──────── */

  function setKernelChip(state, label) {
    const chip = byId('nb-kernel-chip');
    const lbl = byId('nb-kernel-label');
    if (!chip || !lbl) return;
    chip.className = 'nb__kernel nb__kernel--' + state;
    lbl.textContent = label;
  }

  async function ensureKernel() {
    if (kernel) return kernel;
    if (kernelStarting) return kernelStarting;
    setKernelChip('starting', 'starting…');
    kernelStarting = (async () => {
      const resp = await fetch('/api/nb/session', { method: 'POST' });
      const payload = await resp.json().catch(() => ({}));
      if (!resp.ok) {
        setKernelChip('dead', payload.error || `kernel failed (HTTP ${resp.status})`);
        throw new Error(payload.error || `HTTP ${resp.status}`);
      }
      kernel = { sessionId: payload.session_id, pid: payload.pid, db: payload.db };
      setKernelChip('alive', `pid ${kernel.pid} · ${kernel.db}`);
      return kernel;
    })();
    try {
      return await kernelStarting;
    } finally {
      kernelStarting = null;
    }
  }

  function kernelGone(reason) {
    kernel = null;
    setKernelChip('dead', reason || 'kernel died – restart it');
  }

  async function shutdownKernel() {
    if (!kernel) return;
    const id = kernel.sessionId;
    kernel = null;
    setKernelChip('none', 'no kernel');
    try {
      await fetch(`/api/nb/session/${encodeURIComponent(id)}`, { method: 'DELETE' });
    } catch (e) { /* server gone: nothing to clean */ }
  }

  async function restartKernel() {
    await shutdownKernel();
    // Restart resets the Jupyter-style staleness markers: every cell's
    // counter goes back to "not run on this kernel".
    execCounter = 0;
    for (const c of cells) { c.count = null; }
    for (const c of cells) updateGutter(c);
    try { await ensureKernel(); } catch (e) { /* chip already shows it */ }
  }

  /* ──────── markdown rendering (vendored marked + DOMPurify) ──────── */

  function loadScript(src) {
    return new Promise((resolve, reject) => {
      const s = document.createElement('script');
      s.src = src;
      s.onload = resolve;
      s.onerror = () => reject(new Error('failed to load ' + src));
      document.head.appendChild(s);
    });
  }

  function loadStyle(href) {
    return new Promise((resolve, reject) => {
      const l = document.createElement('link');
      l.rel = 'stylesheet';
      l.href = href;
      l.onload = resolve;
      l.onerror = () => reject(new Error('failed to load ' + href));
      document.head.appendChild(l);
    });
  }

  function ensureMdLibs() {
    if (!mdLibs) {
      // marked + DOMPurify are the core renderer: prose must render even if the
      // optional KaTeX assets are missing, so they alone gate the promise.
      mdLibs = Promise.all([
        loadScript('/static/vendor/marked.min.js'),
        loadScript('/static/vendor/purify.min.js'),
      ]);
      // KaTeX for $…$ / $$…$$ math in Markdown cells; auto-render needs the
      // katex global, so it is chained after katex.min.js. Loaded best-effort
      // and off the critical path: a load failure leaves math un-rendered (the
      // $…$ source shows through) rather than degrading the whole cell to raw
      // text.
      loadStyle('/static/vendor/katex.min.css').catch(() => {});
      // Tracked in katexReady (which always resolves, even on load failure)
      // so renderMarkdownInto can await it: math then renders on the first
      // paint instead of racing the 275 KB katex.min.js, while a missing
      // asset still only leaves math un-rendered rather than dropping the cell.
      katexReady = loadScript('/static/vendor/katex.min.js')
        .then(() => loadScript('/static/vendor/auto-render.min.js'))
        .catch(() => {});
    }
    return mdLibs;
  }

  async function renderMarkdownInto(el, source) {
    try {
      await ensureMdLibs();
      const html = window.marked.parse(source, { gfm: true, breaks: false });
      el.innerHTML = window.DOMPurify.sanitize(html);
      // Links (doc references, external URLs) must open in a new tab: a
      // same-tab navigation fires `pagehide`, which beacons the kernel
      // session closed -- coming back then reports "unknown or expired
      // notebook session". Fragment-only links stay in-page.
      for (const a of el.querySelectorAll('a[href]')) {
        if (!(a.getAttribute('href') || '').startsWith('#')) {
          a.target = '_blank';
          a.rel = 'noopener noreferrer';
        }
      }
      // ```sql fences get the same tokenizer as the cell editors (marked
      // tags them code.language-sql). highlightSql escapes its input, so
      // feeding it the decoded textContent cannot reintroduce markup.
      const hl = window.ProvsqlStudio.highlightSql;
      if (hl) {
        for (const code of el.querySelectorAll(
            'pre code.language-sql, pre code.language-postgresql')) {
          code.innerHTML = hl(code.textContent);
        }
      }
      // Render LaTeX math (the rst :math: roles become $…$ / $$…$$). Runs
      // on the live DOM after sanitize, so KaTeX's spans are not stripped;
      // its default ignoredTags skip <pre>/<code>, so SQL fences (and any
      // ``DO $$ … $$`` dollar-quoting in them) are left untouched.
      if (katexReady) await katexReady;
      if (window.renderMathInElement) {
        window.renderMathInElement(el, {
          delimiters: [
            { left: '$$', right: '$$', display: true },
            { left: '$',  right: '$',  display: false },
          ],
          throwOnError: false,
        });
      }
    } catch (e) {
      // Renderer unavailable (vendored file missing): degrade to
      // escaped plain text rather than hiding the user's prose.
      el.textContent = source;
    }
  }

  /* ──────── cell DOM ──────── */

  function cellEl(cell) {
    return cellsEl().querySelector(`[data-cell-id="${cell.id}"]`);
  }

  function updateGutter(cell) {
    const el = cellEl(cell);
    if (!el) return;
    const g = el.querySelector('.nb-cell__count');
    if (!g) return;
    if (running && running.cellId === cell.id) g.textContent = '[*]';
    else if (cell.count != null) g.textContent = `[${cell.count}]`;
    else g.textContent = '[ ]';
  }

  function buildOutputTargets(outEl) {
    outEl.innerHTML = `
      <div class="nb-out__banners"></div>
      <div class="wp-table-wrap nb-out__tablewrap">
        <table class="wp-table wp-table--result">
          <thead><tr></tr></thead>
          <tbody></tbody>
        </table>
      </div>
      <div class="nb-out__meta">
        <span class="nb-out__count"></span> <span class="nb-out__noun">tuples</span><span class="nb-out__trunc" hidden></span>
        · <span class="nb-out__time">–</span> ms
      </div>`;
    return {
      head: outEl.querySelector('thead tr'),
      body: outEl.querySelector('tbody'),
      count: outEl.querySelector('.nb-out__count'),
      noun: outEl.querySelector('.nb-out__noun'),
      banners: outEl.querySelector('.nb-out__banners'),
      truncated: outEl.querySelector('.nb-out__trunc'),
    };
  }

  function renderOutputs(cell) {
    if (cell.type !== 'sql') return;
    const el = cellEl(cell);
    if (!el) return;
    const outEl = el.querySelector('.nb-out');
    if (!outEl) return;
    if (!cell.outputs) {
      outEl.hidden = true;
      outEl.innerHTML = '';
      return;
    }
    outEl.hidden = false;
    const targets = buildOutputTargets(outEl);
    const R = window.ProvsqlStudio.makeBlockRenderer(env, targets);
    const p = cell.outputs;
    R.renderBlocks(p.blocks || [], !!p.wrapped, p.notices || []);
    const timeEl = outEl.querySelector('.nb-out__time');
    if (timeEl) timeEl.textContent = p.elapsed_ms != null ? p.elapsed_ms : '–';
    // Snapshot for the .ipynb text/html bundle (external viewers); the
    // JSON payload stays the source of truth Studio re-renders from.
    cell.htmlSnapshot = outEl.innerHTML;
  }

  function autosize(ta) {
    ta.style.height = 'auto';
    ta.style.height = Math.max(ta.scrollHeight, 34) + 'px';
  }

  function buildCellDom(cell) {
    const div = document.createElement('div');
    div.className = `nb-cell nb-cell--${cell.type}`;
    div.dataset.cellId = cell.id;
    div.innerHTML = `
      <div class="nb-cell__gutter">
        ${cell.type === 'markdown'
          ? ''  /* markdown cells have no execution count, like Jupyter */
          : `<span class="nb-cell__count">[ ]</span>`}
        ${cell.type === 'sql'
          ? `<button type="button" class="nb-cell__run" title="Run this cell (Ctrl+Enter)"><i class="fas fa-play"></i></button>`
          : ''}
        ${cell.type === 'circuit'
          ? `<button type="button" class="nb-cell__run" title="Re-fetch the circuit"><i class="fas fa-sync"></i></button>`
          : ''}
        ${cell.type === 'eval'
          ? `<button type="button" class="nb-cell__run" title="Run this evaluation (Ctrl+Enter)"><i class="fas fa-play"></i></button>`
          : ''}
      </div>
      <div class="nb-cell__main">
        ${cell.type === 'circuit' ? `
          <div class="nb-circ">
            <div class="nb-circ__hdr">
              <i class="fas fa-project-diagram"></i>
              Circuit for <code class="nb-circ__token" title="${cell.token || ''}">${shortToken(cell.token)}</code>
              <button type="button" class="nb-circ__eval wp-btn wp-btn--ghost wp-btn--mini"
                      ${cell.tokenKind === 'agg_token' || cell.tokenKind === 'random_variable' ? 'hidden ' : ''}title="Insert an evaluation cell for this token below (semiring / probability)">
                <i class="fas fa-bolt"></i> Evaluate</button>
              <button type="button" class="nb-circ__jump wp-btn wp-btn--ghost wp-btn--mini"
                      title="Open this token in Circuit mode (eval strip, expansion, inspector)">
                <i class="fas fa-external-link-alt"></i> Circuit mode</button>
            </div>
            <div class="nb-circ__canvas"></div>
          </div>
        ` : ''}
        ${cell.type === 'eval' ? `
          <div class="nb-eval">
            <div class="nb-eval__hdr">
              <i class="fas fa-bolt"></i>
              <code class="nb-eval__token" title="${cell.token || ''}">${shortToken(cell.token)}</code>
              <select class="nb-eval__semiring" title="What to evaluate on this token">
                ${EVAL_SEMIRINGS.map(([v, l]) =>
                  `<option value="${v}"${v === (cell.semiring || 'probability') ? ' selected' : ''}>${l}</option>`).join('')}
              </select>
              <select class="nb-eval__method" title="Probability method (the full per-method controls live in Circuit mode)">
                ${EVAL_METHODS.map((m) =>
                  `<option value="${m}"${m === (cell.method || '') ? ' selected' : ''}>${m || '(default)'}</option>`).join('')}
              </select>
              <input type="text" class="nb-eval__args" placeholder="arguments (eps=…, samples=…)"
                     title="Optional method arguments, key=value comma-separated (e.g. eps=0.05,delta=0.01 or a sample count)"
                     value="${env.escapeAttr(cell.args || '')}">
              <select class="nb-eval__mapping" title="Optional provenance mapping labelling the input tokens" hidden>
                <option value="">(no mapping)</option>
              </select>
            </div>
            <div class="nb-eval__out" hidden></div>
          </div>
        ` : ''}
        ${cell.type === 'sql' ? `
          ${cell.scheme ? `
          <span class="nb-cell__scheme" title="This cell runs under the ${cell.scheme} provenance scheme (overrides the toolbar default)">${cell.scheme}</span>
          ` : ''}
          <div class="wp-editor nb-cell__editor">
            <div class="wp-editor__panel">
              <pre class="wp-editor__hl nb-cell__hl" aria-hidden="true"><code></code></pre>
              <textarea class="wp-editor__ta nb-cell__ta" rows="1" spellcheck="false"
                        placeholder="SELECT ..."></textarea>
            </div>
          </div>
          <div class="nb-out" hidden></div>
        ` : `
          <div class="nb-cell__md" title="Double-click to edit"></div>
          <textarea class="nb-cell__mdta" spellcheck="true" hidden
                    placeholder="Markdown…"></textarea>
        `}
      </div>
      <div class="nb-cell__actions">
        <button type="button" data-act="up" title="Move up"><i class="fas fa-arrow-up"></i></button>
        <button type="button" data-act="down" title="Move down"><i class="fas fa-arrow-down"></i></button>
        <button type="button" data-act="del" title="Delete cell (command mode: dd, z undoes)"><i class="fas fa-trash"></i></button>
        ${cell.type === 'sql'
          ? `<button type="button" data-act="to-circuit" title="Open this query in Circuit mode"><i class="fas fa-project-diagram"></i></button>`
          : ''}
        ${cell.type === 'sql'
          ? `<button type="button" data-act="scheme" title="Provenance scheme for this cell: ${cell.scheme || 'notebook default'} – click to cycle (default → semiring → absorptive → where → boolean)"><i class="fas fa-sliders-h"></i></button>`
          : ''}
        <button type="button" data-act="add-sql" title="Insert SQL cell below (command mode: b below, a above)"><i class="fas fa-plus"></i></button>
        <button type="button" data-act="add-md" title="Insert Markdown cell below (command mode: m converts)"><i class="fab fa-markdown"></i></button>
      </div>`;
    wireCellDom(cell, div);
    return div;
  }

  function wireCellDom(cell, div) {
    if (cell.type === 'sql') {
      const ta = div.querySelector('.nb-cell__ta');
      const code = div.querySelector('.nb-cell__hl code');
      const hl = window.ProvsqlStudio.highlightSql || ((t) => '');
      const refresh = () => {
        code.innerHTML = hl(ta.value);
        autosize(ta);
        cell.source = ta.value;
        scheduleAutosave();
      };
      ta.value = cell.source;
      ta.addEventListener('focus', () => { lastFocused = cell; selectCell(cell, { scroll: false }); });
      ta.addEventListener('input', refresh);
      // Clean pasted / dropped SQL of invisible Unicode (NBSP, zero-width
      // characters…) exactly like the shared query box; the sanitizer
      // re-fires `input`, so refresh() repaints with the cleaned text.
      if (window.ProvsqlStudio.wirePasteSanitizer) {
        window.ProvsqlStudio.wirePasteSanitizer(ta);
      }
      ta.addEventListener('keydown', (e) => {
        if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
          e.preventDefault();
          runCell(cell);
        } else if (e.shiftKey && e.key === 'Enter') {
          e.preventDefault();
          ta.blur();                     // Jupyter: drop to command mode
          runSelectedThenAdvance(cell);
        } else if (e.altKey && e.key === 'Enter') {
          e.preventDefault();
          runAltEnter(cell);
        } else if (e.key === 'Escape') {
          e.preventDefault();
          ta.blur();
          selectCell(cell);
        }
      });
      div.querySelector('.nb-cell__run').addEventListener('click', () => runCell(cell));
      // Initial paint (after insertion, so scrollHeight is real).
      requestAnimationFrame(refresh);
    } else if (cell.type === 'markdown') {
      const view = div.querySelector('.nb-cell__md');
      const ta = div.querySelector('.nb-cell__mdta');
      let everEdited = false;
      const startEdit = () => {
        everEdited = true;
        ta.value = cell.source;
        view.hidden = true;
        ta.hidden = false;
        autosize(ta);
        ta.focus();
      };
      const endEdit = () => {
        cell.source = ta.value;
        ta.hidden = true;
        view.hidden = false;
        renderMarkdownInto(view, cell.source || '*(empty cell: double-click to edit)*');
        scheduleAutosave();
      };
      ta.addEventListener('focus', () => { lastFocused = cell; selectCell(cell, { scroll: false }); });
      view.addEventListener('dblclick', startEdit);
      ta.addEventListener('blur', endEdit);
      ta.addEventListener('input', () => autosize(ta));
      ta.addEventListener('keydown', (e) => {
        if (((e.ctrlKey || e.metaKey) || e.shiftKey) && e.key === 'Enter') {
          e.preventDefault();
          ta.blur();                     // blur renders via endEdit
          if (e.shiftKey) runSelectedThenAdvance(cell);
        } else if (e.key === 'Escape') {
          e.preventDefault();
          ta.blur();
          selectCell(cell);
        }
      });
      mdRenders.push(
        renderMarkdownInto(view, cell.source || '*(empty cell: double-click to edit)*'));
      // Fresh empty markdown cells drop straight into edit mode (the
      // "+ Markdown" flow); cells *converted* to markdown (the `m` key)
      // stay in command mode, like Jupyter, so the keymap keeps working.
      // The everEdited guard stops the deferred auto-edit from
      // REOPENING the editor when something else (focusCell's synthetic
      // dblclick) already opened it and the user finished the edit
      // within the same frame.
      if (!cell.source && !cell._noAutoEdit) {
        requestAnimationFrame(() => { if (!everEdited) startEdit(); });
      }
      delete cell._noAutoEdit;
    }

    if (cell.type === 'circuit') {
      div.querySelector('.nb-cell__run').addEventListener('click',
        () => refreshCircuitCell(cell));
      div.querySelector('.nb-circ__eval').addEventListener('click', () => {
        insertEvalAfter(cell, cell.token, sceneRootFlags(cell.scene));
      });
      div.querySelector('.nb-circ__jump').addEventListener('click', () => {
        // Same carry mechanism as the where-mode jump button: Circuit
        // mode preloads the token on arrival. The row's provenance
        // gate (recorded when the snapshot came from a result-row
        // click on an rv / non-provsql uuid) rides along so the eval
        // strip's "Conditioned by" presets to the row, as it does for
        // in-Circuit-mode clicks.
        try {
          sessionStorage.setItem('ps.preloadCircuit', cell.token);
          if (cell.rowProv) {
            sessionStorage.setItem('ps.preloadCircuitRowProv', cell.rowProv);
          } else {
            sessionStorage.removeItem('ps.preloadCircuitRowProv');
          }
        } catch (e) { /* plain navigation */ }
        window.location.href = '/circuit';
      });
      // Saved notebooks carry the scene; paint it without a fetch.
      if (cell.scene) {
        requestAnimationFrame(() => {
          const box = div.querySelector('.nb-circ__canvas');
          if (box) paintSceneInto(box, cell.scene);
        });
      }
    }

    if (cell.type === 'eval') {
      div.querySelector('.nb-cell__run').addEventListener('click',
        () => runEvalCell(cell));
      const sem = div.querySelector('.nb-eval__semiring');
      const meth = div.querySelector('.nb-eval__method');
      const args = div.querySelector('.nb-eval__args');
      const map = div.querySelector('.nb-eval__mapping');
      // Hide semirings the evaluator would refuse on a Boolean/absorptive
      // root before the user can pick them (parity with Circuit mode); if
      // the persisted selection just got hidden, snap to the first sound
      // option and refresh the dependent controls.
      if (filterEvalSemirings(cell, sem)) sem.value = cell.semiring;
      sem.addEventListener('change', () => {
        cell.semiring = sem.value;
        syncEvalControls(cell, div);
        scheduleAutosave();
      });
      meth.addEventListener('change', () => {
        cell.method = meth.value;
        scheduleAutosave();
      });
      args.addEventListener('input', () => {
        cell.args = args.value;
        scheduleAutosave();
      });
      map.addEventListener('change', () => {
        cell.mapping = map.value;
        scheduleAutosave();
      });
      ensureMappings().then((maps) => {
        for (const m of maps) {
          const opt = document.createElement('option');
          opt.value = m.qname;
          opt.textContent = `${m.qname} (${m.value_type || '?'})`;
          if (m.qname === cell.mapping) opt.selected = true;
          map.appendChild(opt);
        }
      });
      syncEvalControls(cell, div);
      if (cell.result) {
        requestAnimationFrame(() => renderEvalResult(cell));
      }
    }

    div.querySelector('.nb-cell__actions').addEventListener('click', (e) => {
      const btn = e.target.closest('[data-act]');
      if (!btn) return;
      const idx = cells.indexOf(cell);
      if (btn.dataset.act === 'up' && idx > 0) {
        cells.splice(idx, 1); cells.splice(idx - 1, 0, cell);
        cellsEl().insertBefore(div, div.previousElementSibling);
      } else if (btn.dataset.act === 'down' && idx < cells.length - 1) {
        cells.splice(idx, 1); cells.splice(idx + 1, 0, cell);
        cellsEl().insertBefore(div.nextElementSibling, div);
      } else if (btn.dataset.act === 'add-sql' || btn.dataset.act === 'add-md') {
        const c = newCell(btn.dataset.act === 'add-sql' ? 'sql' : 'markdown');
        cells.splice(idx + 1, 0, c);
        div.after(buildCellDom(c));
        focusCell(c);
      } else if (btn.dataset.act === 'scheme') {
        cycleScheme(cell, div);
        return;
      } else if (btn.dataset.act === 'to-circuit') {
        // Carry this cell's SQL into Circuit mode through the standard
        // mode-switch channel: executed cells auto-replay there, drafts
        // just land in the query box.
        try {
          sessionStorage.setItem('ps.sql', cell.source || '');
          if ((cell.source || '').trim() && cell.count != null) {
            sessionStorage.setItem('ps.sql.ran', '1');
          } else {
            sessionStorage.removeItem('ps.sql.ran');
          }
        } catch (e2) { /* sessionStorage disabled: plain navigation */ }
        window.location.href = '/circuit';
        return;
      } else if (btn.dataset.act === 'del') {
        if ((cell.source || '').trim()
            && !window.confirm('Delete this cell?')) return;
        cells.splice(idx, 1);
        div.remove();
        if (!cells.length) appendCell('sql');
      }
      scheduleAutosave();
    });
  }

  // Per-cell scheme cycling
  // (default -> semiring -> absorptive -> where -> boolean).
  // `undefined` means "follow the toolbar's notebook-level default".
  const SCHEME_CYCLE = [undefined, 'semiring', 'absorptive', 'where', 'boolean'];

  function cycleScheme(cell, div) {
    const idx = SCHEME_CYCLE.indexOf(cell.scheme);
    cell.scheme = SCHEME_CYCLE[(idx + 1) % SCHEME_CYCLE.length];
    // Refresh the chip in place (no full rebuild: outputs stay live).
    let chip = div.querySelector('.nb-cell__scheme');
    if (cell.scheme) {
      if (!chip) {
        chip = document.createElement('span');
        chip.className = 'nb-cell__scheme';
        div.querySelector('.nb-cell__editor').before(chip);
      }
      chip.textContent = cell.scheme;
      chip.title = `This cell runs under the ${cell.scheme} provenance `
        + 'scheme (overrides the toolbar default)';
    } else if (chip) {
      chip.remove();
    }
    const btn = div.querySelector('[data-act="scheme"]');
    if (btn) {
      btn.title = `Provenance scheme for this cell: `
        + `${cell.scheme || 'notebook default'} – click to cycle `
        + '(default → semiring → absorptive → where → boolean)';
    }
    scheduleAutosave();
  }

  function focusCell(cell) {
    const el = cellEl(cell);
    if (!el) return;
    const ta = el.querySelector('.nb-cell__ta, .nb-cell__mdta:not([hidden])');
    if (ta) ta.focus();
    else el.querySelector('.nb-cell__md')?.dispatchEvent(new Event('dblclick'));
  }

  function appendCell(type, source) {
    const c = newCell(type, source);
    cells.push(c);
    cellsEl().appendChild(buildCellDom(c));
    return c;
  }

  function focusedCell() {
    // Clicking a toolbar button moves focus to the button itself, so
    // activeElement rarely sits inside a cell at dispatch time; the
    // focus-tracked `lastFocused` is the actual "current" cell.
    const el = document.activeElement && document.activeElement.closest
      ? document.activeElement.closest('.nb-cell')
      : null;
    if (el) {
      const c = cells.find((x) => x.id === el.dataset.cellId);
      if (c) return c;
    }
    if (selected && cells.includes(selected) && selected.type === 'sql') {
      return selected;
    }
    if (lastFocused && cells.includes(lastFocused)) return lastFocused;
    return cells.find((c) => c.type === 'sql') || null;
  }

  /* ──────── circuit cells ──────── */
  /* A circuit cell is a self-contained snapshot of the provenance DAG
     for one token: the /api/circuit scene (positions computed
     server-side) painted by the compact renderer below, which reuses
     circuit.js's CSS vocabulary (.node-group/.node-shape/.node-label/
     .edge/.edge-pos) so the snapshot reads exactly like the Circuit
     mode canvas. The scene JSON is what persists in the .ipynb (plus
     an image/svg+xml copy for external viewers); on load the cell
     re-paints from the JSON without a database. Interactivity beyond
     a depth selector and a refresh deliberately stays in Circuit mode
     (one click away via the jump link). */

  // Child-position labelling rules, mirrored from circuit.js: only
  // gates whose argument order matters get the digits.
  const ORDERED_GATES = new Set(['cmp', 'monus', 'agg', 'arith', 'mixture',
                                 'conditioned', 'case', 'rv']);
  const COMMUTATIVE_AGG = new Set(['sum', 'count', 'min', 'max', 'avg']);
  const COMMUTATIVE_CMP = new Set(['=', '<>', '!=']);
  const NON_COMMUTATIVE_ARITH = new Set([2, 3]);   // MINUS, DIV
  function shouldLabelChildren(parent) {
    if (!ORDERED_GATES.has(parent.type)) return false;
    if (parent.type === 'agg') {
      return !COMMUTATIVE_AGG.has((parent.info1_name || '').toLowerCase());
    }
    if (parent.type === 'cmp') {
      return !COMMUTATIVE_CMP.has(parent.info1_name || '');
    }
    if (parent.type === 'arith') {
      const tag = parent.info1 == null ? null : Number(parent.info1);
      return Number.isFinite(tag) && NON_COMMUTATIVE_ARITH.has(tag);
    }
    return true;
  }
  // A parametric (latent / compound) rv wires one or more of its
  // distribution parameters as tokens; the extra "kind:$0[,$1]" maps wire
  // (pos-1) to a parameter slot, labelled with that family's parameter
  // symbol (μ / σ / λ …) from the scene's rv_families registry.  Mirrors
  // circuit.js's _rvEdgeLabel so a notebook snapshot reads like the Circuit
  // canvas; falls back to the bare position when the registry is absent.
  function rvEdgePosLabel(parent, pos, scene) {
    if (!parent.extra || !scene) return null;
    const m = String(parent.extra).match(/^\s*([a-zA-Z_]+)\s*:(.*)$/);
    if (!m) return null;
    const kind = m[1].toLowerCase();
    const slots = m[2].split(',').map((x) => x.trim());
    const j = slots.indexOf('$' + (pos - 1));   // wire index = pos - 1
    if (j < 0) return null;
    const reg = (scene.rv_families || {})[kind];
    const names = reg && reg.param_names;
    return (names && names[j]) || null;
  }
  function edgePosLabel(parent, pos, scene) {
    if (parent.type === 'mixture') {
      return ({ 1: 'p', 2: 'x', 3: 'y' })[pos] || String(pos);
    }
    if (parent.type === 'conditioned') {
      // A | B, with the joint A∧B for the discrete (uuid|uuid) carrier.
      return ({ 1: 'A', 2: 'B', 3: 'A∧B' })[pos] || String(pos);
    }
    if (parent.type === 'rv') {
      return rvEdgePosLabel(parent, pos, scene) || String(pos);
    }
    return String(pos);
  }

  function svgEl(tag, attrs) {
    const el = document.createElementNS('http://www.w3.org/2000/svg', tag);
    for (const [k, v] of Object.entries(attrs || {})) el.setAttribute(k, v);
    return el;
  }

  // Assumed-class / certificate badges, ported from circuit.js's node
  // renderer (the B / A / D / IF rings and badges). Hardcoded hex (not
  // CSS vars) so the htmlSnapshot SVG embedded in the .ipynb renders
  // standalone in external viewers; the resolved values match
  // colors_and_type.css (purple-700/900) and circuit.js's amber/green/
  // teal fallbacks. The badge geometry (radii, offsets) mirrors
  // circuit.js's node renderer so the inline canvas matches Circuit
  // mode.
  function paintNodeBadges(g, n) {
    const addBadge = (cx, cy, r, fill, stroke, text, fontSize, title) => {
      const grp = svgEl('g', {});
      const tip = svgEl('title');
      tip.textContent = title;
      grp.appendChild(tip);
      grp.appendChild(svgEl('circle', {
        cx, cy, r, fill, stroke, 'stroke-width': 1,
      }));
      const t = svgEl('text', {
        x: cx, y: cy,
        'text-anchor': 'middle',
        'dominant-baseline': 'central',
        'font-size': fontSize,
        'font-weight': 700,
        fill: '#F4F0FA',
      });
      t.textContent = text;
      grp.appendChild(t);
      g.appendChild(grp);
    };
    const absorptive = n.absorptive_assumed || n.absorptive_folded;
    // Boolean-rewrite root: dashed purple ring + "B".
    if (n.boolean_assumed) {
      g.appendChild(svgEl('circle', {
        r: 28, fill: 'none', stroke: '#5A3E8C',
        'stroke-width': 1.4, 'stroke-dasharray': '3 2',
      }));
      addBadge(-16, -18, 7, '#5A3E8C', '#2A1850', 'B', 9,
        'Boolean-rewrite root: interpreted as a Boolean function; '
        + 'only Boolean-compatible semirings are sound here.');
    }
    // Absorptive root / fold: dashed amber ring + "A".
    if (absorptive) {
      g.appendChild(svgEl('circle', {
        r: n.boolean_assumed ? 31 : 28, fill: 'none', stroke: '#b45309',
        'stroke-width': 1.4, 'stroke-dasharray': '3 2',
      }));
      addBadge(16, -18, 7, '#b45309', '#78350f', 'A', 9,
        n.absorptive_assumed
          ? ('Absorptive-truncation root: cyclic recursive query stopped '
             + 'at the absorptive value fixpoint; only absorptive '
             + 'semirings are sound here.')
          : ('Absorptive fold: wires simplified under rules sound in every '
             + 'absorptive semiring; absorptive and Boolean-compatible '
             + 'semirings are sound here.'));
    }
    // d-DNNF certificate: green "D".
    if (n.dnnf_certified) {
      addBadge(16, 18, 7, '#15803d', '#14532d', 'D', 9,
        (n.type === 'plus')
          ? 'Certified deterministic ⊕: children mutually exclusive.'
          : 'Certified decomposable ⊗: children over disjoint variables.');
    }
    // Inversion-free certificate root: dashed teal ring + "IF".
    if (n.if_cert) {
      g.appendChild(svgEl('circle', {
        r: (n.boolean_assumed && absorptive) ? 34
           : (n.boolean_assumed || absorptive) ? 31 : 28,
        fill: 'none', stroke: '#2E7D8A',
        'stroke-width': 1.4, 'stroke-dasharray': '2 2',
      }));
      addBadge(-16, 18, 8, '#2E7D8A', '#1F5660', 'IF', 7,
        'Inversion-free certificate root: compiles to a structured '
        + 'd-DNNF in time linear in the lineage.');
    }
  }

  function paintSceneInto(container, scene) {
    container.innerHTML = '';
    if (!scene || !scene.nodes || !scene.nodes.length) {
      container.textContent = '(empty circuit)';
      return;
    }
    const xs = scene.nodes.map((n) => n.x), ys = scene.nodes.map((n) => n.y);
    const pad = 40;
    const minX = Math.min(...xs) - pad, maxX = Math.max(...xs) + pad;
    const minY = Math.min(...ys) - pad, maxY = Math.max(...ys) + pad;
    // Presentation attributes double the stylesheet below: SVG attributes
    // lose to any CSS rule, so the live look is app.css's unchanged -- but
    // the htmlSnapshot copy of this SVG (the .ipynb image/svg+xml bundle)
    // renders standalone in external viewers (nbviewer, GitHub), which see
    // neither app.css nor colors_and_type.css.  Hex values are the resolved
    // --purple-500 / --purple-700 / --fg-muted.
    const svg = svgEl('svg', {
      class: 'nb-circ__svg',
      // Explicit xmlns: innerHTML serialization omits it, and without it
      // the snapshot is not a parseable standalone SVG document.
      xmlns: 'http://www.w3.org/2000/svg',
      viewBox: `${minX} ${minY} ${maxX - minX} ${maxY - minY}`,
      preserveAspectRatio: 'xMidYMid meet',
      'font-family': 'sans-serif',
    });
    // Cap the on-screen size while keeping the aspect ratio: small
    // scenes render 1:1-ish, large ones shrink to fit the cell.
    svg.style.maxHeight = '360px';
    svg.style.width = '100%';

    const nodesById = {};
    for (const n of scene.nodes) nodesById[n.id] = n;
    // Parallel-edge bow, as in circuit.js, so duplicate wires stay
    // distinguishable.
    const parallelTotal = {};
    for (const e of scene.edges) {
      const k = `${e.from}|${e.to}`;
      parallelTotal[k] = (parallelTotal[k] || 0) + 1;
    }
    const parallelSeen = {};
    for (const e of scene.edges) {
      const from = nodesById[e.from], to = nodesById[e.to];
      if (!from || !to) continue;
      const k = `${e.from}|${e.to}`;
      const total = parallelTotal[k];
      parallelSeen[k] = (parallelSeen[k] || 0) + 1;
      const bow = total > 1 ? ((parallelSeen[k] - 1) - (total - 1) / 2) * 18 : 0;
      svg.appendChild(svgEl('path', {
        class: 'edge',
        fill: 'none',
        stroke: '#5A3E8C',
        'stroke-width': 1.6,
        d: `M ${from.x} ${from.y + 22} `
         + `C ${from.x + bow} ${from.y + 50}, `
         +   `${to.x + bow} ${to.y - 50}, `
         +   `${to.x} ${to.y - 22}`,
      }));
      if (shouldLabelChildren(from) && e.child_pos != null) {
        const dx = from.x - to.x, dy = from.y - to.y;
        const len = Math.hypot(dx, dy) || 1;
        const t = svgEl('text', {
          class: 'edge-pos',
          fill: '#8A7A9B',
          'font-size': 8,
          x: (from.x + to.x) / 2 + bow + (-dy / len) * 9,
          y: (from.y + to.y) / 2 + (dx / len) * 9,
        });
        t.textContent = edgePosLabel(from, e.child_pos, scene);
        svg.appendChild(t);
      }
    }
    for (const n of scene.nodes) {
      const g = svgEl('g', {
        class: `node-group node--${n.type}`
          + (n.frontier ? ' is-frontier' : '')
          + (n.boolean_assumed ? ' is-boolean-assumed' : '')
          + (n.absorptive_assumed || n.absorptive_folded
             ? ' is-absorptive-assumed' : '')
          + (n.dnnf_certified ? ' is-dnnf-certified' : '')
          + (n.if_cert ? ' is-inversion-free' : ''),
        transform: `translate(${n.x},${n.y})`,
      });
      g.appendChild(svgEl('circle', {
        class: 'node-shape', r: 22,
        fill: '#fff', stroke: '#6B4FA0', 'stroke-width': 2,
      }));
      paintNodeBadges(g, n);
      const label = String(n.label == null ? '' : n.label);
      const t = svgEl('text', {
        class: 'node-label',
        fill: '#6B4FA0',
        'font-weight': 600,
        'text-anchor': 'middle',
        'dominant-baseline': 'central',
        'font-size': label.length > 6 ? 9 : (label.length > 3 ? 11 : 14),
      });
      t.textContent = label;
      const tip = svgEl('title');
      tip.textContent = `${n.type}\n${n.id}`;
      g.appendChild(tip);
      g.appendChild(t);
      svg.appendChild(g);
    }
    container.appendChild(svg);
  }

  async function refreshCircuitCell(cell) {
    const el = cellEl(cell);
    const box = el && el.querySelector('.nb-circ__canvas');
    if (!box) return;
    box.textContent = 'loading…';
    try {
      // No depth parameter: like Circuit mode's loadCircuit, the server
      // clamps to its configured max_circuit_depth, so the snapshot and
      // the canvas show the same cut of the DAG.
      const resp = await fetch(
        `/api/circuit/${encodeURIComponent(cell.token)}`);
      const payload = await resp.json();
      if (!resp.ok) throw new Error(payload.error || `HTTP ${resp.status}`);
      cell.scene = payload;
      paintSceneInto(box, cell.scene);
      cell.htmlSnapshot = box.innerHTML;
    } catch (e) {
      box.textContent = `Could not fetch the circuit: ${e.message}`;
    }
    scheduleAutosave();
  }

  // The eval strip's semantics (semiring evaluation, probability of a
  // Boolean event) apply to plain provenance tokens; an agg_token or
  // random_variable root gets no Evaluate affordance -- Circuit mode's
  // dedicated dispatch (distribution profiles, moments, agg inspection)
  // is the investigation surface for those.
  function evalApplies(cell) {
    return cell.tokenKind !== 'agg_token'
        && cell.tokenKind !== 'random_variable';
  }

  function syncCircuitEvalButton(cell) {
    const el = cellEl(cell);
    const btn = el && el.querySelector('.nb-circ__eval');
    if (btn) btn.hidden = !evalApplies(cell);
  }

  function shortToken(t) {
    const s = String(t || '');
    return s.length > 8 ? s.slice(0, 8) + '…' : s;
  }

  // Insert a circuit cell for `token` after `afterCell` -- or, when the
  // next cell is already a circuit cell, retarget it (repeated clicks
  // on result UUIDs swap the snapshot instead of piling cells up).
  function showCircuitFor(token, afterCell, rowProv, tokenKind) {
    const idx = cells.indexOf(afterCell);
    let cell = idx >= 0 && cells[idx + 1] && cells[idx + 1].type === 'circuit'
      ? cells[idx + 1] : null;
    if (cell) {
      cell.token = token;
      cell.rowProv = rowProv || '';
      cell.tokenKind = tokenKind || '';
      syncCircuitEvalButton(cell);
      const el = cellEl(cell);
      const lbl = el && el.querySelector('.nb-circ__token');
      if (lbl) { lbl.textContent = shortToken(token); lbl.title = token; }
    } else {
      cell = newCell('circuit');
      cell.token = token;
      cell.rowProv = rowProv || '';
      cell.tokenKind = tokenKind || '';
      const pos = idx >= 0 ? idx + 1 : cells.length;
      cells.splice(pos, 0, cell);
      const anchorEl = idx >= 0 ? cellEl(afterCell) : null;
      const dom = buildCellDom(cell);
      if (anchorEl) anchorEl.after(dom);
      else cellsEl().appendChild(dom);
    }
    refreshCircuitCell(cell);
  }

  /* ──────── evaluation cells ──────── */
  /* An evaluation cell is one eval-strip invocation as a cell: token +
     evaluation (compiled semiring or probability method) + optional
     arguments / provenance mapping, with the result rendered inline
     and the invocation recorded in metadata.provsql so a saved
     notebook replays it. The narrative form of "and the probability
     of this answer is...". Offered only for plain provenance tokens:
     agg_token / random_variable roots get no Evaluate affordance (the
     strip's semantics don't apply to them; Circuit mode's dedicated
     dispatch -- distribution profiles, moments -- is the place to
     investigate those). */

  const EVAL_SEMIRINGS = [
    ['probability', 'Probability'],
    ['formula', 'Formula'],
    ['boolexpr', 'Boolean expression'],
    ['why', 'Why'],
    ['which', 'Which'],
    ['how', 'How'],
    ['counting', 'Counting'],
  ];

  // Per-semiring soundness flags for the eval-cell dropdown, mirroring
  // circuit.js's _COMPILED_REGISTRY (booleanCompatible / absorptive) and
  // the C++ semiring predicates. 'probability' is intentionally absent:
  // like Circuit mode, it always stays available (it factors through the
  // Boolean function, sound under every assumed class). Kept in sync
  // with circuit.js / test/sql/{safe_query_semiring,absorptive_recursion}.sql.
  const EVAL_SEMIRING_FLAGS = {
    formula:  { booleanCompatible: true,  absorptive: false },
    boolexpr: { booleanCompatible: true,  absorptive: true  },
    why:      { booleanCompatible: false, absorptive: false },
    which:    { booleanCompatible: false, absorptive: false },
    how:      { booleanCompatible: false, absorptive: false },
    counting: { booleanCompatible: false, absorptive: false },
  };

  // The assumed class of a token, read off its circuit scene's root node
  // (the post-elision root carries boolean_assumed / absorptive_assumed /
  // absorptive_folded). Returns null when unknown, in which case the
  // dropdown stays unfiltered -- matching Circuit mode's "scene not
  // loaded -> show everything" fallback.
  function sceneRootFlags(scene) {
    if (!scene || !Array.isArray(scene.nodes)) return null;
    const root = scene.nodes.find((n) => n.id === scene.root);
    if (!root) return null;
    return {
      boolean_assumed: !!root.boolean_assumed,
      absorptive_assumed: !!root.absorptive_assumed,
      absorptive_folded: !!root.absorptive_folded,
    };
  }

  // Hide the eval-strip semirings the C++ evaluator would refuse on a
  // token whose root is assumed Boolean / absorptive, mirroring Circuit
  // mode's syncDropdownVisibility: strictly (absorptive only) for a
  // truncated cyclic-recursion root, leniently (absorptive OR Boolean-
  // compatible) for a fold-marked one, and Boolean-compatible only for a
  // Boolean root. Options absent from the flag table (probability) always
  // stay. Bumps a now-hidden selection to the first visible option.
  function filterEvalSemirings(cell, sel) {
    const a = cell.assumed || {};
    const booleanOnly = !!a.boolean_assumed;
    const absorptiveOnly = !!a.absorptive_assumed;
    const absorptiveFold = !!a.absorptive_folded;
    let firstVisible = null;
    for (const opt of sel.querySelectorAll('option')) {
      const spec = EVAL_SEMIRING_FLAGS[opt.value];
      let hide = false;
      if (spec) {
        if (booleanOnly && spec.booleanCompatible === false) hide = true;
        if (absorptiveOnly && spec.absorptive === false) hide = true;
        if (absorptiveFold && spec.absorptive === false
            && spec.booleanCompatible === false) hide = true;
      }
      opt.hidden = hide;
      opt.disabled = hide;
      if (!hide && !firstVisible) firstVisible = opt.value;
    }
    const cur = sel.querySelector(`option[value="${CSS.escape(sel.value)}"]`);
    if (firstVisible && (!cur || cur.hidden)) {
      sel.value = firstVisible;
      cell.semiring = firstVisible;
      return true;
    }
    return false;
  }

  let mappingsCache = null;   // /api/provenance_mappings payload

  async function ensureMappings() {
    if (mappingsCache) return mappingsCache;
    try {
      const resp = await fetch('/api/provenance_mappings');
      if (resp.ok) mappingsCache = await resp.json();
    } catch (e) { /* mapping picker just stays empty */ }
    if (!mappingsCache) mappingsCache = [];
    return mappingsCache;
  }

  function syncEvalControls(cell, el) {
    const isProb = cell.semiring === 'probability';
    el.querySelector('.nb-eval__method').hidden = !isProb;
    el.querySelector('.nb-eval__mapping').hidden = isProb;
    el.querySelector('.nb-eval__args').hidden = !isProb;
  }

  // '' is the default: the chooser picks the cheapest exact method, so a
  // separate 'exact' entry would be redundant and is left out.
  const EVAL_METHODS = [
    '', 'relative', 'additive', 'independent', 'possible-worlds',
    'sieve', 'd-tree', 'tree-decomposition', 'compilation', 'wmc',
    'monte-carlo', 'karp-luby', 'stopping-rule',
  ];

  // Notice variant of the result renderer, ported from circuit.js's
  // renderEvalNotice: the eval strip floors provsql.verbose_level at 5
  // server-side (see db.evaluate_circuit), so probability / semiring
  // calls surface informational ProvSQL notices (e.g. the gate_cmp
  // probability-side shortcut, approximation guarantees). Circuit mode
  // shows them; the notebook eval cell now does too. Goldenrod
  // cv-kc-notice background + warning icon, with the source badge
  // stripped off the "ProvSQL: " prefix.
  function renderEvalNotice(messages) {
    const esc = env.escapeHtml;
    const list = Array.isArray(messages) ? messages : [messages];
    const formatted = list.map((raw) => {
      const m = (raw || '').match(/^ProvSQL:\s*(.*)$/s);
      const badge = m ? '<span class="wp-srcbadge">ProvSQL</span> ' : '';
      const text = m ? m[1] : (raw || '');
      return `${badge}${esc(text)}`;
    }).join('<br>');
    return `<div class="cv-kc-notice">`
      + `<i class="fas fa-exclamation-triangle"></i> ${formatted}</div>`;
  }

  function renderEvalResult(cell) {
    const el = cellEl(cell);
    const box = el && el.querySelector('.nb-eval__out');
    if (!box) return;
    if (!cell.result) {
      box.hidden = true;
      box.innerHTML = '';
      return;
    }
    box.hidden = false;
    const esc = env.escapeHtml;
    const r = cell.result;
    if (r.error) {
      box.innerHTML = `<div class="wp-error"><i class="fas fa-exclamation-circle"></i> `
        + `${esc(r.error)}${r.detail ? ' – ' + esc(r.detail) : ''}</div>`;
      return;
    }
    let notices = Array.isArray(r.notices) ? r.notices : [];
    // An approximate probability carries a structured guarantee NOTICE;
    // pull it out of the banner and render it as the value interval the
    // true probability lies in -- exactly as Circuit mode does.
    let boundHtml = '';
    if ((cell.semiring || 'probability') === 'probability') {
      const guar = window.ProvsqlStudio.parseGuaranteeNotice(notices);
      if (guar) {
        const est = typeof r.result === 'number' ? r.result : NaN;
        const g = window.ProvsqlStudio.renderGuarantee(guar, est, r.resolved_method || '');
        if (g) boundHtml = ` <span class="nb-eval__meta" title="Approximation guarantee">${esc(g)}</span>`;
        notices = notices.filter((m) => !/approximation-guarantee:/.test(m || ''));
      }
    }
    const noticeHtml = notices.length ? renderEvalNotice(notices) : '';
    const value = (r.result == null) ? '(null)'
      : (typeof r.result === 'object' ? JSON.stringify(r.result, null, 1)
                                      : String(r.result));
    const multiline = value.includes('\n') || value.length > 100;
    box.innerHTML =
      noticeHtml
      + (multiline
        ? `<pre class="nb-eval__value">${esc(value)}</pre>`
        : `<span class="nb-eval__value">${esc(value)}</span>`)
      + (r.resolved_method
          ? ` <span class="nb-eval__meta" title="Method the chooser actually used">via ${esc(r.resolved_method)}</span>`
          : '')
      + boundHtml
      + (cell.elapsed != null
          ? ` <span class="nb-eval__meta">· ${esc(String(cell.elapsed))} ms</span>`
          : '');
    cell.htmlSnapshot = box.innerHTML;
  }

  async function runEvalCell(cell) {
    const el = cellEl(cell);
    const box = el && el.querySelector('.nb-eval__out');
    if (box) { box.hidden = false; box.textContent = 'evaluating…'; }
    const body = {
      token: cell.token,
      semiring: cell.semiring || 'probability',
    };
    if (cell.semiring === 'probability') {
      if (cell.method) body.method = cell.method;
      if (cell.args) body.arguments = cell.args;
    } else if (cell.mapping) {
      body.mapping = cell.mapping;
    }
    const t0 = performance.now();
    let payload;
    try {
      const resp = await fetch('/api/evaluate', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      payload = await resp.json().catch(() => ({}));
      if (!resp.ok && !payload.error) payload.error = `HTTP ${resp.status}`;
    } catch (e) {
      payload = { error: `Network error: ${e.message}` };
    }
    cell.elapsed = Math.round(performance.now() - t0);
    cell.result = payload;
    cell.count = ++execCounter;
    updateGutter(cell);
    renderEvalResult(cell);
    scheduleAutosave();
  }

  // Insert an evaluation cell for `token` after `afterCell` (a circuit
  // cell's "Evaluate" button).
  function insertEvalAfter(afterCell, token, assumed) {
    const idx = cells.indexOf(afterCell);
    const cell = newCell('eval');
    cell.token = token;
    cell.semiring = 'probability';
    cell.method = 'exact';
    cell.args = '';
    cell.mapping = '';
    cell.assumed = assumed || null;
    const pos = idx >= 0 ? idx + 1 : cells.length;
    cells.splice(pos, 0, cell);
    const anchorEl = idx >= 0 ? cellEl(afterCell) : null;
    const dom = buildCellDom(cell);
    if (anchorEl) anchorEl.after(dom);
    else cellsEl().appendChild(dom);
    selectCell(cell);
    scheduleAutosave();
    return cell;
  }

  /* ──────── sidebar: outline + compact relations ──────── */
  /* The where-mode sidebar shows full table contents; next to a cell
     list that is wasted space. The notebook sidebar is deliberately
     short: an outline of the Markdown headings (click scrolls to the
     cell) and a one-line-per-relation schema summary (click inserts
     the relation name at the cursor; full contents stay one
     Schema-panel click away). */

  function outlineEntries() {
    const entries = [];
    for (const c of cells) {
      if (c.type !== 'markdown') continue;
      let inFence = false;
      for (const line of String(c.source || '').split('\n')) {
        if (/^\s*(```|~~~)/.test(line)) { inFence = !inFence; continue; }
        if (inFence) continue;
        const m = line.match(/^(#{1,6})\s+(.+?)\s*#*\s*$/);
        if (m) entries.push({ cellId: c.id, level: m[1].length, text: m[2] });
      }
    }
    return entries;
  }

  function renderSidebar() {
    const root = document.getElementById('sidebar-body');
    if (!root) return;
    const esc = env.escapeHtml, escA = env.escapeAttr;

    const entries = outlineEntries();
    const outlineHtml = entries.length
      ? `<ul class="nb-side__outline">` + entries.map((e) =>
          `<li class="nb-side__h nb-side__h--${e.level}"`
          + ` data-outline-cell="${escA(e.cellId)}"`
          + ` data-outline-level="${e.level}"`
          + ` title="${escA(e.text)}">${esc(e.text)}</li>`
        ).join('') + `</ul>`
      : `<p class="nb-side__empty">Headings in Markdown cells appear here.</p>`;

    let relsHtml = `<p class="nb-side__empty">loading…</p>`;
    if (schemaCache) {
      const rels = schemaCache;
      relsHtml = rels.length
        ? `<ul class="nb-side__rels">` + rels.map((r) => {
            const qname = r.bare_resolves ? r.table : `${r.schema}.${r.table}`;
            const cols = (r.columns || []).map((c) => c.name).join(', ');
            const colsTitle = (r.columns || [])
              .map((c) => `${c.name} ${c.type}`).join('\n');
            // Same PROV-TID / PROV-BID / PROV-OPAQUE / mapping pills the
            // schema panel renders (shared helper), so a relation is
            // classified identically in both places.
            const pill = window.ProvsqlStudio.relProvBadges(r, escA);
            return `<li class="nb-side__rel">`
              + `<button type="button" class="nb-side__relname"`
              + ` data-rel="${escA(qname)}"`
              + ` title="Insert ${escA(qname)} into the current cell">${esc(qname)}</button>`
              + `${pill}`
              + `<div class="nb-side__cols" title="${escA(colsTitle)}">${esc(cols)}</div>`
              + `</li>`;
          }).join('') + `</ul>`
        : `<p class="nb-side__empty">No relations.</p>`;
    }

    root.innerHTML = `
      <div class="nb-side">
        <div class="nb-side__section">
          <h3 class="nb-side__hdr">Outline</h3>
          ${outlineHtml}
        </div>
        <div class="nb-side__section">
          <h3 class="nb-side__hdr">Relations</h3>
          ${relsHtml}
        </div>
      </div>`;
  }

  async function refreshSidebarSchema() {
    try {
      const resp = await fetch('/api/schema');
      if (resp.ok) schemaCache = await resp.json();
    } catch (e) { /* keep the stale list */ }
    renderSidebar();
  }

  // The SQL cell a panel-driven insert targets: the cell whose editor
  // has (or last had) focus, or null when no "querybox is selected" --
  // in which case the inserters below append a fresh cell instead of
  // clobbering an arbitrary one.
  function currentSqlCell() {
    const el = document.activeElement && document.activeElement.closest
      ? document.activeElement.closest('.nb-cell')
      : null;
    if (el) {
      const c = cells.find((x) => x.id === el.dataset.cellId);
      if (c && c.type === 'sql') return c;
    }
    if (lastFocused && cells.includes(lastFocused)
        && lastFocused.type === 'sql') return lastFocused;
    return null;
  }

  function insertSql(text) {
    const cell = currentSqlCell() || appendCell('sql');
    const el = cellEl(cell);
    const ta = el && el.querySelector('.nb-cell__ta');
    if (!ta) return;
    const s = ta.selectionStart != null ? ta.selectionStart : ta.value.length;
    const e = ta.selectionEnd != null ? ta.selectionEnd : s;
    ta.value = ta.value.slice(0, s) + text + ta.value.slice(e);
    ta.setSelectionRange(s + text.length, s + text.length);
    ta.focus();
    ta.dispatchEvent(new Event('input', { bubbles: true }));
  }

  // Whole-statement form (the schema panel's SELECT * / add_provenance
  // / create_provenance_mapping prefills): replace the current cell's
  // content, or land in a fresh cell when none is selected.
  function replaceSql(text) {
    const cell = currentSqlCell() || appendCell('sql');
    const el = cellEl(cell);
    const ta = el && el.querySelector('.nb-cell__ta');
    if (!ta) return;
    ta.value = text;
    ta.setSelectionRange(text.length, text.length);
    ta.focus();
    ta.dispatchEvent(new Event('input', { bubbles: true }));
  }

  function wireSidebar() {
    const root = document.getElementById('sidebar-body');
    if (!root) return;
    root.addEventListener('click', (e) => {
      const h = e.target.closest('[data-outline-cell]');
      if (h) {
        const el = cellsEl().querySelector(
          `[data-cell-id="${CSS.escape(h.dataset.outlineCell)}"]`);
        if (el) {
          // Scroll to the specific heading, not the cell top: a markdown
          // cell often holds the previous section's closing prose above
          // this heading, so block:'start' on the cell would leave the
          // heading partway down.  Match the rendered <hN> by level+text;
          // the heading (and the cell) carry a scroll-margin-top that
          // clears the sticky toolbar.
          const md = el.querySelector('.nb-cell__md');
          const wantTag = 'H' + (h.dataset.outlineLevel || '');
          const wantTxt = h.textContent.trim();
          let target = null;
          if (md) {
            const hs = md.querySelectorAll('h1,h2,h3,h4,h5,h6');
            for (const hh of hs)
              if (hh.tagName === wantTag && hh.textContent.trim() === wantTxt) {
                target = hh; break;
              }
            if (!target)
              for (const hh of hs)
                if (hh.textContent.trim() === wantTxt) { target = hh; break; }
          }
          (target || el).scrollIntoView({ behavior: 'smooth', block: 'start' });
          el.classList.add('nb-cell--flash');
          setTimeout(() => el.classList.remove('nb-cell--flash'), 1200);
        }
        return;
      }
      const rel = e.target.closest('[data-rel]');
      if (rel) insertSql(rel.dataset.rel);
    });
  }

  /* ──────── selection + command mode (Jupyter keymap) ──────── */
  /* Edit mode is "an editor has focus"; Esc drops to command mode with
     the cell selected (highlighted border). Command-mode keys follow
     Jupyter: a/b insert above/below, dd deletes, z undoes the delete,
     m/y convert to Markdown/SQL, j/k or arrows move the selection,
     Enter re-enters edit mode, Shift+Enter runs and advances,
     Ctrl+Enter runs in place. Alt+Enter (both modes) runs and inserts
     a fresh cell below. */

  function selectCell(cell, opts) {
    selected = cell;
    for (const el of cellsEl().querySelectorAll('.nb-cell--selected')) {
      el.classList.remove('nb-cell--selected');
    }
    const el = cell && cellEl(cell);
    if (el) {
      el.classList.add('nb-cell--selected');
      if (!opts || opts.scroll !== false) {
        el.scrollIntoView({ block: 'nearest' });
      }
    }
  }

  function editCell(cell) {
    const el = cellEl(cell);
    if (!el) return;
    if (cell.type === 'sql') {
      el.querySelector('.nb-cell__ta')?.focus();
    } else if (cell.type === 'markdown') {
      const ta = el.querySelector('.nb-cell__mdta');
      if (ta && ta.hidden) el.querySelector('.nb-cell__md')?.dispatchEvent(new Event('dblclick'));
      else ta?.focus();
    }
  }

  function insertCellAt(idx, type) {
    const c = newCell(type);
    cells.splice(idx, 0, c);
    const dom = buildCellDom(c);
    const next = cells[idx + 1];
    const nextEl = next && cellEl(next);
    if (nextEl) cellsEl().insertBefore(dom, nextEl);
    else cellsEl().appendChild(dom);
    scheduleAutosave();
    return c;
  }

  function deleteCell(cell) {
    const idx = cells.indexOf(cell);
    if (idx < 0) return;
    lastDeleted = { cell, index: idx };
    cells.splice(idx, 1);
    cellEl(cell)?.remove();
    if (!cells.length) appendCell('sql');
    selectCell(cells[Math.min(idx, cells.length - 1)]);
    scheduleAutosave();
  }

  function undoDelete() {
    if (!lastDeleted) return;
    const { cell, index } = lastDeleted;
    lastDeleted = null;
    const idx = Math.min(index, cells.length);
    cells.splice(idx, 0, cell);
    const next = cells[idx + 1];
    const nextEl = next && cellEl(next);
    const dom = buildCellDom(cell);
    if (nextEl) cellsEl().insertBefore(dom, nextEl);
    else cellsEl().appendChild(dom);
    updateGutter(cell);
    renderOutputs(cell);
    selectCell(cell);
    scheduleAutosave();
  }

  // m / y conversion: keep the source, drop type-specific baggage
  // (outputs / counters when leaving sql, rendered view when leaving
  // markdown). Circuit cells don't convert. SQL going to Markdown is
  // wrapped in a ```sql fence so it renders as a code block; Markdown
  // coming back to SQL drops the fence again when the whole cell is a
  // single fenced block, so m + y round-trips cleanly.
  function convertCell(cell, type) {
    if (cell.type === type || cell.type === 'circuit'
        || cell.type === 'eval') return;
    if (type === 'markdown' && (cell.source || '').trim()) {
      cell.source = '```sql\n'
        + String(cell.source).replace(/\n+$/, '') + '\n```';
    } else if (type === 'sql') {
      const m = String(cell.source || '').trim()
        .match(/^(```|~~~)[^\n]*\n([\s\S]*?)\n?\1$/);
      if (m) cell.source = m[2];
    }
    const el = cellEl(cell);
    cell.type = type;
    cell.outputs = null;
    cell.count = null;
    cell.htmlSnapshot = null;
    cell._noAutoEdit = true;
    const dom = buildCellDom(cell);
    if (el) el.replaceWith(dom);
    updateGutter(cell);
    selectCell(cell);
    scheduleAutosave();
  }

  function selectSibling(delta) {
    if (!cells.length) return;
    const idx = selected ? cells.indexOf(selected) : -1;
    const next = idx < 0 ? 0 : Math.max(0, Math.min(cells.length - 1, idx + delta));
    selectCell(cells[next]);
  }

  function inEditMode() {
    const ae = document.activeElement;
    return !!(ae && (ae.tagName === 'TEXTAREA' || ae.tagName === 'INPUT'
                     || ae.tagName === 'SELECT' || ae.isContentEditable));
  }

  function commandKeydown(e) {
    if (inEditMode()) return;            // edit-mode keys live on the editors
    if (!selected || !cells.includes(selected)) {
      // Jupyter's invariant: the selection is never empty -- there is
      // always exactly one selected cell for command-mode keys to act
      // on. Re-establish it (visibly) if some path dropped it.
      if (!cells.length) return;
      selectCell(cells[0], { scroll: false });
    }
    const idx = cells.indexOf(selected);
    const plain = !e.ctrlKey && !e.metaKey && !e.altKey && !e.shiftKey;
    if (plain && e.key === 'a') {
      selectCell(insertCellAt(idx, 'sql'));
    } else if (plain && e.key === 'b') {
      selectCell(insertCellAt(idx + 1, 'sql'));
    } else if (plain && e.key === 'd') {
      const now = Date.now();
      if (now - lastDKey < 600) { lastDKey = 0; deleteCell(selected); }
      else lastDKey = now;
      return;                            // don't reset the dd timer below
    } else if (plain && e.key === 'z') {
      undoDelete();
    } else if (plain && e.key === 'm') {
      convertCell(selected, 'markdown');
    } else if (plain && e.key === 'y') {
      convertCell(selected, 'sql');
    } else if (plain && (e.key === 'j' || e.key === 'ArrowDown')) {
      e.preventDefault();
      selectSibling(+1);
    } else if (plain && (e.key === 'k' || e.key === 'ArrowUp')) {
      e.preventDefault();
      selectSibling(-1);
    } else if (plain && e.key === 'Enter') {
      e.preventDefault();
      editCell(selected);
    } else if (e.key === 'Enter' && e.shiftKey && !e.ctrlKey && !e.altKey) {
      e.preventDefault();
      runSelectedThenAdvance(selected);
    } else if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      if (selected.type === 'sql') runCell(selected);
      else if (selected.type === 'circuit') refreshCircuitCell(selected);
      else if (selected.type === 'eval') runEvalCell(selected);
    } else if (e.key === 'Enter' && e.altKey) {
      e.preventDefault();
      runAltEnter(selected);
    } else {
      lastDKey = 0;
      return;
    }
    lastDKey = 0;
  }

  // Shift+Enter, Jupyter semantics: run (or render), then select the
  // next cell in command mode (never opening its editor). The one
  // exception, also Jupyter's: at the last cell a fresh SQL cell is
  // created below and opened in edit mode.
  async function runSelectedThenAdvance(cell) {
    if (cell.type === 'sql') await runCell(cell);
    else if (cell.type === 'circuit' && cell.token) await refreshCircuitCell(cell);
    else if (cell.type === 'eval' && cell.token) await runEvalCell(cell);
    const idx = cells.indexOf(cell);
    const atEnd = idx === cells.length - 1;
    if (atEnd) appendCell('sql');
    selectCell(cells[idx + 1]);
    if (atEnd) editCell(cells[idx + 1]);
  }

  // Alt+Enter: run, then insert a fresh SQL cell below and edit it.
  async function runAltEnter(cell) {
    if (cell.type === 'sql') await runCell(cell);
    const c = insertCellAt(cells.indexOf(cell) + 1, 'sql');
    selectCell(c);
    editCell(c);
  }

  /* ──────── execution ──────── */

  function setRunningUi(on) {
    byId('nb-run').disabled = on;
    byId('nb-run-all').disabled = on;
    byId('nb-restart').disabled = on;
    byId('nb-interrupt').hidden = !on;
  }

  async function runCell(cell) {
    if (cell.type !== 'sql' || running) return;
    let k;
    try {
      k = await ensureKernel();
    } catch (e) {
      // Surface the failure in the cell too -- the kernel chip alone is
      // easy to miss when the eye is on the cell that "did nothing".
      cell.outputs = { blocks: [{ kind: 'error',
        message: `Could not start a kernel: ${e.message}` }], notices: [] };
      renderOutputs(cell);
      return;
    }
    const requestId = (window.crypto && crypto.randomUUID)
      ? crypto.randomUUID()
      : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
    running = { cellId: cell.id, requestId };
    updateGutter(cell);
    setRunningUi(true);
    const t0 = performance.now();
    let payload = null;
    try {
      const resp = await fetch('/api/nb/exec', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          session_id: k.sessionId,
          sql: cell.source,
          // Per-cell override first (e.g. cs7's boolean-provenance
          // narrative); the toolbar selector is the notebook default.
          prov_scheme: cell.scheme || byId('nb-scheme').value,
          request_id: requestId,
        }),
      });
      payload = await resp.json().catch(() => null);
      if (!payload) payload = { blocks: [], notices: [] };
      if (!resp.ok && payload.error) {
        // Session-level failure (expired / busy / dead kernel): shape it
        // like an error block so the cell renders it in place.
        payload.blocks = [{ kind: 'error', message: payload.error }];
      }
    } catch (e) {
      payload = { blocks: [{ kind: 'error', message: `Network error: ${e.message}` }],
                  notices: [] };
    } finally {
      running = null;
      setRunningUi(false);
    }
    payload.elapsed_ms = Math.round(performance.now() - t0);
    cell.outputs = payload;
    cell.count = ++execCounter;
    if (payload.kernel_dead) {
      kernelGone('kernel died – restart it');
      cell.count = null;
    }
    updateGutter(cell);
    renderOutputs(cell);
    scheduleAutosave();
    // DDL in a cell (CREATE TABLE, add_provenance, ...) must reflect in
    // the sidebar and the metadata panels, same as a where-mode exec.
    refreshSidebarSchema();
    if (window.ProvsqlStudio.metadata && window.ProvsqlStudio.metadata.invalidateAll) {
      window.ProvsqlStudio.metadata.invalidateAll();
    }
  }

  async function runAll() {
    for (const cell of cells) {
      if (cell.type === 'circuit') {
        if (cell.token) await refreshCircuitCell(cell);
        continue;
      }
      if (cell.type === 'eval') {
        if (cell.token) await runEvalCell(cell);
        continue;
      }
      if (cell.type !== 'sql' || !(cell.source || '').trim()) continue;
      await runCell(cell);
      // A dead kernel aborts the run: later cells would all fail the
      // same way and bury the actual diagnostic.
      if (!kernel) break;
    }
  }

  async function interrupt() {
    if (!running) return;
    try {
      await fetch(`/api/cancel/${encodeURIComponent(running.requestId)}`,
                  { method: 'POST' });
    } catch (e) { /* the in-flight exec returns either way */ }
  }

  /* ──────── nbformat (de)serialization ──────── */

  function toLines(text) {
    // nbformat convention: array of lines, each keeping its newline.
    const parts = String(text || '').split('\n');
    return parts.map((l, i) => (i < parts.length - 1 ? l + '\n' : l))
                .filter((l, i, a) => !(i === a.length - 1 && l === ''));
  }

  function joinSource(source) {
    return Array.isArray(source) ? source.join('') : String(source || '');
  }

  function toIpynb() {
    return {
      nbformat: 4,
      nbformat_minor: 5,
      metadata: {
        kernelspec: { name: 'provsql-studio', display_name: 'ProvSQL (SQL)',
                      language: 'sql' },
        language_info: { name: 'sql' },
        provsql: {
          scheme: byId('nb-scheme').value,
          // The database binding: which environment this notebook was
          // authored against (no credentials, just the name).
          database: (currentTab() && currentTab().db) || connDb || undefined,
        },
      },
      cells: cells.map((c) => {
        if (c.type === 'markdown') {
          return { cell_type: 'markdown', metadata: {}, source: toLines(c.source) };
        }
        if (c.type === 'eval') {
          const outputs = [];
          if (c.result) {
            const data = { 'application/vnd.provsql.eval+json': c.result };
            if (!c.result.error && c.result.result != null) {
              data['text/plain'] = toLines(
                typeof c.result.result === 'object'
                  ? JSON.stringify(c.result.result)
                  : String(c.result.result));
            }
            outputs.push({ output_type: 'execute_result',
                           execution_count: c.count, metadata: {}, data });
          }
          return {
            cell_type: 'code', execution_count: c.count,
            metadata: { provsql: {
              cell: 'eval', token: c.token,
              semiring: c.semiring || 'probability',
              method: c.method || undefined, arguments: c.args || undefined,
              mapping: c.mapping || undefined,
              // The token's assumed class, so a reloaded notebook keeps
              // disabling the semirings the evaluator would refuse.
              assumed: c.assumed || undefined,
            } },
            source: toLines(
              `-- evaluate ${c.semiring || 'probability'}`
              + (c.method ? `/${c.method}` : '')
              + ` on ${c.token}`),
            outputs,
          };
        }
        if (c.type === 'circuit') {
          // A circuit cell is a code cell whose payload lives in
          // metadata.provsql; the SQL-comment source keeps external
          // viewers (and a hypothetical SQL kernel) from choking.
          const outputs = [];
          if (c.scene) {
            const data = { 'application/vnd.provsql.scene+json': c.scene };
            if (c.htmlSnapshot) data['image/svg+xml'] = toLines(c.htmlSnapshot);
            outputs.push({ output_type: 'execute_result',
                           execution_count: null, metadata: {}, data });
          }
          return {
            cell_type: 'code', execution_count: null,
            metadata: { provsql: { cell: 'circuit', token: c.token,
                                   row_prov: c.rowProv || undefined,
                                   token_kind: c.tokenKind || undefined } },
            source: toLines(`-- circuit ${c.token}`),
            outputs,
          };
        }
        const outputs = [];
        if (c.outputs) {
          const data = {
            'application/vnd.provsql.blocks+json': c.outputs,
          };
          if (c.htmlSnapshot) data['text/html'] = toLines(c.htmlSnapshot);
          outputs.push({ output_type: 'execute_result',
                         execution_count: c.count, metadata: {}, data });
        }
        return { cell_type: 'code', execution_count: c.count,
                 metadata: { provsql: c.scheme ? { scheme: c.scheme } : {} },
                 source: toLines(c.source), outputs };
      }),
    };
  }

  function fromIpynb(doc) {
    if (!doc || doc.nbformat !== 4 || !Array.isArray(doc.cells)) {
      throw new Error('not an nbformat-4 notebook');
    }
    const loaded = [];
    for (const c of doc.cells) {
      if (c.cell_type === 'markdown') {
        loaded.push(newCell('markdown', joinSource(c.source)));
      } else if (c.cell_type === 'code'
                 && c.metadata && c.metadata.provsql
                 && c.metadata.provsql.cell === 'eval') {
        const cell = newCell('eval');
        const p = c.metadata.provsql;
        cell.token = String(p.token || '');
        cell.semiring = String(p.semiring || 'probability');
        cell.method = String(p.method || '');
        cell.args = String(p.arguments || '');
        cell.mapping = String(p.mapping || '');
        cell.assumed = (p.assumed && typeof p.assumed === 'object')
          ? p.assumed : null;
        cell.count = (typeof c.execution_count === 'number') ? c.execution_count : null;
        for (const o of (c.outputs || [])) {
          const r = o && o.data && o.data['application/vnd.provsql.eval+json'];
          if (r) { cell.result = r; break; }
        }
        loaded.push(cell);
      } else if (c.cell_type === 'code'
                 && c.metadata && c.metadata.provsql
                 && c.metadata.provsql.cell === 'circuit') {
        const cell = newCell('circuit');
        cell.token = String(c.metadata.provsql.token || '');
        cell.rowProv = String(c.metadata.provsql.row_prov || '');
        cell.tokenKind = String(c.metadata.provsql.token_kind || '');
        for (const o of (c.outputs || [])) {
          // Re-paint only from the scene JSON (through our own
          // painter); the svg snapshot is for external viewers.
          const s = o && o.data && o.data['application/vnd.provsql.scene+json'];
          if (s) { cell.scene = s; break; }
        }
        loaded.push(cell);
      } else if (c.cell_type === 'code') {
        const cell = newCell('sql', joinSource(c.source));
        const pmeta = (c.metadata && c.metadata.provsql) || {};
        if (['semiring', 'absorptive', 'where', 'boolean'].includes(pmeta.scheme)) {
          cell.scheme = pmeta.scheme;
        }
        cell.count = (typeof c.execution_count === 'number') ? c.execution_count : null;
        for (const o of (c.outputs || [])) {
          // Studio re-renders ONLY from its own JSON payload; the
          // text/html bundle is for external viewers and is never
          // injected back (a tampered file must not stored-XSS us).
          const p = o && o.data && o.data['application/vnd.provsql.blocks+json'];
          if (p) { cell.outputs = p; break; }
        }
        loaded.push(cell);
      }
      // raw cells: dropped (Studio has no use for them)
    }
    const scheme = doc.metadata && doc.metadata.provsql && doc.metadata.provsql.scheme;
    return { loaded, scheme };
  }

  function renderAll() {
    const root = cellsEl();
    root.innerHTML = '';
    mdRenders = [];  // collect this paint's async markdown renders (for scroll restore)
    for (const c of cells) root.appendChild(buildCellDom(c));
    for (const c of cells) { updateGutter(c); renderOutputs(c); }
  }

  // Set the notebook-level scheme AND the cross-mode session channel
  // (ps.opt.provScheme) that Circuit mode's radio selector restores
  // from, so a Notebook->Circuit switch keeps the active notebook's
  // scheme. (Per-notebook metadata still wins when a tab/doc carries
  // one; the channel reflects whatever is currently active.)
  function setScheme(value) {
    if (!['semiring', 'absorptive', 'where', 'boolean'].includes(value)) return;
    byId('nb-scheme').value = value;
    try { sessionStorage.setItem('ps.opt.provScheme', value); } catch (e) {}
  }

  function loadNotebook(doc) {
    const { loaded, scheme } = fromIpynb(doc);
    cells = loaded.length ? loaded : [newCell('sql')];
    execCounter = cells.reduce((m, c) => Math.max(m, c.count || 0), 0);
    if (scheme) setScheme(scheme);
    renderAll();
  }

  /* ──────── persistence: download / file-load / autosave ──────── */

  async function download() {
    const tab = currentTab();
    const suggested = (((tab && tabDisplayName(tab)) || 'notebook')
      .replace(/[^\w.-]+/g, '_')) + '.ipynb';
    const json = JSON.stringify(toIpynb(), null, 1);
    // Real save dialog (destination + filename) where the File System
    // Access API exists (Chromium, secure context -- the Playground's
    // main target). Elsewhere (Firefox, plain-http remote hosts) there
    // is no scriptable picker: fall back to a classic download under
    // the suggested name and let the browser's own "ask where to save"
    // setting decide whether a dialog appears -- no prompt boxes.
    if (window.showSaveFilePicker) {
      try {
        const handle = await window.showSaveFilePicker({
          suggestedName: suggested,
          types: [{ description: 'Jupyter notebook',
                    accept: { 'application/x-ipynb+json': ['.ipynb'] } }],
        });
        const w = await handle.createWritable();
        await w.write(json);
        await w.close();
        return;
      } catch (e) {
        if (e && e.name === 'AbortError') return;  // user cancelled
        // any other failure: fall through to the download path
      }
    }
    const blob = new Blob([json], { type: 'application/x-ipynb+json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = suggested;
    a.click();
    URL.revokeObjectURL(a.href);
  }

  let autosaveTimer = null;
  function flushAutosave() {
    clearTimeout(autosaveTimer);
    persistTabs();
  }
  function scheduleAutosave() {
    clearTimeout(autosaveTimer);
    autosaveTimer = setTimeout(() => {
      // Every model change funnels through here, so the debounced tick
      // doubles as the sidebar/outline + tab-name refresh trigger.
      renderSidebar();
      renderTabBar();
      persistTabs();
    }, 500);
  }

  // Restore the persisted tab set. Returns true when something was
  // restored; the single-notebook 'ps.nb.autosave' blob from before the
  // tab era migrates into a lone tab.
  function restoreTabs() {
    let saved = null;
    try {
      saved = JSON.parse(localStorage.getItem('ps.nb.tabs') || 'null');
    } catch (e) { saved = null; }
    if (!saved || !Array.isArray(saved.tabs) || !saved.tabs.length) {
      let legacy = null;
      try {
        legacy = JSON.parse(localStorage.getItem('ps.nb.autosave') || 'null');
      } catch (e) { legacy = null; }
      if (!legacy) return false;
      try { localStorage.removeItem('ps.nb.autosave'); } catch (e) {}
      const db = (legacy.metadata && legacy.metadata.provsql
                  && legacy.metadata.provsql.database) || connDb;
      tabs = [makeTab('Untitled', db, legacy)];
      activeTabId = tabs[0].id;
      return true;
    }
    tabs = saved.tabs.map((t) => ({
      id: t.id, name: t.name || 'Untitled', db: t.db || null,
      doc: t.doc || null, kernel: null, resume: t.resume || null,
    }));
    nextTabId = tabs.reduce(
      (m, t) => Math.max(m, parseInt(String(t.id).slice(1), 10) || 0), 0) + 1;
    activeTabId = tabs.some((t) => t.id === saved.active)
      ? saved.active : tabs[0].id;
    return true;
  }

  /* ──────── bundled example notebooks ──────── */
  /* The tutorial / case-study notebooks generated from the user guide
     (studio/scripts/rst2nb.py), served by name from
     /api/nb/examples/<name>. Opening one is exactly a file load: a new
     tab bound per the notebook's metadata (the binding banner then
     offers to create/switch to its database). */

  async function populateExamples() {
    const sel = byId('nb-example');
    if (!sel) return;
    let examples = [];
    try {
      const resp = await fetch('/api/nb/examples');
      if (resp.ok) examples = await resp.json();
    } catch (e) { /* treated as no examples below */ }
    if (!examples.length) {
      // Older server (endpoint missing) or a build without the bundled
      // notebooks: hide the menu rather than leaving a dead control.
      sel.hidden = true;
      return;
    }
    sel.hidden = false;
    for (const ex of examples) {
      const opt = document.createElement('option');
      opt.value = ex.name;
      opt.textContent = ex.title || ex.name;
      sel.appendChild(opt);
    }
  }

  async function openExample(name) {
    try {
      const resp = await fetch(`/api/nb/examples/${encodeURIComponent(name)}`);
      const doc = await resp.json();
      if (!resp.ok) throw new Error(doc.error || `HTTP ${resp.status}`);
      fromIpynb(doc);  // validate before opening a tab
      const db = (doc.metadata && doc.metadata.provsql
                  && doc.metadata.provsql.database) || connDb;
      newTab(name, db, doc);
    } catch (e) {
      window.alert(`Could not open example ${name}: ${e.message}`);
    }
  }

  /* ──────── boot ──────── */

  function defaultNotebook() {
    // A single empty SQL cell: no boilerplate Markdown, so fresh tabs
    // stay "Untitled" until the user gives the notebook an H1 heading.
    cells = [newCell('sql')];
    renderAll();
  }

  async function init(studioEnv) {
    env = studioEnv;

    // Result-table UUID / agg_token / random_variable cells carry
    // data-circuit-uuid (makeBlockRenderer marks them in notebook
    // mode); clicking one materialises the circuit snapshot right
    // below the SQL cell that produced it.
    document.addEventListener('keydown', commandKeydown);
    cellsEl().addEventListener('mousedown', (e) => {
      const host = e.target.closest('.nb-cell');
      const cell = host && cells.find((c) => c.id === host.dataset.cellId);
      if (cell) selectCell(cell, { scroll: false });
    });
    cellsEl().addEventListener('click', (e) => {
      const td = e.target.closest('[data-circuit-uuid]');
      if (!td) return;
      const host = td.closest('.nb-cell');
      const cell = host && cells.find((c) => c.id === host.dataset.cellId);
      if (cell) {
        showCircuitFor(td.dataset.circuitUuid, cell,
                       td.dataset.rowProv || '',
                       td.dataset.tokenKind || '');
      }
    });

    byId('nb-run').addEventListener('click', () => {
      const c = focusedCell();
      if (c) runCell(c);
    });
    byId('nb-run-all').addEventListener('click', runAll);
    byId('nb-interrupt').addEventListener('click', interrupt);
    byId('nb-restart').addEventListener('click', () => {
      if (window.confirm('Restart the kernel? Temp tables and session '
                         + 'settings will be lost.')) restartKernel();
    });
    byId('nb-add-sql').addEventListener('click', () => { focusCell(appendCell('sql')); scheduleAutosave(); });
    byId('nb-add-md').addEventListener('click', () => { focusCell(appendCell('markdown')); scheduleAutosave(); });
    byId('nb-scheme').addEventListener('change', () => {
      setScheme(byId('nb-scheme').value);
      scheduleAutosave();
    });
    byId('nb-save').addEventListener('click', download);
    byId('nb-load').addEventListener('click', () => byId('nb-load-input').click());
    byId('nb-example').addEventListener('change', () => {
      const name = byId('nb-example').value;
      byId('nb-example').value = '';
      if (name) openExample(name);
    });
    byId('nb-load-input').addEventListener('change', async () => {
      const input = byId('nb-load-input');
      const file = input.files && input.files[0];
      input.value = '';
      if (!file) return;
      // A .sql file (a dump, a fixture script) lands in the CURRENT
      // notebook as one appended SQL cell, ready to run -- the cell
      // splitter handles multi-statement content, COPY blocks included.
      // Cleaned like a paste (invisible Unicode, CRLF); see app.js.
      if (/\.sql$/i.test(file.name)) {
        try {
          const text = await file.text();
          const clean = window.ProvsqlStudio.sanitizeSqlText
            ? window.ProvsqlStudio.sanitizeSqlText(text.replace(/\r\n/g, '\n'))
            : text.replace(/\r\n/g, '\n');
          focusCell(appendCell('sql', clean));
          scheduleAutosave();
        } catch (e) {
          window.alert(`Could not load ${file.name}: ${e.message}`);
        }
        return;
      }
      try {
        const doc = JSON.parse(await file.text());
        // Validate before opening a tab for it.
        fromIpynb(doc);
        const name = file.name.replace(/\.ipynb$/i, '') || 'notebook';
        const db = (doc.metadata && doc.metadata.provsql
                    && doc.metadata.provsql.database) || connDb;
        // A loaded notebook always lands in its own tab, bound to the
        // database recorded in its metadata; the binding banner offers
        // switch / create / rebind when that is not the live one.
        newTab(name, db, doc);
      } catch (e) {
        window.alert(`Could not load ${file.name}: ${e.message}`);
      }
    });

    // Best-effort kernel cleanup when the tab goes away; the server's
    // idle GC is the real backstop.
    window.addEventListener('pagehide', () => {
      // The debounced autosave may not have fired yet (e.g. a mode
      // switch right after an edit); write synchronously so nothing is
      // lost on navigation. flushAutosave -> persistTabs also records
      // the per-tab resume point (selection + scroll).
      flushAutosave();
      if (navigator.sendBeacon) {
        for (const t of tabs) {
          const k = t.id === activeTabId ? kernel : t.kernel;
          if (k) navigator.sendBeacon(`/api/nb/session/${k.sessionId}/close`);
        }
      }
    });

    wireSidebar();

    // Same body-level switch as circuit mode's fingerprint toggle:
    // formatCell renders every uuid cell as a short/full span pair, and
    // body.show-uuids flips which one is visible -- no re-render.
    const uuidBtn = byId('nb-show-uuids');
    uuidBtn.addEventListener('click', () => {
      const on = !document.body.classList.contains('show-uuids');
      document.body.classList.toggle('show-uuids', on);
      uuidBtn.setAttribute('aria-pressed', String(on));
    });

    wireTabBar();
    // Inherit the session's scheme (the Circuit-mode pick) for fresh
    // notebooks; tabs whose doc records a scheme override it below.
    try {
      const saved = sessionStorage.getItem('ps.opt.provScheme');
      if (saved && ['semiring', 'where', 'boolean'].includes(saved)) {
        byId('nb-scheme').value = saved;
      }
    } catch (e) { /* sessionStorage disabled */ }
    // The current database name anchors every binding decision; fetch
    // it before restoring tabs (one round-trip, also warms /api/conn).
    try {
      const resp = await fetch('/api/conn');
      if (resp.ok) {
        const info = await resp.json();
        connDb = info.database || null;
        connExtVersion = info.extension_version || null;
      }
    } catch (e) { /* banner logic degrades to no-op without connDb */ }

    if (!restoreTabs()) {
      tabs = [makeTab('Untitled', connDb, null)];
      activeTabId = tabs[0].id;
    }
    // Booting on a database different from the active tab's binding
    // (the user switched databases elsewhere): activate the tab bound
    // to the new database, or open a fresh one for it -- the previous
    // tab stays, with its binding shown on the tab.
    const active = currentTab();
    if (active && active.db && connDb && active.db !== connDb) {
      const match = tabs.find((t) => t.db === connDb);
      if (match) {
        activeTabId = match.id;
      } else {
        const t = makeTab('Untitled', connDb, null);
        tabs.push(t);
        activeTabId = t.id;
      }
    }
    loadTabIntoView(currentTab());
    persistTabs();
    refreshSidebarSchema();
    populateExamples();
    // Deep link: /notebook?nb=<example> opens that bundled notebook in
    // its own tab (an already-open tab of the same name is reused).
    const wanted = new URLSearchParams(window.location.search).get('nb');
    if (wanted) {
      const existing = tabs.find((t) => t.name === wanted);
      if (existing) activateTab(existing.id);
      else openExample(wanted);
    }
  }

  // Mode-switch carry (app.js carryQueryForSwitch): the current cell's
  // SQL, with ran=true when that cell was executed on this kernel.
  function currentSqlForCarry() {
    const cell = currentSqlCell();
    if (!cell) return { sql: '', ran: false };
    return { sql: cell.source || '', ran: cell.count != null };
  }

  window.ProvsqlNotebook = { init, insertSql, replaceSql, currentSqlForCarry };
})();
