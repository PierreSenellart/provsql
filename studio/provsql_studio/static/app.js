/* ProvSQL Studio: entry script.
   Wires the shared chrome (mode switcher, example buttons, query form) plus
   both mode-specific sidebars: where-mode shows source-relation tables with
   hover-highlight, circuit-mode shows the provenance DAG (lazy-loaded
   circuit.js). Cross-mode navigation preserves the textarea content via
   sessionStorage and offers a per-row "→ Circuit" jump in where mode. */

(function () {
  const mode = document.body.classList.contains('mode-circuit') ? 'circuit' : 'where';

  // Reflect current mode on the switcher and carry the textarea + a
  // per-mode preload UUID across navigation.
  document.querySelectorAll('.ps-modeswitch__btn').forEach(btn => {
    btn.classList.toggle('is-active', btn.dataset.mode === mode);
    btn.addEventListener('click', () => {
      sessionStorage.setItem('ps.sql', document.getElementById('request').value);
    });
  });
  // Restore the carried-over query if there is one.
  const carried = sessionStorage.getItem('ps.sql');
  if (carried != null) {
    document.getElementById('request').value = carried;
    sessionStorage.removeItem('ps.sql');
  }
  // If the previous page asked us to preload a circuit (via "→ Circuit"
  // button on a where-mode result row), pull the UUID out now so circuit-mode
  // setup can fire it after the result table renders.
  const preloadCircuitUuid = sessionStorage.getItem('ps.preloadCircuit');
  sessionStorage.removeItem('ps.preloadCircuit');

  // GUC toggles. In where mode, where_provenance is forced on (the wrap
  // calls where_provenance(...) and would otherwise return all-empty); the
  // toggle is locked. In circuit mode, both are user-controlled.
  setupGucToggles();

  // ⌘ / Ctrl+Enter submits the query form.
  document.getElementById('request').addEventListener('keydown', (e) => {
    if ((e.metaKey || e.ctrlKey) && e.key === 'Enter') {
      e.preventDefault();
      document.querySelector('form.wp-form').requestSubmit();
    }
  });

  if (mode === 'where') setupWhereMode();
  else                  setupCircuitMode();

  /* ──────── Where mode ──────── */

  async function setupWhereMode() {
    await refreshRelations();
    const body = document.getElementById('result-body');
    body.addEventListener('mouseover', (e) => onResultHover(e, true));
    body.addEventListener('mouseout',  (e) => onResultHover(e, false));
    body.addEventListener('click', (e) => {
      const btn = e.target.closest('[data-jump-circuit]');
      if (!btn) return;
      sessionStorage.setItem('ps.sql', document.getElementById('request').value);
      sessionStorage.setItem('ps.preloadCircuit', btn.dataset.jumpCircuit);
      window.location.href = '/circuit';
    });
    // If a query was carried over (mode switch), re-run it; otherwise leave
    // the result pane empty until the user submits.
    if (document.getElementById('request').value.trim()) {
      runQuery({ preventDefault() {} });
    }
  }

  async function refreshRelations() {
    let relations;
    try {
      const resp = await fetch('/api/relations');
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      relations = await resp.json();
    } catch (e) {
      document.getElementById('sidebar-body').innerHTML =
        `<p style="color:var(--terracotta-500)">Failed to load relations: ${escapeHtml(e.message)}</p>`;
      return;
    }
    renderRelations(relations);
  }

  function renderRelations(relations) {
    const body = document.getElementById('sidebar-body');
    if (!relations.length) {
      body.innerHTML = '<p style="opacity:.7">No provenance-tagged relations. Try <code>SELECT add_provenance(\'mytable\')</code>.</p>';
      return;
    }
    body.innerHTML = relations.map(rel => {
      // Skip the rewriter-added `provsql` UUID column when displaying; its
      // value is already exposed as the row id (used for the hover-highlight).
      // where_provenance numbers cells by user-column position (1-indexed,
      // ignoring provsql), which matches the original i+1 since provsql sits
      // at the end of the column list.
      const visible = rel.columns
        .map((c, i) => ({ c, i }))
        .filter(({ c }) => c.name !== 'provsql');
      return `
      <section class="wp-relation">
        <header class="wp-relation__hdr">
          <h3 class="wp-relation__name">${escapeHtml(rel.regclass)}</h3>
          <span class="wp-relation__meta">${rel.rows.length} tuples · ${visible.length} cols</span>
        </header>
        <div class="wp-table-wrap">
          <table class="wp-table" id="t-${escapeAttr(rel.regclass)}">
            <thead><tr>${visible.map(({ c }) => {
              const cls = isRightAlignedType(c.type_name) ? ' class="is-right"' : '';
              return `<th${cls}>${escapeHtml(c.name)}</th>`;
            }).join('')}</tr></thead>
            <tbody>
              ${rel.rows.map(r => `
                <tr>
                  ${visible.map(({ c, i }) => {
                    const id = `${rel.regclass}:${r.uuid}:${i+1}`;
                    const cls = isRightAlignedType(c.type_name) ? ' is-right' : '';
                    return `<td id="${escapeAttr(id)}" class="${cls.trim()}">${formatCell(r.values[i], c.name)}</td>`;
                  }).join('')}
                </tr>
              `).join('')}
            </tbody>
          </table>
        </div>
      </section>`;
    }).join('');
  }

  function onResultHover(e, on) {
    const cell = e.target.closest('.wp-result__cell');
    if (!cell) return;
    cell.classList.toggle('is-hover', on);
    let firstSource = null;
    (cell.dataset.sources || '').split(';').filter(Boolean).forEach(id => {
      const el = document.getElementById(id);
      if (el) {
        el.classList.toggle('is-source', on);
        if (on && !firstSource) firstSource = el;
      }
    });
    // Bring the first highlighted source into view if it's outside the
    // sidebar's scroll viewport. block:'nearest' avoids unnecessary scroll
    // when the cell is already visible.
    if (on && firstSource) {
      firstSource.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    }
  }

  /* ──────── Circuit mode ──────── */

  function setupGucToggles() {
    const wp = document.getElementById('opt-where-prov');
    const up = document.getElementById('opt-update-prov');
    if (!wp || !up) return;

    // Toggle states persist across mode switches via sessionStorage.
    // where_provenance: the stored value is the user's circuit-mode choice.
    // Where mode forces the displayed state to "on" but never overwrites the
    // stored value, so circuit→where→circuit round-trips preserve the user's
    // pick. update_provenance: freely toggleable everywhere; persists as-is.
    const savedWhere  = sessionStorage.getItem('ps.opt.whereProv') === '1';
    const savedUpdate = sessionStorage.getItem('ps.opt.updateProv') === '1';

    if (mode === 'where') {
      wp.checked = true;
      wp.disabled = true;
      const wrap = document.getElementById('toggle-where-wrap');
      wrap.classList.add('is-locked');
      wrap.title = 'where_provenance is forced on in Where mode (the wrap requires it)';
    } else {
      wp.checked = savedWhere;
    }
    up.checked = savedUpdate;

    wp.addEventListener('change', () => {
      // Don't persist while locked: the displayed `on` is mode-forced, not a
      // user choice we want to remember on top of their circuit-mode pick.
      if (mode !== 'where') {
        sessionStorage.setItem('ps.opt.whereProv', wp.checked ? '1' : '0');
      }
    });
    up.addEventListener('change', () => {
      sessionStorage.setItem('ps.opt.updateProv', up.checked ? '1' : '0');
    });
  }

  function setupCircuitMode() {
    document.getElementById('sidebar-title').textContent = 'Provenance Circuit';
    document.getElementById('sidebar-lead').textContent =
      'Click a UUID cell in the result to render its derivation DAG here.';
    document.getElementById('sidebar-body').innerHTML = circuitSidebarHtml();
    document.getElementById('result-legend').innerHTML =
      '<span class="wp-legend-swatch" style="background:var(--purple-500)"></span> Click a UUID / agg_token cell in the result to inspect its circuit.';

    // Click handler on result-body for UUID/agg_token cells. We rely on the
    // cell having data-circuit-uuid when it's clickable; set during render.
    document.getElementById('result-body').addEventListener('click', (e) => {
      const cell = e.target.closest('.wp-result__cell.is-clickable');
      if (!cell || !cell.dataset.circuitUuid) return;
      loadCircuit(cell.dataset.circuitUuid);
    });

    // If a query was carried over (mode switch / preload), run it so the
    // user has clickable cells immediately; otherwise wait for them to type.
    const carry = preloadCircuitUuid;
    if (document.getElementById('request').value.trim()) {
      runQuery({ preventDefault() {} }).then(() => {
        if (carry) loadCircuit(carry);
      });
    } else if (carry) {
      // No query but a preload UUID: render the circuit directly.
      loadCircuit(carry);
    }
  }

  async function loadCircuit(uuid) {
    await ensureCircuitLib();
    window.ProvsqlCircuit.showLoading();
    let resp;
    try {
      resp = await fetch(`/api/circuit/${encodeURIComponent(uuid)}`);
    } catch (e) {
      window.ProvsqlCircuit.showError(`Network error: ${e.message}`);
      return;
    }
    if (!resp.ok) {
      const err = await resp.json().catch(() => ({}));
      window.ProvsqlCircuit.showError(err.error || `HTTP ${resp.status}`);
      return;
    }
    const scene = await resp.json();
    window.ProvsqlCircuit.renderCircuit(scene);
  }

  let _circuitLibPromise = null;
  function ensureCircuitLib() {
    if (window.ProvsqlCircuit) return Promise.resolve(window.ProvsqlCircuit);
    if (_circuitLibPromise) return _circuitLibPromise;
    _circuitLibPromise = new Promise((resolve, reject) => {
      const s = document.createElement('script');
      s.src = '/static/circuit.js';
      s.onload  = () => { try { window.ProvsqlCircuit.init(); } catch (e) {} resolve(window.ProvsqlCircuit); };
      s.onerror = () => reject(new Error('failed to load circuit.js'));
      document.body.appendChild(s);
    });
    return _circuitLibPromise;
  }

  function circuitSidebarHtml() {
    return `
      <header class="cv-main__hdr" style="padding:0; border-bottom:none; margin-bottom:0.4rem">
        <div>
          <h3 class="cv-main__sub" id="circuit-sub" style="margin:0; font-family:var(--font-ui); font-size:0.78rem; opacity:0.7">Click a UUID cell to render.</h3>
          <span id="circuit-title" style="display:none">Provenance Circuit</span>
        </div>
      </header>
      <div class="cv-toolbar" role="toolbar">
        <button class="cv-tool" id="tool-zoom-out" title="Zoom out"><i class="fas fa-search-minus"></i></button>
        <button class="cv-tool" id="tool-zoom-fit" title="Fit"><i class="fas fa-expand"></i></button>
        <button class="cv-tool" id="tool-zoom-in" title="Zoom in"><i class="fas fa-search-plus"></i></button>
        <span class="cv-tool__sep"></span>
        <button class="cv-tool cv-tool--toggle" id="tool-show-uuids" aria-pressed="false" title="Show UUIDs"><i class="fas fa-fingerprint"></i></button>
        <button class="cv-tool cv-tool--toggle" id="tool-show-formula" aria-pressed="true" title="Show formula"><i class="fas fa-square-root-alt"></i></button>
      </div>
      <div class="cv-canvas" id="canvas">
        <svg id="circuit" preserveAspectRatio="xMidYMid meet">
          <defs>
            <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">
              <path d="M0,0 L10,5 L0,10 z" fill="var(--purple-700)"></path>
            </marker>
            <marker id="arrow-active" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">
              <path d="M0,0 L10,5 L0,10 z" fill="var(--terracotta-500)"></path>
            </marker>
          </defs>
          <g id="circuit-edges"></g>
          <g id="circuit-nodes"></g>
        </svg>
        <aside class="cv-inspector" id="inspector">
          <header class="cv-inspector__hdr">
            <h3 class="cv-inspector__title" id="inspector-title">Node</h3>
            <button class="cv-inspector__close" id="inspector-close"><i class="fas fa-times"></i></button>
          </header>
          <div class="cv-inspector__body" id="inspector-body"></div>
        </aside>
      </div>
      <footer class="cv-formula" id="formula-strip">
        <span class="cv-formula__label">Formula</span>
        <code class="cv-formula__expr" id="formula-expr">–</code>
      </footer>
    `;
  }

  /* ──────── shared helpers ──────── */

  function escapeHtml(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }
  function escapeAttr(s) {
    return escapeHtml(s).replace(/"/g, '&quot;');
  }

  function formatCell(v, columnName) {
    // Classification pill rendering (well-known column name).
    if (columnName === 'classification' && typeof v === 'string') {
      const safe = v.replace(/[^a-zA-Z0-9_]/g, '');
      return `<span class="wp-pill wp-pill--${safe}">${escapeHtml(v)}</span>`;
    }
    return escapeHtml(v == null ? '' : v);
  }

  // PostgreSQL pg_type names that conventionally render right-aligned:
  // numerics (int / numeric / float / money) plus date/time/interval. Also
  // agg_token, whose visible glyph is a number ("3 (*)") even though its
  // underlying storage is a UUID.
  const RIGHT_ALIGNED_TYPES = new Set([
    'int2', 'int4', 'int8', 'smallint', 'integer', 'bigint',
    'numeric', 'decimal',
    'float4', 'float8', 'real',
    'money',
    'agg_token',
    'date', 'time', 'timetz', 'timestamp', 'timestamptz', 'interval',
  ]);
  function isRightAlignedType(typeName) {
    return RIGHT_ALIGNED_TYPES.has((typeName || '').toLowerCase());
  }

  // Expose to runQuery (defined as a global below for the inline onsubmit).
  window.__provsqlStudio = { mode, refreshRelations, escapeHtml, escapeAttr, formatCell, isRightAlignedType };
})();

/* Global runQuery: invoked by the form's inline onsubmit. POSTs to /api/exec
   and renders the response into the result section. */
async function runQuery(ev) {
  ev.preventDefault();

  const env = window.__provsqlStudio || { mode: 'where', escapeHtml: s => s, escapeAttr: s => s, formatCell: v => v };
  const sqlText = document.getElementById('request').value;
  const head    = document.getElementById('result-head');
  const body    = document.getElementById('result-body');
  const count   = document.getElementById('result-count');
  const time    = document.getElementById('result-time');

  // Loading state.
  body.innerHTML = `<tr><td style="opacity:.6; text-align:center; padding:1rem">Running…</td></tr>`;
  count.textContent = '…';
  time.textContent = '…';
  const t0 = performance.now();

  const wpEl = document.getElementById('opt-where-prov');
  const upEl = document.getElementById('opt-update-prov');
  let resp;
  try {
    resp = await fetch('/api/exec', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        sql: sqlText,
        mode: env.mode,
        where_provenance: wpEl ? wpEl.checked : (env.mode === 'where'),
        update_provenance: upEl ? upEl.checked : false,
      }),
    });
  } catch (e) {
    renderError(`Network error: ${e.message}`);
    return false;
  }
  const dt = Math.round(performance.now() - t0);
  time.textContent = dt;

  if (!resp.ok) {
    renderError(`HTTP ${resp.status}`);
    return false;
  }
  const payload = await resp.json();
  renderBlocks(payload.blocks || [], !!payload.wrapped);

  // After every successful exec in where mode, re-fetch relations so
  // add_provenance results show up live.
  if (env.mode === 'where' && env.refreshRelations) env.refreshRelations();

  return false;

  function renderBlocks(blocks, wrapped) {
    // For Stage 2, /api/exec returns at most: zero or more error blocks (from
    // earlier failed statements) followed by the final block. We render only
    // the final block in the result table, but surface earlier errors too.
    const final = blocks[blocks.length - 1];
    const earlier = blocks.slice(0, -1);

    let prelude = '';
    if (earlier.length) {
      prelude = earlier.map(b => b.kind === 'error'
        ? `<div class="wp-error">Earlier statement failed: ${env.escapeHtml(b.message)}</div>`
        : ''
      ).join('');
    }

    if (!final) {
      head.innerHTML = '';
      body.innerHTML = prelude || '<tr><td style="opacity:.6">(no statements)</td></tr>';
      count.textContent = 0;
      return;
    }

    if (final.kind === 'error') {
      head.innerHTML = '';
      body.innerHTML = prelude + `<tr><td><div class="wp-error">Error: ${env.escapeHtml(final.message)}${final.sqlstate ? ` <code>(SQLSTATE ${env.escapeHtml(final.sqlstate)})</code>` : ''}</div></td></tr>`;
      count.textContent = 0;
      return;
    }
    if (final.kind === 'status') {
      head.innerHTML = '';
      body.innerHTML = prelude + `<tr><td>${env.escapeHtml(final.message)}${final.rowcount != null ? ` · ${final.rowcount} tuples affected` : ''}</td></tr>`;
      count.textContent = final.rowcount != null ? final.rowcount : 0;
      return;
    }
    if (final.kind === 'rows') {
      const allCols = final.columns;
      const isWhere   = env.mode === 'where';
      const isCircuit = env.mode === 'circuit';
      // Hide rewriter-injected columns (__prov, __wprov) from display, but
      // keep them indexed so we can still build per-cell data-sources and
      // per-row jump buttons. The bare `provsql` UUID column is hidden in
      // where mode (it duplicates the highlighting metadata) but kept in
      // circuit mode so users can click it to render the row's DAG.
      const displayIdx = [];
      let provIdx = -1, wprovIdx = -1;
      allCols.forEach((c, i) => {
        if (c.name === '__prov')  provIdx = i;
        else if (c.name === '__wprov') wprovIdx = i;
        else if (c.name === 'provsql' && isWhere) { /* hidden in where mode */ }
        else displayIdx.push(i);
      });
      // In circuit mode, surface a hint when there's no UUID/agg_token
      // column to click. We check this once on the columns.
      const hasClickableCols = displayIdx.some(i =>
        ['uuid', 'agg_token'].includes((allCols[i].type_name || '').toLowerCase())
      );

      const headExtra = (isWhere && wrapped) ? '<th></th>' : '';
      head.innerHTML = displayIdx.map(i => {
        const alignCls = env.isRightAlignedType(allCols[i].type_name) ? ' is-right' : '';
        return `<th class="${alignCls.trim()}">${env.escapeHtml(allCols[i].name)}</th>`;
      }).join('') + headExtra;
      body.innerHTML = prelude + final.rows.map(r => {
        const sources = wrapped && wprovIdx >= 0
          ? parseWhereProvenance(r[wprovIdx], displayIdx)
          : null;
        const cells = displayIdx.map((idx, di) => {
          const col = allCols[idx];
          const typeName = (col.type_name || '').toLowerCase();
          const value = r[idx];
          const dataSrc = sources ? sources[di] || '' : '';
          const sourcesAttr = dataSrc ? ` data-sources="${env.escapeAttr(dataSrc)}"` : '';
          let extraCls = '';
          let extraAttr = '';
          if (isCircuit && typeName === 'uuid' && value) {
            extraCls  = ' is-clickable';
            extraAttr = ` data-circuit-uuid="${env.escapeAttr(String(value))}"`;
          }
          if (env.isRightAlignedType(typeName)) extraCls += ' is-right';
          // agg_token cells: their text is "<value> (*)", which doesn't carry
          // the UUID. v1 leaves them non-clickable; cast in SQL to inspect.
          return `<td class="wp-result__cell${extraCls}"${sourcesAttr}${extraAttr}>${env.formatCell(value, col.name)}</td>`;
        }).join('');
        const jumpBtn = (isWhere && wrapped && provIdx >= 0 && r[provIdx])
          ? `<td class="wp-result__cell--actions"><button class="wp-btn wp-btn--mini" type="button" `
            + `data-jump-circuit="${env.escapeAttr(String(r[provIdx]))}" title="Open circuit DAG"><i class="fas fa-project-diagram"></i> Circuit</button></td>`
          : '';
        return `<tr>${cells}${jumpBtn}</tr>`;
      }).join('');
      count.textContent = final.rows.length;

      if (isCircuit && !hasClickableCols && final.rows.length) {
        const legend = document.getElementById('result-legend');
        if (legend) {
          legend.innerHTML =
            '<i class="fas fa-info-circle"></i> No UUID columns in this result: '
            + '<a href="/where" class="ps-modeswitch__btn" style="color:var(--purple-500); text-decoration:underline; padding:0 0.2rem">switch to Where mode</a> '
            + 'to see source-cell highlights, or add <code>provsql.provenance()</code> to your SELECT.';
        }
      }
    }
  }

  function renderError(msg) {
    head.innerHTML = '';
    body.innerHTML = `<tr><td><div class="wp-error">${env.escapeHtml(msg)}</div></td></tr>`;
    count.textContent = 0;
  }

  /* Parse the text returned by where_provenance(provenance()).
     Format (from src/WhereCircuit.cpp / where_provenance.cpp):
       {[table:uuid:col;table:uuid:col],[table:uuid:col],[]}
     Outer braces, comma-separated groups (one per output column of the
     wrapped query), each group is square-bracketed, semicolon-separated
     `table:uuid:col` entries. The groups are in inner-SELECT column order,
     which matches `allCols` exactly, so groups[displayIdx[i]] is the
     source list for the i-th displayed column. We return one
     ready-to-set `data-sources` string per displayed column. */
  function parseWhereProvenance(wprovText, displayIdx) {
    if (!wprovText) return null;
    const m = String(wprovText).match(/^\{(.*)\}$/s);
    if (!m) return null;
    const inner = m[1];

    // Split top-level groups by `,`. Groups themselves don't contain commas
    // (they use `;` between items), so this is safe.
    const groups = inner.length === 0 ? [] : splitTopLevel(inner, ',');
    const perGroup = groups.map(g => {
      const gm = g.match(/^\[(.*)\]$/s);
      if (!gm) return '';
      return gm[1];  // already `table:uuid:col;table:uuid:col`
    });
    // Map each displayed column to its group in the inner-SELECT order.
    // Hidden columns (provsql, __prov, __wprov) are skipped over.
    return displayIdx.map(i => perGroup[i] || '');
  }

  function splitTopLevel(s, sep) {
    const out = [];
    let depth = 0, last = 0;
    for (let i = 0; i < s.length; ++i) {
      const c = s[i];
      if (c === '[' || c === '{' || c === '(') depth++;
      else if (c === ']' || c === '}' || c === ')') depth--;
      else if (c === sep && depth === 0) {
        out.push(s.slice(last, i));
        last = i + 1;
      }
    }
    out.push(s.slice(last));
    return out;
  }
}
