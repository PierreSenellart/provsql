/* ProvSQL Studio: entry script.
   Wires the shared chrome (mode switcher, example buttons, query form) plus
   both mode-specific sidebars: where-mode shows source-relation tables with
   hover-highlight, circuit-mode shows the provenance DAG (lazy-loaded
   circuit.js). Cross-mode navigation preserves the textarea content via
   sessionStorage and offers a per-row "→ Circuit" jump in where mode. */

(function () {
  const mode = document.body.classList.contains('mode-circuit') ? 'circuit' : 'where';

  // Metadata caches (schema panel, eval-strip mapping picker, eval-strip
  // custom-semiring optgroup) lazy-load once and would otherwise stay
  // stale for the lifetime of the page. We mark them dirty after every
  // successful /api/exec so the next time each panel opens, it
  // re-fetches; the toolbar refresh button forces an invalidation
  // explicitly (e.g. after schema changes outside the Studio session).
  // Each consumer is responsible for clearing its own flag once reload
  // succeeds.
  const Metadata = {
    schemaDirty:   true,
    mappingsDirty: true,
    customsDirty:  true,
    invalidateAll() {
      this.schemaDirty   = true;
      this.mappingsDirty = true;
      this.customsDirty  = true;
    },
  };
  window.ProvsqlStudio = window.ProvsqlStudio || {};
  window.ProvsqlStudio.metadata = Metadata;

  // Carry the textarea + a per-mode preload UUID across navigation.
  // The active-tab highlight is driven by CSS off <body class="mode-X">
  // so it doesn't flash when JS lags behind initial render.
  //
  // Two carry channels :
  //   ps.sql      always written : preserves the user's draft across the
  //               switch so they don't lose what they had typed.
  //   ps.sql.ran  written only if the textarea content matches the
  //               most-recently-executed SQL (ps.lastRunSql, set by
  //               runQuery). Drives the auto-replay decision in the new
  //               mode : we re-run on switch only if the query had
  //               actually been executed in the original mode. A draft
  //               sitting in the textarea (page reload, history nav,
  //               in-flight edit) must NOT auto-execute on switch
  //               because of side-effecting queries like add_provenance.
  document.querySelectorAll('.ps-modeswitch__btn').forEach(btn => {
    btn.addEventListener('click', () => {
      carryQueryForSwitch();
    });
  });
  // Restore the carried-over query if there is one. carriedRan controls
  // auto-replay; it's set only when the carried query had actually been
  // run in the previous mode.
  const carried = sessionStorage.getItem('ps.sql');
  const carriedFromSwitch = carried != null;
  const carriedRan = sessionStorage.getItem('ps.sql.ran') === '1';
  if (carriedFromSwitch) {
    document.getElementById('request').value = carried;
    sessionStorage.removeItem('ps.sql');
    sessionStorage.removeItem('ps.sql.ran');
  }
  function carryQueryForSwitch() {
    const sql = document.getElementById('request').value;
    sessionStorage.setItem('ps.sql', sql);
    const lastRun = sessionStorage.getItem('ps.lastRunSql');
    if (sql && lastRun === sql) {
      sessionStorage.setItem('ps.sql.ran', '1');
    } else {
      sessionStorage.removeItem('ps.sql.ran');
    }
  }
  // Expose so the where→circuit jump and the database-switch handler can
  // reuse the same carry rule.
  window.ProvsqlStudio.carryQueryForSwitch = carryQueryForSwitch;
  // If the previous page asked us to preload a circuit (via "→ Circuit"
  // button on a where-mode result row), pull the UUID out now so circuit-mode
  // setup can fire it after the result table renders.
  const preloadCircuitUuid = sessionStorage.getItem('ps.preloadCircuit');
  sessionStorage.removeItem('ps.preloadCircuit');

  // GUC toggles. In where mode, where_provenance is forced on (the wrap
  // calls where_provenance(...) and would otherwise return all-empty); the
  // toggle is locked. In circuit mode, both are user-controlled.
  setupGucToggles();
  // Connection chip in the top nav: pull the live current_user /
  // current_database() once at page load, then poll every 5s so the
  // dot turns terracotta if the server stops responding (e.g. PG was
  // restarted, network blip) and back to green when it recovers. The
  // matching server-side log filter (cli.py) drops these polls from
  // the access log to keep the console quiet.
  fetchConnInfo();
  setInterval(fetchConnInfo, 5000);
  setupConfigPanel();
  setupSchemaPanel();

  // ⌘ / Ctrl+Enter submits the query form. Alt+↑/Alt+↓ steps through the
  // saved query history without opening the dropdown.
  document.getElementById('request').addEventListener('keydown', (e) => {
    if ((e.metaKey || e.ctrlKey) && e.key === 'Enter') {
      e.preventDefault();
      document.querySelector('form.wp-form').requestSubmit();
      return;
    }
    if (e.altKey && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) {
      e.preventDefault();
      stepHistory(e.key === 'ArrowUp' ? +1 : -1);
    }
  });
  setupHistoryDropdown();

  // Clear-query button in the editor gutter : wipes the textarea so the
  // user can start over without selecting + deleting the previous text.
  // The current text is pushed to history first (pushHistory dedupes
  // consecutive entries, so an already-saved query won't double up) so
  // an accidental clear is one Alt+↑ away from recovery.
  document.getElementById('clear-btn')?.addEventListener('click', () => {
    const ta = document.getElementById('request');
    if (!ta) return;
    pushHistory(ta.value);
    ta.value = '';
    ta.setSelectionRange(0, 0);
    ta.focus();
    ta.dispatchEvent(new Event('input', { bubbles: true }));
  });

  // Cancel button (sibling of the Send button, hidden by default; runQuery
  // unhides it for the duration of an in-flight POST /api/exec). Firing
  // POST /api/cancel/<id> in parallel reaches the server on a different
  // worker thread and triggers pg_cancel_backend on the running pid; the
  // original /api/exec then comes back with a 57014 the renderer surfaces
  // as the standard error banner.
  document.getElementById('cancel-btn')?.addEventListener('click', async () => {
    const btn = document.getElementById('cancel-btn');
    const id  = btn.dataset.requestId;
    if (!id) return;
    btn.disabled = true;
    try {
      await fetch(`/api/cancel/${encodeURIComponent(id)}`, { method: 'POST' });
    } catch (e) {
      // Swallow: the in-flight /api/exec will still come back, with or
      // without our cancel landing. Re-enable the button so a follow-up
      // click is possible if the first didn't make it.
      btn.disabled = false;
    }
  });

  // Expose the env that the global runQuery reads (function declarations
  // are hoisted, so the named functions below are safe to reference here
  // even though they appear later in the IIFE). This MUST happen before
  // setupWhereMode / setupCircuitMode runs, because both can auto-replay
  // a carry-over query via runQuery, and if __provsqlStudio is still
  // undefined at that point the fallback default `{mode: 'where', ...}`
  // kicks in : the where-mode wrap then fires on /circuit pages and
  // explodes on aggregation circuits with "Wrong type of gate".
  window.__provsqlStudio = {
    mode, refreshRelations, escapeHtml, escapeAttr, formatCell,
    isRightAlignedType, pushHistory,
  };

  if (mode === 'where') setupWhereMode();
  else                  setupCircuitMode();

  // The setup call has to come AFTER the SQL_KEYWORDS const declaration
  // below: function declarations are hoisted but `const` is not, so calling
  // setupSqlSyntaxHighlight() any earlier would hit the temporal dead zone
  // when refresh() invokes highlightSql() and reads SQL_KEYWORDS.

  /* ──────── SQL syntax highlight (textarea + <pre> overlay) ──────── */

  // Lightweight tokenizer: keyword / function / string / number / comment /
  // operator / identifier / whitespace. Single regex with named alternates so
  // the relative order is preserved (comments before strings before
  // identifiers). Brand-coloured via the hl-* classes in app.css.
  const SQL_KEYWORDS = new Set(([
    'select','from','where','and','or','not','in','is','null','any','some',
    'join','inner','outer','left','right','full','cross','natural','on','using',
    'group','by','order','having','distinct','all',
    'union','intersect','except','as','with','recursive',
    'insert','into','values','update','set','delete','returning',
    'create','table','view','index','schema','sequence','extension','function',
    'drop','alter','add','column','rename','to',
    'primary','key','foreign','references','unique','default','constraint','check',
    'case','when','then','else','end',
    'limit','offset','fetch','first','rows','row','only',
    'true','false','asc','desc','nulls',
    'between','like','ilike','similar','overlaps',
    'exists','cast','collate','escape',
    'begin','commit','rollback','savepoint','transaction','isolation','level',
    'grant','revoke','to','from','public','role','user',
    'if','exists','temp','temporary','unlogged','materialized',
    'analyze','explain','vacuum','copy','do','language',
    'array','within','filter','over','partition','window','range','unbounded','preceding','following','current','interval',
  ]).map(s => s.toLowerCase()));

  function escHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function highlightSql(text) {
    // Token alternates, in priority order:
    //   1 = line comment     -- ...
    //   2 = block comment    /* ... */
    //   3 = single-quoted string (with escaped '')
    //   4 = dollar-quoted string $tag$ ... $tag$
    //   5 = double-quoted identifier "..."
    //   6 = number
    //   7 = identifier
    //   8 = operator
    //   9 = whitespace (passthrough)
    const re = /(--[^\n]*)|(\/\*[\s\S]*?\*\/)|('(?:[^']|'')*'?)|(\$([A-Za-z_][A-Za-z0-9_]*)?\$[\s\S]*?\$\5\$)|("(?:[^"]|"")*"?)|(\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b)|([A-Za-z_][A-Za-z0-9_]*)|([+\-*\/<>=!,;().|&%])|(\s+)/g;
    let out = '';
    let lastIdx = 0;
    let m;
    while ((m = re.exec(text)) !== null) {
      // Anything between matches is unrecognized; emit as-is (escaped).
      if (m.index > lastIdx) out += escHtml(text.slice(lastIdx, m.index));
      lastIdx = re.lastIndex;
      if      (m[1]) out += `<span class="hl-com">${escHtml(m[1])}</span>`;
      else if (m[2]) out += `<span class="hl-com">${escHtml(m[2])}</span>`;
      else if (m[3]) out += `<span class="hl-str">${escHtml(m[3])}</span>`;
      else if (m[4]) out += `<span class="hl-str">${escHtml(m[4])}</span>`;
      else if (m[6]) out += `<span class="hl-str">${escHtml(m[6])}</span>`;
      else if (m[7]) out += `<span class="hl-num">${escHtml(m[7])}</span>`;
      else if (m[8]) {
        const w = m[8];
        if (SQL_KEYWORDS.has(w.toLowerCase())) {
          out += `<span class="hl-kw">${escHtml(w)}</span>`;
        } else {
          // Function-call heuristic: identifier immediately followed by `(`.
          const after = text.charAt(re.lastIndex);
          if (after === '(') {
            out += `<span class="hl-fn">${escHtml(w)}</span>`;
          } else {
            out += escHtml(w);
          }
        }
      }
      else if (m[9]) out += `<span class="hl-op">${escHtml(m[9])}</span>`;
      else if (m[10]) out += m[10];   // whitespace passthrough
    }
    if (lastIdx < text.length) out += escHtml(text.slice(lastIdx));
    // Trailing newline keeps the <pre>'s last-line height aligned with the
    // textarea (which always reserves space for a trailing newline).
    return out + '\n';
  }

  function setupSqlSyntaxHighlight() {
    const ta = document.getElementById('request');
    const hlPre = document.getElementById('request-hl');
    if (!ta || !hlPre) return;
    const code = hlPre.querySelector('code');

    // Restore the user's preferred textarea height across reloads and
    // mode switches. ta.style.height is the same inline style the browser
    // writes on vertical drag, so round-tripping through it avoids the
    // box-sizing drift you'd get from offsetHeight/clientHeight.
    try {
      const saved = localStorage.getItem('ps.editorHeight');
      if (saved && /^\d+(\.\d+)?px$/.test(saved)) {
        const px = parseFloat(saved);
        if (px >= 40 && px <= 4000) ta.style.height = saved;
      }
    } catch (e) { /* localStorage disabled / quota: skip */ }

    function refresh() { code.innerHTML = highlightSql(ta.value); }
    function syncScroll() {
      hlPre.scrollTop  = ta.scrollTop;
      hlPre.scrollLeft = ta.scrollLeft;
    }
    ta.addEventListener('input', refresh);
    ta.addEventListener('scroll', syncScroll);
    // Re-sync after textarea resize: pre is positioned absolutely so its
    // box follows automatically, but scroll position can drift. Same
    // observer persists the user-set height; the initial firing during
    // restore writes back the value we just read, which is harmless.
    new ResizeObserver(() => {
      syncScroll();
      const h = ta.style.height;
      if (h) {
        try { localStorage.setItem('ps.editorHeight', h); }
        catch (e) { /* skip */ }
      }
    }).observe(ta);
    refresh();
  }

  setupSqlSyntaxHighlight();

  /* ──────── Where mode ──────── */

  async function setupWhereMode() {
    await refreshRelations();
    const body = document.getElementById('result-body');
    body.addEventListener('mouseover', (e) => onResultHover(e, true));
    body.addEventListener('mouseout',  (e) => onResultHover(e, false));
    body.addEventListener('click', (e) => {
      const btn = e.target.closest('[data-jump-circuit]');
      if (!btn) return;
      // Same carry rule as the mode-switch tab : only auto-replay in the
      // new mode if the textarea still matches the last-run SQL. The
      // preloadCircuit UUID is always carried so the DAG renders
      // regardless of whether we re-run the query.
      window.ProvsqlStudio.carryQueryForSwitch();
      sessionStorage.setItem('ps.preloadCircuit', btn.dataset.jumpCircuit);
      window.location.href = '/circuit';
    });
    // Quick-nav chips at the top of the sidebar: scroll the sidebar pane
    // so the target header lands at the top. We do this explicitly rather
    // than via scrollIntoView, because scrollIntoView picks the nearest
    // scrollable ancestor and may end up scrolling the page (sticky nav
    // hides the header) instead of the sidebar's own overflow pane.
    document.getElementById('sidebar-body').addEventListener('click', (e) => {
      const btn = e.target.closest('.wp-rel-nav__btn');
      if (!btn) return;
      const target = document.getElementById(btn.dataset.target);
      const sidebar = document.getElementById('sidebar');
      if (!target || !sidebar) return;
      const offset = target.getBoundingClientRect().top
                   - sidebar.getBoundingClientRect().top
                   + sidebar.scrollTop
                   - 10;  // small breathing gap above the header
      sidebar.scrollTo({ top: Math.max(0, offset), behavior: 'smooth' });
    });
    // Auto-replay only when the carried query had actually been executed
    // in the original mode (carriedRan). Plain reloads, history nav, and
    // unrun drafts must NOT auto-execute, because re-running a
    // side-effecting query (typically add_provenance) on switch is
    // dangerous.
    if (carriedRan && document.getElementById('request').value.trim()) {
      runQuery({ preventDefault() {} });
    }
  }

  // Last fetched /api/relations response, kept so the "Input gates only"
  // toggle can re-render without an extra round-trip.
  let _lastRelations = [];
  const INPUT_ONLY_KEY = 'ps.where.inputOnly';
  function inputOnlyEnabled() {
    // Default ON : the typical use case is inspecting the source tables
    // of a query, which are by construction input-gated.
    const v = sessionStorage.getItem(INPUT_ONLY_KEY);
    return v === null ? true : v === '1';
  }
  function setInputOnly(on) {
    sessionStorage.setItem(INPUT_ONLY_KEY, on ? '1' : '0');
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
    _lastRelations = relations;
    renderRelations(relations);
  }

  function renderRelations(relations) {
    const body = document.getElementById('sidebar-body');
    if (!relations.length) {
      body.innerHTML = '<p style="opacity:.7">No provenance-tagged relations. Try <code>SELECT add_provenance(\'mytable\')</code>.</p>';
      return;
    }
    // Apply the "input gates only" filter : a relation passes when its
    // first row's provsql token is an input gate. Empty relations
    // (first_gate_type == null) are dropped under the filter, since
    // there's no input row to point at; rendering them would be noise.
    const inputOnly = inputOnlyEnabled();
    const totalCount = relations.length;
    const filtered = inputOnly
      ? relations.filter(r => r.first_gate_type === 'input')
      : relations;
    const hiddenCount = totalCount - filtered.length;
    const toggleHtml = `
      <div class="wp-rel-filter">
        <label class="wp-rel-filter__label" title="Hide relations whose provsql tokens are derived gates rather than input leaves">
          <input type="checkbox" id="opt-input-only"${inputOnly ? ' checked' : ''}>
          Input gates only${hiddenCount > 0 ? ` <span class="wp-rel-filter__hint">(${hiddenCount} hidden)</span>` : ''}
        </label>
      </div>`;
    if (!filtered.length) {
      body.innerHTML = toggleHtml +
        '<p style="opacity:.7">All provenance-tracked relations carry derived gates. Untick the filter to show them.</p>';
      bindInputOnlyToggle();
      return;
    }
    relations = filtered;
    // Quick-nav chips at the top: one per relation, click scrolls the
    // matching section into view inside the sidebar's own scroll pane.
    const navHtml = relations.length > 1
      ? `<nav class="wp-rel-nav">${
          relations.map(rel =>
            `<button type="button" class="wp-rel-nav__btn" data-target="${escapeAttr(headerId(rel.regclass))}">${escapeHtml(rel.regclass)}</button>`
          ).join('')
        }</nav>`
      : '';
    body.innerHTML = toggleHtml + navHtml + relations.map(rel => {
      // Skip the rewriter-added `provsql` UUID column when displaying; its
      // value is already exposed as the row id (used for the hover-highlight).
      // where_provenance numbers cells by user-column position (1-indexed,
      // ignoring provsql), which matches the original i+1 since provsql sits
      // at the end of the column list.
      const visible = rel.columns
        .map((c, i) => ({ c, i }))
        .filter(({ c }) => c.name !== 'provsql');
      // When list_relations capped the SELECT, surface the cap inline
      // ("100 of ~50000 tuples") so the user knows the sidebar isn't a
      // full mirror of the table. The "~" reflects pg_class.reltuples
      // being a planner estimate, not an exact count.
      const tuples = (() => {
        const shown = rel.rows.length;
        if (rel.truncated) {
          const total = rel.estimated_rows;
          if (total != null && total > shown) {
            return `${shown} of ~${total} tuples`;
          }
          return `${shown}+ tuples (capped)`;
        }
        return `${shown} tuples`;
      })();
      return `
      <section class="wp-relation" id="${escapeAttr(sectionId(rel.regclass))}">
        <header class="wp-relation__hdr" id="${escapeAttr(headerId(rel.regclass))}">
          <h3 class="wp-relation__name">${escapeHtml(rel.regclass)}</h3>
          <span class="wp-relation__meta">${tuples} · ${visible.length} cols</span>
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
                    return `<td id="${escapeAttr(id)}" class="${cls.trim()}">${formatCell(r.values[i], c.type_name)}</td>`;
                  }).join('')}
                </tr>
              `).join('')}
            </tbody>
          </table>
        </div>
      </section>`;
    }).join('');
    bindInputOnlyToggle();
  }

  // Wire the "Input gates only" checkbox emitted by renderRelations. The
  // toggle persists in sessionStorage and re-renders from the cached
  // /api/relations response : no extra round-trip just to flip the filter.
  function bindInputOnlyToggle() {
    const cb = document.getElementById('opt-input-only');
    if (!cb) return;
    cb.addEventListener('change', () => {
      setInputOnly(cb.checked);
      renderRelations(_lastRelations);
    });
  }

  // Stable, CSS-safe id for a relation's section (avoids periods, quotes, ...).
  function sectionId(regclass) {
    return 'rel-' + String(regclass).replace(/[^A-Za-z0-9_]/g, '_');
  }
  // Companion id for the relation's header element (table name + meta).
  function headerId(regclass) {
    return 'hdr-' + String(regclass).replace(/[^A-Za-z0-9_]/g, '_');
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

  /* ──────── query history ──────── */

  const HISTORY_KEY = 'ps.history';
  const HISTORY_CAP = 50;
  let _historyCursor = -1;  // -1 = current draft; 0..N-1 = nth-most-recent saved entry
  let _historyDriving = false;  // suppress the cursor reset when WE set ta.value

  function loadHistory() {
    try {
      const raw = localStorage.getItem(HISTORY_KEY);
      const arr = raw ? JSON.parse(raw) : [];
      return Array.isArray(arr) ? arr : [];
    } catch {
      return [];
    }
  }
  function saveHistory(arr) {
    try {
      localStorage.setItem(HISTORY_KEY, JSON.stringify(arr.slice(0, HISTORY_CAP)));
    } catch {}
  }
  function pushHistory(sql) {
    const trimmed = String(sql || '').trim();
    if (!trimmed) return;
    const arr = loadHistory();
    if (arr.length && arr[0] === trimmed) return;  // skip exact-duplicate consecutive entries
    arr.unshift(trimmed);
    saveHistory(arr);
  }

  function stepHistory(direction) {
    const arr = loadHistory();
    if (!arr.length) return;
    const ta = document.getElementById('request');
    if (_historyCursor === -1) _draft = ta.value;
    const valueAt = (i) => i === -1 ? (_draft || '') : arr[i];
    const current = ta.value;
    // Skip entries identical to what's already in the textarea so Alt+↑/↓
    // always produces a visible change (history can contain consecutive
    // duplicates separated by other queries, and the draft can match arr[0]).
    let next = _historyCursor + direction;
    while (next >= -1 && next < arr.length && valueAt(next) === current) {
      next += direction;
    }
    if (next < -1 || next >= arr.length) return;
    _historyCursor = next;
    _historyDriving = true;
    ta.value = valueAt(next);
    ta.dispatchEvent(new Event('input'));  // refresh syntax highlight
    _historyDriving = false;
  }
  let _draft = '';

  // Reset history-cursor when the USER edits the textarea (so the next
  // Alt+↑ starts from the most-recent entry again). When we set ta.value
  // ourselves from stepHistory, the synthetic input event must NOT reset
  // the cursor : that's what _historyDriving guards against.
  document.getElementById('request').addEventListener('input', () => {
    if (!_historyDriving) _historyCursor = -1;
  });

  function setupHistoryDropdown() {
    const btn  = document.getElementById('history-btn');
    const menu = document.getElementById('history-menu');
    if (!btn || !menu) return;

    function renderMenu() {
      const arr = loadHistory();
      if (!arr.length) {
        menu.innerHTML = '<li class="wp-history__empty">No saved queries yet.</li>';
        return;
      }
      const oneLine = (s) => s.replace(/\s+/g, ' ').trim();
      menu.innerHTML = arr.map((sql, i) =>
        `<li role="option" data-i="${i}" title="${escapeAttr(sql)}">${escapeHtml(oneLine(sql).slice(0, 200))}</li>`
      ).join('') + '<li class="wp-history__clear" data-clear="1">Clear history</li>';
    }
    function open() {
      renderMenu();
      menu.hidden = false;
      btn.setAttribute('aria-expanded', 'true');
    }
    function close() {
      menu.hidden = true;
      btn.setAttribute('aria-expanded', 'false');
    }
    btn.addEventListener('click', (e) => {
      e.stopPropagation();
      if (menu.hidden) open(); else close();
    });
    document.addEventListener('click', (e) => {
      if (!menu.hidden && !menu.contains(e.target) && e.target !== btn) close();
    });
    menu.addEventListener('click', (e) => {
      const li = e.target.closest('li');
      if (!li) return;
      if (li.dataset.clear) {
        saveHistory([]);
        renderMenu();
        return;
      }
      const arr = loadHistory();
      const i = Number(li.dataset.i);
      const sql = arr[i];
      if (sql == null) return;
      const ta = document.getElementById('request');
      ta.value = sql;
      ta.dispatchEvent(new Event('input'));
      _historyCursor = i;
      close();
      ta.focus();
    });
  }

  let _currentConn = null;

  async function fetchConnInfo() {
    const el  = document.getElementById('conn-info');
    // The trigger is the wrapping <button id="conn-dot"> (plug icon +
    // status dot). We set the tooltip on the button so hovering the
    // icon also surfaces it; the inner span carries only the colour
    // class (.is-offline / connected).
    const trigger = document.getElementById('conn-dot');
    const dot = document.querySelector('.wp-nav__dot');
    if (!el) return;
    let reason = '';
    try {
      const resp = await fetch('/api/conn');
      if (!resp.ok) {
        // /api/conn responds with 503 + JSON {error, reason} when the
        // pool can't reach PG. Pull the reason out for the dot tooltip.
        try {
          const body = await resp.json();
          reason = body.reason || body.error || `HTTP ${resp.status}`;
        } catch (_) {
          reason = `HTTP ${resp.status}`;
        }
        throw new Error(reason);
      }
      const c = await resp.json();
      _currentConn = c;
      // Surface the numeric server version on the global so circuit.js
      // can gate version-specific dropdown options (currently the
      // `temporal` compiled semiring, which depends on tstzmultirange
      // and is therefore PG14+ only). Re-syncs the eval strip if it has
      // already been initialised, since /api/conn lands asynchronously.
      window.ProvsqlStudio.serverVersion = Number(c.server_version) || 0;
      if (window.ProvsqlStudio.syncCompiledSemiringAvailability) {
        window.ProvsqlStudio.syncCompiledSemiringAvailability();
      }
      el.textContent = `${c.user}@${c.database}`;
      if (c.host) el.title = `host: ${c.host}`;
      if (dot) dot.classList.remove('is-offline');
      if (trigger) {
        // Surface only the server endpoint. The DB name is already
        // shown right next to the dot in the switcher chip, so we
        // don't repeat it; user@host:port is what the chip can't say.
        const where = c.host
          ? (c.port ? `${c.host}:${c.port}` : c.host)
          : 'local socket';
        trigger.title = `connected to ${c.user}@${where} (click to change)`;
      }
      // No DSN was given on the CLI and no PG* env hinted at a DB, so
      // the server fell back to the postgres maintenance DB. Show a
      // dismissible banner pointing the user at the switcher; once
      // they pick a real DB the flag clears server-side.
      _renderDbAutoHint(!!c.db_is_auto);
      // Render search_path with `provsql` shown as a locked chip so the
      // user can tell at a glance which segment is enforced by Studio.
      // The compose helper on the server already pinned provsql to the
      // end; we just style that segment.
      const sp = document.getElementById('searchpath-val');
      if (sp) {
        const path = c.search_path || '';
        const parts = path.split(',').map(s => s.trim()).filter(Boolean);
        sp.innerHTML = parts.map(p => {
          if (p === 'provsql' || p === '"provsql"') {
            return `<span class="wp-card__sp-locked" title="ProvSQL Studio appends “provsql” to your search_path so its helper functions are reachable as a fallback for unqualified names."><i class="fas fa-lock" aria-hidden="true"></i> provsql</span>`;
          }
          return escapeHtml(p);
        }).join(', ');
      }
    } catch (e) {
      // Don't clobber _currentConn or the displayed identity on a
      // transient blip: the chip keeps showing the last-known db name
      // (so the dropdown still works); only the dot turns red and the
      // tooltip explains.
      if (dot) dot.classList.add('is-offline');
      if (trigger) {
        trigger.title = `Cannot reach the database: ${reason || e.message || 'cannot connect to PostgreSQL'}`;
      }
    }
    setupDbSwitcher();
  }

  function setupDbSwitcher() {
    const btn  = document.getElementById('conn-info');
    const menu = document.getElementById('dbmenu');
    if (!btn || !menu || btn.dataset.bound === '1') return;
    btn.dataset.bound = '1';

    async function openMenu() {
      btn.setAttribute('aria-expanded', 'true');
      menu.hidden = false;
      menu.innerHTML = '<li style="opacity:.6">loading…</li>';
      try {
        const resp = await fetch('/api/databases');
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        const dbs = await resp.json();
        const cur = _currentConn ? _currentConn.database : null;
        menu.innerHTML = dbs.map(name =>
          `<li role="option" data-db="${escapeAttr(name)}" `
          + `class="${name === cur ? 'is-current' : ''}">${escapeHtml(name)}</li>`
        ).join('') || '<li style="opacity:.6">(no accessible databases)</li>';
      } catch (e) {
        menu.innerHTML = `<li style="color:var(--terracotta-500)">Failed: ${escapeHtml(e.message)}</li>`;
      }
    }
    function closeMenu() {
      btn.setAttribute('aria-expanded', 'false');
      menu.hidden = true;
    }
    btn.addEventListener('click', (e) => {
      e.stopPropagation();
      if (menu.hidden) openMenu(); else closeMenu();
    });
    document.addEventListener('click', (e) => {
      if (!menu.hidden && !menu.contains(e.target) && e.target !== btn) closeMenu();
    });
    menu.addEventListener('click', async (e) => {
      const li = e.target.closest('li[data-db]');
      if (!li) return;
      const target = li.dataset.db;
      if (_currentConn && target === _currentConn.database) {
        closeMenu();
        return;
      }
      menu.innerHTML = '<li style="opacity:.6">switching…</li>';
      let resp;
      try {
        resp = await fetch('/api/conn', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ database: target }),
        });
      } catch (err) {
        closeMenu();
        return;
      }
      if (!resp.ok) {
        closeMenu();
        return;
      }
      // Reloading is the cleanest way to reset every cached relation list,
      // result table, circuit cache, etc., to the new database's contents.
      // The previous query is meaningless against the new database (table
      // names rarely match), so we wipe the textarea : push the current
      // text to history first so the user can recover it via Alt+↑.
      const ta = document.getElementById('request');
      if (ta) pushHistory(ta.value);
      sessionStorage.removeItem('ps.sql');
      sessionStorage.removeItem('ps.sql.ran');
      sessionStorage.removeItem('ps.lastRunSql');
      if (ta) ta.value = '';
      window.location.reload();
    });
  }

  /* ──────── Config popover (provsql.active + provsql.verbose_level) ──────── */

  function setupConfigPanel() {
    const btn    = document.getElementById('config-btn');
    const panel  = document.getElementById('config-panel');
    const active = document.getElementById('cfg-active');
    const verb   = document.getElementById('cfg-verbose');
    const status = document.getElementById('cfg-status');
    if (!btn || !panel || !active || !verb) return;

    let loaded = false;

    const verbOut = document.getElementById('cfg-verbose-out');
    const depth   = document.getElementById('cfg-depth');
    const depthOut = document.getElementById('cfg-depth-out');
    const sidebarRows = document.getElementById('cfg-sidebar-rows');
    const resultRows = document.getElementById('cfg-result-rows');
    const circuitNodes = document.getElementById('cfg-circuit-nodes');
    const timeout = document.getElementById('cfg-timeout');
    const sp      = document.getElementById('cfg-search-path');

    async function loadConfig() {
      try {
        const resp = await fetch('/api/config');
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        const cfg = await resp.json();
        const eff = cfg.effective || {};
        active.checked = (eff['provsql.active'] || 'on') !== 'off';
        verb.value     = eff['provsql.verbose_level'] || '0';
        if (verbOut) verbOut.textContent = verb.value;
        const opts = cfg.options || {};
        if (depth && opts.max_circuit_depth != null) {
          depth.value = String(opts.max_circuit_depth);
          if (depthOut) depthOut.textContent = depth.value;
        }
        if (sidebarRows && opts.max_sidebar_rows != null) {
          sidebarRows.value = String(opts.max_sidebar_rows);
        }
        if (resultRows && opts.max_result_rows != null) {
          resultRows.value = String(opts.max_result_rows);
        }
        if (circuitNodes && opts.max_circuit_nodes != null) {
          circuitNodes.value = String(opts.max_circuit_nodes);
        }
        if (timeout && opts.statement_timeout_seconds != null) {
          timeout.value = String(opts.statement_timeout_seconds);
        }
        if (sp && opts.search_path != null) {
          sp.value = opts.search_path;
        }
        loaded = true;
        showStatus('');
      } catch (e) {
        showStatus(`Failed to load: ${e.message}`, true);
      }
    }
    function showStatus(msg, isErr) {
      if (!msg) {
        status.hidden = true;
        status.classList.remove('is-error');
        return;
      }
      status.textContent = msg;
      status.classList.toggle('is-error', !!isErr);
      status.hidden = false;
    }
    async function setGuc(name, value) {
      try {
        const resp = await fetch('/api/config', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ key: name, value: String(value) }),
        });
        if (!resp.ok) {
          const err = await resp.json().catch(() => ({}));
          throw new Error(err.error || `HTTP ${resp.status}`);
        }
        showStatus(`Saved: ${name} = ${value}`);
        // Auto-clear the status after a couple of seconds.
        clearTimeout(setGuc._t);
        setGuc._t = setTimeout(() => showStatus(''), 2000);
      } catch (e) {
        showStatus(e.message, true);
      }
    }

    function open() {
      panel.hidden = false;
      btn.setAttribute('aria-expanded', 'true');
      if (!loaded) loadConfig();
    }
    function close() {
      panel.hidden = true;
      btn.setAttribute('aria-expanded', 'false');
    }
    btn.addEventListener('click', (e) => {
      e.stopPropagation();
      if (panel.hidden) open(); else close();
    });
    document.addEventListener('click', (e) => {
      if (!panel.hidden && !panel.contains(e.target) && e.target !== btn) close();
    });
    active.addEventListener('change', () => {
      setGuc('provsql.active', active.checked ? 'on' : 'off');
    });
    // Live-update the value display as the slider drags; only POST on
    // release (`change`) so we don't hammer /api/config every step.
    verb.addEventListener('input', () => {
      if (verbOut) verbOut.textContent = verb.value;
    });
    verb.addEventListener('change', () => {
      const n = Math.max(0, Math.min(100, parseInt(verb.value || '0', 10) || 0));
      verb.value = String(n);
      if (verbOut) verbOut.textContent = verb.value;
      setGuc('provsql.verbose_level', n);
    });

    if (depth) {
      depth.addEventListener('input', () => {
        if (depthOut) depthOut.textContent = depth.value;
      });
      depth.addEventListener('change', () => {
        const n = Math.max(1, Math.min(50, parseInt(depth.value || '8', 10) || 8));
        depth.value = String(n);
        if (depthOut) depthOut.textContent = depth.value;
        setGuc('max_circuit_depth', n);
      });
    }
    if (sidebarRows) {
      sidebarRows.addEventListener('change', async () => {
        const n = Math.max(1, Math.min(5000, parseInt(sidebarRows.value || '100', 10) || 100));
        sidebarRows.value = String(n);
        await setGuc('max_sidebar_rows', n);
        // Refresh the where-mode sidebar so the new cap takes effect
        // immediately rather than on the next mode entry.
        if (document.body.classList.contains('mode-where')) {
          refreshRelations();
        }
      });
    }
    if (resultRows) {
      resultRows.addEventListener('change', () => {
        const n = Math.max(1, Math.min(100000, parseInt(resultRows.value || '1000', 10) || 1000));
        resultRows.value = String(n);
        setGuc('max_result_rows', n);
      });
    }
    if (circuitNodes) {
      circuitNodes.addEventListener('change', () => {
        const n = Math.max(10, Math.min(10000, parseInt(circuitNodes.value || '200', 10) || 200));
        circuitNodes.value = String(n);
        setGuc('max_circuit_nodes', n);
      });
    }
    if (timeout) {
      timeout.addEventListener('change', () => {
        const n = Math.max(1, Math.min(3600, parseInt(timeout.value || '30', 10) || 30));
        timeout.value = String(n);
        setGuc('statement_timeout_seconds', n);
      });
    }
    if (sp) {
      // Persist on blur and on Enter so the user's typing isn't saved
      // mid-edit. The trailing `, provsql` is enforced server-side; the
      // user only types the leading schemas they care about.
      const commit = () => {
        const v = (sp.value || '').trim();
        sp.value = v;
        setGuc('search_path', v);
        // Refresh the search_path readout under the query box so it
        // reflects the new value immediately rather than on the next
        // 5s connection-info poll.
        fetchConnInfo();
      };
      sp.addEventListener('blur', commit);
      sp.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { e.preventDefault(); sp.blur(); }
      });
    }

    // Probability-display decimals : pure UI setting (no server roundtrip),
    // persisted in localStorage so the choice survives reloads. The eval
    // strip's float branch reads getProbDecimals() at render time.
    const probDec = document.getElementById('cfg-prob-decimals');
    if (probDec) {
      probDec.value = String(getProbDecimals());
      probDec.addEventListener('change', () => {
        const n = Math.max(0, Math.min(15, parseInt(probDec.value || '4', 10)));
        probDec.value = String(n);
        try { localStorage.setItem('ps.probDecimals', String(n)); } catch {}
      });
    }
  }

  // Default rounding for probability results. Configurable via the Config
  // panel's "Probability decimals" field; falls back to 4 when unset.
  function getProbDecimals() {
    const raw = (() => { try { return localStorage.getItem('ps.probDecimals'); } catch { return null; } })();
    const n = parseInt(raw || '', 10);
    return Number.isFinite(n) && n >= 0 && n <= 15 ? n : 4;
  }
  window.ProvsqlStudio.getProbDecimals = getProbDecimals;

  // Schema browser: top-nav button opening a popover that lists every
  // SELECT-able relation grouped by schema, with a search box and click
  // to insert "schema.relation" at the cursor in the textarea. The
  // /api/schema fetch is lazy (first open) and the result is cached for
  // the page session: if the schema actually changes (CREATE TABLE etc.),
  // a page reload re-fetches.
  function setupSchemaPanel() {
    const btn    = document.getElementById('schema-btn');
    const panel  = document.getElementById('schema-panel');
    const body   = document.getElementById('schema-body');
    const search = document.getElementById('schema-search');
    if (!btn || !panel || !body || !search) return;

    let loaded = false;
    let entries = [];
    // True when every relation lives in the same schema, so we can insert
    // the bare table name on click instead of the qualified schema.table
    // form. Multi-schema databases keep the qualified form to disambiguate.
    let singleSchema = true;

    async function load() {
      body.innerHTML = '<p class="wp-schema__empty">Loading…</p>';
      try {
        const resp = await fetch('/api/schema');
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        entries = await resp.json();
        singleSchema = new Set(entries.map(r => r.schema)).size <= 1;
        loaded = true;
        // Successful reload : clear the dirty flag so subsequent opens
        // reuse the cache until the next exec.
        if (window.ProvsqlStudio?.metadata) {
          window.ProvsqlStudio.metadata.schemaDirty = false;
        }
        render();
      } catch (e) {
        body.innerHTML =
          `<p class="wp-schema__empty">Failed to load schema: ${escapeHtml(e.message || String(e))}</p>`;
      }
    }

    function render() {
      const q = (search.value || '').trim().toLowerCase();
      const filtered = q
        ? entries.filter(r =>
            r.schema.toLowerCase().includes(q) ||
            r.table.toLowerCase().includes(q)  ||
            // Skip the bookkeeping `provsql` column so search results
            // match the visible column list : typing "provsql" should not
            // match every provenance-tracked relation through this hidden
            // column (use the PROV pill for that).
            r.columns.some(c => c.name !== 'provsql' && c.name.toLowerCase().includes(q))
          )
        : entries;
      if (filtered.length === 0) {
        body.innerHTML = '<p class="wp-schema__empty">No matches.</p>';
        return;
      }
      const bySchema = new Map();
      for (const r of filtered) {
        if (!bySchema.has(r.schema)) bySchema.set(r.schema, []);
        bySchema.get(r.schema).push(r);
      }
      let html = '';
      for (const [schemaName, rels] of bySchema) {
        html += '<div class="wp-schema__group">';
        html += `<h5 class="wp-schema__schema">${escapeHtml(schemaName)}</h5>`;
        for (const r of rels) {
          const qname  = `${r.schema}.${r.table}`;
          const insert = singleSchema ? r.table : qname;
          // Hide the bookkeeping `provsql` uuid column from the user-visible
          // column list : its presence is what the PROV pill already signals.
          const visibleCols = r.columns.filter(c => c.name !== 'provsql');
          // Provenance-tracked tables (not mappings, not views) can have
          // a provenance mapping created on any of their columns. Render
          // each column name as a clickable span so the user can prefill
          // the corresponding `create_provenance_mapping(...)` call.
          const canMap = r.has_provenance && !r.is_mapping && r.kind === 'table';
          const cols   = visibleCols.map(c => {
            if (canMap) {
              return `<span class="wp-schema__col" data-action="create-mapping"`
                + ` data-qname="${escapeAttr(qname)}"`
                + ` data-table="${escapeAttr(r.table)}"`
                + ` data-col="${escapeAttr(c.name)}"`
                + ` title="Click to create a provenance mapping on ${escapeAttr(c.name)}"`
                + `>${escapeHtml(c.name)}</span>`;
            }
            return escapeHtml(c.name);
          }).join(', ');
          // Mapping is the more specific classification: a mapping view
          // typically also carries an implicit provsql column (the planner
          // re-injects it for any view that selects from a provenance-tracked
          // table), but tagging it as both is noisy. Show only "mapping".
          const showProv = r.has_provenance && !r.is_mapping;
          const provCls   = showProv ? ' wp-schema__rel--prov' : '';
          const provBadge = showProv
            ? `<span class="wp-schema__rel-prov" title="Provenance-tracked (provsql uuid column)">prov</span>`
            : '';
          const mapBadge = r.is_mapping
            ? `<span class="wp-schema__rel-map" title="Provenance mapping (value + provenance uuid columns)">mapping</span>`
            : '';
          const titleSuffix =
              (showProv ? ' · provenance-tracked' : '')
            + (r.is_mapping ? ' · provenance mapping' : '');
          // Column count for the tooltip mirrors what's actually listed
          // (provsql column hidden when has_provenance), so the tooltip
          // and the comma-separated list don't disagree.
          const visibleCount = r.has_provenance ? r.columns.length - 1 : r.columns.length;
          // add/remove_provenance only target plain tables (the underlying
          // ALTER TABLE rejects views/matviews); mappings already serve a
          // separate purpose so we don't offer the toggle on them.
          const canAddRemove = r.kind === 'table' && !r.is_mapping;
          let actions = '';
          if (canAddRemove) {
            if (r.has_provenance) {
              actions =
                `<button type="button" class="wp-schema__rel-action" `
                + `data-action="remove-prov" data-qname="${escapeAttr(qname)}" `
                + `title="Insert SELECT remove_provenance('${escapeAttr(qname)}');">`
                + `<i class="fas fa-minus"></i> prov</button>`;
            } else {
              actions =
                `<button type="button" class="wp-schema__rel-action" `
                + `data-action="add-prov" data-qname="${escapeAttr(qname)}" `
                + `title="Insert SELECT add_provenance('${escapeAttr(qname)}');">`
                + `<i class="fas fa-plus"></i> prov</button>`;
            }
          }
          // Outer is a div with role=button so the inner action buttons can
          // be real <button> elements (nested buttons aren't valid HTML).
          html +=
            `<div class="wp-schema__rel${provCls}" role="button" tabindex="0"`
            + ` data-qname="${escapeAttr(insert)}"`
            + ` title="${escapeAttr(qname)}: ${visibleCount} column${visibleCount === 1 ? '' : 's'}${titleSuffix}">`
            + `<span class="wp-schema__rel-name">${escapeHtml(r.table)}</span>`
            + `<span class="wp-schema__rel-kind">${escapeHtml(r.kind)}</span>`
            + provBadge
            + mapBadge
            + (actions ? `<span class="wp-schema__rel-actions">${actions}</span>` : '');
          if (cols) {
            // `cols` is already escaped per-column inside the map above
            // (it's a mix of HTML spans and escaped text), so don't double-escape.
            html += `<span class="wp-schema__cols">${cols}</span>`;
          }
          html += `</div>`;
        }
        html += '</div>';
      }
      body.innerHTML = html;
    }

    function insertAtCursor(text) {
      const ta = document.getElementById('request');
      if (!ta) return;
      const start = ta.selectionStart != null ? ta.selectionStart : ta.value.length;
      const end   = ta.selectionEnd   != null ? ta.selectionEnd   : ta.value.length;
      ta.value = ta.value.slice(0, start) + text + ta.value.slice(end);
      const newPos = start + text.length;
      ta.setSelectionRange(newPos, newPos);
      ta.focus();
      // Notify the syntax-highlight overlay (it listens for `input`).
      ta.dispatchEvent(new Event('input', { bubbles: true }));
    }

    // Used by the per-relation action buttons (add/remove_provenance):
    // they generate a complete standalone query, so blow the previous
    // textarea content away rather than concatenate.
    function replaceQuery(text) {
      const ta = document.getElementById('request');
      if (!ta) return;
      ta.value = text;
      ta.setSelectionRange(text.length, text.length);
      ta.focus();
      ta.dispatchEvent(new Event('input', { bubbles: true }));
    }

    body.addEventListener('click', (e) => {
      // Action elements (add/remove_provenance row buttons,
      // create-mapping column spans) replace the textarea with a complete
      // SELECT call so the user can review and run it directly. Match
      // these first so they don't fall through to the row-level qname
      // insert.
      const action = e.target.closest('[data-action]');
      if (action) {
        const a = action.dataset.action;
        const q = action.dataset.qname;
        let sql;
        if (a === 'add-prov' && q) {
          sql = `SELECT add_provenance('${q}');`;
        } else if (a === 'remove-prov' && q) {
          sql = `SELECT remove_provenance('${q}');`;
        } else if (a === 'create-mapping' && q && action.dataset.col) {
          const col   = action.dataset.col;
          const table = action.dataset.table || q;
          // Default mapping name `<table>_<col>_mapping` is a sensible
          // starting point; the user can tweak it in the textarea before
          // hitting Send.
          const mname = `${table}_${col}_mapping`;
          sql = `SELECT create_provenance_mapping('${mname}', '${q}', '${col}');`;
        }
        if (sql) {
          replaceQuery(sql);
          close();
        }
        return;
      }
      const rel = e.target.closest('.wp-schema__rel');
      if (!rel || !rel.dataset.qname) return;
      insertAtCursor(rel.dataset.qname);
      close();
    });
    // The relation row is now a div role=button (so the inner action
    // <button>s nest validly). Wire Enter/Space so keyboard users can
    // still activate the row insert.
    body.addEventListener('keydown', (e) => {
      if (e.key !== 'Enter' && e.key !== ' ') return;
      const rel = e.target.closest('.wp-schema__rel');
      if (!rel || rel !== e.target || !rel.dataset.qname) return;
      e.preventDefault();
      insertAtCursor(rel.dataset.qname);
      close();
    });
    search.addEventListener('input', () => { if (loaded) render(); });
    search.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') close();
    });

    function open() {
      panel.hidden = false;
      btn.setAttribute('aria-expanded', 'true');
      // Re-fetch on open if either we never loaded, or an exec since the
      // last load may have changed the schema (the dirty flag is set by
      // runQuery and by the toolbar refresh button).
      const dirty = window.ProvsqlStudio?.metadata?.schemaDirty;
      if (!loaded || dirty) load();
      setTimeout(() => search.focus(), 0);
    }
    function close() {
      panel.hidden = true;
      btn.setAttribute('aria-expanded', 'false');
    }
    btn.addEventListener('click', (e) => {
      e.stopPropagation();
      if (panel.hidden) open(); else close();
    });
    document.addEventListener('click', (e) => {
      if (!panel.hidden && !panel.contains(e.target) && e.target !== btn) close();
    });
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape' && !panel.hidden) close();
    });

    // Toolbar refresh button. Forces a reload of the three metadata
    // caches; schema reloads in place if its panel is open, the eval-
    // strip caches reload lazily on next dropdown open via their dirty
    // flags. Also briefly spins the icon so the user sees feedback even
    // when the panel is closed.
    const refreshBtn = document.getElementById('metadata-refresh-btn');
    if (refreshBtn) {
      refreshBtn.addEventListener('click', () => {
        if (window.ProvsqlStudio?.metadata) {
          window.ProvsqlStudio.metadata.invalidateAll();
        }
        const icon = refreshBtn.querySelector('i');
        if (icon) {
          icon.classList.add('fa-spin');
          setTimeout(() => icon.classList.remove('fa-spin'), 600);
        }
        if (!panel.hidden) load();
      });
    }
  }

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
    // Coming from where mode's per-row "→ Circuit" jump: the carried UUID
    // was minted by a where-provenance wrap, so the same query must run
    // with where_provenance on here for the resulting circuit to contain
    // the project/eq gates the user is trying to inspect. Force the
    // toggle on (the user can untick it for follow-up runs); the change
    // is programmatic so it doesn't fire `change` and doesn't get
    // persisted to sessionStorage.
    if (carry) {
      const wp = document.getElementById('opt-where-prov');
      if (wp) wp.checked = true;
    }
    // Load circuit.js so its init() can wire the toolbar buttons (zoom,
    // show-uuids). loadCircuit() also calls this, but only
    // when a circuit is being rendered : without the unconditional load
    // here, a circuit-mode page that just runs a query (no carry, no
    // immediate circuit fetch) would leave the toolbar buttons unbound.
    //
    // Microtask deferral: `ensureCircuitLib` closes over
    // `_circuitLibPromise`, which is declared with `let` further down in
    // this IIFE. setupCircuitMode runs synchronously during IIFE eval,
    // so calling it directly here would hit a TDZ. Queueing as a
    // microtask defers the call until after the IIFE returns, by which
    // time the binding is initialised. The function is idempotent (it
    // caches the promise), so the later loadCircuit() callers piggyback.
    queueMicrotask(ensureCircuitLib);

    // Auto-replay only when the carried query had actually been executed
    // (carriedRan), or when we arrived via a where→circuit jump (carry):
    // the jump implies a successful query run in where mode whose row
    // the user clicked. On plain reload / unrun draft, do NOT re-execute
    // (side-effects like add_provenance must not fire on their own).
    const shouldReplay =
      (carriedRan || carry) && document.getElementById('request').value.trim();
    if (shouldReplay) {
      runQuery({ preventDefault() {} }).then(() => {
        if (carry) loadCircuit(carry);
      });
    } else if (carry) {
      // No query but a preload UUID: render the circuit directly.
      loadCircuit(carry);
    }
  }

  async function loadCircuit(uuid, opts) {
    await ensureCircuitLib();
    window.ProvsqlCircuit.showLoading();
    const depthArg = opts && Number.isFinite(opts.depth) ? opts.depth : null;
    const url = `/api/circuit/${encodeURIComponent(uuid)}`
              + (depthArg != null ? `?depth=${depthArg}` : '');
    let resp;
    try {
      resp = await fetch(url);
    } catch (e) {
      window.ProvsqlCircuit.showError(`Network error: ${e.message}`);
      return;
    }
    if (!resp.ok) {
      const err = await resp.json().catch(() => ({}));
      // 413: structured "circuit too large" payload. Render the
      // actionable banner with a "Render at depth N-1" retry button
      // that re-fires loadCircuit at a lower depth.
      if (resp.status === 413 && err && err.error === 'circuit too large') {
        // Pass rootUuid so the eval strip can still target the
        // unrendered circuit: evaluation only needs the token, not a
        // displayed DAG.
        window.ProvsqlCircuit.showTooLarge(
          err,
          (lowerDepth) => loadCircuit(uuid, { depth: lowerDepth }),
          { rootUuid: uuid },
        );
        return;
      }
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
      <div class="cv-toolbar" role="toolbar">
        <button class="cv-tool" id="tool-zoom-out" title="Zoom out"><i class="fas fa-search-minus"></i></button>
        <button class="cv-tool" id="tool-zoom-fit" title="Fit to screen"><i class="fas fa-expand"></i></button>
        <button class="cv-tool" id="tool-zoom-in" title="Zoom in"><i class="fas fa-search-plus"></i></button>
        <span class="cv-tool__sep"></span>
        <button class="cv-tool" id="tool-reset-layout" title="Reset node positions (undo any drag-to-move)"><i class="fas fa-undo"></i></button>
        <button class="cv-tool cv-tool--toggle" id="tool-show-uuids" aria-pressed="false" title="Show UUIDs"><i class="fas fa-fingerprint"></i></button>
        <button class="cv-tool cv-tool--toggle" id="tool-fullscreen" aria-pressed="false" title="Fullscreen circuit (Esc to exit)"><i class="fas fa-expand-arrows-alt"></i></button>
        <span id="circuit-title" hidden>Provenance Circuit</span>
        <span class="cv-toolbar__info" id="circuit-sub">Click a UUID cell to render.</span>
      </div>
      <div class="cv-canvas" id="canvas">
        <div class="cv-banner" id="cv-banner" hidden></div>
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
            <a class="wp-help"
               href="https://provsql.org/docs/user/studio.html#studio-circuit-inspector"
               target="_blank" rel="noopener"
               title="Inspector: gate-specific metadata. Input/update gates expose probability with click-to-edit."
               aria-label="Help: inspector panel"><i class="fas fa-question-circle"></i></a>
            <button class="cv-inspector__close" id="inspector-close"><i class="fas fa-times"></i></button>
          </header>
          <div class="cv-inspector__body" id="inspector-body"></div>
        </aside>
      </div>
      <footer class="cv-eval" id="eval-strip">
        <div class="cv-eval__hdr">
          <h4 class="cv-eval__label">Evaluate<a class="wp-help"
             href="https://provsql.org/docs/user/studio.html#studio-circuit-eval-strip"
             target="_blank" rel="noopener"
             title="Run a semiring (compiled, custom, or Other: probability / PROV-XML) against the pinned node, else the root."
             aria-label="Help: evaluation strip"><i class="fas fa-question-circle"></i></a></h4>
          <span class="cv-eval__target" id="eval-target" title="Token the evaluation will be run against (selected node, otherwise the root)"></span>
        </div>
        <div class="cv-eval__form">
          <select class="cv-eval__semiring" id="eval-semiring">
            <optgroup label="Compiled Semirings">
              <option value="boolexpr">Boolean expression</option>
              <option value="boolean">Boolean</option>
              <option value="counting">Counting</option>
              <option value="why">Why-provenance</option>
              <option value="which">Which-provenance (lineage)</option>
              <option value="formula">Formula</option>
              <option value="tropical">Tropical (min-plus)</option>
              <option value="viterbi">Viterbi (max-times)</option>
              <option value="temporal">Temporal (interval-union)</option>
            </optgroup>
            <optgroup label="Custom Semirings" id="eval-custom-group" hidden>
              <!-- populated lazily from /api/custom_semirings -->
            </optgroup>
            <optgroup label="Other">
              <option value="probability">Probability</option>
              <option value="prov-xml">PROV-XML export</option>
            </optgroup>
          </select>
          <select class="cv-eval__mapping" id="eval-mapping" hidden>
            <option value="">(no mappings found)</option>
          </select>
          <span class="cv-eval__hint" id="eval-mapping-hint" hidden></span>
          <select class="cv-eval__method" id="eval-method" hidden>
            <optgroup label="Exact">
              <option value="">(default)</option>
              <option value="independent">independent</option>
              <option value="possible-worlds">possible-worlds</option>
              <option value="tree-decomposition">tree-decomposition</option>
              <option value="compilation">compilation</option>
            </optgroup>
            <optgroup label="Approximate">
              <option value="monte-carlo">monte-carlo</option>
              <option value="weightmc">weightmc</option>
            </optgroup>
          </select>
          <input type="number" class="cv-eval__args" id="eval-args-mc" hidden
                 min="1" step="1" placeholder="samples" value="10000"
                 autocomplete="off" title="Monte-Carlo sample count">
          <select class="cv-eval__args" id="eval-args-compiler" hidden
                  title="Knowledge compiler ProvSQL will invoke">
            <option value="d4">d4</option>
            <option value="c2d">c2d</option>
            <option value="minic2d">minic2d</option>
            <option value="dsharp">dsharp</option>
          </select>
          <input type="text" class="cv-eval__args" id="eval-args-wmc" hidden
                 value="0.8;0.2" placeholder="epsilon;delta"
                 autocomplete="off" spellcheck="false"
                 title="WeightMC parameters: epsilon;delta (defaults: 0.8;0.2)">
        </div>
        <div class="cv-eval__action-row">
          <button class="cv-eval__run wp-btn wp-btn--mini" id="eval-run" type="button">
            <i class="fas fa-play"></i> Run
          </button>
          <span class="cv-eval__btnpair">
            <button type="button" class="cv-eval__clear" id="eval-clear" title="Clear result" aria-label="Clear result" hidden>
              <i class="fas fa-eraser"></i>
            </button>
            <button type="button" class="cv-eval__copy" id="eval-copy" title="Copy result to clipboard" aria-label="Copy result" hidden>
              <i class="fas fa-clipboard"></i>
            </button>
          </span>
          <span class="cv-eval__result" id="eval-result"></span>
          <span class="cv-eval__bound"  id="eval-bound"></span>
          <span class="cv-eval__time"   id="eval-time"></span>
        </div>
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

  function formatCell(v, typeName) {
    // UUID-typed columns render as a short/full pair so the circuit
    // panel's "Show UUIDs" toggle can flip between abbreviated and full
    // display via a body-level CSS class : no re-render needed. The
    // outer span carries the full value as a title so hover always
    // reveals the original even when collapsed.
    if ((typeName || '').toLowerCase() === 'uuid' && v != null && v !== '') {
      const s = String(v);
      // Match circuit.js's shortUuid so both views render UUIDs the same
      // way: 4 hex chars + ellipsis is enough for cursory same/different
      // identification; the full value is a "Show UUIDs" click away.
      const shortStr = s.length > 4 ? s.slice(0, 4) + '…' : s;
      return `<span class="wp-uuid" title="${escapeAttr(s)}">`
           + `<span class="wp-uuid__short">${escapeHtml(shortStr)}</span>`
           + `<span class="wp-uuid__full">${escapeHtml(s)}</span>`
           + `</span>`;
    }
    // Multi-line text values (e.g. provsql.view_circuit's ASCII tree
    // dump, or any TEXT column that contains newlines) need pre-wrap +
    // monospace so newlines and indentation survive : the table cell's
    // default whitespace handling collapses them otherwise.
    if (typeof v === 'string' && v.indexOf('\n') !== -1) {
      return `<pre class="wp-cell-pre">${escapeHtml(v)}</pre>`;
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
    'uuid',
  ]);
  function isRightAlignedType(typeName) {
    return RIGHT_ALIGNED_TYPES.has((typeName || '').toLowerCase());
  }

  // Connection-editor popover anchored to the green/red status dot.
  // Lets the user paste an arbitrary libpq DSN (host, port, user,
  // password, dbname, options) so they can switch server / role
  // without restarting the studio. Uses POST /api/conn { dsn: "…" }.
  function setupConnEditor() {
    const dot    = document.getElementById('conn-dot');
    const panel  = document.getElementById('dsn-panel');
    const input  = document.getElementById('dsn-input');
    const apply  = document.getElementById('dsn-apply');
    const status = document.getElementById('dsn-status');
    if (!dot || !panel || !input || !apply || dot.dataset.bound === '1') return;
    dot.dataset.bound = '1';

    function showStatus(msg, isErr) {
      if (!status) return;
      if (!msg) { status.hidden = true; status.textContent = ''; status.classList.remove('is-error'); return; }
      status.hidden = false;
      status.textContent = msg;
      status.classList.toggle('is-error', !!isErr);
    }
    function open() {
      // Prefill with the password-stripped DSN the server reports.
      // The user retypes the password when switching role/host if
      // needed; we don't echo secrets back.
      if (_currentConn && _currentConn.dsn) input.value = _currentConn.dsn;
      panel.hidden = false;
      dot.setAttribute('aria-expanded', 'true');
      showStatus('');
      input.focus();
      input.select();
    }
    function close() {
      panel.hidden = true;
      dot.setAttribute('aria-expanded', 'false');
    }

    dot.addEventListener('click', (e) => {
      e.stopPropagation();
      if (panel.hidden) open(); else close();
    });
    document.addEventListener('click', (e) => {
      if (!panel.hidden && !panel.contains(e.target) && e.target !== dot) close();
    });
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') { e.preventDefault(); close(); }
      else if (e.key === 'Enter') { e.preventDefault(); apply.click(); }
    });

    apply.addEventListener('click', async () => {
      const dsn = (input.value || '').trim();
      if (!dsn) { showStatus('Empty DSN', true); return; }
      apply.disabled = true;
      showStatus('Connecting…');
      try {
        const resp = await fetch('/api/conn', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ dsn }),
        });
        const data = await resp.json().catch(() => ({}));
        if (!resp.ok) {
          showStatus(data.reason || data.error || `HTTP ${resp.status}`, true);
          return;
        }
        // Refresh the page so cached relations / circuit / result table
        // reflect the new server's contents (same approach as the DB
        // switcher's POST handler). sessionStorage carries the SQL
        // textarea across the reload.
        const ta = document.getElementById('request');
        if (ta) sessionStorage.setItem('ps.sql', ta.value);
        window.location.reload();
      } catch (e) {
        showStatus(`Network error: ${e.message}`, true);
      } finally {
        apply.disabled = false;
      }
    });
  }
  setupConnEditor();

  // Show / clear the "no DB selected" hint banner. Lives at the top of
  // the page (above the wp-shell grid) so it's the first thing the user
  // sees on a freshly-launched studio with no --dsn. Single instance
  // keyed by id so repeated /api/conn polls don't duplicate it.
  function _renderDbAutoHint(show) {
    const id = 'db-auto-hint';
    let el = document.getElementById(id);
    if (!show) {
      if (el) el.remove();
      return;
    }
    if (el) return;
    el = document.createElement('div');
    el.id = id;
    el.className = 'wp-db-hint';
    el.innerHTML =
      `<i class="fas fa-info-circle"></i> `
      + `No database picked at launch: currently on the <code>postgres</code> `
      + `maintenance DB. `
      + `<button type="button" class="wp-db-hint__btn" id="db-auto-pick">`
      + `Pick a database…</button>`;
    // Insert directly after <nav>; using body.firstChild was unsafe
    // because whitespace text nodes sit before the nav element and
    // pushed the banner above it.
    const nav = document.querySelector('.wp-nav');
    if (nav && nav.parentNode) {
      nav.parentNode.insertBefore(el, nav.nextSibling);
    } else {
      document.body.appendChild(el);
    }
    // Open the database menu (the user@dbname chip in the top-right of
    // the nav) on click. The chip's own click toggles the dropdown, so
    // we forward the event to it. Crucially, the original click event
    // ALSO bubbles up to a document-level handler in setupDbSwitcher
    // that closes any open menu when the click target isn't the chip
    // : without stopPropagation here, the menu would open and instantly
    // close again.
    const pick = document.getElementById('db-auto-pick');
    if (pick) pick.onclick = (e) => {
      e.stopPropagation();
      const chip = document.getElementById('conn-info');
      if (chip) chip.click();
    };
  }

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
  // Clear the previous run's truncation hint and notice / error banners
  // so they don't linger next to the new query's "running…" placeholder.
  // renderError still writes into result-banners on a failed POST, so
  // wiping here is safe : the success path repopulates them on render.
  const truncMark = document.getElementById('result-truncated');
  if (truncMark) {
    truncMark.textContent = '';
    truncMark.hidden = true;
  }
  const banners = document.getElementById('result-banners');
  if (banners) banners.innerHTML = '';
  const t0 = performance.now();

  // Wipe any previous circuit so it doesn't linger next to the new query's
  // result. The user may click a UUID cell in the new result to render a
  // fresh DAG; until then the canvas should be empty.
  if (env.mode === 'circuit') {
    window.ProvsqlCircuit?.clearScene?.();
  }

  // Cancel-button wiring: tag the in-flight request so /api/cancel/<id>
  // can resolve it back to the backend pid. The Send -> Cancel swap is
  // deferred 100ms so very fast queries (which return before the timer
  // fires) never flicker the row; clearTimeout in the finally branch
  // cancels the swap, and the same finally restores Send unconditionally.
  const requestId = (window.crypto && crypto.randomUUID)
    ? crypto.randomUUID()
    : `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  const runBtn    = document.getElementById('run-btn');
  const cancelBtn = document.getElementById('cancel-btn');
  if (cancelBtn) {
    cancelBtn.dataset.requestId = requestId;
    cancelBtn.disabled = false;
  }
  const swapTimer = setTimeout(() => {
    if (cancelBtn) cancelBtn.hidden = false;
    if (runBtn)    runBtn.hidden    = true;
  }, 100);

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
        request_id: requestId,
      }),
    });
  } catch (e) {
    renderError(`Network error: ${e.message}`);
    return false;
  } finally {
    clearTimeout(swapTimer);
    if (cancelBtn) cancelBtn.hidden = true;
    if (runBtn)    runBtn.hidden    = false;
  }
  const dt = Math.round(performance.now() - t0);
  time.textContent = dt;

  if (!resp.ok) {
    renderError(`HTTP ${resp.status}`);
    return false;
  }
  const payload = await resp.json();
  renderBlocks(payload.blocks || [], !!payload.wrapped, payload.notices || []);

  // Append the just-submitted query to the persistent history (skipping
  // exact-duplicate consecutive entries). We do this regardless of the
  // server's outcome so users can recall a query that errored to fix it.
  if (env.pushHistory) env.pushHistory(sqlText);

  // Record the just-run SQL so a subsequent mode/database switch knows
  // whether the textarea content was actually executed (the carry handler
  // compares this to the textarea value to decide whether to set the
  // ran-flag, which gates auto-replay in the new mode).
  try { sessionStorage.setItem('ps.lastRunSql', sqlText); } catch {}

  // After every successful exec in where mode, re-fetch relations so
  // add_provenance results show up live.
  if (env.mode === 'where' && env.refreshRelations) env.refreshRelations();

  // Mark all metadata caches dirty: the user may have just run a CREATE
  // TABLE, add_provenance, create_provenance_mapping, or a CREATE
  // FUNCTION that defines a new custom-semiring wrapper. Each panel
  // re-fetches lazily on next open.
  if (window.ProvsqlStudio?.metadata?.invalidateAll) {
    window.ProvsqlStudio.metadata.invalidateAll();
  }

  return false;

  // Render a single NOTICE / WARNING / ERROR / INFO banner. Severity drives
  // colour + icon; the literal severity tag is omitted (the visual style
  // already conveys it). ProvSQL-emitted messages all carry a "ProvSQL: "
  // prefix (provsql_error.h's macros prepend it); strip the prefix and
  // render it as a brand pill so the source is obvious without duplicating
  // it inline.
  function renderDiag(severity, message, sqlstate) {
    const sev = String(severity || 'NOTICE').toUpperCase();
    let cls, icon;
    if (sev === 'ERROR' || sev === 'FATAL' || sev === 'PANIC') {
      cls = 'wp-error';   icon = 'fa-exclamation-circle';
    } else if (sev === 'WARNING') {
      cls = 'wp-warning'; icon = 'fa-exclamation-triangle';
    } else {
      cls = 'wp-notice';  icon = 'fa-info-circle';
    }
    const raw = message || '';
    const m = raw.match(/^ProvSQL:\s*(.*)$/s);
    const badge = m ? '<span class="wp-srcbadge">ProvSQL</span> ' : '';
    const text  = m ? m[1] : raw;
    // XX000 is the generic "internal_error" catch-all that provsql_error()
    // raises (the C macro doesn't set a specific errcode); appending it
    // adds noise without information, so skip it.
    const tail  = (sqlstate && sqlstate !== 'XX000')
      ? ` <code>(SQLSTATE ${env.escapeHtml(sqlstate)})</code>`
      : '';

    // Long messages (parse-tree dumps from provsql.verbose_level >= 50,
    // full pre/post-rewrite SQL at >= 20) collapse behind a <details> so
    // they don't push the result table off-screen. The first line stays
    // visible as the summary; clicking the disclosure triangle reveals
    // the rest.
    const newlineIdx = text.indexOf('\n');
    const isLong = newlineIdx >= 0 && (
      (text.match(/\n/g) || []).length > 1 || text.length > 240
    );
    if (isLong) {
      const head = text.slice(0, newlineIdx);
      const rest = text.slice(newlineIdx + 1);
      return `<details class="${cls} wp-diag--collapsible">`
           + `<summary><i class="fas ${icon}"></i> ${badge}${env.escapeHtml(head)}${tail}</summary>`
           + `<div class="wp-diag__body">${env.escapeHtml(rest)}</div>`
           + `</details>`;
    }
    return `<div class="${cls}"><i class="fas ${icon}"></i> ${badge}${env.escapeHtml(text)}${tail}</div>`;
  }

  function renderBlocks(blocks, wrapped, notices) {
    // /api/exec returns zero or more error blocks (from earlier failed
    // statements) followed by the final block. We render only the final
    // block in the result table; everything informational (failed-prelude
    // errors, server NOTICE / WARNING messages, Studio's own observations
    // via severity=INFO) goes into the dedicated #result-banners slot
    // above the table : putting <div>s straight into <tbody> is invalid
    // HTML and the browser hoists them into the first <td>.
    const final = blocks[blocks.length - 1];
    const earlier = blocks.slice(0, -1);

    // Reset the truncation marker on every render; the rows branch
    // re-shows it when final.truncated. Status / error / empty paths
    // therefore never leak a stale "first N; more available" hint.
    const truncMark = document.getElementById('result-truncated');
    if (truncMark) {
      truncMark.textContent = '';
      truncMark.hidden = true;
    }

    const banners = document.getElementById('result-banners');
    let bannerHtml = '';
    // Earlier-failed prelude statements: render each as an ERROR banner
    // alongside notices/warnings.
    for (const b of earlier) {
      if (b.kind === 'error') {
        bannerHtml += renderDiag('ERROR', b.message, b.sqlstate);
      }
    }
    // Server-side notices/warnings + Studio's own INFO observations.
    for (const n of (notices || [])) {
      bannerHtml += renderDiag(n.severity, n.message);
    }
    if (banners) banners.innerHTML = bannerHtml;

    if (!final) {
      head.innerHTML = '';
      body.innerHTML = '<tr><td style="opacity:.6">(no statements)</td></tr>';
      count.textContent = 0;
      return;
    }

    if (final.kind === 'error') {
      // Append the final error to the same banner stack as earlier errors
      // and notices, so the visual treatment is uniform (icon + ProvSQL
      // pill + same colours + multi-line preserved). The result table
      // collapses to empty.
      if (banners) {
        banners.innerHTML += renderDiag('ERROR', final.message, final.sqlstate);
      }
      head.innerHTML = '';
      body.innerHTML = '';
      count.textContent = 0;
      return;
    }
    if (final.kind === 'status') {
      head.innerHTML = '';
      body.innerHTML = `<tr><td>${env.escapeHtml(final.message)}${final.rowcount != null ? ` · ${final.rowcount} tuples affected` : ''}</td></tr>`;
      count.textContent = final.rowcount != null ? final.rowcount : 0;
      return;
    }
    if (final.kind === 'rows') {
      const allCols = final.columns;
      const isWhere   = env.mode === 'where';
      const isCircuit = env.mode === 'circuit';
      // The studio session has provsql.aggtoken_text_as_uuid = on, so
      // agg_token cells arrive as bare UUIDs. The server pre-resolves
      // each unique UUID's "value (*)" via agg_token_value_text and
      // ships the map in final.agg_display; the front-end renders the
      // friendly form while keeping the UUID for click-through.
      const aggDisplay = final.agg_display || {};
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
      const headExtra = (isWhere && wrapped) ? '<th></th>' : '';
      head.innerHTML = displayIdx.map(i => {
        const alignCls = env.isRightAlignedType(allCols[i].type_name) ? ' is-right' : '';
        return `<th class="${alignCls.trim()}">${env.escapeHtml(allCols[i].name)}</th>`;
      }).join('') + headExtra;
      body.innerHTML = final.rows.map(r => {
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
          // agg_token cells: their on-wire text is the underlying UUID
          // (because provsql.aggtoken_text_as_uuid is on for studio
          // sessions). Make them clickable in circuit mode like
          // regular UUID cells; the cell's text content is replaced
          // with the "value (*)" form pulled from agg_display, and the
          // tooltip carries the UUID so users can confirm which
          // circuit the cell points at without inspecting the DOM.
          let displayValue = value;
          if (typeName === 'agg_token' && value) {
            if (isCircuit) {
              extraCls  = (extraCls + ' is-clickable').trim();
              extraAttr = ` data-circuit-uuid="${env.escapeAttr(String(value))}"`;
              if (extraCls.length) extraCls = ' ' + extraCls;
            }
            extraAttr += ` title="${env.escapeAttr(String(value))}"`;
            const friendly = aggDisplay[value];
            if (friendly) displayValue = friendly;
          }
          if (env.isRightAlignedType(typeName)) extraCls += ' is-right';
          return `<td class="wp-result__cell${extraCls}"${sourcesAttr}${extraAttr}>${env.formatCell(displayValue, col.type_name)}</td>`;
        }).join('');
        const jumpBtn = (isWhere && wrapped && provIdx >= 0 && r[provIdx])
          ? `<td class="wp-result__cell--actions"><button class="wp-btn wp-btn--mini" type="button" `
            + `data-jump-circuit="${env.escapeAttr(String(r[provIdx]))}" title="Open circuit DAG"><i class="fas fa-project-diagram"></i> Circuit</button></td>`
          : '';
        return `<tr>${cells}${jumpBtn}</tr>`;
      }).join('');
      count.textContent = final.rows.length;
      const truncated = document.getElementById('result-truncated');
      if (truncated) {
        if (final.truncated && final.max_rows != null) {
          truncated.textContent = ` (first ${final.max_rows}; more available)`;
          truncated.hidden = false;
        } else {
          truncated.textContent = '';
          truncated.hidden = true;
        }
      }
    }
  }

  function renderError(msg) {
    const banners = document.getElementById('result-banners');
    if (banners) banners.innerHTML = renderDiag('ERROR', msg);
    head.innerHTML = '';
    body.innerHTML = '';
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
