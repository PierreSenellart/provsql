/* ProvSQL Studio: notebook mode (lazy-loaded by app.js, mirroring
   circuit.js). An ordered list of Markdown + SQL cells executed against
   a pinned, stateful database session (the "kernel": /api/nb/session +
   /api/nb/exec), with per-cell results rendered by the same block
   renderer as the shared result pane (app.js makeBlockRenderer).
   Persistence: Jupyter nbformat v4 (.ipynb) download / load, plus a
   localStorage autosave of the working draft. See
   doc/TODO/studio-notebook-mode.md. */
(function () {
  'use strict';

  let env = null;          // window.__provsqlStudio, passed by init()
  let kernel = null;       // {sessionId, pid, db} | null
  let kernelStarting = null;  // in-flight ensureKernel() promise
  let cells = [];          // [{id, type, source, outputs, count, htmlSnapshot}]
  let execCounter = 0;
  let running = null;      // {cellId, requestId} | null
  let mdLibs = null;       // promise for marked + DOMPurify
  let lastFocused = null;  // last cell whose editor had focus
  let schemaCache = null;  // /api/schema payload for the sidebar
  let tabs = [];           // [{id, name, db, doc, kernel, resume}]
  let activeTabId = null;
  let connDb = null;       // current connection's database (from /api/conn)
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
        requestAnimationFrame(() => requestAnimationFrame(
          () => window.scrollTo(0, y)));
      }
    } else {
      if (cells.length) selectCell(cells[0], { scroll: false });
      window.scrollTo(0, 0);
    }
    renderTabBar();
    updateBindingBanner();
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

  function newTab(name, db, doc) {
    flushActiveTab();
    const tab = makeTab(name, db, doc);
    tabs.push(tab);
    activeTabId = tab.id;
    loadTabIntoView(tab);
    persistTabs();
    return tab;
  }

  function closeTab(id) {
    const idx = tabs.findIndex((t) => t.id === id);
    if (idx < 0) return;
    const tab = tabs[idx];
    const nonTrivial = tab.doc
      ? (tab.doc.cells || []).some((c) => String(
          Array.isArray(c.source) ? c.source.join('') : c.source || '').trim())
      : (id === activeTabId && cells.some((c) => (c.source || '').trim()));
    if (nonTrivial && !window.confirm(`Close tab “${tab.name}”?`)) return;
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
        + ` data-tab="${escA(t.id)}" title="${escA(name)}${t.db ? ' — ' + escA(t.db) : ''}">`
        + `<span class="nb__tab-name">${esc(name)}</span>`
        + (foreign ? `<span class="nb__tab-db">${esc(t.db)}</span>` : '')
        + `<button type="button" class="nb__tab-close" data-tab-close="${escA(t.id)}"`
        + ` title="Close tab" aria-label="Close tab">×</button>`
        + `</span>`;
    }).join('')
    + `<button type="button" class="nb__tab-add" id="nb-tab-add"`
    + ` title="New notebook tab (bound to the current database)">+</button>`;
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
      const tabEl = e.target.closest('[data-tab]');
      if (tabEl) activateTab(tabEl.dataset.tab);
    });
  }

  /* Binding banner: the active tab's database vs the live connection. */
  function updateBindingBanner() {
    const banner = byId('nb-binding-banner');
    if (!banner) return;
    const tab = currentTab();
    const foreign = tab && tab.db && connDb && tab.db !== connDb;
    banner.hidden = !foreign;
    if (!foreign) { banner.innerHTML = ''; return; }
    const esc = env.escapeHtml;
    banner.innerHTML =
      `<span class="nb__banner-msg"><i class="fas fa-database"></i> `
      + `This notebook is bound to <strong>${esc(tab.db)}</strong>; `
      + `you are connected to <strong>${esc(connDb)}</strong>.</span>`
      + `<button type="button" class="wp-btn wp-btn--mini" id="nb-bind-switch">`
      + `Switch to ${esc(tab.db)}</button> `
      + `<button type="button" class="wp-btn wp-btn--ghost wp-btn--mini" id="nb-bind-create">`
      + `Create ${esc(tab.db)}</button> `
      + `<button type="button" class="wp-btn wp-btn--ghost wp-btn--mini" id="nb-bind-keep">`
      + `Rebind to ${esc(connDb)}</button>`;
    byId('nb-bind-switch').addEventListener('click', () => switchConnectionTo(tab.db));
    byId('nb-bind-create').addEventListener('click', async () => {
      const resp = await fetch('/api/databases', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name: tab.db }),
      });
      const payload = await resp.json().catch(() => ({}));
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
    setKernelChip('dead', reason || 'kernel died — restart it');
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

  function ensureMdLibs() {
    if (!mdLibs) {
      mdLibs = Promise.all([
        loadScript('/static/vendor/marked.min.js'),
        loadScript('/static/vendor/purify.min.js'),
      ]);
    }
    return mdLibs;
  }

  async function renderMarkdownInto(el, source) {
    try {
      await ensureMdLibs();
      const html = window.marked.parse(source, { gfm: true, breaks: false });
      el.innerHTML = window.DOMPurify.sanitize(html);
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
        <span class="nb-out__count"></span> tuples<span class="nb-out__trunc" hidden></span>
        · <span class="nb-out__time">–</span> ms
      </div>`;
    return {
      head: outEl.querySelector('thead tr'),
      body: outEl.querySelector('tbody'),
      count: outEl.querySelector('.nb-out__count'),
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
        <span class="nb-cell__count">[ ]</span>
        ${cell.type === 'sql'
          ? `<button type="button" class="nb-cell__run" title="Run this cell (Ctrl+Enter)"><i class="fas fa-play"></i></button>`
          : ''}
        ${cell.type === 'circuit'
          ? `<button type="button" class="nb-cell__run" title="Re-fetch the circuit"><i class="fas fa-sync"></i></button>`
          : ''}
      </div>
      <div class="nb-cell__main">
        ${cell.type === 'circuit' ? `
          <div class="nb-circ">
            <div class="nb-circ__hdr">
              <i class="fas fa-project-diagram"></i>
              Circuit for <code class="nb-circ__token" title="${cell.token || ''}">${shortToken(cell.token)}</code>
              <button type="button" class="nb-circ__jump wp-btn wp-btn--ghost wp-btn--mini"
                      title="Open this token in Circuit mode (eval strip, expansion, inspector)">
                <i class="fas fa-external-link-alt"></i> Circuit mode</button>
            </div>
            <div class="nb-circ__canvas"></div>
          </div>
        ` : ''}
        ${cell.type === 'sql' ? `
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
        <button type="button" data-act="add-sql" title="Insert SQL cell below"><i class="fas fa-plus"></i></button>
        <button type="button" data-act="add-md" title="Insert Markdown cell below"><i class="fab fa-markdown"></i></button>
        ${cell.type === 'sql'
          ? `<button type="button" data-act="to-circuit" title="Open this query in Circuit mode"><i class="fas fa-project-diagram"></i></button>`
          : ''}
        <button type="button" data-act="del" title="Delete cell"><i class="fas fa-trash"></i></button>
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
      ta.addEventListener('keydown', (e) => {
        if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
          e.preventDefault();
          runCell(cell);
        } else if (e.shiftKey && e.key === 'Enter') {
          e.preventDefault();
          runCell(cell).then(() => focusNextOrCreate(cell));
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
      renderMarkdownInto(view, cell.source || '*(empty cell: double-click to edit)*');
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
      div.querySelector('.nb-circ__jump').addEventListener('click', () => {
        // Same carry mechanism as the where-mode jump button: Circuit
        // mode preloads the token on arrival.
        try { sessionStorage.setItem('ps.preloadCircuit', cell.token); } catch (e) {}
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

  function focusCell(cell) {
    const el = cellEl(cell);
    if (!el) return;
    const ta = el.querySelector('.nb-cell__ta, .nb-cell__mdta:not([hidden])');
    if (ta) ta.focus();
    else el.querySelector('.nb-cell__md')?.dispatchEvent(new Event('dblclick'));
  }

  function focusNextOrCreate(cell) {
    const idx = cells.indexOf(cell);
    if (idx === cells.length - 1) appendCell('sql');
    focusCell(cells[idx + 1]);
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
  const ORDERED_GATES = new Set(['cmp', 'monus', 'agg', 'arith', 'mixture']);
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
  function edgePosLabel(parent, pos) {
    if (parent.type === 'mixture') {
      return ({ 1: 'p', 2: 'x', 3: 'y' })[pos] || String(pos);
    }
    return String(pos);
  }

  function svgEl(tag, attrs) {
    const el = document.createElementNS('http://www.w3.org/2000/svg', tag);
    for (const [k, v] of Object.entries(attrs || {})) el.setAttribute(k, v);
    return el;
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
    const svg = svgEl('svg', {
      class: 'nb-circ__svg',
      viewBox: `${minX} ${minY} ${maxX - minX} ${maxY - minY}`,
      preserveAspectRatio: 'xMidYMid meet',
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
          x: (from.x + to.x) / 2 + bow + (-dy / len) * 9,
          y: (from.y + to.y) / 2 + (dx / len) * 9,
        });
        t.textContent = edgePosLabel(from, e.child_pos);
        svg.appendChild(t);
      }
    }
    for (const n of scene.nodes) {
      const g = svgEl('g', {
        class: `node-group node--${n.type}` + (n.frontier ? ' is-frontier' : ''),
        transform: `translate(${n.x},${n.y})`,
      });
      g.appendChild(svgEl('circle', { class: 'node-shape', r: 22 }));
      const label = String(n.label == null ? '' : n.label);
      const t = svgEl('text', {
        class: 'node-label',
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

  function shortToken(t) {
    const s = String(t || '');
    return s.length > 8 ? s.slice(0, 8) + '…' : s;
  }

  // Insert a circuit cell for `token` after `afterCell` -- or, when the
  // next cell is already a circuit cell, retarget it (repeated clicks
  // on result UUIDs swap the snapshot instead of piling cells up).
  function showCircuitFor(token, afterCell) {
    const idx = cells.indexOf(afterCell);
    let cell = idx >= 0 && cells[idx + 1] && cells[idx + 1].type === 'circuit'
      ? cells[idx + 1] : null;
    if (cell) {
      cell.token = token;
      const el = cellEl(cell);
      const lbl = el && el.querySelector('.nb-circ__token');
      if (lbl) { lbl.textContent = shortToken(token); lbl.title = token; }
    } else {
      cell = newCell('circuit');
      cell.token = token;
      const pos = idx >= 0 ? idx + 1 : cells.length;
      cells.splice(pos, 0, cell);
      const anchorEl = idx >= 0 ? cellEl(afterCell) : null;
      const dom = buildCellDom(cell);
      if (anchorEl) anchorEl.after(dom);
      else cellsEl().appendChild(dom);
    }
    refreshCircuitCell(cell);
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
          + ` data-outline-cell="${escA(e.cellId)}">${esc(e.text)}</li>`
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
            const pill = r.has_provenance
              ? `<span class="wp-result__col-prov" title="provenance-tracked">prov</span>`
              : '';
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
          el.scrollIntoView({ behavior: 'smooth', block: 'start' });
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
    if (cell.type === type || cell.type === 'circuit') return;
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
      if (!cells.length) return;
      selected = cells[0];
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
  // next cell, creating one at the end.
  async function runSelectedThenAdvance(cell) {
    if (cell.type === 'sql') await runCell(cell);
    else if (cell.type === 'circuit' && cell.token) await refreshCircuitCell(cell);
    const idx = cells.indexOf(cell);
    if (idx === cells.length - 1) appendCell('sql');
    selectCell(cells[idx + 1]);
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
          prov_scheme: byId('nb-scheme').value,
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
      kernelGone('kernel died — restart it');
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
            metadata: { provsql: { cell: 'circuit', token: c.token } },
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
                 metadata: { provsql: {} }, source: toLines(c.source), outputs };
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
                 && c.metadata.provsql.cell === 'circuit') {
        const cell = newCell('circuit');
        cell.token = String(c.metadata.provsql.token || '');
        for (const o of (c.outputs || [])) {
          // Re-paint only from the scene JSON (through our own
          // painter); the svg snapshot is for external viewers.
          const s = o && o.data && o.data['application/vnd.provsql.scene+json'];
          if (s) { cell.scene = s; break; }
        }
        loaded.push(cell);
      } else if (c.cell_type === 'code') {
        const cell = newCell('sql', joinSource(c.source));
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
    for (const c of cells) root.appendChild(buildCellDom(c));
    for (const c of cells) { updateGutter(c); renderOutputs(c); }
  }

  function loadNotebook(doc) {
    const { loaded, scheme } = fromIpynb(doc);
    cells = loaded.length ? loaded : [newCell('sql')];
    execCounter = cells.reduce((m, c) => Math.max(m, c.count || 0), 0);
    if (scheme && ['semiring', 'where', 'boolean'].includes(scheme)) {
      byId('nb-scheme').value = scheme;
    }
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
      if (cell) showCircuitFor(td.dataset.circuitUuid, cell);
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
    byId('nb-scheme').addEventListener('change', scheduleAutosave);
    byId('nb-save').addEventListener('click', download);
    byId('nb-load').addEventListener('click', () => byId('nb-load-input').click());
    byId('nb-load-input').addEventListener('change', async () => {
      const input = byId('nb-load-input');
      const file = input.files && input.files[0];
      input.value = '';
      if (!file) return;
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
    // The current database name anchors every binding decision; fetch
    // it before restoring tabs (one round-trip, also warms /api/conn).
    try {
      const resp = await fetch('/api/conn');
      if (resp.ok) connDb = (await resp.json()).database || null;
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
