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

  const byId = (id) => document.getElementById(id);
  const cellsEl = () => byId('nb-cells');

  let nextCellId = 1;
  function newCell(type, source) {
    return { id: 'c' + (nextCellId++), type, source: source || '',
             outputs: null, count: null, htmlSnapshot: null };
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
      </div>
      <div class="nb-cell__main">
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
      ta.addEventListener('focus', () => { lastFocused = cell; });
      ta.addEventListener('input', refresh);
      ta.addEventListener('keydown', (e) => {
        if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
          e.preventDefault();
          runCell(cell);
        } else if (e.shiftKey && e.key === 'Enter') {
          e.preventDefault();
          runCell(cell).then(() => focusNextOrCreate(cell));
        }
      });
      div.querySelector('.nb-cell__run').addEventListener('click', () => runCell(cell));
      // Initial paint (after insertion, so scrollHeight is real).
      requestAnimationFrame(refresh);
    } else {
      const view = div.querySelector('.nb-cell__md');
      const ta = div.querySelector('.nb-cell__mdta');
      const startEdit = () => {
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
      ta.addEventListener('focus', () => { lastFocused = cell; });
      view.addEventListener('dblclick', startEdit);
      ta.addEventListener('blur', endEdit);
      ta.addEventListener('input', () => autosize(ta));
      ta.addEventListener('keydown', (e) => {
        if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
          e.preventDefault();
          ta.blur();
        }
      });
      renderMarkdownInto(view, cell.source || '*(empty cell: double-click to edit)*');
      if (!cell.source) requestAnimationFrame(startEdit);
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
    if (lastFocused && cells.includes(lastFocused)) return lastFocused;
    return cells.find((c) => c.type === 'sql') || null;
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
        provsql: { scheme: byId('nb-scheme').value },
      },
      cells: cells.map((c) => {
        if (c.type === 'markdown') {
          return { cell_type: 'markdown', metadata: {}, source: toLines(c.source) };
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

  function download() {
    const blob = new Blob([JSON.stringify(toIpynb(), null, 1)],
                          { type: 'application/x-ipynb+json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'notebook.ipynb';
    a.click();
    URL.revokeObjectURL(a.href);
  }

  let autosaveTimer = null;
  function scheduleAutosave() {
    clearTimeout(autosaveTimer);
    autosaveTimer = setTimeout(() => {
      // Every model change funnels through here, so the debounced tick
      // doubles as the sidebar/outline refresh trigger.
      renderSidebar();
      try {
        localStorage.setItem('ps.nb.autosave', JSON.stringify(toIpynb()));
      } catch (e) { /* quota / disabled: drafts just don't persist */ }
    }, 500);
  }

  function restoreAutosave() {
    try {
      const raw = localStorage.getItem('ps.nb.autosave');
      if (!raw) return false;
      loadNotebook(JSON.parse(raw));
      return true;
    } catch (e) {
      return false;
    }
  }

  /* ──────── boot ──────── */

  function defaultNotebook() {
    cells = [
      newCell('markdown',
        '# ProvSQL notebook\n\nSQL cells run against a **stateful session**: '
        + 'temp tables and `SET`s persist across cells until you restart the '
        + 'kernel. Results render inline and save with the notebook '
        + '(Jupyter-compatible `.ipynb`).'),
      newCell('sql'),
    ];
    renderAll();
  }

  function init(studioEnv) {
    env = studioEnv;

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
        loadNotebook(JSON.parse(await file.text()));
        scheduleAutosave();
      } catch (e) {
        window.alert(`Could not load ${file.name}: ${e.message}`);
      }
    });

    // Best-effort kernel cleanup when the tab goes away; the server's
    // idle GC is the real backstop.
    window.addEventListener('pagehide', () => {
      if (kernel && navigator.sendBeacon) {
        navigator.sendBeacon(`/api/nb/session/${kernel.sessionId}/close`);
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

    if (!restoreAutosave()) defaultNotebook();
    setKernelChip('none', 'no kernel');
    refreshSidebarSchema();
  }

  window.ProvsqlNotebook = { init, insertSql, replaceSql };
})();
