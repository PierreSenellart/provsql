/* ProvSQL Studio — entry script.
   Wires the shared chrome (mode switcher, example buttons, query form) plus
   the where-mode sidebar (relations + hover-highlight) by calling the Flask
   /api/exec, /api/relations, and /api/config endpoints. Circuit-mode wiring
   is a placeholder here and lands in Stage 3. */

(function () {
  const mode = document.body.classList.contains('mode-circuit') ? 'circuit' : 'where';

  // Reflect current mode on the switcher.
  document.querySelectorAll('.ps-modeswitch__btn').forEach(btn => {
    btn.classList.toggle('is-active', btn.dataset.mode === mode);
  });

  // Example-query buttons paste into the textarea.
  document.querySelectorAll('.wp-btn--ex').forEach(btn => {
    btn.addEventListener('click', () => {
      document.getElementById('request').value = btn.dataset.q;
    });
  });

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
    // Run the default query so the page isn't empty on first load.
    runQuery({ preventDefault() {} });
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
    body.innerHTML = relations.map(rel => `
      <section class="wp-relation">
        <header class="wp-relation__hdr">
          <h3 class="wp-relation__name">${escapeHtml(rel.regclass)}</h3>
          <span class="wp-relation__meta">${rel.rows.length} tuples · ${rel.columns.length} cols · <code>provsql</code> tagged</span>
        </header>
        <div class="wp-table-wrap">
          <table class="wp-table" id="t-${escapeAttr(rel.regclass)}">
            <thead><tr>${rel.columns.map(c => `<th>${escapeHtml(c.name)}</th>`).join('')}</tr></thead>
            <tbody>
              ${rel.rows.map(r => `
                <tr>
                  ${r.values.map((v, i) => {
                    const id = `${rel.regclass}:${r.uuid}:${i+1}`;
                    return `<td id="${escapeAttr(id)}">${formatCell(v, rel.columns[i].name)}</td>`;
                  }).join('')}
                </tr>
              `).join('')}
            </tbody>
          </table>
        </div>
      </section>`).join('');
  }

  function onResultHover(e, on) {
    const cell = e.target.closest('.wp-result__cell');
    if (!cell) return;
    cell.classList.toggle('is-hover', on);
    (cell.dataset.sources || '').split(';').filter(Boolean).forEach(id => {
      const el = document.getElementById(id);
      if (el) el.classList.toggle('is-source', on);
    });
  }

  /* ──────── Circuit mode placeholder ──────── */

  function setupCircuitMode() {
    document.getElementById('sidebar-title').textContent = 'Provenance Circuit';
    document.getElementById('sidebar-lead').textContent = 'Click a UUID or agg_token cell in the result to render the DAG here.';
    document.getElementById('sidebar-body').innerHTML =
      '<p style="opacity:.75; font-size:.9rem">Circuit visualiser wiring lands in Stage 3. The vendored <code>circuit.js</code> ships the layout + interactions; Studio will lazy-load it into this sidebar after the user clicks a typed cell.</p>';
    document.getElementById('form-hint').innerHTML =
      '<i class="fas fa-info-circle"></i> Circuit mode: <code>provsql.where_provenance</code> off by default';
    document.getElementById('result-legend').innerHTML =
      '<span class="wp-legend-swatch" style="background:var(--purple-500)"></span> Click a UUID / agg_token cell in the result to inspect its circuit.';
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

  // Expose to runQuery (defined as a global below for the inline onsubmit).
  window.__provsqlStudio = { mode, refreshRelations, escapeHtml, escapeAttr, formatCell };
})();

/* Global runQuery — invoked by the form's inline onsubmit. POSTs to /api/exec
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

  let resp;
  try {
    resp = await fetch('/api/exec', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sql: sqlText, mode: env.mode }),
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
      body.innerHTML = prelude + `<tr><td>${env.escapeHtml(final.message)}${final.rowcount != null ? ` · ${final.rowcount} rows affected` : ''}</td></tr>`;
      count.textContent = final.rowcount != null ? final.rowcount : 0;
      return;
    }
    if (final.kind === 'rows') {
      const allCols = final.columns;
      // Hide rewriter-injected columns (__prov, __wprov) from display but
      // keep them so we can build per-cell data-sources.
      const displayIdx = [];
      let provIdx = -1, wprovIdx = -1;
      allCols.forEach((c, i) => {
        if (c.name === '__prov')  provIdx = i;
        else if (c.name === '__wprov') wprovIdx = i;
        else displayIdx.push(i);
      });

      head.innerHTML = displayIdx.map(i => `<th>${env.escapeHtml(allCols[i].name)}</th>`).join('');
      body.innerHTML = prelude + final.rows.map(r => {
        const sources = wrapped && wprovIdx >= 0
          ? parseWhereProvenance(r[wprovIdx], displayIdx)
          : null;
        return `<tr>${displayIdx.map((idx, di) => {
          const dataSrc = sources ? sources[di] || '' : '';
          const sourcesAttr = dataSrc ? ` data-sources="${env.escapeAttr(dataSrc)}"` : '';
          return `<td class="wp-result__cell"${sourcesAttr}>${env.formatCell(r[idx], allCols[idx].name)}</td>`;
        }).join('')}</tr>`;
      }).join('');
      count.textContent = final.rows.length;
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
     `table:uuid:col` entries. We return one ready-to-set `data-sources`
     string per displayed column. The wrapped query has the same column
     order as the inner SELECT, so groups[i] corresponds to displayIdx[i]. */
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
    // Drop the trailing __prov / __wprov entries (the wrapping adds two
    // columns to the inner SELECT, but the output groups correspond to the
    // inner column order — they're empty for those wrapper-only columns).
    return perGroup.slice(0, displayIdx.length);
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
