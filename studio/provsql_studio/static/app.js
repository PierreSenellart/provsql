/* ProvSQL Studio: entry script.
   Wires the shared chrome (mode switcher, example buttons, query form) plus
   both mode-specific sidebars: where-mode shows source-relation tables with
   hover-highlight, circuit-mode shows the provenance DAG (lazy-loaded
   circuit.js). Cross-mode navigation preserves the textarea content via
   sessionStorage and offers a per-row "→ Circuit" jump in where mode. */

(function () {
  const mode = document.body.classList.contains('mode-circuit') ? 'circuit'
             : document.body.classList.contains('mode-contributions') ? 'contributions'
             : document.body.classList.contains('mode-temporal') ? 'temporal'
             : document.body.classList.contains('mode-notebook') ? 'notebook'
             : 'where';

  // The nav help icon deep-links into the current mode's section of the
  // Studio chapter (the static href in index.html points at the chapter
  // top as the no-JS fallback).
  const helpBtn = document.getElementById('help-btn');
  if (helpBtn) {
    helpBtn.href = 'https://provsql.org/docs/user/studio.html#studio-' + mode + '-mode';
  }

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
    // /api/kc/tools cache: which external knowledge compilers and
    // model counters the backend can actually reach (PATH +
    // provsql.tool_search_path). Consumed by the circuit-mode eval
    // strip to hide unselectable options. `null` until the first
    // successful fetch; the dirty flag is set by invalidateAll so the
    // refresh button forces a re-fetch (e.g. after installing a
    // missing tool or editing the tool_search_path GUC).
    toolsDirty:    true,
    toolsCache:    null,
    invalidateAll() {
      this.schemaDirty   = true;
      this.mappingsDirty = true;
      this.customsDirty  = true;
      this.toolsDirty    = true;
      this.toolsCache    = null;
    },
  };
  window.ProvsqlStudio = window.ProvsqlStudio || {};
  window.ProvsqlStudio.metadata = Metadata;

  // The top-nav popovers (Schema, Config, Tools) are mutually exclusive:
  // opening one closes the others.  Each calls this from its open() before
  // showing itself (their own click handlers stopPropagation, so the others'
  // outside-click handlers never fire on their own).
  const NAV_PANELS = [
    { panel: 'schema-panel', btn: 'schema-btn' },
    { panel: 'config-panel', btn: 'config-btn' },
    { panel: 'tools-panel',  btn: 'tools-btn' },
  ];
  function closeOtherNavPanels(exceptPanelId) {
    for (const { panel, btn } of NAV_PANELS) {
      if (panel === exceptPanelId) continue;
      const p = document.getElementById(panel);
      const b = document.getElementById(btn);
      if (p) p.hidden = true;
      if (b) b.setAttribute('aria-expanded', 'false');
    }
  }

  // Hide eval-strip options whose external tool is missing on the
  // backend. The map comes from /api/kc/tools (one round-trip per
  // refresh) and lists which `eval-args-compiler` and
  // `eval-args-wmc-tool` values are reachable through the backend's
  // resolved PATH (plus the provsql.tool_search_path GUC). Already-
  // selected options that became unavailable are reset to the first
  // surviving entry so the eval strip never lands the user on an
  // option that's guaranteed to error out.
  //
  // Soft-fail: 501 (extension too old) or a network error leaves
  // every option visible. That degrades to today's behaviour where
  // an absent tool surfaces as a probability_evaluate error at run
  // time, which is acceptable when the discovery surface itself is
  // not installed.
  async function refreshToolAvailability() {
    if (!Metadata.toolsDirty && Metadata.toolsCache) {
      applyToolAvailability(Metadata.toolsCache);
      return Metadata.toolsCache;
    }
    let data = null;
    try {
      const resp = await fetch('/api/kc/tools');
      if (resp.ok) data = await resp.json();
    } catch (_e) {
      // network failure: leave dropdowns untouched.
    }
    Metadata.toolsCache = data;
    Metadata.toolsDirty = false;
    if (data) applyToolAvailability(data);
    return data;
  }

  // Friendly display labels for the tools ProvSQL ships with; any tool an
  // administrator registers later falls back to its bare registry name.
  const TOOL_LABELS = {
    'd4': 'd4', 'd4v2': 'd4v2', 'c2d': 'c2d', 'minic2d': 'miniC2D',
    'dsharp': 'Dsharp',
    'panini-obdd': 'Panini → OBDD', 'panini-obdd-and': 'Panini → OBDD[AND]',
    'panini-decdnnf': 'Panini → Decision-DNNF',
    'ganak': 'Ganak', 'sharpsat-td': 'SharpSAT-TD', 'dpmc': 'DPMC',
    'weightmc': 'WeightMC',
    'tree-decomposition': 'tree-decomposition',
    'interpret-as-dd': 'interpret as d-D', 'default': 'default (fallback chain)',
  };
  const _toolLabel = (name) => TOOL_LABELS[name] || name;

  // Rebuild a <select> from catalog entries [{name, available}].  A tool
  // the backend cannot reach (not on the resolved PATH) is dropped
  // entirely -- offering it would only set the user up for a guaranteed
  // run-time error; the Tools panel remains the discovery surface for
  // registered-but-missing tools.  The selection is kept if still listed,
  // else moved to the first available option.  A `change` event fires so
  // dependent UI updates.
  function _rebuildSelect(selectEl, entries) {
    const prev = selectEl.value;
    const avail = entries.filter((e) => e.available);
    selectEl.replaceChildren();
    for (const e of avail) {
      const opt = document.createElement('option');
      opt.value = e.name;
      opt.textContent = _toolLabel(e.name);
      selectEl.appendChild(opt);
    }
    if (!avail.length) {
      // Nothing usable: show a disabled placeholder rather than an empty
      // box, so the strip explains itself.
      const opt = document.createElement('option');
      opt.value = '';
      opt.textContent = '(no tool available)';
      opt.disabled = true;
      selectEl.appendChild(opt);
    }
    selectEl.value = avail.some((e) => e.name === prev) ? prev
      : (avail.length ? avail[0].name : '');
    selectEl.dispatchEvent(new Event('change'));
  }

  function applyToolAvailability(data) {
    if (!data) return;
    const compSel = document.getElementById('eval-args-compiler');
    const wmcSel  = document.getElementById('eval-args-wmc-tool');
    if (compSel) {
      // Registered external compilers (preference order from the API),
      // then the always-available in-process meta-routes.
      const entries = (data.compile || []).map(
        (t) => ({ name: t.name, available: t.available }));
      for (const name of (data.inprocess_compilers || []))
        entries.push({ name, available: true });
      _rebuildSelect(compSel, entries);
    }
    if (wmcSel) {
      _rebuildSelect(wmcSel,
        (data.wmc || []).map((t) => ({ name: t.name, available: t.available })));
    }
  }

  // Expose so circuit.js (which renders the eval strip on circuit-mode
  // entry) can prime the cache after wiring the dropdowns.
  window.ProvsqlStudio.refreshToolAvailability = refreshToolAvailability;

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

  // Mode switcher is a dropdown (four modes, soon five, would overflow the
  // header as buttons): the trigger shows the current mode, the menu lists
  // all modes. The current mode's icon + label are copied from its menu item
  // so there is a single source of truth for both.
  (function setupModeSwitcher() {
    const trigger = document.getElementById('modeswitch-btn');
    const menu    = document.getElementById('modeswitch-menu');
    if (!trigger || !menu) return;
    const current = menu.querySelector(`.ps-modeswitch__btn[data-mode="${mode}"]`);
    if (current) {
      const ico = document.getElementById('modeswitch-icon');
      const lbl = document.getElementById('modeswitch-label');
      const srcIcon = current.querySelector('i');
      if (ico && srcIcon) ico.className = srcIcon.className + ' ps-modeswitch__curicon';
      if (lbl) lbl.textContent = current.textContent.trim();
    }
    const close = () => { menu.hidden = true; trigger.setAttribute('aria-expanded', 'false'); };
    const open  = () => { menu.hidden = false; trigger.setAttribute('aria-expanded', 'true'); };
    trigger.addEventListener('click', (e) => {
      e.stopPropagation();
      if (menu.hidden) open(); else close();
    });
    document.addEventListener('click', (e) => {
      if (!menu.hidden && !menu.contains(e.target) && e.target !== trigger) close();
    });
    document.addEventListener('keydown', (e) => {
      if (e.key === 'Escape' && !menu.hidden) close();
    });
  })();

  // Site-wide click-to-sort on any <thead> column header. Numeric
  // columns (marked with class="num") sort numerically; everything else
  // sorts by localeCompare on the cell's textContent. Click cycles
  // ascending → descending. Event delegation: this single listener
  // covers every table ever rendered into the DOM, including the ones
  // dynamically injected into #eval-result (KC benchmark) and the
  // query-result table that runQuery rewrites on each batch.
  document.addEventListener('click', (ev) => {
    const th = ev.target.closest('thead th');
    if (!th) return;
    const table = th.closest('table');
    if (!table) return;
    const tbody = table.querySelector(':scope > tbody');
    if (!tbody) return;
    const headerRow = th.parentElement;
    const idx = Array.from(headerRow.children).indexOf(th);
    if (idx < 0) return;
    const isNum = th.classList.contains('num');
    // Toggle: ascending on first click of a fresh header, then flip.
    const next = th.dataset.sort === 'asc' ? 'desc' : 'asc';
    for (const h of headerRow.children) delete h.dataset.sort;
    th.dataset.sort = next;
    const rows = Array.from(tbody.querySelectorAll(':scope > tr'));
    const keyFor = (row) => row.children[idx]?.textContent.trim() ?? '';
    rows.sort((a, b) => {
      const av = keyFor(a);
      const bv = keyFor(b);
      if (isNum) {
        const an = parseFloat(av);
        const bn = parseFloat(bv);
        const aNaN = Number.isNaN(an);
        const bNaN = Number.isNaN(bn);
        // Empty / non-numeric cells (e.g. "–" for a failed row) sink
        // to the bottom irrespective of direction; they aren't
        // meaningful entries on a numeric column.
        if (aNaN && bNaN) return 0;
        if (aNaN) return 1;
        if (bNaN) return -1;
        return next === 'asc' ? an - bn : bn - an;
      }
      return next === 'asc'
        ? av.localeCompare(bv) : bv.localeCompare(av);
    });
    for (const r of rows) tbody.appendChild(r);
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
    // Notebook mode carries the current cell's SQL (the hidden #request
    // box is stale there); a cell that was executed on the kernel
    // counts as "ran", so Circuit/Where mode auto-replays it under the
    // same rule as a sent query.
    if (mode === 'notebook' && window.ProvsqlNotebook
        && window.ProvsqlNotebook.currentSqlForCarry) {
      const cur = window.ProvsqlNotebook.currentSqlForCarry();
      sessionStorage.setItem('ps.sql', cur.sql || '');
      if (cur.sql && cur.ran) sessionStorage.setItem('ps.sql.ran', '1');
      else sessionStorage.removeItem('ps.sql.ran');
      return;
    }
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
  // Optional companion to the preload: the row's provenance gate, so
  // the eval strip's "Conditioned by" presets exactly as it would on an
  // in-mode click (set by the notebook's circuit-cell jump).
  const preloadCircuitRowProv = sessionStorage.getItem('ps.preloadCircuitRowProv');
  sessionStorage.removeItem('ps.preloadCircuitRowProv');
  // Same mechanism for the where-mode "→ Contributions" jump: the token to
  // pin once Contributions mode's result table has rendered.
  const preloadContribUuid = sessionStorage.getItem('ps.preloadContrib');
  sessionStorage.removeItem('ps.preloadContrib');

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
  setupToolsPanel();

  // Nav-bar "empty database" (broom): drop every user schema in the
  // connected DB after an explicit, name-bearing confirm. Backed by
  // POST /api/database/empty; the page reloads onto the clean slate
  // (kernels were closed server-side, caches dropped).
  document.getElementById('empty-db-btn')?.addEventListener('click', async () => {
    let dbName = '';
    try {
      const resp = await fetch('/api/conn');
      if (resp.ok) dbName = (await resp.json()).database || '';
    } catch (e) { /* confirm below still names the action */ }
    if (!window.confirm(
        `Empty the database${dbName ? ` “${dbName}”` : ''}?\n\n`
        + 'This drops EVERY user schema (tables, views, mappings, '
        + 'functions) and cannot be undone. The provsql extension '
        + 'itself survives.')) return;
    try {
      const resp = await fetch('/api/database/empty', { method: 'POST' });
      const payload = await resp.json().catch(() => ({}));
      if (!resp.ok) {
        window.alert(payload.error || `HTTP ${resp.status}`);
        return;
      }
    } catch (e) {
      window.alert(`Network error: ${e.message}`);
      return;
    }
    window.location.reload();
  });

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

  // Pasted / dropped SQL is cleaned of invisible Unicode on arrival (see
  // sanitizeSqlText below; the module-scope declaration is hoisted).
  wirePasteSanitizer(document.getElementById('request'));

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

  // Load-SQL button right under the eraser : reads a local .sql file
  // into the textarea, replacing its content. The previous text is
  // pushed to history first, so the same Alt+↑ recovery applies as for
  // the eraser. The hidden file input's value is reset on every change
  // so picking the same file twice in a row still fires the event.
  const loadSqlInput = document.getElementById('load-sql-input');
  document.getElementById('load-sql-btn')?.addEventListener('click', () => {
    loadSqlInput?.click();
  });
  loadSqlInput?.addEventListener('change', async () => {
    const file = loadSqlInput.files && loadSqlInput.files[0];
    loadSqlInput.value = '';
    if (!file) return;
    const ta = document.getElementById('request');
    if (!ta) return;
    let text;
    try {
      text = await file.text();
    } catch (e) {
      // Reading a just-picked local file essentially never fails
      // (revoked permission, file deleted mid-pick); leave the box
      // untouched rather than wiping it with nothing to show.
      console.error(`Could not read ${file.name}:`, e);
      return;
    }
    pushHistory(ta.value);
    // Same cleanup as a paste: a file saved from a rich-text source can
    // carry a BOM or NBSPs that PostgreSQL's lexer rejects.
    ta.value = sanitizeSqlText(text.replace(/\r\n/g, '\n'));
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
    isRightAlignedType, matchesProvType, pushHistory,
  };
  window.ProvsqlStudio.highlightSql = highlightSql;
  // The notebook front-end (static/notebook.js, loaded on demand) attaches
  // the same paste cleanup to its per-cell SQL textareas, and runs loaded
  // .sql files through the same text cleanup.
  window.ProvsqlStudio.wirePasteSanitizer = wirePasteSanitizer;
  window.ProvsqlStudio.sanitizeSqlText = sanitizeSqlText;

  // The provenance / mapping pills for a schema relation record `r`, shared
  // by the schema panel and the notebook sidebar so both classify a relation
  // identically: PROV-TID / PROV-BID / PROV-OPAQUE (from r.prov_kind, 1.6.0+;
  // a bare PROV when the kind is unknown), or "mapping", or nothing. `escA`
  // is an attribute-escaper. Returns an HTML string (possibly empty).
  function relProvBadges(r, escA) {
    const showProv = r.has_provenance && !r.is_mapping;
    let provLabel = 'prov';
    let provTip = 'Provenance-tracked (provsql uuid column)';
    let provKindCls = '';
    if (r.prov_kind === 'tid') {
      provLabel = 'prov-tid';
      provTip   = 'Provenance-tracked, independent leaves (TID): '
                + 'one independent random variable per row, '
                + 'standard probabilistic semantics.';
      provKindCls = ' wp-schema__rel-prov--tid';
    } else if (r.prov_kind === 'bid') {
      provLabel = 'prov-bid';
      provTip   = 'Provenance-tracked, block-correlated (BID): '
                + 'rows sharing the same block key are mutually '
                + 'exclusive (set via repair_key).';
      provKindCls = ' wp-schema__rel-prov--bid';
    } else if (r.prov_kind === 'opaque') {
      provTip   = 'Provenance-tracked, opaque kind: the relation '
                + 'either carries user-supplied or shared provsql '
                + 'tokens, or its body has structure the classifier '
                + 'cannot certify TID / BID (multi-source join, '
                + 'sublink). The safe-query rewriter refuses to '
                + 'fire on it.';
      provKindCls = ' wp-schema__rel-prov--opaque';
    }
    const provBadge = showProv
      ? `<span class="wp-schema__rel-prov${provKindCls}" `
        + `title="${escA(provTip)}">${provLabel}</span>`
      : '';
    const mapBadge = r.is_mapping
      ? `<span class="wp-schema__rel-map" title="Provenance mapping (value + provenance uuid columns)">mapping</span>`
      : '';
    return provBadge + mapBadge;
  }
  window.ProvsqlStudio.relProvBadges = relProvBadges;

  // Approximation-guarantee helpers, shared by Circuit mode and the notebook
  // eval strip so an approximate probability is shown the same way in both:
  // the structured "approximation-guarantee" NOTICE the extension emits is
  // parsed and rendered as the value interval the true probability lies in.
  function parseGuaranteeNotice(messages) {
    for (const raw of (Array.isArray(messages) ? messages : [])) {
      const m = (raw || '').match(/approximation-guarantee:\s*(.*)$/);
      if (!m) continue;
      const kv = {};
      m[1].trim().split(/\s+/).forEach((tok) => {
        const i = tok.indexOf('=');
        if (i > 0) kv[tok.slice(0, i)] = tok.slice(i + 1);
      });
      return kv;
    }
    return null;
  }
  // Render a parsed guarantee UNIFORMLY as the value interval [lo, hi] the
  // true probability is guaranteed to lie in, given the point estimate --
  // whether the guarantee is additive, relative, or the d-tree's bound.
  //   additive: |est − p| ≤ ε   => p ∈ [est − ε, est + ε]
  //   relative: |est − p| ≤ ε·p => p ∈ [est/(1+ε), est/(1−ε)]
  // δ (when present) gives the confidence 1−δ; δ = 0 is deterministic.
  function renderGuarantee(kv, estimate, resolvedMethod) {
    if (!kv) return '';
    const eps = parseFloat(kv.eps);
    if (!Number.isFinite(eps) || !(typeof estimate === 'number')
        || !Number.isFinite(estimate)) return '';
    let lo, hi;
    if (kv.kind === 'additive') { lo = estimate - eps; hi = estimate + eps; }
    else if (kv.kind === 'relative') {
      lo = estimate / (1 + eps);
      hi = eps < 1 ? estimate / (1 - eps) : 1;
    } else return '';
    lo = Math.max(0, lo);
    hi = Math.min(1, hi);
    const dec = window.ProvsqlStudio.getProbDecimals
      ? window.ProvsqlStudio.getProbDecimals() : 4;
    const delta = kv.delta != null ? parseFloat(kv.delta) : NaN;
    const conf = !Number.isFinite(delta) ? ''
      : delta <= 0 ? ', certain'
      : `, prob ≥ ${(100 * (1 - delta)).toFixed(delta < 0.01 ? 1 : 0)}%`;
    const n = kv.samples != null ? parseInt(kv.samples, 10) : NaN;
    const smp = Number.isFinite(n) && n > 0 ? `, ${n.toLocaleString()} samples` : '';
    // The mechanism/tool name, but only when it adds information: when it is
    // the method already shown ("via stopping-rule"), repeating it as
    // "[stopping-rule]" is noise. It stays for the distinct case (e.g. the
    // external "weightmc" tool behind the "wmc" method).
    const tool = (kv.tool && kv.tool !== resolvedMethod) ? ` [${kv.tool}]` : '';
    return `(Pr ∈ [${lo.toFixed(dec)}, ${hi.toFixed(dec)}]${conf}${smp})${tool}`;
  }
  window.ProvsqlStudio.parseGuaranteeNotice = parseGuaranteeNotice;
  window.ProvsqlStudio.renderGuarantee = renderGuarantee;

  if (mode === 'where')              setupWhereMode();
  else if (mode === 'notebook')      setupNotebookMode();
  else if (mode === 'contributions') setupContributionsMode();
  else if (mode === 'temporal')      setupTemporalMode();
  else                               setupCircuitMode();

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
      // Same carry rule as the mode-switch tab : only auto-replay in the
      // new mode if the textarea still matches the last-run SQL. The
      // preloaded UUID is always carried so the target renders
      // regardless of whether we re-run the query.
      const cBtn = e.target.closest('[data-jump-contributions]');
      if (cBtn) {
        window.ProvsqlStudio.carryQueryForSwitch();
        sessionStorage.setItem('ps.preloadContrib', cBtn.dataset.jumpContributions);
        window.location.href = '/contributions';
        return;
      }
      const btn = e.target.closest('[data-jump-circuit]');
      if (!btn) return;
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

  async function setupNotebookMode() {
    // The notebook keeps the where-mode relations sidebar (the natural
    // companion while writing cells) and swaps the right card's query
    // form + result pane for the cell list. All cell logic lives in
    // notebook.js, lazy-loaded like circuit.js.
    const form = document.querySelector('.wp-form');
    if (form) form.hidden = true;
    const resultPane = document.getElementById('result-pane');
    if (resultPane) resultPane.hidden = true;
    const pane = document.getElementById('notebook-pane');
    if (pane) pane.hidden = false;
    // The notebook renders its own compact sidebar (outline + one-line
    // relation summaries) -- the where-mode full-table browser eats too
    // much space next to a cell list.
    const s = document.createElement('script');
    s.src = '/static/notebook.js';
    s.onload = () => {
      if (window.ProvsqlNotebook && window.ProvsqlNotebook.init) {
        window.ProvsqlNotebook.init(window.__provsqlStudio);
      }
    };
    document.head.appendChild(s);
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
        return `${shown} tuple${shown === 1 ? '' : 's'}`;
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
      // provsql.tool_search_path is superuser-only (PGC_SUSET): record
      // whether this session may set it, so the Config panel can present
      // the field as admin-managed instead of letting the write fail.
      // Missing (older server) is treated as settable for back-compat.
      window.ProvsqlStudio.toolSearchPathSettable =
        (c.tool_search_path_settable !== false);
      if (window.ProvsqlStudio.applyToolSearchPathPolicy) {
        window.ProvsqlStudio.applyToolSearchPathPolicy();
      }
      // Footer version chip: ProvSQL extension version (NULL when the
      // extension is not installed on the connected database) and
      // Studio package version. Discreet: runs once per /api/conn poll
      // and is a no-op if the spans aren't present (e.g. tests).
      const extEl = document.getElementById('version-ext');
      const studioEl = document.getElementById('version-studio');
      if (extEl) extEl.textContent = c.extension_version ? `ProvSQL ${c.extension_version}` : '';
      if (studioEl) studioEl.textContent = c.studio_version ? `Studio ${c.studio_version}` : '';
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
    const tsp     = document.getElementById('cfg-tool-search-path');
    const simplify = document.getElementById('cfg-simplify-on-load');
    const mcSeed   = document.getElementById('cfg-monte-carlo-seed');
    const rvSamples = document.getElementById('cfg-rv-mc-samples');
    const fallback  = document.getElementById('cfg-fallback-compiler');

    // provsql.tool_search_path is superuser-only (PGC_SUSET). For a
    // non-superuser session the field is read-only / admin-managed: the
    // server swallows any write, so present it as such rather than
    // letting the user think their edit took effect. Driven by the flag
    // /api/conn publishes on window.ProvsqlStudio.toolSearchPathSettable;
    // re-applied whenever the connection poll refreshes it.
    function applyToolSearchPathPolicy() {
      if (!tsp) return;
      const settable =
        (window.ProvsqlStudio.toolSearchPathSettable !== false);
      tsp.readOnly = !settable;
      tsp.classList.toggle('is-admin-managed', !settable);
      tsp.style.opacity = settable ? '' : '0.6';
      tsp.style.cursor = settable ? '' : 'not-allowed';
      tsp.placeholder = settable ? '(server PATH)' : '(admin-managed)';
      tsp.title = settable
        ? ''
        : 'provsql.tool_search_path is superuser-only; its value is '
          + 'managed by the database administrator and cannot be changed '
          + 'from a non-superuser session.';
    }
    window.ProvsqlStudio.applyToolSearchPathPolicy =
      applyToolSearchPathPolicy;

    async function loadConfig() {
      try {
        const resp = await fetch('/api/config');
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        const cfg = await resp.json();
        const eff = cfg.effective || {};
        active.checked = (eff['provsql.active'] || 'on') !== 'off';
        verb.value     = eff['provsql.verbose_level'] || '0';
        if (verbOut) verbOut.textContent = verb.value;
        if (simplify) {
          simplify.checked = (eff['provsql.simplify_on_load'] || 'on') !== 'off';
        }
        if (mcSeed && eff['provsql.monte_carlo_seed'] != null) {
          mcSeed.value = String(eff['provsql.monte_carlo_seed']);
        }
        if (rvSamples && eff['provsql.rv_mc_samples'] != null) {
          rvSamples.value = String(eff['provsql.rv_mc_samples']);
        }
        if (fallback) {
          // Populate from the live registry's compile tools (no hardcoded
          // list); unavailable ones are dropped, except the current GUC
          // value (a compiler the admin set but that is not installed),
          // which is kept -- disabled and labelled -- so the select still
          // reflects the actual setting.
          const cat = await refreshToolAvailability();
          const compile = (cat && cat.compile) || [];
          const current = eff['provsql.fallback_compiler']
            ? String(eff['provsql.fallback_compiler']) : '';
          if (compile.length) {
            fallback.replaceChildren();
            for (const t of compile) {
              if (!t.available && t.name !== current) continue;
              const opt = document.createElement('option');
              opt.value = t.name;
              opt.textContent = t.available ? _toolLabel(t.name)
                                            : `${_toolLabel(t.name)} (not on PATH)`;
              if (!t.available) opt.disabled = true;
              fallback.appendChild(opt);
            }
          }
          if (current) fallback.value = current;
        }
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
        if (tsp && opts.tool_search_path != null) {
          tsp.value = opts.tool_search_path;
        }
        applyToolSearchPathPolicy();
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
      closeOtherNavPanels('config-panel');
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
    if (simplify) {
      simplify.addEventListener('change', () => {
        setGuc('provsql.simplify_on_load', simplify.checked ? 'on' : 'off');
      });
    }
    if (mcSeed) {
      // -1 means "non-deterministic"; clamp absurd negatives but allow
      // any non-negative literal seed (including 0).
      mcSeed.addEventListener('change', () => {
        const raw = parseInt(mcSeed.value, 10);
        const n = Number.isFinite(raw) ? Math.max(-1, raw) : -1;
        mcSeed.value = String(n);
        setGuc('provsql.monte_carlo_seed', n);
      });
    }
    if (rvSamples) {
      // 0 is meaningful (disables the MC fallback); clamp negatives.
      rvSamples.addEventListener('change', () => {
        const raw = parseInt(rvSamples.value, 10);
        const n = Number.isFinite(raw) ? Math.max(0, raw) : 10000;
        rvSamples.value = String(n);
        setGuc('provsql.rv_mc_samples', n);
      });
    }
    if (fallback) {
      fallback.addEventListener('change', () => {
        setGuc('provsql.fallback_compiler', fallback.value);
      });
    }
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
    if (tsp) {
      // provsql.tool_search_path: same blur-and-Enter commit pattern as
      // search_path. Empty string falls back to the server's PATH.
      const commitTsp = () => {
        // Superuser-only GUC: a non-superuser session can't set it, and
        // the field is read-only in that case, so don't round-trip a
        // write the server would only swallow.
        if (window.ProvsqlStudio.toolSearchPathSettable === false) return;
        const v = (tsp.value || '').trim();
        tsp.value = v;
        setGuc('tool_search_path', v);
      };
      tsp.addEventListener('blur', commitTsp);
      tsp.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') { e.preventDefault(); tsp.blur(); }
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

  /* ──────── Tools panel: the external-tool registry (provsql.tools) ──────── */

  function setupToolsPanel() {
    const btn      = document.getElementById('tools-btn');
    const panel    = document.getElementById('tools-panel');
    const body     = document.getElementById('tools-body');
    const status   = document.getElementById('tools-status');
    const readonly = document.getElementById('tools-readonly');
    if (!btn || !panel || !body) return;

    const listView = document.getElementById('tools-list-view');
    const formView = document.getElementById('tools-form-view');
    const formTitle = document.getElementById('tools-form-title');
    const addBtn   = document.getElementById('tools-add-btn');
    const backBtn  = document.getElementById('tools-form-back');
    const registerBtn = document.getElementById('tool-register-btn');
    const nameInput = document.getElementById('tool-name');
    const kindSel  = document.getElementById('tool-kind');
    const execField = document.getElementById('tool-exec-field');
    const argtplField = document.getElementById('tool-argtpl-field');
    const argtplcField = document.getElementById('tool-argtplc-field');
    const connField = document.getElementById('tool-conn-field');
    const connSel  = document.getElementById('tool-conn');
    const endpointField = document.getElementById('tool-endpoint-field');
    const infmtBox = document.getElementById('tool-infmt');
    const outSel   = document.getElementById('tool-outfmt');
    const parserSel = document.getElementById('tool-parser');
    let canManage = false;

    // Operation -> the input / output / parser values that make sense and are
    // implemented (mirrors the seeded registry in src/ToolRegistry.cpp), so the
    // form offers only valid choices.
    const PRESETS = {
      compile: { inputs: ['dimacs-cnf', 'circuit-bcs12'], outputs: ['ddnnf-nnf', 'panini-dd'], parsers: ['nnf', 'panini-dd'] },
      wmc:     { inputs: ['dimacs-cnf'], outputs: ['decimal'], parsers: ['wmc-line', 'weightmc'] },
      render:  { inputs: ['dot'], outputs: ['ascii'], parsers: ['ascii'] },
    };
    const OP_LABEL = { compile: 'Compilation', wmc: 'Weighted counting', render: 'Rendering' };
    const OP_ORDER = ['compile', 'wmc', 'render'];
    const KIND_TITLE = {
      cli: 'cli: ProvSQL spawns this binary once per call.',
      kcmcp: 'kcmcp: a warm socket server, reached over the KCMCP protocol.',
    };

    function showStatus(msg, isError) {
      if (!status) return;
      status.textContent = msg || '';
      status.hidden = !msg;
      status.classList.toggle('is-error', !!isError);
    }
    async function postJSON(url, bodyObj) {
      const resp = await fetch(url, { method: 'POST',
        headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(bodyObj) });
      let data = {};
      try { data = await resp.json(); } catch { /* non-JSON */ }
      return { ok: resp.ok, status: resp.status, data };
    }
    const errOf = (r) => (r.data && (r.data.detail || r.data.error)) || ('HTTP ' + r.status);

    function selectedOps() {
      return Array.from(document.querySelectorAll('.tool-op:checked')).map((c) => c.value);
    }
    function unionPreset(key) {
      const out = [];
      selectedOps().forEach((op) => (PRESETS[op] ? PRESETS[op][key] : [])
        .forEach((v) => { if (!out.includes(v)) out.push(v); }));
      return out;
    }
    function refreshPresets() {
      const inputs = unionPreset('inputs');
      const checked = new Set(Array.from(infmtBox.querySelectorAll('input:checked')).map((c) => c.value));
      infmtBox.textContent = '';
      inputs.forEach((fmt, idx) => {
        const lab = document.createElement('label');
        const cb = document.createElement('input');
        cb.type = 'checkbox'; cb.className = 'tool-infmt'; cb.value = fmt;
        cb.checked = checked.has(fmt) || (checked.size === 0 && idx === 0);
        lab.appendChild(cb); lab.appendChild(document.createTextNode(' ' + fmt));
        infmtBox.appendChild(lab);
      });
      const fill = (sel, vals) => {
        const cur = sel.value;
        sel.textContent = '';
        vals.forEach((v) => { const o = document.createElement('option'); o.value = v; o.textContent = v; sel.appendChild(o); });
        if (vals.includes(cur)) sel.value = cur;
      };
      fill(outSel, unionPreset('outputs'));
      fill(parserSel, unionPreset('parsers'));
    }
    function applyConnFields() {
      endpointField.hidden = !(kindSel.value === 'kcmcp' && connSel.value === 'endpoint');
    }
    function applyKindFields() {
      const kcmcp = kindSel.value === 'kcmcp';
      execField.hidden = kcmcp;
      argtplField.hidden = kcmcp;
      argtplcField.hidden = kcmcp;
      connField.hidden = !kcmcp;
      applyConnFields();
    }
    kindSel.addEventListener('change', applyKindFields);
    connSel.addEventListener('change', applyConnFields);
    document.querySelectorAll('.tool-op').forEach((c) => c.addEventListener('change', refreshPresets));

    function badge(text, cls, title) {
      const b = document.createElement('span');
      b.className = 'wp-tools__badge' + (cls ? ' ' + cls : '');
      b.textContent = text;
      if (title) b.title = title;
      return b;
    }
    function renderRow(t) {
      const row = document.createElement('div');
      row.className = 'wp-tools__row';
      const dot = document.createElement('span');
      dot.className = 'wp-tools__dot ' + (t.available ? 'wp-tools__dot--ok' : 'wp-tools__dot--no');
      dot.title = t.available ? 'available' : 'unavailable';
      row.appendChild(dot);
      const name = document.createElement('span');
      name.className = 'wp-tools__rname';
      name.textContent = t.name;
      if (!t.enabled) name.classList.add('is-disabled');
      row.appendChild(name);
      row.appendChild(badge(t.kind, 'wp-tools__badge--' + t.kind, KIND_TITLE[t.kind]));
      const target = document.createElement('span');
      target.className = 'wp-tools__target';
      target.textContent = t.kind === 'kcmcp' ? t.endpoint : t.executable;
      target.title = target.textContent;
      row.appendChild(target);
      if (canManage) {
        const pref = document.createElement('input');
        pref.type = 'number'; pref.className = 'wp-tools__pref'; pref.value = t.preference; pref.step = '1';
        pref.title = 'preference (higher is selected first)';
        pref.addEventListener('change', async () => {
          const v = parseInt(pref.value, 10);
          if (Number.isNaN(v) || v === t.preference) return;
          const r = await postJSON('/api/kc/registry/preference', { name: t.name, preference: v });
          if (r.ok) afterMutation(); else { showStatus(errOf(r), true); pref.value = t.preference; }
        });
        row.appendChild(pref);
        const en = document.createElement('input');
        en.type = 'checkbox'; en.className = 'wp-tools__en'; en.checked = t.enabled; en.title = 'enabled';
        en.addEventListener('change', async () => {
          const r = await postJSON('/api/kc/registry/enable', { name: t.name, enabled: en.checked });
          if (r.ok) afterMutation(); else { showStatus(errOf(r), true); en.checked = t.enabled; }
        });
        row.appendChild(en);
        const edit = document.createElement('button');
        edit.type = 'button'; edit.className = 'wp-tools__edit'; edit.title = 'edit';
        edit.innerHTML = '<i class="fas fa-pen"></i>';
        edit.addEventListener('click', () => showForm(t));
        row.appendChild(edit);
        const del = document.createElement('button');
        del.type = 'button'; del.className = 'wp-tools__del'; del.title = 'unregister';
        del.innerHTML = '<i class="fas fa-times"></i>';
        del.addEventListener('click', async () => {
          if (!window.confirm('Unregister tool "' + t.name + '"?')) return;
          const r = await postJSON('/api/kc/registry/unregister', { name: t.name });
          if (r.ok) afterMutation(); else showStatus(errOf(r), true);
        });
        row.appendChild(del);
      } else {
        const pref = document.createElement('span');
        pref.className = 'wp-tools__pref-ro'; pref.textContent = t.preference;
        row.appendChild(pref);
        const en = document.createElement('span');
        en.className = 'wp-tools__en-ro'; en.textContent = t.enabled ? 'on' : 'off';
        row.appendChild(en);
      }
      return row;
    }
    function render(tools) {
      body.textContent = '';
      if (!tools.length) {
        const p = document.createElement('p'); p.className = 'wp-tools__empty';
        p.textContent = 'No tools registered.'; body.appendChild(p); return;
      }
      OP_ORDER.forEach((op) => {
        const inOp = tools.filter((t) => (t.operations || []).includes(op));
        if (!inOp.length) return;
        const h = document.createElement('h5');
        h.className = 'wp-tools__grouphdr'; h.textContent = OP_LABEL[op];
        body.appendChild(h);
        inOp.forEach((t) => body.appendChild(renderRow(t)));
      });
    }

    function showList() { formView.hidden = true; listView.hidden = false; showStatus(''); }
    function showForm(tool) {
      formTitle.textContent = tool ? ('Edit ' + tool.name) : 'Register a tool';
      nameInput.value = tool ? tool.name : '';
      nameInput.readOnly = !!tool;   // name identifies the record; fixed when editing
      const ops = tool ? (tool.operations.length ? tool.operations : ['compile']) : ['compile'];
      document.querySelectorAll('.tool-op').forEach((c) => { c.checked = ops.includes(c.value); });
      kindSel.value = tool ? tool.kind : 'cli';
      document.getElementById('tool-exec').value = tool ? tool.executable : '';
      document.getElementById('tool-argtpl').value = tool ? tool.argtpl : '';
      document.getElementById('tool-argtplc').value = tool ? tool.argtpl_circuit : '';
      const ep = tool ? tool.endpoint : '';
      connSel.value = (tool && tool.kind === 'kcmcp' && ep && ep !== 'managed') ? 'endpoint' : 'managed';
      document.getElementById('tool-endpoint').value = (ep && ep !== 'managed') ? ep : '';
      document.getElementById('tool-pref').value = tool ? tool.preference : 0;
      refreshPresets();
      if (tool) {
        infmtBox.querySelectorAll('input').forEach((cb) => { cb.checked = (tool.input_formats || []).includes(cb.value); });
        if (Array.from(outSel.options).some((o) => o.value === tool.output_format)) outSel.value = tool.output_format;
        if (Array.from(parserSel.options).some((o) => o.value === tool.parser)) parserSel.value = tool.parser;
      }
      applyKindFields();
      listView.hidden = true; formView.hidden = false; showStatus('');
      nameInput.focus();
    }
    if (addBtn) addBtn.addEventListener('click', () => showForm(null));
    if (backBtn) backBtn.addEventListener('click', showList);

    async function load() {
      try {
        const resp = await fetch('/api/kc/registry');
        const data = await resp.json().catch(() => ({}));
        if (!resp.ok) {
          body.textContent = '';
          const p = document.createElement('p'); p.className = 'wp-tools__empty';
          p.textContent = data.hint || data.error || ('HTTP ' + resp.status);
          body.appendChild(p);
          if (addBtn) addBtn.hidden = true;
          return;
        }
        canManage = !!data.can_manage;
        if (readonly) readonly.hidden = canManage;
        if (addBtn) addBtn.hidden = !canManage;
        render(data.tools || []);
      } catch (e) { showStatus(e.message, true); }
    }

    // Reload the panel after a change, and invalidate the /api/kc/tools cache
    // so the circuit-mode eval strip's compiler / wmc dropdowns pick the
    // change up too (a newly registered or re-enabled tool, a dropped one).
    function afterMutation() {
      load();
      Metadata.toolsDirty = true;
      Metadata.toolsCache = null;
      if (window.ProvsqlStudio.refreshToolAvailability)
        window.ProvsqlStudio.refreshToolAvailability();
    }

    if (registerBtn) registerBtn.addEventListener('click', async () => {
      const ops = selectedOps();
      const kcmcp = kindSel.value === 'kcmcp';
      const spec = {
        name: (nameInput.value || '').trim(),
        kind: kindSel.value,
        operations: ops,
        executable: kcmcp ? '' : (document.getElementById('tool-exec').value || '').trim(),
        argtpl: kcmcp ? '' : (document.getElementById('tool-argtpl').value || '').trim(),
        argtpl_circuit: kcmcp ? '' : (document.getElementById('tool-argtplc').value || '').trim(),
        endpoint: kcmcp ? (connSel.value === 'managed' ? 'managed'
                           : (document.getElementById('tool-endpoint').value || '').trim()) : '',
        input_formats: Array.from(infmtBox.querySelectorAll('input:checked')).map((c) => c.value),
        output_format: outSel.value,
        parser: parserSel.value,
        preference: parseInt(document.getElementById('tool-pref').value, 10) || 0,
        enabled: true,
      };
      if (!spec.name) { showStatus('name is required', true); return; }
      if (!ops.length) { showStatus('select at least one operation', true); return; }
      if (kcmcp && connSel.value === 'endpoint' && !spec.endpoint) {
        showStatus('an endpoint address is required', true); return;
      }
      const r = await postJSON('/api/kc/registry/register', spec);
      if (r.ok) { showList(); afterMutation(); } else { showStatus(errOf(r), true); }
    });

    function open() { closeOtherNavPanels('tools-panel'); panel.hidden = false; btn.setAttribute('aria-expanded', 'true'); showList(); load(); }
    function close() { panel.hidden = true; btn.setAttribute('aria-expanded', 'false'); }
    btn.addEventListener('click', (e) => { e.stopPropagation(); if (panel.hidden) open(); else close(); });
    document.addEventListener('click', (e) => {
      if (!panel.hidden && !panel.contains(e.target) && e.target !== btn) close();
    });
  }


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

    async function load() {
      body.innerHTML = '<p class="wp-schema__empty">Loading…</p>';
      try {
        const resp = await fetch('/api/schema');
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        entries = await resp.json();
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
      // Within each schema, surface provenance-tracked tables first
      // (the user's primary working surface), then plain tables/views,
      // then mapping tables (rarely useful to inspect on their own;
      // they are auto-discovered by the eval strip). Alphabetical
      // within each rank, preserved by JS Array.sort's stability.
      const relRank = r => r.is_mapping ? 2 : (r.has_provenance ? 0 : 1);
      for (const rels of bySchema.values()) {
        rels.sort((a, b) => relRank(a) - relRank(b));
      }
      let html = '';
      for (const [schemaName, rels] of bySchema) {
        html += '<div class="wp-schema__group">';
        html += `<h5 class="wp-schema__schema">${escapeHtml(schemaName)}</h5>`;
        for (const r of rels) {
          const qname  = `${r.schema}.${r.table}`;
          // `bare_resolves` is the Python side's authoritative answer to
          // "would `SELECT ... FROM <bare>` find this exact relation
          // under the current search_path?". Use the bare name only when
          // the answer is yes; otherwise qualify so the click prefill
          // actually executes against the relation the user clicked.
          const insert = r.bare_resolves ? r.table : qname;
          // Hide the bookkeeping `provsql` uuid column from the user-visible
          // column list : its presence is what the PROV pill already signals.
          const visibleCols = r.columns.filter(c => c.name !== 'provsql');
          // A column in a composite (multi-attribute) key is only *part of*
          // the key; a lone key column is the whole key. Two key kinds get
          // distinct decoration: the primary key (solid underline, the
          // relational-schema convention) and a repair_key "BID" grouping
          // key (dotted underline), whose rows are mutually exclusive across
          // possible worlds. See case study 7.
          const pkWord = visibleCols.filter(c => c.pk).length > 1
            ? 'part of the primary key' : 'primary key';
          const bidWord = visibleCols.filter(c => c.bidkey).length > 1
            ? 'part of the grouping key' : 'grouping key';
          // Key decoration for a column, or null when it is in neither key.
          const keyDecor = c => {
            if (c.pk)
              return { cls: 'wp-schema__pk', word: pkWord, tip: pkWord };
            if (c.bidkey)
              return { cls: 'wp-schema__bidkey', word: bidWord,
                tip: bidWord + ' (repair_key: rows sharing it are '
                  + 'mutually exclusive across possible worlds)' };
            return null;
          };
          // Provenance-tracked tables (not mappings, not views) can have
          // a provenance mapping created on any of their columns. Render
          // each column name as a clickable span so the user can prefill
          // the corresponding `create_provenance_mapping(...)` call.
          const canMap = r.has_provenance && !r.is_mapping && r.kind === 'table';
          // ProvSQL-extended column types carry circuit references rather
          // than plain scalars, so query operators on them are intercepted by
          // the planner hook. Flag them with a small terracotta pill so the
          // user can spot them in the schema panel; matchesProvType handles
          // both the unqualified form (provsql on search_path) and the
          // qualified one.
          const colPill = c => {
            if (matchesProvType(c.type, 'random_variable')) {
              return `<span class="wp-schema__col-rv" title="random_variable: query operators on this column lift into provenance gates at planning time">rv</span>`;
            }
            if (matchesProvType(c.type, 'agg_token')) {
              return `<span class="wp-schema__col-agg" title="agg_token: each value carries a circuit UUID; click cells to inspect the underlying gate">agg</span>`;
            }
            return '';
          };
          const cols   = visibleCols.map(c => {
            const pill = colPill(c);
            // create_provenance_mapping needs a column whose value tags an
            // input gate. random_variable / agg_token columns don't carry an
            // extractable tag value (each row is a circuit gate or composite,
            // not a scalar), so the click affordance would prefill a
            // meaningless call. Render the column name as plain text in that
            // case.
            // Underline the column when it is part of a key (solid for the
            // primary key, dotted for a repair_key grouping key).
            const key = keyDecor(c);
            const keyCls = key ? ` ${key.cls}` : '';
            const keyTitle = key ? ` (${key.word})` : '';
            if (canMap && !pill) {
              return `<span class="wp-schema__col${keyCls}" data-action="create-mapping"`
                + ` data-qname="${escapeAttr(qname)}"`
                + ` data-table="${escapeAttr(r.table)}"`
                + ` data-col="${escapeAttr(c.name)}"`
                + ` title="Click to create a provenance mapping on ${escapeAttr(c.name)}${keyTitle}"`
                + `>${escapeHtml(c.name)}</span>`;
            }
            const nameHtml = key
              ? `<span class="${key.cls}" title="${escapeAttr(key.tip)}">${escapeHtml(c.name)}</span>`
              : escapeHtml(c.name);
            return `${nameHtml}${pill}`;
          }).join(', ');
          // Mapping is the more specific classification: a mapping view
          // typically also carries an implicit provsql column (the planner
          // re-injects it for any view that selects from a provenance-tracked
          // table), but tagging it as both is noisy. Show only "mapping".
          const showProv = r.has_provenance && !r.is_mapping;
          const provCls   = showProv ? ' wp-schema__rel--prov' : '';
          // The PROV / mapping pills (split into PROV-TID / PROV-BID /
          // PROV-OPAQUE when r.prov_kind is known) are built by the shared
          // helper so the notebook sidebar renders them identically.
          const relBadges = relProvBadges(r, escapeAttr);
          const titleSuffix =
              (showProv ? ' · provenance-tracked' : '')
            + (r.is_mapping ? ' · provenance mapping' : '');
          // Column count for the tooltip mirrors the comma-separated
          // list rendered above: `visibleCols` is the post-filter view
          // (hiding the bookkeeping `provsql` column when present), so
          // its length is authoritative.  Naive `r.columns.length - 1`
          // would undercount views by one : views' planner-injected
          // `provsql` column never lands in pg_attribute, so it isn't
          // in `r.columns` to begin with, and the subtraction would
          // remove a non-existent entry.
          const visibleCount = visibleCols.length;
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
            + relBadges
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
      // Notebook mode has no shared #request box: route to the current
      // (or a fresh) SQL cell instead.
      if (document.body.classList.contains('mode-notebook')
          && window.ProvsqlNotebook) {
        window.ProvsqlNotebook.insertSql(text);
        return;
      }
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
      if (document.body.classList.contains('mode-notebook')
          && window.ProvsqlNotebook) {
        window.ProvsqlNotebook.replaceSql(text);
        return;
      }
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
      // Clicking a relation row replaces the textarea with a ready-to-run
      // SELECT * FROM <relation> ; in practice that is what the user
      // wants nine times out of ten, so saving the keystrokes wins over
      // the bare-name insert.
      replaceQuery(`SELECT * FROM ${rel.dataset.qname};`);
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
      replaceQuery(`SELECT * FROM ${rel.dataset.qname};`);
      close();
    });
    search.addEventListener('input', () => { if (loaded) render(); });
    search.addEventListener('keydown', (e) => {
      if (e.key === 'Escape') close();
    });

    function open() {
      closeOtherNavPanels('schema-panel');
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
        // Re-fetch the tool-availability map so newly-installed (or
        // newly-removed) compilers are reflected in the eval-strip
        // dropdowns immediately. invalidateAll above already cleared
        // the cache; this triggers the actual round-trip and
        // re-applies the filter to the live <select> elements.
        if (window.ProvsqlStudio?.refreshToolAvailability) {
          window.ProvsqlStudio.refreshToolAvailability();
        }
      });
    }
  }

  function setupGucToggles() {
    const fs = document.getElementById('prov-scheme-fieldset');
    const up = document.getElementById('opt-update-prov');
    if (!fs || !up) return;
    const radios = fs.querySelectorAll('input[name="prov-scheme"]');

    // Persisted across mode switches. The stored value is the user's
    // last *circuit-mode* pick; Where UI mode locks the selector to
    // `where` but does not overwrite the stored value, so a
    // circuit→where→circuit round-trip preserves the user's pick.
    // `boolean`/`absorptive`/`semiring`/`where`; default `semiring`.
    const savedMode = sessionStorage.getItem('ps.opt.provScheme') || 'semiring';
    const savedUpdate = sessionStorage.getItem('ps.opt.updateProv') === '1';

    const setMode = (m) => {
      radios.forEach((r) => { r.checked = (r.value === m); });
    };

    if (mode === 'where') {
      setMode('where');
      fs.classList.add('is-locked');
      fs.title = 'Where UI mode requires where-provenance (the cell-highlight wrap depends on it). '
               + 'Switch to Circuit mode to pick Boolean or Semiring.';
      radios.forEach((r) => { r.disabled = true; });
    } else {
      setMode(['where', 'boolean', 'absorptive'].includes(savedMode) ? savedMode : 'semiring');
    }
    up.checked = savedUpdate;

    fs.addEventListener('change', () => {
      if (mode === 'where') return;
      const picked = fs.querySelector('input[name="prov-scheme"]:checked');
      if (picked) sessionStorage.setItem('ps.opt.provScheme', picked.value);
    });
    up.addEventListener('change', () => {
      sessionStorage.setItem('ps.opt.updateProv', up.checked ? '1' : '0');
    });
  }

  function setupCircuitMode() {
    document.getElementById('sidebar-title').textContent = 'Provenance Circuit';
    document.getElementById('sidebar-body').innerHTML = circuitSidebarHtml();
    // Hide eval-args options for missing external tools.  Fire-and-
    // forget: by the time the user opens the Compilation / wmc
    // dropdowns the fetch is usually back; if it isn't, the dropdown
    // briefly shows every option and is filtered when the response
    // lands.
    refreshToolAvailability();
    document.getElementById('result-legend').innerHTML =
      '<span class="wp-legend-swatch" style="background:var(--purple-500)"></span> Click a UUID / agg_token cell in the result to inspect its circuit.';

    // Click handler on result-body for UUID/agg_token cells. We rely on the
    // cell having data-circuit-uuid when it's clickable; set during render.
    // data-row-prov (also set during render) is forwarded so the eval
    // strip's "Condition on" auto-preset reflects the row the user just
    // clicked, including when the target was the row's random_variable
    // cell (whose scene root is the RV itself, not the row's prov).
    document.getElementById('result-body').addEventListener('click', (e) => {
      const cell = e.target.closest('.wp-result__cell.is-clickable');
      if (!cell || !cell.dataset.circuitUuid) return;
      loadCircuit(cell.dataset.circuitUuid, { rowProv: cell.dataset.rowProv || '' });
    });

    // If a query was carried over (mode switch / preload), run it so the
    // user has clickable cells immediately; otherwise wait for them to type.
    const carry = preloadCircuitUuid;
    // Coming from where mode's per-row "→ Circuit" jump: the carried UUID
    // was minted by a where-provenance wrap, so the same query must run
    // with where_provenance on here for the resulting circuit to contain
    // the project/eq gates the user is trying to inspect. Force the
    // selector to the `where` flavour (the user can switch it back for
    // follow-up runs); the radio flip is programmatic so it does not
    // fire `change` and does not get persisted to sessionStorage.
    if (carry) {
      const r = document.querySelector('input[name="prov-scheme"][value="where"]');
      if (r) r.checked = true;
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
        if (carry) loadCircuit(carry, { rowProv: preloadCircuitRowProv || '' });
      });
    } else if (carry) {
      // No query but a preload UUID: render the circuit directly.
      // Deferred for the same TDZ reason as the ensureCircuitLib call
      // above: setupCircuitMode runs synchronously during IIFE eval and
      // loadCircuit reaches the let-declared _circuitLibPromise. The
      // where->circuit jump never hit this branch (it always carries a
      // query, so the .then() above ran after the IIFE); the notebook's
      // circuit-cell jump carries only the UUID and does.
      queueMicrotask(() => loadCircuit(carry,
        { rowProv: preloadCircuitRowProv || '' }));
    }
  }

  /* ──────── Contributions mode ──────── */

  // ─────────────────────── Temporal mode ───────────────────────
  // A timeline over provenance validity. Two orthogonal controls: a SOURCE
  // (a tracked Relation, or an arbitrary Query interpreted over the temporal
  // semiring via a validity mapping) and a TIME operation (As of / During /
  // History of / Full). A relation source uses the timetravel/timeslice/
  // history SRFs; a query source wraps the SQL with sr_temporal and filters on
  // it. Each returned row is one lane; its tstzmultirange is one bar per
  // disjoint sub-range. "As of" adds a draggable scrubber (debounced).
  function temporalSidebarHtml() {
    return `
      <div class="cv-temporal">
        <div class="cv-temporal__controls" role="toolbar" aria-label="Temporal controls">
          <div class="cv-temporal__seg" role="radiogroup" aria-label="Source">
            <button type="button" class="cv-temporal__src is-active" data-source="relation" title="A provenance-tracked relation or temporal view.">Relation</button>
            <button type="button" class="cv-temporal__src" data-source="query" title="Interpret the query in the box over the temporal (interval-union) semiring through a validity mapping.">Query</button>
          </div>
          <label class="cv-temporal__ctl" id="temporal-rel-ctl">
            <span>Relation</span>
            <select id="temporal-relation" title="A provenance-tracked relation or temporal view the time-travel functions accept."></select>
          </label>
          <label class="cv-temporal__ctl" id="temporal-map-ctl" hidden>
            <span>Validity mapping</span>
            <select id="temporal-mapping" title="The token→validity mapping sr_temporal evaluates over (e.g. time_validity_view)."></select>
          </label>
          <div class="cv-temporal__seg" role="radiogroup" aria-label="Time operation">
            <button type="button" class="cv-temporal__op is-active" data-timeop="asof" title="Rows valid at a single instant.">As of</button>
            <button type="button" class="cv-temporal__op" data-timeop="during" title="Rows valid during a window.">During</button>
            <button type="button" class="cv-temporal__op" data-timeop="history" title="All versions matching a key column (relation only).">History of</button>
            <button type="button" class="cv-temporal__op" data-timeop="full" title="Every row, with its full validity (query only).">Full</button>
          </div>
          <div class="cv-temporal__inputs" id="temporal-inputs"></div>
        </div>
        <div class="cv-temporal__status" id="temporal-status" hidden></div>
        <div class="cv-temporal__timeline" id="temporal-timeline"></div>
      </div>`;
  }

  function setupTemporalMode() {
    document.getElementById('sidebar-title').textContent = 'Temporal';
    document.getElementById('sidebar-body').innerHTML = temporalSidebarHtml();
    document.getElementById('result-legend').innerHTML =
      '<span class="wp-legend-swatch" style="background:var(--blue-500,#3b6ea5)"></span> '
      + 'Pick a relation and a time query; each row is drawn on the timeline by its validity.';

    const state = { source: 'relation', timeop: 'asof', relation: null,
                    at: null, from: null, to: null, col: null, val: '',
                    mapping: null, mappings: [], columns: [], data: null };
    const relSel  = document.getElementById('temporal-relation');
    const relCtl  = document.getElementById('temporal-rel-ctl');
    const mapSel  = document.getElementById('temporal-mapping');
    const mapCtl  = document.getElementById('temporal-map-ctl');
    const inputs  = document.getElementById('temporal-inputs');
    const statusEl = document.getElementById('temporal-status');
    const tl       = document.getElementById('temporal-timeline');
    const srcBtns = Array.from(document.querySelectorAll('.cv-temporal__src'));
    const opBtns  = Array.from(document.querySelectorAll('.cv-temporal__op'));
    const reqEl   = document.getElementById('request');

    const isoDay = (d) => (d || '').slice(0, 10);
    const todayIso = new Date().toISOString().slice(0, 10);
    // The time operations valid for each source.
    const OPS = { relation: ['asof', 'during', 'history'], query: ['asof', 'during', 'full'] };

    let debounceTimer = null;
    const debouncedFetch = () => {
      clearTimeout(debounceTimer);
      debounceTimer = setTimeout(fetchTemporal, 200);
    };

    // Show the right source control; enable only the time ops valid for the
    // current source, snapping the selection if it became invalid.
    function syncAvailability() {
      if (relCtl) relCtl.hidden = state.source !== 'relation';
      if (mapCtl) mapCtl.hidden = state.source !== 'query';
      const valid = OPS[state.source];
      if (!valid.includes(state.timeop)) state.timeop = valid[0];
      opBtns.forEach((b) => {
        b.disabled = !valid.includes(b.dataset.timeop);
        b.classList.toggle('is-active', b.dataset.timeop === state.timeop);
      });
      srcBtns.forEach((b) => b.classList.toggle('is-active', b.dataset.source === state.source));
    }

    // Render the time-op inputs (date / window / key) into #temporal-inputs.
    function renderInputs() {
      if (state.timeop === 'asof') {
        state.at = state.at || todayIso;
        inputs.innerHTML =
          `<label class="cv-temporal__ctl"><span>At</span>`
          + `<input type="date" id="temporal-at" value="${escapeAttr(isoDay(state.at))}"></label>`;
        document.getElementById('temporal-at').addEventListener('change', (e) => {
          state.at = e.target.value; debouncedFetch();
        });
      } else if (state.timeop === 'during') {
        state.from = state.from || '2000-01-01';
        state.to   = state.to   || todayIso;
        inputs.innerHTML =
          `<label class="cv-temporal__ctl"><span>From</span>`
          + `<input type="date" id="temporal-from" value="${escapeAttr(isoDay(state.from))}"></label>`
          + `<label class="cv-temporal__ctl"><span>To</span>`
          + `<input type="date" id="temporal-to" value="${escapeAttr(isoDay(state.to))}"></label>`;
        document.getElementById('temporal-from').addEventListener('change', (e) => {
          state.from = e.target.value; debouncedFetch();
        });
        document.getElementById('temporal-to').addEventListener('change', (e) => {
          state.to = e.target.value; debouncedFetch();
        });
      } else if (state.timeop === 'history') {
        const colsOpts = (state.columns || [])
          .map((c) => `<option value="${escapeAttr(c)}"${c === state.col ? ' selected' : ''}>${escapeHtml(c)}</option>`)
          .join('');
        inputs.innerHTML =
          `<label class="cv-temporal__ctl"><span>Column</span>`
          + `<select id="temporal-col">${colsOpts}</select></label>`
          + `<label class="cv-temporal__ctl"><span>Value</span>`
          + `<input type="text" id="temporal-val" value="${escapeAttr(state.val || '')}" placeholder="e.g. Prime Minister"></label>`;
        document.getElementById('temporal-col').addEventListener('change', (e) => {
          state.col = e.target.value; debouncedFetch();
        });
        document.getElementById('temporal-val').addEventListener('input', (e) => {
          state.val = e.target.value; debouncedFetch();
        });
      } else { // full: no time inputs (every row, full validity)
        inputs.innerHTML = '';
      }
    }

    srcBtns.forEach((btn) => {
      btn.addEventListener('click', () => {
        state.source = btn.dataset.source;
        syncAvailability();
        renderInputs();
        if (state.source === 'query') {
          populateMappings();
          // Seed the box with a starter query unless it already holds the
          // user's own SQL (i.e. not a generated SRF query).
          if (reqEl && (!reqEl.value.trim() || /\bAS t\(/.test(reqEl.value)
              || /provsql\.(timeslice|timetravel|history)/.test(reqEl.value))) {
            reqEl.value = `SELECT *\nFROM ${state.relation || 'your_relation'}\nLIMIT 50`;
          }
        }
        fetchTemporal();
      });
    });

    opBtns.forEach((btn) => {
      btn.addEventListener('click', () => {
        if (btn.disabled) return;
        state.timeop = btn.dataset.timeop;
        syncAvailability();
        renderInputs();
        fetchTemporal();
      });
    });

    relSel.addEventListener('change', () => { state.relation = relSel.value || null; fetchTemporal(); });
    mapSel.addEventListener('change', () => { state.mapping = mapSel.value || null; fetchTemporal(); });
    // Re-draw from the box when the user edits the query (query source only).
    if (reqEl) reqEl.addEventListener('change', () => { if (state.source === 'query') debouncedFetch(); });

    syncAvailability();
    populateRelations();
    renderInputs();

    async function populateRelations() {
      let rows = [];
      try {
        const resp = await fetch('/api/temporal_relations');
        if (resp.ok) rows = await resp.json();
      } catch { /* leave empty; the picker just shows the placeholder */ }
      relSel.innerHTML = '<option value="">— pick a relation —</option>'
        + rows.map((m) => `<option value="${escapeAttr(m.qname)}">${escapeHtml(m.display_name || m.qname)}</option>`).join('');
      // Auto-select a likely temporal view (one ending in _validity is a
      // mapping; prefer a plain view), else the first relation.
      if (rows.length) {
        const pref = rows.find((m) => !/_validity$/.test(m.name)) || rows[0];
        relSel.value = pref.qname; state.relation = pref.qname;
        if (state.source === 'relation') fetchTemporal();
      }
    }

    async function populateMappings() {
      if (!state.mappings.length) {
        try {
          const resp = await fetch('/api/temporal_mappings');
          if (resp.ok) state.mappings = await resp.json();
        } catch { /* leave empty; the picker shows nothing */ }
        mapSel.innerHTML = state.mappings
          .map((m) => `<option value="${escapeAttr(m.qname)}">${escapeHtml(m.display_name || m.qname)}</option>`)
          .join('');
      }
      if (!state.mapping && state.mappings.length) state.mapping = state.mappings[0].qname;
      if (mapSel && state.mapping) mapSel.value = state.mapping;
    }

    function setStatus(msg, isError) {
      if (!msg) { statusEl.hidden = true; statusEl.textContent = ''; return; }
      statusEl.hidden = false;
      statusEl.textContent = msg;
      statusEl.classList.toggle('is-error', !!isError);
    }

    async function fetchTemporal() {
      const body = { source: state.source, timeop: state.timeop };
      if (state.source === 'query') {
        body.query = reqEl ? reqEl.value : '';
        body.mapping = state.mapping;
        if (!body.query.trim()) {
          tl.innerHTML = ''; setStatus('Type a query in the box.'); return;
        }
        if (!body.mapping) { setStatus('Pick a validity mapping.', true); return; }
      } else {
        if (!state.relation) { tl.innerHTML = ''; setStatus('Pick a relation to begin.'); return; }
        body.relation = state.relation;
      }
      if (state.timeop === 'asof') body.at_time = state.at;
      else if (state.timeop === 'during') { body.from_time = state.from; body.to_time = state.to; }
      else if (state.timeop === 'history') { body.col_names = [state.col]; body.col_values = [state.val]; }
      setStatus('Querying…');
      let data;
      try {
        const resp = await fetch('/api/temporal', {
          method: 'POST', headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(body),
        });
        data = await resp.json();
        if (!resp.ok) { setStatus(data.error || 'Temporal query failed', true); return; }
      } catch (e) { setStatus('Request failed: ' + e, true); return; }
      state.data = data;
      state.columns = data.columns || [];
      // For a relation source, show the composed SQL in the shared query box
      // (the timeline is the view, this is the runnable query behind it). For a
      // query source the box is the user's input, so leave it untouched.
      if (state.source !== 'query' && reqEl && data.sql) reqEl.value = data.sql;
      // History needs a column picker; (re)render inputs once columns are known.
      if (state.timeop === 'history' && !state.col && state.columns.length) {
        state.col = state.columns[0]; renderInputs();
      }
      setStatus(data.result.length ? '' : 'No rows match.');
      renderTimeline(data);
      // Mirror the rows into the shared result table so the query box, the
      // result table, and the timeline all agree (the table is the tabular
      // companion to the timeline). The box already holds the SQL to run.
      if (reqEl && reqEl.value.trim()) {
        try { runQuery({ preventDefault() {} }); } catch (e) { /* table is secondary */ }
      }
    }

    // ── timeline rendering ──
    const parseT = (s) => (s == null ? null : Date.parse(s));
    function renderTimeline(data) {
      const rows = data.result || [];
      const segs = [];
      rows.forEach((r) => (r.valid_time || []).forEach((iv) => {
        const lo = parseT(iv.lower), hi = parseT(iv.upper);
        if (lo != null) segs.push(lo);
        if (hi != null) segs.push(hi);
      }));
      if (!segs.length) { tl.innerHTML = ''; return; }
      let t0 = Math.min(...segs), t1 = Math.max(...segs);
      if (t0 === t1) { t1 = t0 + 86400000; }            // a 1-day window if degenerate
      const pad = (t1 - t0) * 0.04; t0 -= pad; t1 += pad;
      const span = t1 - t0;
      const xOf = (t) => ((t - t0) / span) * 100;        // percent
      const clampPct = (p) => Math.max(0, Math.min(100, p));

      // Year ticks across the domain.
      const y0 = new Date(t0).getUTCFullYear(), y1 = new Date(t1).getUTCFullYear();
      const step = Math.max(1, Math.ceil((y1 - y0) / 8));
      let axis = '';
      for (let y = Math.ceil(y0 / step) * step; y <= y1; y += step) {
        const x = xOf(Date.UTC(y, 0, 1));
        if (x < 0 || x > 100) continue;
        axis += `<div class="cv-temporal__tick" style="left:${x}%"><span>${y}</span></div>`;
      }

      const lanes = rows.map((r) => {
        // Label from the row's plain columns; skip multirange values
        // (e.g. a table's own `validity`) since the bars already show them.
        const label = state.columns.map((c) => r[c])
          .filter((v) => v != null && !/^\{[[(]/.test(String(v))).join(' · ');
        const bars = (r.valid_time || []).map((iv) => {
          const lo = parseT(iv.lower), hi = parseT(iv.upper);
          const a = clampPct(xOf(lo == null ? t0 : lo));
          const b = clampPct(xOf(hi == null ? t1 : hi));
          const open = (lo == null ? ' is-open-l' : '') + (hi == null ? ' is-open-r' : '');
          const tip = `${iv.lower || '−∞'} → ${iv.upper || '+∞'}`;
          return `<div class="cv-temporal__bar${open}" style="left:${a}%;width:${Math.max(0.4, b - a)}%" title="${escapeAttr(tip)}"></div>`;
        }).join('');
        return `<div class="cv-temporal__lane" data-uuid="${escapeAttr(r.provsql || '')}">`
             + `<div class="cv-temporal__lanelbl" title="${escapeAttr(label)}">${escapeHtml(label || '(row)')}</div>`
             + `<div class="cv-temporal__track">${bars}</div></div>`;
      }).join('');

      // "As of" scrubber: a draggable playhead at state.at.
      let playhead = '';
      if (state.timeop === 'asof' && state.at) {
        const px = clampPct(xOf(parseT(state.at + 'T00:00:00Z')));
        playhead = `<div class="cv-temporal__playhead" id="temporal-playhead" style="left:${px}%" title="Drag to time-travel"></div>`;
      }

      tl.innerHTML =
        `<div class="cv-temporal__axis">${axis}${playhead}</div>`
        + `<div class="cv-temporal__lanes">${lanes}</div>`;

      if (state.timeop === 'asof') wireScrubber(t0, span);
    }

    // Drag the playhead → set state.at (UTC day) → debounced re-snapshot.
    function wireScrubber(t0, span) {
      const ph = document.getElementById('temporal-playhead');
      const axis = tl.querySelector('.cv-temporal__axis');
      if (!ph || !axis) return;
      const toDate = (clientX) => {
        const rect = axis.getBoundingClientRect();
        const frac = Math.max(0, Math.min(1, (clientX - rect.left) / rect.width));
        return new Date(t0 + frac * span).toISOString().slice(0, 10);
      };
      let dragging = false;
      const onMove = (ev) => {
        if (!dragging) return;
        const day = toDate(ev.clientX);
        state.at = day;
        const px = ((Date.parse(day + 'T00:00:00Z') - t0) / span) * 100;
        ph.style.left = Math.max(0, Math.min(100, px)) + '%';
        const atInput = document.getElementById('temporal-at');
        if (atInput) atInput.value = day;
        debouncedFetch();
      };
      ph.addEventListener('mousedown', (e) => { dragging = true; e.preventDefault(); });
      window.addEventListener('mousemove', onMove);
      window.addEventListener('mouseup', () => { dragging = false; });
      // Clicking anywhere on the axis also moves the playhead.
      axis.addEventListener('click', (e) => {
        if (e.target === ph) return;
        const day = toDate(e.clientX);
        state.at = day;
        const atInput = document.getElementById('temporal-at');
        if (atInput) atInput.value = day;
        fetchTemporal();
      });
    }
  }

  function contributionsSidebarHtml() {
    return `
      <div class="cv-contrib">
        <div class="cv-contrib__controls" role="toolbar" aria-label="Contribution controls">
          <label class="cv-contrib__ctl">
            <span>Measure</span>
            <select id="contrib-measure" title="Shapley value (averaged over orderings) or Banzhaf power index (averaged over coalitions).">
              <option value="shapley">Shapley</option>
              <option value="banzhaf">Banzhaf</option>
            </select>
          </label>
          <label class="cv-contrib__ctl">
            <span>Method</span>
            <select id="contrib-method" title="How the d-D circuit behind the contribution is built. 'auto' cost-selects the cheapest route (interpret-as-dd / tree-decomposition / compilation) like the probability chooser; the named routes force one. 'compilation' uses the external d-DNNF compiler picked beside it.">
              <option value="">auto (cost-based)</option>
              <option value="tree-decomposition">tree-decomposition</option>
              <option value="interpret-as-dd">interpret as d-D</option>
              <option value="compilation">compilation</option>
            </select>
          </label>
          <label class="cv-contrib__ctl" id="contrib-compiler-ctl" hidden>
            <span>Compiler</span>
            <select id="contrib-compiler" title="External d-DNNF compiler for the 'compilation' route, populated from the live tool registry (the same compilers Probability evaluate offers).">
            </select>
          </label>
          <label class="cv-contrib__ctl">
            <span>Labels</span>
            <select id="contrib-mapping" title="Provenance mapping used to label inputs; 'source row' resolves each input to its tracked row via resolve_input.">
              <option value="">source row</option>
            </select>
          </label>
          <button class="cv-tool cv-tool--toggle cv-contrib__uuids" id="contrib-show-uuids"
                  type="button" aria-pressed="false" title="Show full UUIDs">
            <i class="fas fa-fingerprint"></i>
          </button>
        </div>
        <div class="cv-contrib__targetline">
          <span class="cv-contrib__targetlbl">Target</span>
          <span class="cv-contrib__target" id="contrib-target" title="">none pinned</span>
          <span class="cv-contrib__time" id="contrib-time"></span>
        </div>
        <div class="cv-contrib__status" id="contrib-status" hidden></div>
        <div class="cv-contrib__chart" id="contrib-chart"></div>
      </div>`;
  }

  function setupContributionsMode() {
    document.getElementById('sidebar-title').textContent = 'Contributions';
    document.getElementById('sidebar-body').innerHTML = contributionsSidebarHtml();
    document.getElementById('result-legend').innerHTML =
      '<span class="wp-legend-swatch" style="background:var(--terracotta-500)"></span> '
      + 'Click a UUID cell in the result to compute its per-input Shapley / Banzhaf contributions.';

    const state = { token: null };
    const chart     = document.getElementById('contrib-chart');
    const statusEl  = document.getElementById('contrib-status');
    const timeEl    = document.getElementById('contrib-time');
    const measureSel = document.getElementById('contrib-measure');
    const methodSel  = document.getElementById('contrib-method');
    const mapSel     = document.getElementById('contrib-mapping');
    const compilerSel = document.getElementById('contrib-compiler');
    const compilerCtl = document.getElementById('contrib-compiler-ctl');
    const targetEl   = document.getElementById('contrib-target');
    const uuidsBtn   = document.getElementById('contrib-show-uuids');

    // Short / full UUID pair, matching the result table's formatCell output
    // so the body-level `show-uuids` class (driven by the fingerprint toggle
    // below) flips abbreviated vs full display here too, with no re-render.
    const uuidPairHtml = (s) => {
      const str = String(s == null ? '' : s);
      const short = str.length > 4 ? str.slice(0, 4) + '…' : str;
      return `<span class="wp-uuid" title="${escapeAttr(str)}">`
           + `<span class="wp-uuid__short">${escapeHtml(short)}</span>`
           + `<span class="wp-uuid__full">${escapeHtml(str)}</span>`
           + `</span>`;
    };

    populateContribMappings();
    populateContribCompilers();

    // The compiler picker only applies to the 'compilation' route; show it
    // exactly when that method is selected, mirroring the eval strip.
    const syncCompilerVisibility = () => {
      if (compilerCtl) compilerCtl.hidden = (methodSel.value !== 'compilation');
    };
    methodSel.addEventListener('change', syncCompilerVisibility);
    syncCompilerVisibility();

    // Fingerprint toggle: expand every abbreviated UUID on the page (result
    // table cells, target line, unresolved bar labels) to its full form via
    // the shared `body.show-uuids` class the circuit toolbar also drives.
    if (uuidsBtn) {
      uuidsBtn.setAttribute('aria-pressed',
        String(document.body.classList.contains('show-uuids')));
      uuidsBtn.addEventListener('click', () => {
        const on = !document.body.classList.contains('show-uuids');
        document.body.classList.toggle('show-uuids', on);
        uuidsBtn.setAttribute('aria-pressed', String(on));
      });
    }

    // Click a contribution value to expand it to full precision (toggle) and
    // copy that full-precision form to the clipboard. Mirrors the circuit
    // eval strip's click-to-flip + copy on probability results.
    chart.addEventListener('click', (e) => {
      const val = e.target.closest('.cv-contrib__val.is-clickable');
      if (!val) return;
      const expanded = val.dataset.expanded === '1';
      const next = expanded ? val.dataset.rounded : val.dataset.full;
      if (next == null) return;
      val.textContent = next;
      val.dataset.expanded = expanded ? '' : '1';
      val.title = expanded ? 'Click to show full precision and copy'
                           : 'Click to show rounded value';
      copyToClipboard(val.dataset.full || '');
      val.classList.add('is-copied');
      setTimeout(() => val.classList.remove('is-copied'), 800);
    });

    // Re-fetch when a control changes, but only once a token is pinned.
    [measureSel, methodSel, mapSel, compilerSel].forEach((el) => {
      el.addEventListener('change', () => { if (state.token) fetchContributions(); });
    });

    // Cell click pins the token (same is-clickable / data-circuit-uuid
    // affordance circuit mode uses; contributions joins clickableUuid).
    document.getElementById('result-body').addEventListener('click', (e) => {
      const cell = e.target.closest('.wp-result__cell.is-clickable');
      if (!cell || !cell.dataset.circuitUuid) return;
      state.token = cell.dataset.circuitUuid;
      fetchContributions();
    });

    // Carry from a where-mode "→ Contributions" jump, mirroring the
    // circuit-mode preload: replay the query when it is still the last-run
    // SQL, then pin the carried token.
    const carry = preloadContribUuid;
    const shouldReplay =
      (carriedRan || carry) && document.getElementById('request').value.trim();
    if (shouldReplay) {
      runQuery({ preventDefault() {} }).then(() => {
        if (carry) { state.token = carry; fetchContributions(); }
      });
    } else if (carry) {
      state.token = carry;
      queueMicrotask(fetchContributions);
    }

    async function populateContribMappings() {
      let rows = [];
      try {
        const resp = await fetch('/api/provenance_mappings');
        if (resp.ok) rows = await resp.json();
      } catch { /* leave the default "source row" option only */ }
      for (const m of rows) {
        const opt = document.createElement('option');
        opt.value = m.qname;
        opt.textContent = m.display_name || m.qname;
        mapSel.appendChild(opt);
      }
    }

    // Populate the compiler picker with the external d-DNNF compilers the
    // backend can actually reach, reusing the eval strip's /api/kc/tools
    // discovery (cached on Metadata).  In-process routes are separate Method
    // options here, so only the external `compile` tools belong in this list.
    // Soft-fail: a missing discovery surface leaves an empty box, and picking
    // 'compilation' then surfaces a run-time error, as in the eval strip.
    async function populateContribCompilers() {
      if (!compilerSel) return;
      const data = await refreshToolAvailability();
      const entries = ((data && data.compile) || []).filter((t) => t.available);
      compilerSel.replaceChildren();
      for (const t of entries) {
        const opt = document.createElement('option');
        opt.value = t.name;
        opt.textContent = _toolLabel(t.name);
        compilerSel.appendChild(opt);
      }
      if (!entries.length) {
        const opt = document.createElement('option');
        opt.value = '';
        opt.textContent = '(no compiler available)';
        opt.disabled = true;
        compilerSel.appendChild(opt);
      }
    }

    async function fetchContributions() {
      const measure = measureSel.value;
      const method  = methodSel.value || null;
      const mapping = mapSel.value || null;
      // The 'compilation' route names its external d-DNNF compiler through the
      // `arguments` field (shapley_all_vars forwards it to makeDD/compilation);
      // an empty value lets ProvSQL pick the highest-preference compiler.
      const args = (method === 'compilation' && compilerSel)
        ? (compilerSel.value || null) : null;
      if (state.token) {
        targetEl.innerHTML = uuidPairHtml(state.token);
      } else {
        targetEl.textContent = 'none pinned';
      }
      targetEl.title = state.token || '';
      if (timeEl) timeEl.textContent = '';
      statusEl.hidden = false;
      statusEl.textContent = 'Computing…';
      chart.innerHTML = '';
      // Round-trip time, captured around the fetch + JSON parse the same way
      // the circuit eval strip times /api/evaluate, so contribution cost is
      // comparable across the Method routes.
      const t0 = performance.now();
      let resp;
      try {
        resp = await fetch('/api/contributions', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ token: state.token, measure, method, mapping, arguments: args }),
        });
      } catch (e) {
        if (timeEl) timeEl.textContent = `· ${Math.round(performance.now() - t0)} ms`;
        statusEl.textContent = 'Network error: ' + e.message;
        return;
      }
      const data = await resp.json().catch(() => ({}));
      if (timeEl) timeEl.textContent = `· ${Math.round(performance.now() - t0)} ms`;
      if (!resp.ok) {
        statusEl.textContent = data.detail || data.error || ('HTTP ' + resp.status);
        return;
      }
      renderContributions(data, mapping);
    }

    function renderContributions(data, mapping) {
      const items = data.result || [];
      const measureName = data.measure === 'banzhaf' ? 'Banzhaf' : 'Shapley';
      if (!items.length) {
        statusEl.hidden = false;
        statusEl.textContent = 'No input variables contribute to this token.';
        chart.innerHTML = '';
        return;
      }
      const CAP = 200;
      const shown = items.slice(0, CAP);
      const nums = shown
        .map((r) => (typeof r.value === 'number' ? Math.abs(r.value) : 0));
      const maxAbs = Math.max(1e-12, ...nums);
      if (items.length > CAP) {
        statusEl.hidden = false;
        statusEl.textContent =
          `Showing top ${CAP} of ${items.length} inputs by |${measureName}|.`;
      } else {
        statusEl.hidden = true;
      }
      // Probability decimals follow the Config panel's setting (default 4),
      // matching the circuit eval strip; the full-precision form is one click
      // away and lands on the clipboard.
      const dec = getProbDecimals();
      chart.innerHTML = shown.map((r) => {
        const v = (typeof r.value === 'number') ? r.value : null;
        const frac = v == null ? 0 : Math.abs(v) / maxAbs;
        const half = (frac * 50).toFixed(2);
        const pos = (v == null) || v >= 0;
        const style = pos
          ? `left:50%;width:${half}%`
          : `left:${(50 - half).toFixed(2)}%;width:${half}%`;
        const rounded = v == null ? '—' : v.toFixed(dec);
        const valAttrs = v == null ? ''
          : ` data-full="${escapeAttr(String(v))}" data-rounded="${escapeAttr(rounded)}"`
            + ` title="Click to show full precision and copy"`;
        const valCls = 'cv-contrib__val' + (pos ? '' : ' is-neg')
          + (v == null ? '' : ' is-clickable');
        const hasLabel = r.label != null && String(r.label) !== '';
        const labelHtml = hasLabel
          ? escapeHtml(String(r.label))
          : `<span class="cv-contrib__lazy" data-var="${escapeAttr(r.variable)}">`
            + uuidPairHtml(r.variable) + `</span>`;
        const titleAttr = escapeAttr(hasLabel ? String(r.label) : r.variable);
        return `<div class="cv-contrib__bar">
          <div class="cv-contrib__barhead">
            <span class="cv-contrib__label" title="${titleAttr}">${labelHtml}</span>
            <span class="${valCls}"${valAttrs}>${rounded}</span>
          </div>
          <div class="cv-contrib__track">
            <div class="cv-contrib__fill ${pos ? 'is-pos' : 'is-neg'}" style="${style}"></div>
          </div>
        </div>`;
      }).join('');
      // Lazily resolve source-row labels when no mapping is selected.
      // Capped so a wide input relation doesn't fan out hundreds of
      // /api/leaf requests; users who want every label pick a mapping.
      if (!mapping) resolveContribLabels(chart, 60);
    }

    async function resolveContribLabels(root, limit) {
      const chips = Array.from(root.querySelectorAll('.cv-contrib__lazy'))
        .slice(0, limit);
      for (const chip of chips) {
        const uuid = chip.dataset.var;
        if (!uuid) continue;
        let data;
        try {
          const resp = await fetch(`/api/leaf/${encodeURIComponent(uuid)}`);
          if (!resp.ok) continue;
          data = await resp.json();
        } catch { continue; }
        const label = labelFromLeaf(data);
        if (!label) continue;
        chip.textContent = label.text;
        chip.classList.remove('cv-contrib__lazy');
        const cell = chip.closest('.cv-contrib__label');
        // The visible label is values-only (compact, clipped to one line);
        // the tooltip names every column so the row is readable on hover.
        if (cell) cell.title = label.title;
      }
    }

    function labelFromLeaf(data) {
      const m = (data && data.matches && data.matches[0]) || null;
      if (!m) return null;
      const rel = m.relation || '';
      // `resolve_input` already strips the bookkeeping `provsql` token and
      // the row is reordered to table-column order, so iterate it directly.
      let entries = [];
      try {
        entries = Object.entries(m.row || {}).filter(([, val]) => val != null);
      } catch { /* relation-only fallback */ }
      const values = entries.map(([, val]) => String(val)).join(', ');
      // Visible: "relation: v1, v2, …" (clipped by CSS). Tooltip: the
      // relation then one "column: value" per line.
      const text = rel && values ? `${rel}: ${values}` : (rel || values || '');
      if (!text) return null;
      const named = entries.map(([k, val]) => `${k}: ${val}`).join('\n');
      const title = rel && named ? `${rel}\n${named}` : (named || rel || text);
      return { text, title };
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
    window.ProvsqlCircuit.renderCircuit(scene, {
      rowProv: (opts && opts.rowProv) || '',
    });
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
        <span class="cv-tool__sep" id="tool-kc-back-sep" hidden></span>
        <button class="cv-tool cv-tool--kc-back" id="tool-kc-back" title="Back to the original provenance circuit" aria-label="Back to the original provenance circuit" hidden><i class="fas fa-arrow-left"></i></button>
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
            <!-- Compiled-semiring sub-optgroups (Boolean & symbolic,
                 Lineage, Numeric, Interval-valued) are injected at init
                 from circuit.js's compiled-semiring registry, ahead of
                 the "Custom Semirings" / "Other" groups below. -->
            <optgroup label="Custom Semirings" id="eval-custom-group" hidden>
              <!-- populated lazily from /api/custom_semirings -->
            </optgroup>
            <optgroup label="Distribution">
              <option value="distribution-profile">Distribution profile</option>
              <option value="moment">Moment</option>
              <option value="sample">Sample</option>
            </optgroup>
            <optgroup label="Probability">
              <option value="probability">Marginal probability</option>
              <option value="kc-benchmark">Probability benchmark</option>
            </optgroup>
            <optgroup label="Knowledge compilation">
              <option value="kc-cnf">Tseytin CNF</option>
              <option value="kc-ddnnf">Compiled d-D circuit</option>
              <option value="kc-nnf">Compiled d-D (NNF text)</option>
              <option value="kc-td">Tree decomposition</option>
            </optgroup>
            <optgroup label="Other">
              <option value="prov-xml">PROV-XML export</option>
            </optgroup>
          </select>
          <select class="cv-eval__mapping" id="eval-mapping" hidden>
            <option value="">(no mappings found)</option>
          </select>
          <span class="cv-eval__hint" id="eval-mapping-hint" hidden></span>
          <select class="cv-eval__method" id="eval-method" hidden>
            <optgroup label="By guarantee">
              <option value="exact" class="cv-eval__opt-strong" title="Exact probability. The system picks the cheapest exact method (independent / possible-worlds / sieve / tree-decomposition / compilation); the method actually used is shown next to the result.">exact</option>
              <option value="relative" class="cv-eval__opt-strong" title="Granted tolerance: a (1±ε) RELATIVE guarantee with confidence 1−δ. The system picks the mechanism – it returns an exact value when one is cheap, otherwise an FPRAS estimate. The method actually used is shown next to the result.">relative (1±ε)</option>
              <option value="additive" class="cv-eval__opt-strong" title="Granted tolerance: |estimate − p| ≤ ε ADDITIVE, confidence 1−δ. The sample count is independent of p, so it stays robust on rare events. Returns an exact value when one is cheap, otherwise Monte-Carlo.">additive (±ε)</option>
            </optgroup>
            <optgroup label="Exact">
              <option value="independent" title="Exact evaluation assuming all input tokens are mutually independent; linear-time, but errors if the circuit is not independent (a leaf reaches the root along two paths).">independent</option>
              <option value="possible-worlds" title="Exact evaluation by exhaustive enumeration of all possible worlds: exponential in the number of input tokens, practical only for small circuits.">possible-worlds</option>
              <option value="sieve" title="Exact inclusion-exclusion over a monotone DNF (cost 2^m in the clause count m). Errors on non-DNF circuits.">sieve</option>
              <option value="d-tree" title="Deterministic anytime d-tree (Olteanu-Huang-Koch): exact probability by Shannon decomposition + independence, with memoisation. Also the engine behind the deterministic (δ=0) relative/additive paths, where it returns a CERTIFIED value interval. Monotone-DNF circuits only.">d-tree</option>
              <!-- shown only when the root carries an inversion-free
                   certificate; toggled in syncDropdownVisibility -->
              <option value="inversion-free" hidden title="Exact polynomial-time path for the inversion-free UCQ(OBDD) class (hierarchical, tuple-independent queries): builds a structured d-DNNF over a query-derived variable order, staying linear in the lineage. Offered only because the planner attached an inversion-free certificate to this root.">inversion-free</option>
              <!-- shown only when the eval target is a Möbius (μ) gate;
                   toggled in syncDropdownVisibility -->
              <option value="mobius" hidden title="Exact linear-time path for safe-by-cancellation UCQs: evaluates the signed Möbius combination Σ_i c_i·P(child_i) directly over its certified-independent islands. Offered only because the planner built a Möbius (μ) root for this token.">mobius</option>
              <option value="tree-decomposition" title="Exact evaluation via a tree decomposition of the Boolean circuit, compiled in-process to a d-DNNF (no external tool). Fails if the treewidth exceeds the supported maximum.">tree-decomposition</option>
              <option value="compilation" title="Exact evaluation by compiling the circuit to a d-DNNF with an external knowledge compiler (picked in the next dropdown), then evaluating the d-DNNF in linear time.">compilation</option>
            </optgroup>
            <optgroup label="Weighted model counting">
              <option value="wmc" title="Exact (Ganak / SharpSAT-TD / DPMC) or approximate (WeightMC), depending on the tool picked in the next dropdown.">wmc</option>
            </optgroup>
            <optgroup label="Approximate">
              <option value="monte-carlo" title="Monte-Carlo sampling: a fixed sample count, or an ADDITIVE (eps, delta) guarantee (|estimate − p| ≤ ε with confidence 1−δ, by Hoeffding). The absolute error makes it uninformative on rare events (p ≪ ε); use karp-luby there.">monte-carlo</option>
              <option value="karp-luby" title="Karp-Luby FPRAS for DNF-shaped (monotone OR-of-ANDs) circuits: a relative (eps, delta) guarantee whose sample count is independent of the probability, so it stays accurate on rare events. Errors on non-DNF circuits.">karp-luby</option>
              <option value="stopping-rule" title="Whole-circuit relative (ε, δ) FPRAS (Dagum–Karp-Luby-Ross stopping rule). Unlike karp-luby it works on ANY circuit, not just DNF; its sample count grows as 1/p.">stopping-rule</option>
            </optgroup>
          </select>
          <span class="cv-eval__approx" id="eval-args-approx" hidden>
            <select class="cv-eval__method cv-eval__approx-mode" id="eval-approx-mode"
                    title="Accuracy target: a fixed sample count, or an (eps, delta) error guarantee (relative for karp-luby, additive for monte-carlo).">
              <option value="samples">samples</option>
              <option value="epsdelta">ε, δ</option>
            </select>
            <input type="number" class="cv-eval__args" id="eval-args-mc"
                   min="1" step="1" placeholder="samples" value="10000"
                   autocomplete="off" title="Number of samples">
            <span class="cv-eval__approx-ed" id="eval-approx-ed" hidden>
              <label class="cv-eval__approx-lbl">ε
                <input type="number" id="eval-approx-eps" min="0" max="1"
                       step="0.01" value="0.1" autocomplete="off"
                       title="Error target ε, in (0, 1]"></label>
              <label class="cv-eval__approx-lbl">δ
                <input type="number" id="eval-approx-delta" min="0" max="1"
                       step="0.01" value="0.05" autocomplete="off"
                       title="Failure probability δ, in (0, 1)"></label>
            </span>
          </span>
          <input type="number" class="cv-eval__args" id="eval-args-bench-samples" hidden
                 min="1" step="1" placeholder="samples" value="10000"
                 autocomplete="off" title="Sample count used by the benchmark's sampling methods (monte-carlo and karp-luby)">
          <select class="cv-eval__args" id="eval-args-compiler" hidden
                  title="How to obtain the d-D circuit: a registered external compiler, the in-process tree-decomposition builder, direct interpretation of the Boolean circuit as a d-D, or the default makeDD fallback chain (interpretAsDD → tree-decomposition → fallback compiler). The external compilers are populated from the live tool registry; the in-process routes below always apply.">
            <option value="tree-decomposition">tree-decomposition</option>
            <option value="interpret-as-dd">interpret as d-D</option>
            <!-- shown only when the root carries an inversion-free
                 certificate; toggled in syncDropdownVisibility -->
            <option value="inversion-free" hidden>inversion-free</option>
            <option value="default">default (fallback chain)</option>
          </select>
          <select class="cv-eval__args" id="eval-args-wmc-tool" hidden
                  title="Which weighted model counter to invoke (populated from the live tool registry); leave unset to let ProvSQL pick the highest-preference available one."></select>
          <input type="number" class="cv-eval__args" id="eval-args-dtree-eps" hidden
                 min="0" max="1" step="0.01" placeholder="ε (exact)"
                 autocomplete="off"
                 title="Optional additive accuracy target ε for the d-tree: leave empty for the exact probability, or set ε in (0, 1] to stop the anytime recursion early at a CERTIFIED interval of half-width ≤ ε (|estimate − p| ≤ ε, with certainty – no failure probability).">

          <input type="number" class="cv-eval__args" id="eval-args-bins" hidden
                 min="1" step="1" placeholder="bins" value="30"
                 autocomplete="off" title="Histogram bin count for the distribution profile">
          <input type="number" class="cv-eval__args" id="eval-args-moment-k" hidden
                 min="0" step="1" placeholder="k" value="2"
                 autocomplete="off" title="Moment order k (k=1 gives the mean, k=2 with central=on gives the variance)">
          <select class="cv-eval__args" id="eval-args-moment-central" hidden
                  title="Raw moment E[X^k] vs central moment E[(X − E[X])^k]">
            <option value="false">raw</option>
            <option value="true">central</option>
          </select>
          <input type="number" class="cv-eval__args" id="eval-args-sample-n" hidden
                 min="1" step="1" placeholder="target n" value="100"
                 autocomplete="off"
                 title="Target number of samples.  Without conditioning, every draw is kept, so this is just the draw count.  With conditioning, rv_sample uses rejection sampling: draws from the unconditional distribution are accepted only when the event holds, capped at provsql.rv_mc_samples trials.  If the event is rare, you may get fewer accepts than requested.">
          <span class="cv-eval__args-group" id="eval-args-condition-group" hidden>
            <span class="cv-eval__args-label">Conditioned by:</span>
            <span class="cv-eval__cond-badge" id="eval-args-condition-badge" hidden
                  title="The Condition input was auto-filled with the row's provenance gate, the canonical conditioning event for expected(rv, provenance()).  Edit it to override.">
              <i class="fas fa-link"></i> row prov
            </span>
            <input type="text" class="cv-eval__args" id="eval-args-condition"
                   placeholder="condition on (UUID)" autocomplete="off"
                   spellcheck="false" size="20"
                   title="Optional conditioning gate UUID: the result becomes the conditional distribution X | event.  Auto-filled with the row's provenance when you click into a row's circuit; clear it for the unconditional distribution.">
          </span>
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
        <div class="cv-eval__notice" id="eval-notice" hidden></div>
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

  // Copy plain text to the clipboard, with the same insecure-origin /
  // older-browser fallback the circuit eval strip's copy button uses, so
  // the click-to-copy contribution values also work on http:// dev servers.
  async function copyToClipboard(text) {
    if (!text) return;
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      const ta = document.createElement('textarea');
      ta.value = text;
      ta.setAttribute('readonly', '');
      ta.style.position = 'fixed';
      ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      try { document.execCommand('copy'); } catch {}
      ta.remove();
    }
  }

  function formatCell(v, typeName) {
    // UUID-typed columns render as a short/full pair so the circuit
    // panel's "Show UUIDs" toggle can flip between abbreviated and full
    // display via a body-level CSS class : no re-render needed. The
    // outer span carries the full value as a title so hover always
    // reveals the original even when collapsed.
    const lowerType = (typeName || '').toLowerCase();
    if ((lowerType === 'uuid' || lowerType === 'random_variable') && v != null && v !== '') {
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
    'random_variable',
    'date', 'time', 'timetz', 'timestamp', 'timestamptz', 'interval',
    'uuid',
  ]);
  function isRightAlignedType(typeName) {
    return RIGHT_ALIGNED_TYPES.has((typeName || '').toLowerCase());
  }

  // Recognise a ProvSQL-extended column type regardless of search_path:
  // `random_variable` and `agg_token` may surface either unqualified
  // (provsql is on the search_path) or qualified (`provsql.random_variable`).
  // Used by the schema panel's column-list pills and by the result-table
  // header to flag the same columns once the data is rendered.
  function matchesProvType(typeName, base) {
    const s = String(typeName || '').toLowerCase();
    return s === base || s === `provsql.${base}`;
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

// Per-kind metadata used by the result-table `provsql` column header
// pill.  Labels deliberately mirror the schema panel's prov-tid /
// prov-bid / prov-opaque pills so the two affordances read as the
// same idea.  Lives at module scope rather than inside runQuery so
// the hoisted renderBlocks function can read it without hitting the
// const TDZ on the first call.
const CLASSIFIER_LABELS = {
  tid:    'prov-tid',
  bid:    'prov-bid',
  // OPAQUE falls back to the bare "prov" label : the muted-tone
  // styling on the pill is enough to signal "kind unknown" and
  // "prov-opaque" reads as redundant against the explainer tooltip.
  opaque: 'prov',
};
const CLASSIFIER_EXPLAINERS = {
  tid:    'The query result is independent at the row level (TID): '
        + 'distinct output rows have disjoint lineages.',
  bid:    'The query result is block-correlated (BID): rows sharing '
        + 'a block key value are mutually exclusive.',
  opaque: 'The query result is opaque: correlations across rows are '
        + 'not certified by the classifier.',
};

/* Normalize SQL text that arrived via the clipboard, a drag-and-drop or a
   file: rich-text copy (docs pages, PDFs, mail clients) routinely smuggles
   in invisible Unicode that PostgreSQL's lexer rejects with a baffling
   `syntax error at or near "￼"`. Zero-width and bidi format characters
   are dropped, the non-breaking-space family becomes a plain space, and
   Unicode line / paragraph separators become newlines. Applied to the whole
   text, string literals included: a pasted literal that genuinely needs an
   NBSP is far rarer than a doc copy that chokes on one. */
function sanitizeSqlText(text) {
  return String(text)
    // zero-width spaces / joiners, bidi marks and embeddings, word joiner,
    // BOM/ZWNBSP, object replacement character, soft hyphen
    .replace(/[\u200B-\u200F\u202A-\u202E\u2060\uFEFF\uFFFC\u00AD]/g, '')
    // NBSP, ogham space mark, the en/em/figure/punctuation/thin/hair space
    // family, narrow NBSP, math space, ideographic space
    .replace(/[\u00A0\u1680\u2000-\u200A\u202F\u205F\u3000]/g, ' ')
    // Unicode line / paragraph separators
    .replace(/[\u2028\u2029]/g, '\n');
}

/* Attach the sanitizer to a SQL-editing textarea: after a paste or drop,
   rewrite the value in place (cursor preserved relative to the cleaned
   prefix) and re-fire `input` so highlight / autosize listeners repaint.
   The synthetic event carries no inputType, so it cannot re-trigger. */
function wirePasteSanitizer(ta) {
  ta.addEventListener('input', (e) => {
    if (e.inputType !== 'insertFromPaste' && e.inputType !== 'insertFromDrop') return;
    const dirty = ta.value;
    const clean = sanitizeSqlText(dirty);
    if (clean === dirty) return;
    const pos = sanitizeSqlText(dirty.slice(0, ta.selectionStart)).length;
    ta.value = clean;
    ta.setSelectionRange(pos, pos);
    ta.dispatchEvent(new Event('input', { bubbles: true }));
  });
}

/* Global runQuery: invoked by the form's inline onsubmit. POSTs to /api/exec
   and renders the response into the result section. */
/* Block renderer factory: turns an /api/exec-shaped payload (blocks /
   wrapped / notices) into DOM, against a caller-supplied set of target
   elements. Extracted from runQuery so the notebook mode (one renderer
   per cell, each with its own table/banner/count elements) reuses the
   exact same rendering -- type-aware cells, UUID affordances, NOTICE
   banners, classifier pill -- as the shared result pane. */
function makeBlockRenderer(env, targets) {
  const { head, body, count } = targets;
  // Keep the count element purely numeric (tests and callers read it as a
  // number); the singular/plural noun lives in its own optional element.
  const setCount = (n) => {
    count.textContent = n;
    if (targets.noun) targets.noun.textContent = n === 1 ? 'tuple' : 'tuples';
  };

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

  // Recognises the classifier NOTICE emitted by the planner hook when
  // provsql.classify_top_level is on. Three shapes :
  //   "ProvSQL: query result is <KIND> (sources: schema.t1, schema.t2)"
  //   "ProvSQL: query result is <KIND> (no provenance-tracked sources)"
  //   "ProvSQL: query result is OPAQUE"
  // The OPAQUE form omits the parenthetical because the source list
  // is only partial when the shape gate trips (a SubLink, set
  // operation, GROUP BY ... hides relations from the rtable walk),
  // and surfacing partial sources would falsely suggest completeness.
  // Returns { kind, sources } on a match (sources is an array of
  // identifier strings, possibly empty), or null otherwise.
  function parseClassifierNotice(message) {
    if (!message) return null;
    const m = String(message).match(
      /^ProvSQL:\s*query result is (TID|BID|OPAQUE)(?:\s*\((.*)\))?\s*$/
    );
    if (!m) return null;
    const kind = m[1].toLowerCase();
    const tail = (m[2] || '').trim();
    let sources = [];
    if (tail.startsWith('sources:')) {
      sources = tail.slice('sources:'.length).split(',')
                    .map(s => s.trim())
                    .filter(s => s.length > 0);
    }
    return { kind, sources };
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
    const truncMark = targets.truncated;
    if (truncMark) {
      truncMark.textContent = '';
      truncMark.hidden = true;
    }

    const banners = targets.banners;
    let bannerHtml = '';
    // Earlier-failed prelude statements: render each as an ERROR banner
    // alongside notices/warnings.
    for (const b of earlier) {
      if (b.kind === 'error') {
        bannerHtml += renderDiag('ERROR', b.message, b.sqlstate);
      }
    }
    // Server-side notices/warnings + Studio's own INFO observations.
    // The classifier NOTICE emitted by the provsql.classify_top_level
    // GUC is hoisted out of the banner stream and used to upgrade the
    // result-table's `provsql` column header pill from a plain "prov"
    // to "prov-tid" / "prov-bid" / "prov-opaque" with the sources
    // surfaced in the hover tooltip.  Last one wins when multiple
    // statements emit NOTICEs (we surface the user-visible last
    // SELECT's classification).
    let classifyInfo = null;
    for (const n of (notices || [])) {
      const classified = parseClassifierNotice(n.message);
      if (classified) {
        classifyInfo = classified;
        continue;
      }
      bannerHtml += renderDiag(n.severity, n.message);
    }
    if (banners) banners.innerHTML = bannerHtml;

    if (!final) {
      head.innerHTML = '';
      body.innerHTML = '<tr><td style="opacity:.6">(no statements)</td></tr>';
      setCount(0);
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
      setCount(0);
      return;
    }
    if (final.kind === 'status') {
      head.innerHTML = '';
      body.innerHTML = `<tr><td>${env.escapeHtml(final.message)}${final.rowcount != null ? ` · ${final.rowcount} tuple${final.rowcount === 1 ? '' : 's'} affected` : ''}</td></tr>`;
      setCount(final.rowcount != null ? final.rowcount : 0);
      return;
    }
    if (final.kind === 'rows') {
      const allCols = final.columns;
      const isWhere   = env.mode === 'where';
      const isCircuit = env.mode === 'circuit';
      // Notebook cells make UUID-ish values clickable too (the click
      // inserts a circuit cell below, wired in notebook.js); only the
      // single-UUID auto-render below stays circuit-mode-only.
      const clickableUuid = isCircuit || env.mode === 'contributions' || env.mode === 'notebook';
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
      // Pick the row's provenance UUID for the auto-conditioning hint
      // we stamp on each clickable cell below: prefer the rewriter's
      // __prov column when present (always set on tracked queries with
      // provsql.active), otherwise fall back to a user-selected
      // `provsql` column.  Used by circuit-mode click-through so the
      // eval strip's "Condition on" input can default to the row's
      // provenance even when the click target is the `random_variable`
      // cell (whose scene root is the RV itself, not the row's prov).
      let rowProvIdx = provIdx;
      if (rowProvIdx === -1) {
        const i = allCols.findIndex(c => c.name === 'provsql');
        if (i !== -1) rowProvIdx = i;
      }
      const headExtra = (isWhere && wrapped) ? '<th></th>' : '';
      // Header decoration mirrors the schema-panel column list: every <th>
      // gets a title attribute with the Postgres type so the user can hover
      // any column to discover its type, plus a small pill for columns
      // with ProvSQL semantics:
      //  - terracotta `rv` for `random_variable` (operators lifted to gates)
      //  - terracotta `agg` for `agg_token` (UUID + running value)
      //  - purple `prov` for the `provsql` uuid column (the row's
      //    provenance gate from add_provenance)
      // The first two key off type_name; the third keys off the column
      // name because `provsql` is just `uuid` at the type level.
      const matches = env.matchesProvType || ((t, b) => String(t || '').toLowerCase() === b);
      // The `provsql` column pill is upgraded to a kind-aware variant
      // (prov-tid / prov-bid / prov-opaque) when the classifier NOTICE
      // emitted by provsql.classify_top_level identifies the kind of
      // the user's query result.  Hover surfaces the explainer plus
      // the list of provenance-tracked source relations.  Without a
      // classifier NOTICE (older extension, classifier off, no
      // sources walked) the pill stays the plain "prov" form.
      const provLabel = classifyInfo
        ? (CLASSIFIER_LABELS[classifyInfo.kind] || 'prov')
        : 'prov';
      const provClassMod = classifyInfo
        ? ' wp-result__col-prov--' + classifyInfo.kind
        : '';
      let provTip = `provsql: the row's provenance gate UUID (added by add_provenance)`;
      if (classifyInfo) {
        const explain = CLASSIFIER_EXPLAINERS[classifyInfo.kind]
                     || 'Query-time provenance classification.';
        // OPAQUE NOTICEs no longer carry a sources list (the
        // partial form they used to emit was misleading), so we
        // omit the "Sources: ..." line for OPAQUE entirely.
        let srcLine = '';
        if (classifyInfo.kind !== 'opaque') {
          srcLine = classifyInfo.sources.length
                  ? 'Sources: ' + classifyInfo.sources.join(', ')
                  : 'No provenance-tracked sources.';
        }
        provTip = srcLine ? explain + '\n\n' + srcLine : explain;
      }
      const headerPill = (col) => {
        const typeName = col.type_name || '';
        if (matches(typeName, 'random_variable')) {
          return `<span class="wp-result__col-rv" title="random_variable: query operators on this column lift into provenance gates at planning time">rv</span>`;
        }
        if (matches(typeName, 'agg_token')) {
          return `<span class="wp-result__col-agg" title="agg_token: each value carries a circuit UUID; click cells to inspect the underlying gate">agg</span>`;
        }
        if (col.name === 'provsql') {
          return `<span class="wp-result__col-prov${provClassMod}" `
               + `title="${env.escapeAttr(provTip)}">${provLabel}</span>`;
        }
        return '';
      };
      head.innerHTML = displayIdx.map(i => {
        const col = allCols[i];
        const typeName = col.type_name || '';
        const alignCls = env.isRightAlignedType(typeName) ? ' is-right' : '';
        const titleAttr = typeName ? ` title="${env.escapeAttr(typeName)}"` : '';
        // Wrap the column name in its own span so callers that read
        // header text (e.g. tests, accessibility tooling) can grab the
        // name independently of the trailing pill.
        const nameHtml = `<span class="wp-result__col-name">${env.escapeHtml(col.name)}</span>`;
        return `<th class="${alignCls.trim()}"${titleAttr}>${nameHtml}${headerPill(col)}</th>`;
      }).join('') + headExtra;
      body.innerHTML = final.rows.map(r => {
        const sources = wrapped && wprovIdx >= 0
          ? parseWhereProvenance(r[wprovIdx], displayIdx)
          : null;
        // Row's provenance UUID, attached to every clickable cell of
        // this row so circuit-mode click-through can carry the row
        // context into the eval strip's "Condition on" auto-preset.
        const rowProv = rowProvIdx >= 0 && r[rowProvIdx]
          ? String(r[rowProvIdx])
          : '';
        const rowProvAttr = rowProv
          ? ` data-row-prov="${env.escapeAttr(rowProv)}"`
          : '';
        const cells = displayIdx.map((idx, di) => {
          const col = allCols[idx];
          const typeName = (col.type_name || '').toLowerCase();
          const value = r[idx];
          const dataSrc = sources ? sources[di] || '' : '';
          const sourcesAttr = dataSrc ? ` data-sources="${env.escapeAttr(dataSrc)}"` : '';
          let extraCls = '';
          let extraAttr = '';
          // random_variable is binary-coercible with uuid and its on-wire
          // text form is a bare UUID, so it click-throughs the same way
          // a uuid cell does.
          if (clickableUuid && (typeName === 'uuid' || typeName === 'random_variable') && value) {
            extraCls  = ' is-clickable';
            extraAttr = ` data-circuit-uuid="${env.escapeAttr(String(value))}"`
                      + ` data-token-kind="${env.escapeAttr(typeName)}"${rowProvAttr}`;
          }
          // agg_token cells: their on-wire text is the underlying UUID
          // (because provsql.aggtoken_text_as_uuid is on for studio
          // sessions). Make them clickable in circuit mode like
          // regular UUID cells; the cell's text content is replaced
          // with the "value (*)" form pulled from agg_display, and the
          // tooltip carries the UUID so users can confirm which
          // circuit the cell points at without inspecting the DOM.
          let displayValue = value;
          let renderType = col.type_name;
          if (typeName === 'agg_token' && value) {
            if (clickableUuid) {
              extraCls  = (extraCls + ' is-clickable').trim();
              extraAttr = ` data-circuit-uuid="${env.escapeAttr(String(value))}"`
                        + ` data-token-kind="agg_token"${rowProvAttr}`;
              if (extraCls.length) extraCls = ' ' + extraCls;
            }
            extraAttr += ` title="${env.escapeAttr(String(value))}"`;
            const friendly = aggDisplay[value];
            if (friendly) {
              displayValue = friendly;
            } else {
              // No scalar to show (e.g. a conditioned agg_token): render the
              // bare UUID through the uuid path so it abbreviates to the
              // short/full pair instead of dumping the full string.
              renderType = 'uuid';
            }
          }
          if (env.isRightAlignedType(typeName)) extraCls += ' is-right';
          return `<td class="wp-result__cell${extraCls}"${sourcesAttr}${extraAttr}>${env.formatCell(displayValue, renderType)}</td>`;
        }).join('');
        const jumpBtn = (isWhere && wrapped && provIdx >= 0 && r[provIdx])
          ? `<td class="wp-result__cell--actions"><button class="wp-btn wp-btn--mini" type="button" `
            + `data-jump-circuit="${env.escapeAttr(String(r[provIdx]))}" title="Open circuit DAG"><i class="fas fa-project-diagram"></i> Circuit</button>`
            + `<button class="wp-btn wp-btn--mini" type="button" `
            + `data-jump-contributions="${env.escapeAttr(String(r[provIdx]))}" title="Open Shapley / Banzhaf contributions"><i class="fas fa-chart-bar"></i> Contributions</button></td>`
          : '';
        return `<tr>${cells}${jumpBtn}</tr>`;
      }).join('');
      setCount(final.rows.length);
      const truncated = targets.truncated;
      if (truncated) {
        if (final.truncated && final.max_rows != null) {
          truncated.textContent = ` (first ${final.max_rows}; more available)`;
          truncated.hidden = false;
        } else {
          truncated.textContent = '';
          truncated.hidden = true;
        }
      }
      // Auto-render the single clickable UUID: when a circuit- or
      // contributions-mode query returns exactly one cell the user
      // could click through, save them the click.  Two+ candidates
      // stay ambiguous (let the user pick); zero means nothing to
      // render.  A subsequent where-mode-jump preload still runs after
      // this via the mode's then() callback and overwrites the
      // auto-rendered scene with the user-chosen one.
      //
      // Implementation: dispatch a synthetic click on the cell so the
      // existing click handler (wired inside the setupCircuitMode /
      // setupContributionsMode IIFE) takes care of loadCircuit /
      // fetchContributions.  This file's `renderBlocks` is at
      // module-global scope and can't reach those directly, but
      // clicking the cell DOM element travels through whichever
      // listener was installed for the current mode.
      if (isCircuit || env.mode === 'contributions') {
        const clickable = body.querySelectorAll('[data-circuit-uuid]');
        if (clickable.length === 1) {
          clickable[0].click();
        }
      }
    }
  }

  function renderError(msg) {
    const banners = targets.banners;
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

  return { renderBlocks, renderError, renderDiag };
}
window.ProvsqlStudio = window.ProvsqlStudio || {};
window.ProvsqlStudio.makeBlockRenderer = makeBlockRenderer;

async function runQuery(ev) {
  ev.preventDefault();

  const env = window.__provsqlStudio || { mode: 'where', escapeHtml: s => s, escapeAttr: s => s, formatCell: v => v };
  // Last-line cleanup for invisible Unicode, covering entry paths the paste
  // hook does not see (?q= deep links, programmatic fills): fix the textarea
  // in place so what runs is what the user sees highlighted.
  const reqTa = document.getElementById('request');
  {
    const clean = sanitizeSqlText(reqTa.value);
    if (clean !== reqTa.value) {
      reqTa.value = clean;
      reqTa.dispatchEvent(new Event('input', { bubbles: true }));
    }
  }
  const sqlText = reqTa.value;
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

  const R = makeBlockRenderer(env, {
    head, body, count,
    noun: document.getElementById('result-noun'),
    banners: document.getElementById('result-banners'),
    truncated: document.getElementById('result-truncated'),
  });

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

  const upEl = document.getElementById('opt-update-prov');
  const provSchemeEl = document.querySelector('input[name="prov-scheme"]:checked');
  let resp;
  try {
    resp = await fetch('/api/exec', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        sql: sqlText,
        mode: env.mode,
        prov_scheme: env.mode === 'where' ? 'where' : (provSchemeEl ? provSchemeEl.value : 'semiring'),
        update_provenance: upEl ? upEl.checked : false,
        request_id: requestId,
      }),
    });
  } catch (e) {
    R.renderError(`Network error: ${e.message}`);
    return false;
  } finally {
    clearTimeout(swapTimer);
    if (cancelBtn) cancelBtn.hidden = true;
    if (runBtn)    runBtn.hidden    = false;
  }
  const dt = Math.round(performance.now() - t0);
  time.textContent = dt;

  if (!resp.ok) {
    R.renderError(`HTTP ${resp.status}`);
    return false;
  }
  const payload = await resp.json();
  R.renderBlocks(payload.blocks || [], !!payload.wrapped, payload.notices || []);

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

}
