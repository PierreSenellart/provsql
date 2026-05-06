/* ProvSQL Studio: circuit-mode renderer.
   Lazy-loaded by app.js once the user clicks a UUID/agg_token cell in
   circuit-mode results. The DOT layout is computed server-side
   (provsql_studio/circuit.py via `dot -Tjson`); this module just paints,
   handles zoom/pan/pin, and delegates expansion to the server. */

(function () {
  if (window.ProvsqlCircuit) return; // idempotent; app.js may call setup repeatedly

  const NS = 'http://www.w3.org/2000/svg';

  const TYPE_SUMMARY = {
    plus:     'Plus gate (⊕): alternatives (duplicate elimination, multi-derivation)',
    times:    'Times gate (⊗): combined use (join, cross product)',
    monus:    'Monus gate (⊖): m-semiring difference (EXCEPT)',
    project:  'Project gate (Π): column projection (where-provenance only)',
    eq:       'Eq gate (⋈): equijoin witness (where-provenance only)',
    agg:      'Aggregation gate: aggregate function',
    semimod:  'Semimodule scalar (⋆): tensor product of scalar and semiring value',
    cmp:      'Compare gate: aggregate-value comparison',
    delta:    'Delta gate (δ): δ-semiring operator',
    value:    'Value gate: scalar constant',
    mulinput: 'Multivalued input (⋮)',
    input:    'Input gate (ι): base tuple',
    one:      'One (𝟙): semiring ⊗ identity (always true)',
    zero:     'Zero (𝟘): semiring ⊕ identity (always false)',
    update:   'Update gate (υ): INSERT / UPDATE / DELETE',
  };

  // ─── state ────────────────────────────────────────────────────────────

  let state = {
    scene: null,        // {nodes, edges, root, depth} from /api/circuit
    showUuids: false,
    zoom: 1,
    pan: { x: 0, y: 0 },
    pinnedNode: null,
  };
  let svg = null, edgeLayer = null, nodeLayer = null;
  let titleEl = null, subEl = null;
  let inspectorEl = null, inspectorTitle = null, inspectorBody = null;

  // ─── public API ───────────────────────────────────────────────────────

  window.ProvsqlCircuit = {
    init,                 // bind DOM handles after the sidebar markup is injected
    renderCircuit,        // (scene): replace the current scene
    setStatus,            // (title, sub): update header copy
    showLoading,          // (): placeholder while fetching
    showError,            // (msg)
  };

  // ─── init ─────────────────────────────────────────────────────────────

  function init() {
    svg = document.getElementById('circuit');
    edgeLayer = document.getElementById('circuit-edges');
    nodeLayer = document.getElementById('circuit-nodes');
    titleEl = document.getElementById('circuit-title');
    subEl = document.getElementById('circuit-sub');
    inspectorEl = document.getElementById('inspector');
    inspectorTitle = document.getElementById('inspector-title');
    inspectorBody = document.getElementById('inspector-body');

    // Toolbar.
    document.getElementById('tool-zoom-in').onclick  = () => { state.zoom = Math.min(2.5, state.zoom * 1.2); fitView(); };
    document.getElementById('tool-zoom-out').onclick = () => { state.zoom = Math.max(0.4, state.zoom / 1.2); fitView(); };
    document.getElementById('tool-zoom-fit').onclick = () => { state.zoom = 1; state.pan = { x: 0, y: 0 }; fitView(); };
    const uBtn = document.getElementById('tool-show-uuids');
    // Sync body class with the initial pressed state so query-result UUID
    // cells (rendered by formatCell with paired short/full spans) start
    // out matching whatever the toggle currently shows.
    document.body.classList.toggle('show-uuids', state.showUuids);
    uBtn.onclick = () => {
      state.showUuids = !state.showUuids;
      uBtn.setAttribute('aria-pressed', String(state.showUuids));
      // Drives the .wp-uuid__short / .wp-uuid__full visibility in the
      // result table without re-rendering it; works even when no circuit
      // has been loaded yet (the toggle is shared between the result
      // table and the circuit view).
      document.body.classList.toggle('show-uuids', state.showUuids);
      if (state.scene) {
        paint();
        // If a node is pinned, its inspector is showing the abbreviated
        // (or full) UUID under the old toggle state. Re-render so the
        // displayed uuid line tracks the new toggle.
        if (state.pinnedNode) {
          const pinned = state.scene.nodes.find(n => n.id === state.pinnedNode);
          if (pinned) openInspector(pinned);
        }
      }
    };
    // Close = clear pin: dismiss the inspector AND drop state.pinnedNode
    // so subsequent paint() / Show-UUIDs toggles don't reopen it. The X
    // button used to call closeInspector directly (CSS-only hide), which
    // left pinnedNode set; the show-uuids handler then saw a "pinned"
    // node and re-rendered the panel.
    document.getElementById('inspector-close').onclick = clearPin;

    // Semiring-evaluation strip wiring. The select drives which side
    // control is visible: a provenance-mapping picker for compiled
    // semirings, a method picker for probability, neither for boolexpr
    // (whose leaf labels are the gate UUIDs themselves).
    initEvalStrip();

    // Pan via drag.
    let dragging = false, dragStart = null;
    svg.addEventListener('mousedown', (e) => {
      if (e.target.closest('.node-group')) return;
      dragging = true;
      dragStart = { x: e.clientX, y: e.clientY, panX: state.pan.x, panY: state.pan.y };
    });
    window.addEventListener('mousemove', (e) => {
      if (!dragging) return;
      const dx = (e.clientX - dragStart.x) / state.zoom;
      const dy = (e.clientY - dragStart.y) / state.zoom;
      state.pan.x = dragStart.panX - dx;
      state.pan.y = dragStart.panY - dy;
      fitView();
    });
    window.addEventListener('mouseup', () => { dragging = false; });
    // Background click clears the pin.
    svg.addEventListener('click', (e) => {
      if (e.target.closest('.node-group')) return;
      if (state.pinnedNode) clearPin();
    });

    // Wheel-to-zoom. Same clamp as the toolbar buttons (0.4..2.5) but
    // a smaller per-tick factor so successive notches feel smooth. We
    // need passive: false to call preventDefault — otherwise the
    // browser also scrolls the page while the user is zooming the
    // canvas.
    svg.addEventListener('wheel', (e) => {
      e.preventDefault();
      const factor = e.deltaY < 0 ? 1.12 : 1 / 1.12;
      state.zoom = Math.max(0.4, Math.min(2.5, state.zoom * factor));
      fitView();
    }, { passive: false });

    // Re-fit when the canvas's on-screen size changes (e.g. window
    // resize, sidebar reflow). fitView builds the viewBox from the
    // SVG's clientWidth/clientHeight, so a stale fit otherwise leaves
    // the circuit clipped or letterboxed against the new geometry.
    if (window.ResizeObserver) {
      new ResizeObserver(() => { if (state.scene) fitView(); }).observe(svg);
    } else {
      window.addEventListener('resize', () => { if (state.scene) fitView(); });
    }
  }

  function setStatus(title, sub) {
    if (titleEl && title != null) titleEl.textContent = title;
    if (subEl && sub != null)     subEl.textContent = sub;
  }

  function showLoading() {
    if (edgeLayer) edgeLayer.innerHTML = '';
    if (nodeLayer) {
      nodeLayer.innerHTML =
        '<text x="50%" y="50%" text-anchor="middle" dominant-baseline="central" '
        + 'fill="var(--fg-muted)" font-family="var(--font-ui)" font-size="14">'
        + 'Loading…</text>';
    }
    setStatus('Provenance Circuit', 'Fetching subgraph…');
  }

  function showError(msg) {
    if (edgeLayer) edgeLayer.innerHTML = '';
    if (nodeLayer) {
      nodeLayer.innerHTML =
        '<text x="50%" y="50%" text-anchor="middle" dominant-baseline="central" '
        + 'fill="var(--terracotta-500)" font-family="var(--font-ui)" font-size="14">'
        + escapeHtml(String(msg)) + '</text>';
    }
    setStatus('Provenance Circuit', 'Error.');
  }

  function renderCircuit(scene) {
    state.scene = scene;
    state.pinnedNode = null;
    // Each new circuit starts from a clean fit: reset zoom + pan so
    // the whole graph fits in the viewport regardless of how the user
    // had panned/zoomed the previous one. The fitView() inside paint()
    // then sizes the viewBox around the new bounding box.
    state.zoom = 1;
    state.pan = { x: 0, y: 0 };
    closeInspector();
    paint();
    refreshEvalTarget();
  }

  // ─── paint ────────────────────────────────────────────────────────────

  function paint() {
    if (!state.scene || !state.scene.nodes.length) {
      showError('Empty circuit (no nodes returned).');
      return;
    }
    edgeLayer.innerHTML = '';
    nodeLayer.innerHTML = '';

    const nodesById = Object.fromEntries(state.scene.nodes.map(n => [n.id, n]));

    // Gate types whose children carry a meaningful order: cmp's
    // lhs/rhs, monus's minuend/subtrahend, and agg — but agg only
    // when the function is order-sensitive (array_agg, string_agg,
    // json_agg, …). For sum/count/min/max/avg the result is
    // independent of the input order, so the digits would be noise.
    // semimod is omitted: its value/scalar split is implied by gate
    // type (the scalar always comes from a `value` child). eq has a
    // single child so positional labels would be redundant.
    const ORDERED_GATES = new Set(['cmp', 'monus', 'agg']);
    const COMMUTATIVE_AGG = new Set(['sum', 'count', 'min', 'max', 'avg']);
    function shouldLabelChildren(parent) {
      if (!ORDERED_GATES.has(parent.type)) return false;
      if (parent.type === 'agg') {
        const fn = (parent.info1_name || '').toLowerCase();
        return !COMMUTATIVE_AGG.has(fn);
      }
      return true;
    }

    // edges
    for (const e of state.scene.edges) {
      const from = nodesById[e.from], to = nodesById[e.to];
      if (!from || !to) continue;
      const path = svgEl('path', {
        class: 'edge',
        d: `M ${from.x} ${from.y + 22} C ${from.x} ${from.y + 50}, ${to.x} ${to.y - 50}, ${to.x} ${to.y - 22}`,
        'data-from': e.from, 'data-to': e.to,
      });
      edgeLayer.appendChild(path);

      // Position label at the child end of the edge for ordered gates.
      // We offset 16px away from the child centre, in the direction of
      // the parent — so the digit sits just outside the child circle
      // along the incoming edge, regardless of layout angle.
      if (shouldLabelChildren(from) && e.child_pos != null) {
        const dx = from.x - to.x;
        const dy = from.y - to.y;
        const len = Math.hypot(dx, dy) || 1;
        const offset = 32;  // r=22 + a small gap so the digit clears the stroke
        const lx = to.x + (dx / len) * offset;
        const ly = to.y + (dy / len) * offset;
        const tag = svgEl('text', {
          class: 'edge-pos',
          x: lx, y: ly,
          'text-anchor': 'middle', 'dominant-baseline': 'central',
        });
        tag.textContent = String(e.child_pos);
        edgeLayer.appendChild(tag);
      }
    }

    // nodes
    for (const n of state.scene.nodes) {
      const cls = `node-group node--${n.type}` + (n.frontier ? ' is-frontier' : '');
      const g = svgEl('g', { class: cls, 'data-id': n.id, transform: `translate(${n.x},${n.y})` });
      g.appendChild(svgEl('circle', { class: 'node-shape', r: 22 }));
      const label = svgEl('text', { class: 'node-label', y: -2 });
      label.textContent = n.label || n.type[0];
      g.appendChild(label);
      // Meta line below: only leaf gates (input / update — both reference
      // a source row) get one. Internal gates stay bare — dropping a
      // 36-char UUID under each circle overlapped the edge curves and
      // made nothing readable; the full UUID is one click away in the
      // inspector. Leaves always render their compact form (relation id
      // when info1 is set, otherwise an abbreviated UUID) regardless of
      // the "Show UUIDs" toggle: leaves are dense enough that the full
      // UUID would overflow neighbouring nodes.
      const isLeafGate = n.type === 'input' || n.type === 'update';
      const metaText = isLeafGate
        ? (n.info1 ? `tbl ${n.info1}` : shortUuid(n.id))
        : '';
      if (metaText) {
        const meta = svgEl('text', { class: 'node-meta', y: 38 });
        meta.textContent = metaText;
        g.appendChild(meta);
      }
      // Frontier marker: small "+" badge top-right
      if (n.frontier) {
        const badge = svgEl('circle', { class: 'frontier-badge', cx: 16, cy: -16, r: 7,
                                        fill: 'var(--gold-500)', stroke: 'var(--gold-700)' });
        const bt = svgEl('text', { x: 16, y: -16, 'text-anchor': 'middle',
                                   'dominant-baseline': 'central', 'font-size': 11,
                                   'font-weight': '700', fill: 'var(--purple-900)',
                                   'pointer-events': 'none' });
        bt.textContent = '+';
        g.appendChild(badge);
        g.appendChild(bt);
      }
      g.addEventListener('click', (ev) => { ev.stopPropagation(); onNodeClick(n); });
      g.addEventListener('mouseenter', () => highlightSubtree(n.id, true));
      g.addEventListener('mouseleave', () => { if (!state.pinnedNode) highlightSubtree(n.id, false); });
      nodeLayer.appendChild(g);
    }

    fitView();

    if (titleEl) titleEl.textContent = 'Provenance Circuit';
    if (subEl) {
      // Emit the root UUID as a short/full pair so the toolbar's "Show
      // UUIDs" button toggles its display via the body-level CSS class
      // (no need to rerun the painter on toggle).
      const root = state.scene.root;
      subEl.innerHTML =
        `${state.scene.nodes.length} gates · root `
        + `<span class="wp-uuid">`
        + `<span class="wp-uuid__short">${escapeHtml(shortUuid(root))}</span>`
        + `<span class="wp-uuid__full">${escapeHtml(root)}</span>`
        + `</span>`;
    }
  }

  function fitView() {
    if (!state.scene) return;
    const xs = state.scene.nodes.map(n => n.x);
    const ys = state.scene.nodes.map(n => n.y);
    const minX = Math.min(...xs) - 60, maxX = Math.max(...xs) + 60;
    const minY = Math.min(...ys) - 60, maxY = Math.max(...ys) + 60;
    const bbW = Math.max(maxX - minX, 200);
    const bbH = Math.max(maxY - minY, 150);
    const cx = minX + bbW / 2 + state.pan.x;
    const cy = minY + bbH / 2 + state.pan.y;

    // Match the viewBox aspect ratio to the SVG element's on-screen
    // aspect ratio. With preserveAspectRatio="xMidYMid meet" any
    // mismatch is rendered as letterbox bands inside the canvas
    // border, so the circuit appears to live in a smaller area than
    // the bordered rectangle. We take the dimensions from the parent
    // .cv-canvas (the visibly-bordered container) rather than the
    // SVG itself: SVG sizing inside a flex parent can be reported as
    // half-height in some browsers because of the SVG element's
    // intrinsic-aspect-ratio quirks, so we anchor on the container
    // whose box model is unambiguous.
    const host = svg.parentElement || svg;
    const elW = host.clientWidth  || bbW;
    const elH = host.clientHeight || bbH;
    const aspect = elW / elH;
    let vbW, vbH;
    if (bbW / bbH > aspect) { vbW = bbW;          vbH = bbW / aspect; }
    else                    { vbH = bbH;          vbW = bbH * aspect; }
    vbW /= state.zoom;
    vbH /= state.zoom;
    svg.setAttribute('viewBox', `${cx - vbW / 2} ${cy - vbH / 2} ${vbW} ${vbH}`);
  }

  // ─── interactions ─────────────────────────────────────────────────────

  function onNodeClick(node) {
    if (node.frontier) {
      expandFrontier(node);
      return;
    }
    pinNode(node);
  }

  function pinNode(node) {
    if (state.pinnedNode === node.id) {
      clearPin();
      return;
    }
    state.pinnedNode = node.id;
    document.querySelectorAll('.node-group').forEach(g => g.classList.remove('is-active', 'is-pinned'));
    document.querySelectorAll('.edge').forEach(p => p.classList.remove('is-active'));
    highlightSubtree(node.id, true, true);
    openInspector(node);
    refreshEvalTarget();
  }

  function clearPin() {
    document.querySelectorAll('.node-group').forEach(g => g.classList.remove('is-pinned'));
    document.querySelectorAll('.edge').forEach(p => p.classList.remove('is-active'));
    state.pinnedNode = null;
    closeInspector();
    refreshEvalTarget();
  }

  function descendants(id) {
    if (!state.scene) return new Set();
    const out = new Set([id]);
    const stack = [id];
    while (stack.length) {
      const cur = stack.pop();
      for (const e of state.scene.edges) {
        if (e.from === cur && !out.has(e.to)) {
          out.add(e.to);
          stack.push(e.to);
        }
      }
    }
    return out;
  }

  function highlightSubtree(id, on, pinned = false) {
    const set = descendants(id);
    document.querySelectorAll('.node-group').forEach(g => {
      const match = set.has(g.dataset.id);
      g.classList.toggle('is-active', on && match && !pinned);
      g.classList.toggle('is-pinned', pinned && match);
    });
    document.querySelectorAll('.edge').forEach(p => {
      const match = set.has(p.dataset.from) && set.has(p.dataset.to);
      p.classList.toggle('is-active', on && match);
    });
  }

  // ─── inspector ────────────────────────────────────────────────────────

  function openInspector(node) {
    inspectorEl.classList.add('is-open');
    inspectorTitle.textContent = TYPE_SUMMARY[node.type] || `Gate (${node.type})`;
    let html = '<dl>';
    html += `<dt>type</dt><dd>${escapeHtml(node.type)}</dd>`;
    // Match the in-circuit display: abbreviated UUID by default, full
    // value only when the "Show UUIDs" toggle is pressed. The title
    // attribute keeps the full string available on hover for the
    // collapsed form.
    const uuidText = state.showUuids ? node.id : shortUuid(node.id);
    html += `<dt>uuid</dt><dd title="${escapeHtml(node.id)}">${escapeHtml(uuidText)}</dd>`;
    html += `<dt>depth</dt><dd>${node.depth}</dd>`;
    if (node.info1 != null) html += `<dt>info1</dt><dd>${escapeHtml(node.info1)}</dd>`;
    if (node.info2 != null) html += `<dt>info2</dt><dd>${escapeHtml(node.info2)}</dd>`;
    // `extra` is set by project (input→output column mapping array),
    // value (scalar), and agg (computed scalar). Label it by gate type
    // so the meaning is obvious without a docs lookup.
    if (node.extra != null && node.extra !== '') {
      const label = node.type === 'project' ? 'mapping'
                  : node.type === 'value'   ? 'value'
                  : node.type === 'agg'     ? 'value'
                  : 'extra';
      html += `<dt>${label}</dt><dd>${escapeHtml(node.extra)}</dd>`;
    }
    html += '</dl>';
    if (node.type === 'input' || node.type === 'mulinput') {
      html += '<p><em>Resolving source row…</em></p>';
    } else if (node.frontier) {
      html += '<p>Frontier node: click again to expand its subtree.</p>';
    }
    inspectorBody.innerHTML = html;

    if (node.type === 'input' || node.type === 'mulinput') {
      fetchLeafRow(node.id);
    }
  }

  function closeInspector() {
    if (inspectorEl) inspectorEl.classList.remove('is-open');
  }

  async function fetchLeafRow(uuid) {
    let resp;
    try {
      resp = await fetch(`/api/leaf/${encodeURIComponent(uuid)}`);
    } catch {
      return;
    }
    if (!resp.ok) {
      replaceLeafBody('<p style="color:var(--fg-muted)">No source row found for this UUID.</p>');
      return;
    }
    const payload = await resp.json();
    const matches = payload.matches || [];
    if (!matches.length) {
      replaceLeafBody('<p style="color:var(--fg-muted)">No source row found.</p>');
      return;
    }
    const items = matches.map(m => {
      const cells = Object.entries(m.row || {}).map(
        ([k, v]) => `<dt>${escapeHtml(k)}</dt><dd>${escapeHtml(v == null ? '' : String(v))}</dd>`
      ).join('');
      return `<p><strong>${escapeHtml(m.relation)}</strong></p><dl>${cells}</dl>`;
    }).join('<hr>');
    replaceLeafBody(items);
  }

  function replaceLeafBody(html) {
    // Replace the placeholder paragraph at the bottom of the inspector body.
    const ps = inspectorBody.querySelectorAll('p');
    if (ps.length) ps[ps.length - 1].outerHTML = html;
    else inspectorBody.insertAdjacentHTML('beforeend', html);
  }

  // ─── expansion ────────────────────────────────────────────────────────

  async function expandFrontier(node) {
    const root = state.scene && state.scene.root;
    if (!root) return;
    setStatus(null, `Expanding ${shortUuid(node.id)}…`);
    let resp;
    try {
      resp = await fetch(`/api/circuit/${encodeURIComponent(root)}/expand`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ frontier_node_uuid: node.id, additional_depth: state.scene.depth }),
      });
    } catch (e) {
      showError(`Network error: ${e.message}`);
      return;
    }
    if (!resp.ok) {
      const err = await resp.json().catch(() => ({}));
      showError(err.error || `HTTP ${resp.status}`);
      return;
    }
    const sub = await resp.json();
    mergeSubgraph(node, sub);
    paint();
  }

  function mergeSubgraph(anchor, sub) {
    if (!state.scene || !sub || !sub.nodes) return;
    // Geometric anchor: shift sub.nodes so that sub's root lands on anchor's
    // (x, y), then drop the sub-root (it's the same node as `anchor`).
    const subRoot = sub.nodes.find(n => n.id === sub.root);
    const dx = anchor.x - (subRoot ? subRoot.x : 0);
    const dy = anchor.y - (subRoot ? subRoot.y : 0);

    const known = new Set(state.scene.nodes.map(n => n.id));
    for (const n of sub.nodes) {
      if (known.has(n.id)) continue;
      state.scene.nodes.push({ ...n, x: n.x + dx, y: n.y + dy });
    }
    const knownEdges = new Set(state.scene.edges.map(e => `${e.from}->${e.to}`));
    for (const e of sub.edges) {
      if (knownEdges.has(`${e.from}->${e.to}`)) continue;
      state.scene.edges.push(e);
    }
    // The anchor is no longer a frontier (we just expanded it).
    const idx = state.scene.nodes.findIndex(n => n.id === anchor.id);
    if (idx >= 0) state.scene.nodes[idx] = { ...state.scene.nodes[idx], frontier: false };
  }

  // ─── semiring evaluation ──────────────────────────────────────────────

  // Semirings that take a provenance mapping (regclass with `value` + `provenance`).
  const _SR_NEEDS_MAPPING = new Set(['boolean', 'counting', 'why', 'formula']);

  // For each probability method that takes an `arguments` value, point
  // at the dedicated control. Each control keeps its own state (so the
  // user's MC sample count survives a round-trip through compilation
  // and back) and offers an input shape that matches the expected value:
  // a number field for samples, a dropdown of ProvSQL-known compilers,
  // a free-form text field pre-filled with the WeightMC defaults.
  const _PROB_ARG_CONTROL = {
    'monte-carlo': 'eval-args-mc',
    'compilation': 'eval-args-compiler',
    'weightmc':    'eval-args-wmc',
  };

  function initEvalStrip() {
    const sel    = document.getElementById('eval-semiring');
    const map    = document.getElementById('eval-mapping');
    const meth   = document.getElementById('eval-method');
    const run    = document.getElementById('eval-run');
    const result = document.getElementById('eval-result');
    if (!sel || !map || !meth || !run) return;

    const argControls = Object.values(_PROB_ARG_CONTROL)
      .map(id => document.getElementById(id))
      .filter(Boolean);

    let mappingsLoaded = false;

    function syncControls() {
      const v = sel.value;
      map.hidden  = !_SR_NEEDS_MAPPING.has(v);
      meth.hidden = v !== 'probability';
      // Show only the args control that matches the current probability
      // method (if any); hide every other one so the row stays compact.
      const wantedId = (v === 'probability') ? _PROB_ARG_CONTROL[meth.value] : null;
      for (const ctrl of argControls) ctrl.hidden = (ctrl.id !== wantedId);
      result.textContent = '';  // stale once the input shape changes
      const timeEl  = document.getElementById('eval-time');
      const boundEl = document.getElementById('eval-bound');
      if (timeEl)  timeEl.textContent  = '';
      if (boundEl) boundEl.textContent = '';
      if (!map.hidden && !mappingsLoaded) loadMappings();
    }

    async function loadMappings() {
      mappingsLoaded = true;
      try {
        const resp = await fetch('/api/provenance_mappings');
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        const list = await resp.json();
        if (!list.length) {
          map.innerHTML = '<option value="">(no mappings — run create_provenance_mapping)</option>';
          return;
        }
        // `display_name` drops the schema when the relation is search_path-
        // visible (resolves unambiguously without qualification), keeping
        // labels short for the common public-schema case. The option's
        // value is still the qualified name so the regclass cast on the
        // server can never resolve to the wrong schema.
        map.innerHTML = list.map(m => {
          const label = m.display_name || m.qname;
          return `<option value="${escapeHtml(m.qname)}" title="${escapeHtml(m.qname)}">${escapeHtml(label)}</option>`;
        }).join('');
      } catch (e) {
        // Allow a retry on next semiring change.
        mappingsLoaded = false;
        map.innerHTML = `<option value="">(load failed: ${escapeHtml(e.message)})</option>`;
      }
    }

    sel.addEventListener('change', syncControls);
    // Method change also affects whether the args input is shown / what
    // its placeholder reads.
    meth.addEventListener('change', syncControls);
    run.addEventListener('click', runEvaluation);
    syncControls();
  }

  function refreshEvalTarget() {
    const tgt = document.getElementById('eval-target');
    if (!tgt) return;
    if (!state.scene) {
      tgt.textContent = '';
      return;
    }
    const id = state.pinnedNode || state.scene.root;
    const label = state.pinnedNode ? 'selected node' : 'root';
    // Emit the same short/full pair as the result-table UUID cells, so
    // `body.show-uuids` (toggled by the toolbar's "Show UUIDs" button)
    // swaps the displayed form via CSS without us having to re-render
    // here on every toggle change.
    tgt.innerHTML =
      `→ ${label} `
      + `<span class="wp-uuid">`
      + `<span class="wp-uuid__short">${escapeHtml(shortUuid(id))}</span>`
      + `<span class="wp-uuid__full">${escapeHtml(id)}</span>`
      + `</span>`;
    tgt.title = `Evaluation runs on the ${label}: ${id}`;
  }

  async function runEvaluation() {
    const sel    = document.getElementById('eval-semiring');
    const map    = document.getElementById('eval-mapping');
    const meth   = document.getElementById('eval-method');
    const run    = document.getElementById('eval-run');
    const result = document.getElementById('eval-result');
    const time   = document.getElementById('eval-time');
    const bound  = document.getElementById('eval-bound');
    if (time)  time.textContent  = '';
    if (bound) bound.textContent = '';
    if (!state.scene) {
      result.textContent = 'no circuit loaded';
      result.dataset.kind = 'error';
      return;
    }
    const token = state.pinnedNode || state.scene.root;
    const semiring = sel.value;
    const body = { token, semiring };
    if (_SR_NEEDS_MAPPING.has(semiring)) {
      const m = map.value || '';
      if (!m) {
        result.textContent = 'pick a provenance mapping';
        result.dataset.kind = 'error';
        return;
      }
      body.mapping = m;
    }
    if (semiring === 'probability') {
      body.method = meth.value || '';
      // Pull the argument from whichever per-method control is wired up
      // (number field for monte-carlo, compiler dropdown for
      // compilation, text field for weightmc). Methods that ignore args
      // (independent / tree-decomposition / possible-worlds / default)
      // have no entry here, so we just don't send `arguments`.
      const ctrlId = _PROB_ARG_CONTROL[meth.value];
      if (ctrlId) {
        const ctrl = document.getElementById(ctrlId);
        const a = (ctrl?.value || '').trim();
        if (a) body.arguments = a;
      }
    }

    run.disabled = true;
    result.textContent = 'evaluating…';
    result.dataset.kind = 'pending';
    // Round-trip time, captured around the fetch + JSON parse the same
    // way runQuery times /api/exec. Mirrors the "evaluated in N ms"
    // chip in the result-table footer so users can compare evaluation
    // cost across methods (e.g. monte-carlo vs tree-decomposition).
    const t0 = performance.now();
    try {
      const resp = await fetch('/api/evaluate', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      const data = await resp.json();
      const dt = Math.round(performance.now() - t0);
      if (time) time.textContent = `· ${dt} ms`;
      if (!resp.ok) {
        result.textContent = data.detail || data.error || `HTTP ${resp.status}`;
        result.dataset.kind = 'error';
        return;
      }
      // Show the value verbatim. Probability gets clipped to 4 decimals
      // for readability; everything else is already a string from the
      // server cast or a JSON-native scalar.
      let display;
      if (data.kind === 'float' && typeof data.result === 'number') {
        display = data.result.toFixed(4);
      } else if (data.result == null) {
        display = '(null)';
      } else {
        display = String(data.result);
      }
      result.textContent = '= ' + display;
      result.dataset.kind = 'ok';
      result.title = `${data.kind} value`;
      // Monte-Carlo: append a Hoeffding-style 95% absolute-error bound
      // ε = sqrt(ln(2/α) / (2N))  (α = 0.05)
      // The bound is distribution-free and only depends on the sample
      // count, so we read it back from the args input. Other methods are
      // exact (or have their own internal bounds), so no annotation.
      if (semiring === 'probability' && body.method === 'monte-carlo') {
        const n = parseInt(body.arguments || '', 10);
        if (Number.isFinite(n) && n > 0) {
          const eps = Math.sqrt(Math.log(40) / (2 * n));
          if (bound) bound.textContent =
            `(± ${eps.toFixed(eps < 0.01 ? 4 : 3)} with 95% probability)`;
        }
      }
    } catch (e) {
      // The fetch itself failed (no response) — record the time-to-fail
      // so the user can tell a hung connection (timeout) from an
      // immediate refusal.
      const dt = Math.round(performance.now() - t0);
      if (time) time.textContent = `· ${dt} ms`;
      result.textContent = `Network error: ${e.message}`;
      result.dataset.kind = 'error';
    } finally {
      run.disabled = false;
    }
  }

  // ─── helpers ──────────────────────────────────────────────────────────

  function svgEl(tag, attrs = {}) {
    const el = document.createElementNS(NS, tag);
    for (const k in attrs) el.setAttribute(k, attrs[k]);
    return el;
  }

  function escapeHtml(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function shortUuid(u) {
    if (!u) return '–';
    // Match the result-table abbreviation in app.js's formatCell so the
    // two views stay visually consistent — 4 hex chars are enough for a
    // cursory same/different check, full uuids are one click away.
    return u.length > 4 ? `${u.slice(0, 4)}…` : u;
  }
})();
