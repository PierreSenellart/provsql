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
  // Connection chip in the top nav: pull the live current_user /
  // current_database() once at page load.
  fetchConnInfo();
  setupConfigPanel();

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

  // Expose the env that the global runQuery reads (function declarations
  // are hoisted, so the named functions below are safe to reference here
  // even though they appear later in the IIFE). This MUST happen before
  // setupWhereMode / setupCircuitMode runs, because both can auto-replay
  // a carry-over query via runQuery, and if __provsqlStudio is still
  // undefined at that point the fallback default `{mode: 'where', ...}`
  // kicks in — the where-mode wrap then fires on /circuit pages and
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

    function refresh() { code.innerHTML = highlightSql(ta.value); }
    function syncScroll() {
      hlPre.scrollTop  = ta.scrollTop;
      hlPre.scrollLeft = ta.scrollLeft;
    }
    ta.addEventListener('input', refresh);
    ta.addEventListener('scroll', syncScroll);
    // Re-sync after textarea resize: pre is positioned absolutely so its
    // box follows automatically, but scroll position can drift.
    new ResizeObserver(syncScroll).observe(ta);
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
      sessionStorage.setItem('ps.sql', document.getElementById('request').value);
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
    // Quick-nav chips at the top: one per relation, click scrolls the
    // matching section into view inside the sidebar's own scroll pane.
    const navHtml = relations.length > 1
      ? `<nav class="wp-rel-nav">${
          relations.map(rel =>
            `<button type="button" class="wp-rel-nav__btn" data-target="${escapeAttr(headerId(rel.regclass))}">${escapeHtml(rel.regclass)}</button>`
          ).join('')
        }</nav>`
      : '';
    body.innerHTML = navHtml + relations.map(rel => {
      // Skip the rewriter-added `provsql` UUID column when displaying; its
      // value is already exposed as the row id (used for the hover-highlight).
      // where_provenance numbers cells by user-column position (1-indexed,
      // ignoring provsql), which matches the original i+1 since provsql sits
      // at the end of the column list.
      const visible = rel.columns
        .map((c, i) => ({ c, i }))
        .filter(({ c }) => c.name !== 'provsql');
      return `
      <section class="wp-relation" id="${escapeAttr(sectionId(rel.regclass))}">
        <header class="wp-relation__hdr" id="${escapeAttr(headerId(rel.regclass))}">
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
    const next = _historyCursor + direction;
    if (next < -1 || next >= arr.length) return;
    _historyCursor = next;
    _historyDriving = true;
    ta.value = next === -1 ? (_draft || '') : arr[next];
    ta.dispatchEvent(new Event('input'));  // refresh syntax highlight
    _historyDriving = false;
  }
  let _draft = '';

  // Reset history-cursor when the USER edits the textarea (so the next
  // Alt+↑ starts from the most-recent entry again). When we set ta.value
  // ourselves from stepHistory, the synthetic input event must NOT reset
  // the cursor — that's what _historyDriving guards against.
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
    const el = document.getElementById('conn-info');
    if (!el) return;
    try {
      const resp = await fetch('/api/conn');
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const c = await resp.json();
      _currentConn = c;
      el.textContent = `${c.user}@${c.database}`;
      if (c.host) el.title = `host: ${c.host}`;
    } catch (e) {
      el.textContent = '–';
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
      // sessionStorage preserves the SQL textarea across the reload.
      sessionStorage.setItem('ps.sql', document.getElementById('request').value);
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
    const timeout = document.getElementById('cfg-timeout');

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
        if (timeout && opts.statement_timeout_seconds != null) {
          timeout.value = String(opts.statement_timeout_seconds);
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
    if (timeout) {
      timeout.addEventListener('change', () => {
        const n = Math.max(1, Math.min(3600, parseInt(timeout.value || '30', 10) || 30));
        timeout.value = String(n);
        setGuc('statement_timeout_seconds', n);
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
    'uuid',
  ]);
  function isRightAlignedType(typeName) {
    return RIGHT_ALIGNED_TYPES.has((typeName || '').toLowerCase());
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
  renderBlocks(payload.blocks || [], !!payload.wrapped, payload.notices || []);

  // Append the just-submitted query to the persistent history (skipping
  // exact-duplicate consecutive entries). We do this regardless of the
  // server's outcome so users can recall a query that errored to fix it.
  if (env.pushHistory) env.pushHistory(sqlText);

  // After every successful exec in where mode, re-fetch relations so
  // add_provenance results show up live.
  if (env.mode === 'where' && env.refreshRelations) env.refreshRelations();

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
    return `<div class="${cls}"><i class="fas ${icon}"></i> ${badge}${env.escapeHtml(text)}${tail}</div>`;
  }

  function renderBlocks(blocks, wrapped, notices) {
    // /api/exec returns zero or more error blocks (from earlier failed
    // statements) followed by the final block. We render only the final
    // block in the result table; everything informational (failed-prelude
    // errors, server NOTICE / WARNING messages, Studio's own observations
    // via severity=INFO) goes into the dedicated #result-banners slot
    // above the table — putting <div>s straight into <tbody> is invalid
    // HTML and the browser hoists them into the first <td>.
    const final = blocks[blocks.length - 1];
    const earlier = blocks.slice(0, -1);

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
