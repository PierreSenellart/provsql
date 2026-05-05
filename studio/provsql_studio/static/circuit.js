/* ProvSQL Studio — circuit-mode renderer.
   Lazy-loaded by app.js once the user clicks a UUID/agg_token cell in
   circuit-mode results. The DOT layout is computed server-side
   (provsql_studio/circuit.py via `dot -Tjson`); this module just paints,
   handles zoom/pan/pin, and delegates expansion to the server. */

(function () {
  if (window.ProvsqlCircuit) return; // idempotent — app.js may call setup repeatedly

  const NS = 'http://www.w3.org/2000/svg';

  const TYPE_SUMMARY = {
    plus:     'Plus gate (+) — disjunction (UNION, projection, multi-derivation)',
    times:    'Times gate (×) — conjunction (joins, conjunctive WHERE)',
    monus:    'Monus gate (−) — m-semiring difference (EXCEPT)',
    project:  'Project gate (π) — column projection (where-provenance only)',
    eq:       'Eq gate (=) — equijoin witness (where-provenance only)',
    agg:      'Aggregation gate (Σ) — GROUP BY aggregate',
    semimod:  'Semimodule scalar (⊙) — scalar × aggregate',
    cmp:      'Compare gate (≷) — aggregate-value comparison',
    delta:    'Delta gate (δ) — δ-semiring operator',
    value:    'Value gate (v) — scalar constant',
    mulinput: 'Multivalued input (⋮)',
    input:    'Input gate — base tuple',
    one:      'One — semiring identity (always-true)',
    zero:     'Zero — semiring identity (always-false)',
  };

  // ─── state ────────────────────────────────────────────────────────────

  let state = {
    scene: null,        // {nodes, edges, root, depth} from /api/circuit
    showUuids: false,
    showFormula: true,
    zoom: 1,
    pan: { x: 0, y: 0 },
    pinnedNode: null,
  };
  let svg = null, edgeLayer = null, nodeLayer = null;
  let titleEl = null, subEl = null;
  let formulaEl = null, formulaStrip = null;
  let inspectorEl = null, inspectorTitle = null, inspectorBody = null;

  // ─── public API ───────────────────────────────────────────────────────

  window.ProvsqlCircuit = {
    init,                 // bind DOM handles after the sidebar markup is injected
    renderCircuit,        // (scene) — replace the current scene
    setStatus,            // (title, sub) — update header copy
    showLoading,          // () — placeholder while fetching
    showError,            // (msg)
  };

  // ─── init ─────────────────────────────────────────────────────────────

  function init() {
    svg = document.getElementById('circuit');
    edgeLayer = document.getElementById('circuit-edges');
    nodeLayer = document.getElementById('circuit-nodes');
    titleEl = document.getElementById('circuit-title');
    subEl = document.getElementById('circuit-sub');
    formulaEl = document.getElementById('formula-expr');
    formulaStrip = document.getElementById('formula-strip');
    inspectorEl = document.getElementById('inspector');
    inspectorTitle = document.getElementById('inspector-title');
    inspectorBody = document.getElementById('inspector-body');

    // Toolbar.
    document.getElementById('tool-zoom-in').onclick  = () => { state.zoom = Math.min(2.5, state.zoom * 1.2); fitView(); };
    document.getElementById('tool-zoom-out').onclick = () => { state.zoom = Math.max(0.4, state.zoom / 1.2); fitView(); };
    document.getElementById('tool-zoom-fit').onclick = () => { state.zoom = 1; state.pan = { x: 0, y: 0 }; fitView(); };
    const uBtn = document.getElementById('tool-show-uuids');
    uBtn.onclick = () => {
      state.showUuids = !state.showUuids;
      uBtn.setAttribute('aria-pressed', String(state.showUuids));
      paint();
    };
    const fBtn = document.getElementById('tool-show-formula');
    fBtn.onclick = () => {
      state.showFormula = !state.showFormula;
      fBtn.setAttribute('aria-pressed', String(state.showFormula));
      formulaStrip.style.display = state.showFormula ? 'flex' : 'none';
    };
    document.getElementById('inspector-close').onclick = closeInspector;

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
    if (formulaEl) formulaEl.textContent = '—';
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
    closeInspector();
    paint();
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
    }

    // nodes
    for (const n of state.scene.nodes) {
      const cls = `node-group node--${n.type}` + (n.frontier ? ' is-frontier' : '');
      const g = svgEl('g', { class: cls, 'data-id': n.id, transform: `translate(${n.x},${n.y})` });
      g.appendChild(svgEl('circle', { class: 'node-shape', r: 22 }));
      const label = svgEl('text', { class: 'node-label', y: 1 });
      label.textContent = n.label || n.type[0];
      g.appendChild(label);
      // Meta line below: short UUID by default; full UUID in toggle mode; for
      // input gates, info1/info2 (table id, n_columns) when no UUID toggle.
      const metaText = state.showUuids
        ? n.id
        : (n.type === 'input' && n.info1 ? `tbl ${n.info1}` : shortUuid(n.id));
      const meta = svgEl('text', { class: 'node-meta', y: 38 });
      meta.textContent = metaText;
      g.appendChild(meta);
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

    // formula
    if (formulaEl) {
      formulaEl.innerHTML = state.showFormula ? formulaHtml(state.scene) : '<span style="opacity:.5">(hidden)</span>';
      formulaStrip.style.display = state.showFormula ? 'flex' : 'none';
    }

    setStatus(
      'Provenance Circuit',
      `${state.scene.nodes.length} gates, BFS depth ${state.scene.depth} · root ${shortUuid(state.scene.root)}`
    );
  }

  function fitView() {
    if (!state.scene) return;
    const xs = state.scene.nodes.map(n => n.x);
    const ys = state.scene.nodes.map(n => n.y);
    const minX = Math.min(...xs) - 60, maxX = Math.max(...xs) + 60;
    const minY = Math.min(...ys) - 60, maxY = Math.max(...ys) + 60;
    const w = Math.max(maxX - minX, 200), h = Math.max(maxY - minY, 150);
    const cx = minX + w / 2 + state.pan.x;
    const cy = minY + h / 2 + state.pan.y;
    const halfW = w / (2 * state.zoom);
    const halfH = h / (2 * state.zoom);
    svg.setAttribute('viewBox', `${cx - halfW} ${cy - halfH} ${halfW * 2} ${halfH * 2}`);
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
  }

  function clearPin() {
    document.querySelectorAll('.node-group').forEach(g => g.classList.remove('is-pinned'));
    document.querySelectorAll('.edge').forEach(p => p.classList.remove('is-active'));
    state.pinnedNode = null;
    closeInspector();
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
    html += `<dt>uuid</dt><dd>${escapeHtml(node.id)}</dd>`;
    html += `<dt>depth</dt><dd>${node.depth}</dd>`;
    if (node.info1 != null) html += `<dt>info1</dt><dd>${escapeHtml(node.info1)}</dd>`;
    if (node.info2 != null) html += `<dt>info2</dt><dd>${escapeHtml(node.info2)}</dd>`;
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

  // ─── formula rendering ────────────────────────────────────────────────

  function formulaHtml(scene) {
    if (!scene.nodes.length) return '<span style="opacity:.5">(empty)</span>';
    const childrenOf = {};
    for (const e of scene.edges) {
      (childrenOf[e.from] = childrenOf[e.from] || []).push(e);
    }
    function recur(nodeId, depth) {
      const node = scene.nodes.find(n => n.id === nodeId);
      if (!node) return '';
      if (depth > 4) return '<span class="leaf">…</span>';
      const ch = (childrenOf[nodeId] || []).slice().sort((a, b) => (a.child_pos || 0) - (b.child_pos || 0));
      if (!ch.length) {
        const cls = (node.type === 'input' || node.type === 'mulinput') ? 'leaf' : 'one';
        return `<span class="${cls}">${escapeHtml(node.label)}</span>`;
      }
      const op = ` <span class="op-${node.type}">${escapeHtml(node.label)}</span> `;
      const inner = ch.map(e => recur(e.to, depth + 1)).join(op);
      return `(${inner})`;
    }
    return recur(scene.root, 0);
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
    if (!u) return '—';
    return u.length > 12 ? `${u.slice(0, 4)}…${u.slice(-4)}` : u;
  }
})();
