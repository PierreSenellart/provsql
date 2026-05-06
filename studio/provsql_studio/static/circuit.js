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
    // Drag-to-move offsets, keyed by node id. Survive frontier expansion
    // (paint() reads them on every repaint) so the user's manual nudges
    // aren't undone when new nodes appear; reset on renderCircuit() so a
    // new circuit always starts from the Graphviz layout.
    dragOffsets: Object.create(null),
  };
  let svg = null, edgeLayer = null, nodeLayer = null;
  let titleEl = null, subEl = null;
  let inspectorEl = null, inspectorTitle = null, inspectorBody = null;
  // Active node-drag session, populated on mousedown over a .node-group
  // and consumed by the window-level mousemove/mouseup handlers.
  let _drag = null;

  // Gate types whose children carry a meaningful order: cmp's lhs/rhs,
  // monus's minuend/subtrahend, and agg : but agg only when the function
  // is order-sensitive (array_agg, string_agg, json_agg, …). For
  // sum/count/min/max/avg the result is independent of input order, so
  // the digits would be noise. semimod is omitted: its value/scalar
  // split is implied by gate type. eq has a single child so positional
  // labels would be redundant.
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

  // ─── public API ───────────────────────────────────────────────────────

  window.ProvsqlCircuit = {
    init,                 // bind DOM handles after the sidebar markup is injected
    renderCircuit,        // (scene): replace the current scene
    setStatus,            // (title, sub): update header copy
    showLoading,          // (): placeholder while fetching
    showError,            // (msg)
    clearScene,           // (): wipe canvas, inspector, eval result, and target
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

    // Fullscreen toggle: a body-level class pins .cv-canvas to the
    // viewport via CSS; the ResizeObserver already wired up below
    // catches the new size and reflows the viewBox via fitView. Esc
    // exits : that's the standard convention for fullscreen and saves
    // a trip to the toolbar.
    const fsBtn = document.getElementById('tool-fullscreen');
    if (fsBtn) {
      fsBtn.onclick = () => toggleFullscreen();
      window.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && document.body.classList.contains('circuit-fullscreen')) {
          toggleFullscreen(false);
        }
      });
    }

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
    // need passive: false to call preventDefault : otherwise the
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

    // Drag-to-move circuit nodes. Per-node mousedown handlers (set in
    // paint()) seed `_drag`; the window-level move/up handlers track the
    // gesture so the drag continues even if the pointer leaves the
    // node's circle.
    window.addEventListener('mousemove', _onDragMove);
    window.addEventListener('mouseup',   _onDragEnd);

    const resetBtn = document.getElementById('tool-reset-layout');
    if (resetBtn) resetBtn.onclick = resetLayout;
  }

  // Wipe any user-applied positional offsets. Re-paints so the next
  // frame restores the Graphviz layout. The "I made it worse" escape
  // hatch flagged in the v0.2 TODO: positions are accumulated tweaks,
  // and there is no per-node "reset this one" affordance, so a single
  // canvas-wide reset is the simplest recovery path.
  function resetLayout() {
    state.dragOffsets = Object.create(null);
    if (state.scene) paint();
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

  function clearScene() {
    if (edgeLayer) edgeLayer.innerHTML = '';
    if (nodeLayer) nodeLayer.innerHTML = '';
    state.scene = null;
    state.pinnedNode = null;
    state.dragOffsets = Object.create(null);
    closeInspector();
    setStatus('Provenance Circuit', 'Click a UUID cell to render.');
    refreshEvalTarget();
    clearEvalResult();
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
    // then sizes the viewBox around the new bounding box. Drop any
    // node-drag offsets accumulated against the previous circuit:
    // they're keyed by uuid, so a stray entry from a different DAG
    // would otherwise re-apply if the same uuid recurred.
    state.zoom = 1;
    state.pan = { x: 0, y: 0 };
    state.dragOffsets = Object.create(null);
    closeInspector();
    paint();
    refreshEvalTarget();
    clearEvalResult();
  }

  // ─── paint ────────────────────────────────────────────────────────────

  function paint() {
    if (!state.scene || !state.scene.nodes.length) {
      showError('Empty circuit (no nodes returned).');
      return;
    }
    nodeLayer.innerHTML = '';

    paintEdges();

    // nodes
    for (const n of state.scene.nodes) {
      const cls = `node-group node--${n.type}` + (n.frontier ? ' is-frontier' : '');
      const p = nodePos(n);
      const g = svgEl('g', { class: cls, 'data-id': n.id, transform: `translate(${p.x},${p.y})` });
      g.appendChild(svgEl('circle', { class: 'node-shape', r: 22 }));
      const label = svgEl('text', { class: 'node-label', y: -2 });
      label.textContent = n.label || n.type[0];
      g.appendChild(label);
      // Meta line below: only leaf gates (input / update : both reference
      // a source row) get one. Internal gates stay bare : dropping a
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
      g.addEventListener('mousedown', (ev) => _onNodeMouseDown(ev, n, g));
      g.addEventListener('click', (ev) => {
        ev.stopPropagation();
        // A drag that crossed the movement threshold sets this flag so
        // the post-mouseup click does not pin / expand the node we just
        // dropped : the user's intent was to move it, not to click it.
        if (g._suppressClick) { g._suppressClick = false; return; }
        onNodeClick(n);
      });
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
    // Bounding box is over the displaced positions: a node the user
    // dragged outside the original Graphviz envelope still belongs
    // inside the viewBox, otherwise "Fit" silently clips it.
    const ps = state.scene.nodes.map(nodePos);
    const xs = ps.map(p => p.x);
    const ys = ps.map(p => p.y);
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

  // ─── edges + position helpers ────────────────────────────────────────

  // The painted (x, y) for a node: layout coordinate plus any drag offset.
  function nodePos(n) {
    const o = state.dragOffsets[n.id];
    return o ? { x: n.x + o.dx, y: n.y + o.dy } : { x: n.x, y: n.y };
  }

  // Rebuilds every edge path + ordered-child position label from
  // current nodePos(). Cheap (a few dozen paths typically), and lets
  // drag-move re-flow incident edges without diffing.
  function paintEdges() {
    edgeLayer.innerHTML = '';
    if (!state.scene) return;
    const nodesById = Object.fromEntries(state.scene.nodes.map(n => [n.id, n]));
    for (const e of state.scene.edges) {
      const from = nodesById[e.from], to = nodesById[e.to];
      if (!from || !to) continue;
      const fp = nodePos(from), tp = nodePos(to);
      const path = svgEl('path', {
        class: 'edge',
        d: `M ${fp.x} ${fp.y + 22} C ${fp.x} ${fp.y + 50}, ${tp.x} ${tp.y - 50}, ${tp.x} ${tp.y - 22}`,
        'data-from': e.from, 'data-to': e.to,
      });
      edgeLayer.appendChild(path);

      // Position label at the child end of the edge for ordered gates.
      // Offset 32px (r=22 + a small gap) away from the child centre
      // along the edge direction so the digit clears the stroke.
      if (shouldLabelChildren(from) && e.child_pos != null) {
        const dx = fp.x - tp.x;
        const dy = fp.y - tp.y;
        const len = Math.hypot(dx, dy) || 1;
        const offset = 32;
        const lx = tp.x + (dx / len) * offset;
        const ly = tp.y + (dy / len) * offset;
        const tag = svgEl('text', {
          class: 'edge-pos',
          x: lx, y: ly,
          'text-anchor': 'middle', 'dominant-baseline': 'central',
        });
        tag.textContent = String(e.child_pos);
        edgeLayer.appendChild(tag);
      }
    }
    // The pinned-subtree edge highlight lives on `.is-active` classes
    // we just discarded; reapply so a drag-while-pinned doesn't lose
    // the visual cue.
    if (state.pinnedNode) {
      const set = descendants(state.pinnedNode);
      edgeLayer.querySelectorAll('.edge').forEach(p => {
        if (set.has(p.dataset.from) && set.has(p.dataset.to)) p.classList.add('is-active');
      });
    }
  }

  // ─── drag-to-move ────────────────────────────────────────────────────

  // Convert client (mouse) coordinates to the SVG's user-space, so a
  // delta in pixels translates correctly regardless of the current
  // zoom / pan / aspect ratio. Reading getScreenCTM() each call is
  // fine: the SVG only resizes on layout changes, not per mousemove.
  function clientToSvg(clientX, clientY) {
    const ctm = svg.getScreenCTM();
    if (!ctm) return { x: clientX, y: clientY };
    const pt = svg.createSVGPoint();
    pt.x = clientX; pt.y = clientY;
    const p = pt.matrixTransform(ctm.inverse());
    return { x: p.x, y: p.y };
  }

  function _onNodeMouseDown(e, n, g) {
    if (e.button !== 0) return;
    // Don't kick off the SVG-level pan handler underneath us.
    e.stopPropagation();
    // Fresh interaction: clear any stale click-suppress flag from a
    // previous drag whose mouseup happened off-element (no click event
    // delivered to clear it the natural way).
    g._suppressClick = false;
    const start = clientToSvg(e.clientX, e.clientY);
    const off = state.dragOffsets[n.id] || { dx: 0, dy: 0 };
    _drag = {
      nodeId: n.id, group: g,
      sx: start.x, sy: start.y,
      origDx: off.dx, origDy: off.dy,
      clientX: e.clientX, clientY: e.clientY,
      didDrag: false,
    };
  }

  function _onDragMove(e) {
    if (!_drag) return;
    // Movement threshold (~4px in screen space): below this, we treat
    // the gesture as a click in waiting and don't perturb the layout.
    const dpx = Math.hypot(e.clientX - _drag.clientX, e.clientY - _drag.clientY);
    if (!_drag.didDrag && dpx < 4) return;
    _drag.didDrag = true;
    const cur = clientToSvg(e.clientX, e.clientY);
    state.dragOffsets[_drag.nodeId] = {
      dx: _drag.origDx + (cur.x - _drag.sx),
      dy: _drag.origDy + (cur.y - _drag.sy),
    };
    // Translate the moved group; meta line, label, and frontier badge
    // are children of the group, so they follow for free.
    const node = state.scene && state.scene.nodes.find(x => x.id === _drag.nodeId);
    if (node && _drag.group) {
      const p = nodePos(node);
      _drag.group.setAttribute('transform', `translate(${p.x},${p.y})`);
    }
    // Reflow incident edges (cheap full rebuild beats diffing).
    paintEdges();
  }

  function _onDragEnd() {
    if (_drag && _drag.didDrag && _drag.group) {
      // Eat the click event the browser is about to deliver: the user
      // dragged the node, they didn't click it.
      _drag.group._suppressClick = true;
    }
    _drag = null;
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
    // The title already says which gate type this is, so we drop the
    // separate `type` row from the body. depth stays : it tells the
    // user how far this gate is from the root, useful when navigating
    // a deep BFS.
    // Match the in-circuit display: abbreviated UUID by default, full
    // value only when the "Show UUIDs" toggle is pressed. The title
    // attribute keeps the full string available on hover for the
    // collapsed form.
    const uuidText = state.showUuids ? node.id : shortUuid(node.id);
    html += `<dt>uuid</dt><dd title="${escapeHtml(node.id)}">${escapeHtml(uuidText)}</dd>`;
    html += `<dt>depth</dt><dd>${node.depth}</dd>`;
    // info1 / info2 are gate-type-specific integers in the raw schema
    // (see provsql.set_infos doc). Translate to a human-readable form
    // wherever we can: aggregate function name (info1) + result type
    // (info2) for `agg`, comparison operator name for `cmp`, the
    // multivalued variable's actual value for `mulinput`, attribute
    // indices for `eq`. Anything else falls back to raw `infoN`.
    for (const fact of _gateInfos(node)) {
      html += `<dt>${escapeHtml(fact.label)}</dt><dd>${escapeHtml(fact.value)}</dd>`;
    }
    // `extra` is set by project (input→output column mapping array),
    // value (scalar), and agg (computed scalar). Project's mapping is
    // PG's text-encoded array-of-pairs ({{1,1},{2,3}}); pretty-print
    // it as "input col → output col" lines so the user doesn't have to
    // parse the punctuation.
    if (node.extra != null && node.extra !== '') {
      if (node.type === 'project') {
        const pairs = _parseProjectMapping(node.extra);
        if (pairs.length) {
          const items = pairs.map(([a, b]) =>
            `<li>input col ${escapeHtml(a)} → output col ${escapeHtml(b)}</li>`
          ).join('');
          html += `<dt>mapping</dt><dd><ul class="cv-inspector__mapping">${items}</ul></dd>`;
        } else {
          html += `<dt>mapping</dt><dd>${escapeHtml(node.extra)}</dd>`;
        }
      } else if (node.type === 'value' || node.type === 'agg') {
        html += `<dt>value</dt><dd>${escapeHtml(node.extra)}</dd>`;
      } else {
        html += `<dt>extra</dt><dd>${escapeHtml(node.extra)}</dd>`;
      }
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
  // Custom-semiring options (encoded as `custom:<schema>.<name>`) also need a
  // mapping; see `needsMapping`.
  const _SR_NEEDS_MAPPING = new Set(['boolean', 'counting', 'why', 'formula']);
  function needsMapping(v) {
    return _SR_NEEDS_MAPPING.has(v) || v.startsWith('custom:');
  }

  // PG type names psycopg surfaces as either JS numbers (smallints, ints,
  // floats) or strings (numeric / Decimal). Either way we render with 4
  // decimals for parity with the probability-value formatter.
  const _CUSTOM_NUMERIC_TYPES = new Set([
    'numeric', 'double precision', 'real',
    'integer', 'bigint', 'smallint',
    'int', 'int2', 'int4', 'int8', 'float4', 'float8',
  ]);
  function formatCustomValue(value, typeName) {
    if (value == null) return '(null)';
    if (_CUSTOM_NUMERIC_TYPES.has(typeName)) {
      const n = typeof value === 'number' ? value : parseFloat(value);
      if (Number.isFinite(n)) return n.toFixed(4);
    }
    if (typeName === 'boolean' && typeof value === 'boolean') {
      return value ? 'true' : 'false';
    }
    // Multiranges, enums, ranges, text : already display-ready as strings.
    return String(value);
  }

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
    let customsLoaded  = false;
    // Last loader payloads; kept around so a semiring change can re-render
    // the mapping dropdown (with the right type-compatibility filter
    // applied) without an extra round-trip.
    let _mappings = [];
    let _customs  = [];

    // Both caches are dirty by default, and are flipped to dirty again
    // by `runQuery` after every successful exec. Each loader clears its
    // own flag once the fetch returns. The picker's `mousedown` handler
    // re-runs the loaders if the corresponding dirty flag is set, so a
    // newly-created mapping or wrapper shows up the moment the user
    // opens the dropdown.
    function metadataDirty(key) {
      return !!window.ProvsqlStudio?.metadata?.[key];
    }
    function clearMetadataDirty(key) {
      if (window.ProvsqlStudio?.metadata) {
        window.ProvsqlStudio.metadata[key] = false;
      }
    }

    function syncControls() {
      const v = sel.value;
      map.hidden  = !needsMapping(v);
      meth.hidden = v !== 'probability';
      // Show only the args control that matches the current probability
      // method (if any); hide every other one so the row stays compact.
      const wantedId = (v === 'probability') ? _PROB_ARG_CONTROL[meth.value] : null;
      for (const ctrl of argControls) ctrl.hidden = (ctrl.id !== wantedId);
      // Stale once the input shape changes : wipe result + bound +
      // time + the clear button.
      clearEvalResult();
      if (!map.hidden && (!mappingsLoaded || metadataDirty('mappingsDirty'))) {
        loadMappings();
      } else if (!map.hidden) {
        // Fresh cache : re-render with the (possibly new) type filter.
        renderMappingOptions();
      }
      updateMappingHint();
    }

    // Type expected for the mapping's `value` column under the current
    // semiring choice. Custom semirings expose it as the wrapper's return
    // type (the convention is `wrapper return type == mapping value type`,
    // since the typed `zero`/`plus`/`times` inside provenance_evaluate
    // pin the value column's type). Returns null when no filter applies.
    function expectedValueType() {
      const v = sel.value;
      if (!v.startsWith('custom:')) return null;
      const qname = v.slice('custom:'.length);
      const c = _customs.find(x => x.qname === qname);
      return c ? c.return_type : null;
    }

    // Render the mapping <option>s from the cached list, filtered by the
    // current semiring's expected value type. Compiled / probability
    // semirings get the full list (their kernels accept any value type
    // polymorphically); custom semirings get only the type-compatible
    // ones, with a clear empty-state if none match.
    function renderMappingOptions() {
      if (!_mappings.length) {
        map.innerHTML = '<option value="">(no mappings : run create_provenance_mapping)</option>';
        map.disabled = true;
        return;
      }
      const expect = expectedValueType();
      const list = expect
        ? _mappings.filter(m => m.value_type === expect)
        : _mappings;
      if (!list.length) {
        map.innerHTML =
          `<option value="">(no compatible mappings : value type ≠ ${escapeHtml(expect)})</option>`;
        map.disabled = true;
        return;
      }
      map.disabled = false;
      const previousValue = map.value || '';
      map.innerHTML = list.map(m => {
        const label = m.display_name || m.qname;
        const tagged = `${label} (${m.value_type})`;
        const title = `${m.qname} : value ${m.value_type}`;
        return `<option value="${escapeHtml(m.qname)}" title="${escapeHtml(title)}">${escapeHtml(tagged)}</option>`;
      }).join('');
      if (previousValue && [...map.options].some(o => o.value === previousValue)) {
        map.value = previousValue;
      }
    }

    // Hint text next to the mapping dropdown for compiled semirings whose
    // value-type expectations can't be enforced statically (the wrapper
    // signature uses regclass without a type constraint, but the kernel
    // still implicitly expects values it can interpret as boolean / int).
    // The user can pick anything but knows what's expected.
    const _COMPILED_HINTS = {
      boolean:  'Expects boolean values.',
      counting: 'Expects numeric values.',
    };
    function updateMappingHint() {
      const hint = document.getElementById('eval-mapping-hint');
      if (!hint) return;
      const expect = expectedValueType();
      if (expect) {
        hint.textContent = `Filtered to ${expect}`;
        hint.hidden = map.hidden;
        return;
      }
      const msg = _COMPILED_HINTS[sel.value];
      if (msg && !map.hidden) {
        hint.textContent = msg;
        hint.hidden = false;
      } else {
        hint.hidden = true;
        hint.textContent = '';
      }
    }

    async function loadCustomSemirings() {
      const grp = document.getElementById('eval-custom-group');
      if (!grp) return;
      try {
        const resp = await fetch('/api/custom_semirings');
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        const list = await resp.json();
        _customs = list;
        if (!list.length) {
          grp.hidden = true;
          customsLoaded = true;
          clearMetadataDirty('customsDirty');
          return;
        }
        grp.innerHTML = list.map(c => {
          const label = c.display_name || c.qname;
          const title = `${c.qname} → ${c.return_type}`;
          return `<option value="custom:${escapeHtml(c.qname)}" title="${escapeHtml(title)}">${escapeHtml(label)}</option>`;
        }).join('');
        grp.hidden = false;
        customsLoaded = true;
        clearMetadataDirty('customsDirty');
        // The user may already have a custom semiring selected from a
        // previous session : now that we know its return_type, refresh
        // the mapping dropdown's filter and the hint text.
        if (mappingsLoaded && !map.hidden) {
          renderMappingOptions();
          updateMappingHint();
        }
      } catch (e) {
        // Discovery failure is non-fatal: leave the optgroup hidden so
        // the rest of the strip stays usable. Don't clear the dirty
        // flag : next dropdown open should retry.
        grp.hidden = true;
      }
    }

    async function loadMappings() {
      mappingsLoaded = true;
      try {
        const resp = await fetch('/api/provenance_mappings');
        if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
        _mappings = await resp.json();
        clearMetadataDirty('mappingsDirty');
        // `display_name` drops the schema when the relation is search_path-
        // visible (resolves unambiguously without qualification), keeping
        // labels short for the common public-schema case. The option's
        // value is still the qualified name so the regclass cast on the
        // server can never resolve to the wrong schema. Type-tagging plus
        // optional filter happens in renderMappingOptions, which also
        // preserves the user's current selection across the refresh.
        renderMappingOptions();
        updateMappingHint();
      } catch (e) {
        // Allow a retry on next semiring change.
        mappingsLoaded = false;
        _mappings = [];
        map.innerHTML = `<option value="">(load failed: ${escapeHtml(e.message)})</option>`;
        map.disabled = true;
      }
    }

    sel.addEventListener('change', syncControls);
    // Refresh on dropdown open : if the user has run an exec since the
    // last load (or hit the toolbar refresh button), the dirty flag is
    // set, and we silently re-fetch before the dropdown actually opens.
    // mousedown beats the native open by a frame; that's enough for a
    // freshly-created wrapper to appear before the user picks.
    sel.addEventListener('mousedown', () => {
      if (!customsLoaded || metadataDirty('customsDirty')) loadCustomSemirings();
    });
    map.addEventListener('mousedown', () => {
      if (metadataDirty('mappingsDirty')) loadMappings();
    });
    // Method change also affects whether the args input is shown / what
    // its placeholder reads.
    meth.addEventListener('change', syncControls);
    run.addEventListener('click', runEvaluation);
    const clearBtn = document.getElementById('eval-clear');
    if (clearBtn) clearBtn.onclick = clearEvalResult;
    loadCustomSemirings();
    syncControls();
  }

  // Wipe the result chip + bound + time. Useful in fullscreen where a
  // verbose Why / Formula output otherwise obscures the canvas with no
  // way to dismiss without re-running on a smaller circuit.
  function clearEvalResult() {
    const result = document.getElementById('eval-result');
    const bound  = document.getElementById('eval-bound');
    const time   = document.getElementById('eval-time');
    const clear  = document.getElementById('eval-clear');
    if (result) { result.textContent = ''; delete result.dataset.kind; }
    if (bound)  bound.textContent  = '';
    if (time)   time.textContent   = '';
    if (clear)  clear.hidden = true;
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
    const selValue = sel.value;
    // `custom:<schema>.<name>` packs the wrapper identity into the option
    // value; unpack here so the request shape stays {semiring, function}.
    const isCustom = selValue.startsWith('custom:');
    const semiring = isCustom ? 'custom' : selValue;
    const body = { token, semiring };
    if (isCustom) body.function = selValue.slice('custom:'.length);
    if (needsMapping(selValue)) {
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
      } else if (data.kind === 'custom') {
        display = formatCustomValue(data.result, data.type_name);
      } else if (data.result == null) {
        display = '(null)';
      } else {
        display = String(data.result);
      }
      result.textContent = '= ' + display;
      result.dataset.kind = 'ok';
      result.title = data.kind === 'custom'
        ? `${data.function} → ${data.type_name}`
        : `${data.kind} value`;
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
      // The fetch itself failed (no response) : record the time-to-fail
      // so the user can tell a hung connection (timeout) from an
      // immediate refusal.
      const dt = Math.round(performance.now() - t0);
      if (time) time.textContent = `· ${dt} ms`;
      result.textContent = `Network error: ${e.message}`;
      result.dataset.kind = 'error';
    } finally {
      run.disabled = false;
      // Whatever happened (ok / error / network fail), there's now
      // something in the result chip the user may want to dismiss.
      const clear = document.getElementById('eval-clear');
      if (clear) clear.hidden = false;
    }
  }

  function toggleFullscreen(force) {
    const on = (typeof force === 'boolean')
      ? force
      : !document.body.classList.contains('circuit-fullscreen');
    document.body.classList.toggle('circuit-fullscreen', on);
    const btn = document.getElementById('tool-fullscreen');
    if (btn) {
      btn.setAttribute('aria-pressed', String(on));
      const icon = btn.querySelector('i');
      if (icon) {
        // FA5 names: expand/compress-arrows-alt are the standard
        // four-way fullscreen pair (the v6 *-from-center / *-to-center
        // names don't exist in 5.x).
        icon.classList.toggle('fa-expand-arrows-alt', !on);
        icon.classList.toggle('fa-compress-arrows-alt', on);
      }
    }
    // The ResizeObserver on the SVG handles the reflow, but its callback
    // may fire on a microtask boundary; explicit fitView keeps things
    // tight on browsers where the observer is slow to deliver the first
    // resize after a layout-changing class flip.
    if (state.scene) fitView();
  }

  // ─── inspector helpers ────────────────────────────────────────────────

  // Translate a node's gate-type-specific (info1, info2) pair into
  // human-readable facts. Always includes the children count (a useful
  // structural property: which times has 4 inputs? which agg has 12?).
  function _gateInfos(node) {
    const out = [];
    const childCount = (state.scene && state.scene.edges)
      ? state.scene.edges.filter(e => e.from === node.id).length
      : 0;
    if (childCount > 0) out.push({ label: 'children', value: String(childCount) });

    const t = node.type;
    if (t === 'agg') {
      // info1 = aggregate function oid → proname; info2 = result type
      // oid → typname. Label stays "function" / "result type" even on
      // the unresolved-name fallback (rare: dropped type / function);
      // the user gets a number instead of a name but doesn't have to
      // mentally translate "oid" themselves.
      const fn = node.info1_name || node.info1;
      if (fn != null) out.push({ label: 'function', value: fn });
      const rt = node.info2_name || node.info2;
      if (rt != null) out.push({ label: 'result type', value: rt });
    } else if (t === 'cmp') {
      const op = node.info1_name || node.info1;
      if (op != null) out.push({ label: 'operator', value: op });
    } else if (t === 'eq') {
      // info1 / info2 are attribute indices for the two equijoin sides.
      if (node.info1 != null) out.push({ label: 'left attr',  value: node.info1 });
      if (node.info2 != null) out.push({ label: 'right attr', value: node.info2 });
    } else if (t === 'mulinput') {
      // info1 = the multivalued variable's value.
      if (node.info1 != null) out.push({ label: 'value', value: node.info1 });
    } else if (t === 'input' || t === 'update') {
      // info1 = source relation id (already shown as `tbl X` under the
      // node), info2 = column count. Surface column count here so the
      // inspector adds something the canvas doesn't.
      if (node.info1 != null) out.push({ label: 'relation id', value: node.info1 });
      if (node.info2 != null) out.push({ label: 'columns',     value: node.info2 });
    } else {
      // No type-specific translation : fall back to raw fields if set.
      if (node.info1 != null) out.push({ label: 'info1', value: node.info1 });
      if (node.info2 != null) out.push({ label: 'info2', value: node.info2 });
    }
    return out;
  }

  // Parse PG's text-encoded ARRAY of two-element ARRAYs ({{1,1},{2,3}}…)
  // into a list of [input, output] pairs. Returns [] on anything we
  // don't recognise so the caller can fall back to the raw text.
  function _parseProjectMapping(s) {
    if (!s) return [];
    const m = String(s).match(/^\{(.*)\}$/);
    if (!m) return [];
    const inner = m[1];
    const out = [];
    // Match every {a,b} group; both elements are integers in practice.
    const re = /\{\s*(-?\d+)\s*,\s*(-?\d+)\s*\}/g;
    let g;
    while ((g = re.exec(inner)) !== null) out.push([g[1], g[2]]);
    return out;
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
    // two views stay visually consistent : 4 hex chars are enough for a
    // cursory same/different check, full uuids are one click away.
    return u.length > 4 ? `${u.slice(0, 4)}…` : u;
  }
})();
