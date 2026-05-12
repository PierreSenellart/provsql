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
    rv:       'Random variable: continuous distribution leaf',
    arith:    'Arithmetic gate: scalar operation over child gates',
    mixture:  'Mixture (Mix): Bernoulli-weighted choice between two scalar RV branches, or a categorical block (key + N mulinput outcomes)',
  };

  // gate_arith info1 holds a PROVSQL_ARITH_* tag (src/provsql_utils.h).
  // Mirror the server-side _ARITH_OP_GLYPH in circuit.py so the
  // inspector can label the operator without an extra round-trip.
  const ARITH_OP_NAME = {
    0: 'plus (+)',
    1: 'times (×)',
    2: 'minus (−)',
    3: 'divide (÷)',
    4: 'negate (−x)',
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
    // Row context for the current scene: the `__prov` (or `provsql`)
    // UUID of the result-table row whose click loaded this circuit.
    // Used by autoPresetConditionInput so the eval strip's "Condition
    // on" input defaults to the row's provenance gate (i.e. the
    // canonical conditioning event for `expected(rv, provenance())`),
    // even when the click target was the row's random_variable cell
    // (whose scene root is the RV itself, not the row's prov).
    rowProv: null,
    // Tracks which row provsql we last auto-preset against, so a row
    // context change (clicking another row's cell) overwrites the
    // Condition input even if the user had manually pasted a UUID
    // for the previous row.  Manual edits within the same row are
    // still respected (the `input` listener clears autoset, and we
    // don't overwrite while lastAutoPresetRow matches).
    lastAutoPresetRow: null,
  };
  let svg = null, edgeLayer = null, nodeLayer = null, bannerEl = null;
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
  // labels would be redundant.  mixture's three wires (p / x / y) are
  // positional and get the semantic labels defined in EDGE_POS_LABEL
  // below.
  const ORDERED_GATES = new Set(['cmp', 'monus', 'agg', 'arith', 'mixture']);
  // Per-(parent type, 1-based child_pos) edge-label overrides for
  // gates whose wires have well-known names.  Falls back to the bare
  // digit (1, 2, 3, …) for any entry not in the map.  mixture(p, x, y)
  // mirrors the SQL constructor's parameter names so the rendered edge
  // labels match the user-facing API.  Entries may also be functions
  // (parent_node, child_pos) → string for shape-dependent labels: the
  // categorical-form gate_mixture has wires [key, mul_1, ..., mul_n]
  // and labels them accordingly when more than three wires are present.
  function _mixtureEdgeLabel(parent, child_pos) {
    // Distinguish the categorical form structurally rather than by
    // wire count: a bimodal categorical (provsql.categorical with
    // two outcomes) has only three wires ([key, mul_1, mul_2]) yet
    // is still categorical, while a classic 3-wire mixture is
    // [p_input, x_scalar, y_scalar].  The discriminator is the
    // types of the non-first wires -- all gate_mulinput in the
    // categorical form, gate_rv / gate_value / gate_arith /
    // gate_mixture in the classic form.
    if (state.scene && state.scene.edges && state.scene.nodes) {
      const nodes_by_id = {};
      for (const n of state.scene.nodes) nodes_by_id[n.id] = n;
      const children = state.scene.edges
        .filter(e => e.from === parent.id)
        .sort((a, b) => a.child_pos - b.child_pos);
      if (children.length >= 2) {
        const wire0 = nodes_by_id[children[0].to];
        const isCategorical = wire0 && wire0.type === 'input'
          && children.slice(1).every(c => {
               const t = nodes_by_id[c.to];
               return t && t.type === 'mulinput';
             });
        if (isCategorical) {
          // Only the key wire has a distinguished role; the
          // mulinputs are unordered outcomes of the same block, so
          // positional digits would be misleading.  Return null on
          // the mulinput wires to suppress the edge label entirely.
          return child_pos === 1 ? 'key' : null;
        }
      }
    }
    // Classic 3-wire mixture: [p, x, y].
    return ({ 1: 'p', 2: 'x', 3: 'y' })[child_pos] ?? String(child_pos);
  }
  const EDGE_POS_LABEL = {
    mixture: _mixtureEdgeLabel,
  };
  const COMMUTATIVE_AGG = new Set(['sum', 'count', 'min', 'max', 'avg']);
  // = and <> are commutative; lhs/rhs digits add noise for those cmp
  // gates the same way they would for SUM/COUNT/etc.  The strict
  // comparators (<, <=, >, >=) keep the positional digits since
  // flipping them changes semantics.
  const COMMUTATIVE_CMP = new Set(['=', '<>', '!=']);
  // PROVSQL_ARITH_* tags whose result depends on argument order:
  // 2 = MINUS, 3 = DIV.  PLUS / TIMES are commutative (no labels);
  // NEG has a single child (label would be a lone "1", redundant).
  const NON_COMMUTATIVE_ARITH = new Set([2, 3]);
  function shouldLabelChildren(parent) {
    if (!ORDERED_GATES.has(parent.type)) return false;
    if (parent.type === 'agg') {
      const fn = (parent.info1_name || '').toLowerCase();
      return !COMMUTATIVE_AGG.has(fn);
    }
    if (parent.type === 'cmp') {
      const op = parent.info1_name || '';
      return !COMMUTATIVE_CMP.has(op);
    }
    if (parent.type === 'arith') {
      const tag = parent.info1 == null ? null : Number(parent.info1);
      return Number.isFinite(tag) && NON_COMMUTATIVE_ARITH.has(tag);
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
    showTooLarge,         // (payload, onRetry): structured 413 banner with retry button
    clearScene,           // (): wipe canvas, inspector, eval result, and target
  };

  // ─── init ─────────────────────────────────────────────────────────────

  function init() {
    svg = document.getElementById('circuit');
    edgeLayer = document.getElementById('circuit-edges');
    nodeLayer = document.getElementById('circuit-nodes');
    bannerEl = document.getElementById('cv-banner');
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
    // semirings (mapping is optional for boolexpr and prov-xml), a
    // method picker for probability.
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

  function hideBanner() {
    if (!bannerEl) bannerEl = document.getElementById('cv-banner');
    if (bannerEl) {
      bannerEl.hidden = true;
      bannerEl.innerHTML = '';
    }
  }

  function showLoading() {
    hideBanner();
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
    hideBanner();
    if (edgeLayer) edgeLayer.innerHTML = '';
    if (nodeLayer) nodeLayer.innerHTML = '';
    state.scene = null;
    state.pinnedNode = null;
    state.rowProv = null;
    state.lastAutoPresetRow = null;
    state.dragOffsets = Object.create(null);
    closeInspector();
    setStatus('Provenance Circuit', 'Click a UUID cell to render.');
    refreshEvalTarget();
    clearEvalResult();
  }

  function showError(msg) {
    hideBanner();
    if (edgeLayer) edgeLayer.innerHTML = '';
    if (nodeLayer) {
      nodeLayer.innerHTML =
        '<text x="50%" y="50%" text-anchor="middle" dominant-baseline="central" '
        + 'fill="var(--terracotta-500)" font-family="var(--font-ui)" font-size="14">'
        + escapeHtml(String(msg)) + '</text>';
    }
    setStatus('Provenance Circuit', 'Error.');
  }

  // Structured "circuit too large" banner. payload comes straight from
  // the 413 body: {node_count, cap, depth, depth_1_size, hint}. onRetry
  // is invoked with the suggested lower depth when the user clicks the
  // retry button; the button is suppressed entirely when depth <= 1 or
  // when even depth-1 wouldn't fit under the cap (the wide-bound case).
  //
  // opts.rootUuid: when given (loadCircuit's 413 path), install a stub
  // scene rooted at that UUID so the eval strip can still fire against
  // it -- the eval API only needs the token, not a rendered DAG, so a
  // too-large circuit shouldn't lock the user out of evaluation.
  // Omit it from expandFrontier's 413 path: the existing rendered
  // scene is still the right eval target there.
  function showTooLarge(payload, onRetry, opts) {
    if (!bannerEl) bannerEl = document.getElementById('cv-banner');
    if (edgeLayer) edgeLayer.innerHTML = '';
    if (nodeLayer) nodeLayer.innerHTML = '';
    if (opts && opts.rootUuid) {
      state.scene = {
        root: opts.rootUuid,
        nodes: [],
        edges: [],
        depth: payload && payload.depth != null ? payload.depth : 0,
      };
      state.pinnedNode = null;
      state.dragOffsets = Object.create(null);
      closeInspector();
      refreshEvalTarget();
    }
    if (!bannerEl) return;
    const count = payload && payload.node_count != null ? payload.node_count : 0;
    const cap   = payload && payload.cap != null ? payload.cap : 0;
    const depth = payload && payload.depth != null ? payload.depth : null;
    const d1    = payload && payload.depth_1_size != null ? payload.depth_1_size : null;
    // Offer "Render at depth 1" only when the user is at depth > 1 AND
    // the depth-1 view (root + direct children) actually fits under
    // the cap. Wide-bound circuits (e.g. an aggregation root with
    // thousands of children) leave d1 > cap, so the button vanishes
    // rather than promising a render that would 413 again.
    const offerD1 = depth != null && depth > 1 && d1 != null && d1 <= cap;

    let html = '<div class="cv-banner__title">Circuit too large to render</div>';
    html += '<p class="cv-banner__body">This subgraph has <strong>'
         +  count.toLocaleString() + '</strong> nodes; the cap is <strong>'
         +  cap.toLocaleString() + '</strong>';
    if (depth != null) {
      html += ' (rendering at depth <strong>' + depth + '</strong>)';
    }
    html += '.</p>';
    if (offerD1) {
      html += '<div class="cv-banner__actions">'
           +  '<button type="button" class="cv-banner__btn" id="cv-banner-retry">'
           +  'Render at depth 1, then expand interactively</button></div>';
    }
    bannerEl.innerHTML = html;
    bannerEl.hidden = false;

    if (offerD1 && typeof onRetry === 'function') {
      const btn = document.getElementById('cv-banner-retry');
      if (btn) btn.addEventListener('click', () => onRetry(1), { once: true });
    }
    setStatus('Provenance Circuit', 'Circuit too large.');
  }

  function renderCircuit(scene, opts) {
    hideBanner();
    state.scene = scene;
    state.pinnedNode = null;
    state.rowProv = (opts && opts.rowProv) ? String(opts.rowProv) : null;
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

    // Some labels carry payload (rv distribution params, value
    // scalars like "1e9", agg function names) wider than the
    // circle's default font-size can fit.  Measure each label after
    // attach and shrink its font until it fits the usable diameter
    // (~40px for r=22 minus stroke + 1px padding on each side); the
    // 5.5px floor keeps glyphs legible.  Inline style on the label
    // beats any cascading rule from app.css because SVG presentation
    // attributes lose to CSS, but the inline style itself wins.
    nodeLayer.querySelectorAll('.node-label').forEach(el => {
      const maxW = 40;
      const bb = el.getBBox();
      if (bb.width > maxW) {
        const cur = parseFloat(getComputedStyle(el).fontSize) || 11;
        const scaled = Math.max(5.5, cur * maxW / bb.width);
        el.style.fontSize = scaled.toFixed(2) + 'px';
      }
    });

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
    /* Anchor the viewBox on the BBOX CENTRE rather than on minX/minY +
     * a clamped width.  For multi-node scenes the two coincide (the
     * bbox is its own centroid); for single-node scenes the clamped
     * dimensions used to extend the box right + down from the node,
     * dragging the node into the top-left quadrant of the canvas
     * instead of sitting in the middle. */
    const bbW = Math.max(maxX - minX, 200);
    const bbH = Math.max(maxY - minY, 150);
    const cx = (minX + maxX) / 2 + state.pan.x;
    const cy = (minY + maxY) / 2 + state.pan.y;

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
    // When the same parent → child pair appears more than once (gate_arith
    // with both args pointing at the same RV, gate_times with duplicate
    // factors, ...) the straight-line geometry would stack the curves
    // on top of each other so the user sees a single edge.  Count
    // parallel edges per (from, to) and bow each one sideways so all
    // of them stay visible and the child_pos label has a distinct slot.
    const parallelTotal = {};
    for (const e of state.scene.edges) {
      const k = `${e.from}|${e.to}`;
      parallelTotal[k] = (parallelTotal[k] || 0) + 1;
    }
    const parallelSeen = {};
    for (const e of state.scene.edges) {
      const from = nodesById[e.from], to = nodesById[e.to];
      if (!from || !to) continue;
      const fp = nodePos(from), tp = nodePos(to);
      const k = `${e.from}|${e.to}`;
      const total = parallelTotal[k];
      parallelSeen[k] = (parallelSeen[k] || 0) + 1;
      const idx = parallelSeen[k] - 1;
      // Symmetric horizontal bow: single edge stays straight; for n>1
      // edges we space them at ±18px steps centred on the straight
      // line, so total spread scales with multiplicity.
      const bow = total > 1 ? (idx - (total - 1) / 2) * 18 : 0;
      const path = svgEl('path', {
        class: 'edge',
        d: `M ${fp.x} ${fp.y + 22} `
         + `C ${fp.x + bow} ${fp.y + 50}, `
         +   `${tp.x + bow} ${tp.y - 50}, `
         +   `${tp.x} ${tp.y - 22}`,
        'data-from': e.from, 'data-to': e.to,
      });
      edgeLayer.appendChild(path);

      // Position label at the edge midpoint with a small perpendicular
      // nudge so the digit sits next to the edge stroke rather than on
      // top of it.  Mid-edge instead of near-child because labelling at
      // the child end overlaps the arrowhead at typical node radii;
      // mid-edge clears both the arrow and the node circles regardless
      // of edge length.  The perpendicular sign rotates the unit vector
      // 90° clockwise so labels consistently land on the same side of
      // every edge.  Bow already shifts both the curve and the
      // midpoint, so labels track parallel curves automatically.
      if (shouldLabelChildren(from) && e.child_pos != null) {
        // Compute the label first; a labelMap function that returns
        // null/empty suppresses the edge label entirely (used by
        // the categorical-form mixture for its unordered mulinput
        // outcomes -- only the key wire gets a label there).
        const labelMap = EDGE_POS_LABEL[from.type];
        let label;
        if (typeof labelMap === 'function') {
          label = labelMap(from, e.child_pos);
        } else if (labelMap && labelMap[e.child_pos] != null) {
          label = labelMap[e.child_pos];
        } else {
          label = String(e.child_pos);
        }
        if (label != null && label !== '') {
          const dx = fp.x - tp.x;
          const dy = fp.y - tp.y;
          const len = Math.hypot(dx, dy) || 1;
          const perp = 9;
          const lx = (fp.x + tp.x) / 2 + bow + (-dy / len) * perp;
          const ly = (fp.y + tp.y) / 2 + ( dx / len) * perp;
          const tag = svgEl('text', {
            class: 'edge-pos',
            x: lx, y: ly,
            'text-anchor': 'middle', 'dominant-baseline': 'central',
          });
          tag.textContent = label;
          edgeLayer.appendChild(tag);
        }
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
      } else if (node.type === 'mulinput') {
        // The categorical-mixture form stores the outcome value in
        // extra; _gateInfos already exposes it as `value`, so the
        // generic `extra` fallback below would duplicate it.  Skip.
      } else if (node.type === 'rv') {
        // extra is "<kind>:<p1>[,<p2>]"; split for readability.
        const spec = parseDistributionSpec(node.extra);
        if (spec) {
          html += `<dt>distribution</dt><dd>${escapeHtml(spec.kind)}</dd>`;
          spec.params.forEach((p, i) => {
            // cv-inspector__dt--keep-case opts these rows out of the
            // inspector's default text-transform: uppercase so the
            // Greek-letter param names (μ, σ, λ) keep their proper
            // lowercase form.
            html += `<dt class="cv-inspector__dt--keep-case">`
                 + `${escapeHtml(spec.paramNames[i])}</dt>`
                 + `<dd>${escapeHtml(String(p))}</dd>`;
          });
        } else {
          html += `<dt>distribution</dt><dd>${escapeHtml(node.extra)}</dd>`;
        }
      } else {
        html += `<dt>extra</dt><dd>${escapeHtml(node.extra)}</dd>`;
      }
    }
    html += '</dl>';
    if (node.type === 'rv') {
      const density = renderRvDensity(parseDistributionSpec(node.extra));
      if (density) html += density;
    }
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
    // Probability is per-input-gate (the UUID itself), not per-resolved-row.
    // Append it to the gate-metadata <dl> as another <dt>/<dd> row so it
    // sits in the same visual stream as uuid / depth / info1, rather
    // than getting a separate paragraph that breaks the rhythm. The dd
    // is click-to-edit: clicking it swaps the displayed value for a
    // number input, Enter fires POST /api/set_prob, Esc / blur cancels.
    // We surface the probability BEFORE deciding on the source-row body
    // so anonymous Bernoullis -- gate_inputs created by
    // provsql.mixture(p_value, ...) or by hand via create_gate + set_prob
    // without a tracked source table -- still see their probability
    // even though no row in any tracked relation references the UUID.
    if (payload.probability != null) {
      const dl = inspectorBody.querySelector('dl');
      if (dl) {
        dl.insertAdjacentHTML(
          'beforeend',
          `<dt>probability</dt>`
          + `<dd class="cv-prob__editable" title="Click to edit"`
          + ` data-prob-uuid="${escapeHtml(uuid)}"`
          + ` data-prob-value="${escapeHtml(String(payload.probability))}">`
          + `${escapeHtml(formatProbabilityValue(payload.probability))}</dd>`,
        );
        const dd = dl.querySelector('dd[data-prob-uuid]');
        if (dd) dd.addEventListener('click', () => editProbability(dd));
      }
    }
    if (!matches.length) {
      replaceLeafBody(
        '<p style="color:var(--fg-muted)">'
        + (payload.probability != null
          ? 'Anonymous input gate: no source row maps to this UUID.'
          : 'No source row found.')
        + '</p>');
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

  // After a successful set_prob write, refresh the corresponding
  // node's in-circle label so the user sees the new value without
  // having to reload the circuit.  Only applies to ANONYMOUS gate_input
  // gates (no source row in any tracked relation: hand-minted
  // Bernoullis like provsql.mixture's p_token, the simplifier's
  // synthetic dec-in-N anchor for a categorical block, or `create_gate
  // + set_prob` in user code).  The server stamps `tracked_input` on
  // each node; we read that directly rather than re-deriving the
  // catalog lookup client-side.  Inputs tied to a tracked relation
  // keep their ι glyph regardless of probability -- ι reads as "this
  // is a variable" and the per-row probability stays one click into
  // the inspector away.  Mirrors circuit.py's _is_anonymous_input +
  // _format_prob_label exactly.
  function updateNodeProbLabel(uuid, p) {
    if (!state.scene || !Array.isArray(state.scene.nodes)) return;
    const node = state.scene.nodes.find(n => n.id === uuid);
    if (!node || node.type !== 'input') return;
    if (node.tracked_input) return;
    let newLabel;
    if (!Number.isFinite(p) || p === 1) {
      newLabel = 'ι';  // gate-input default glyph (no prob known)
    } else if (p === 0) {
      newLabel = '0%';
    } else {
      const pct = p * 100;
      if (pct > 0 && pct < 0.01) {
        newLabel = pct.toExponential(1) + '%';
      } else {
        let s = pct.toFixed(2).replace(/\.?0+$/, '');
        newLabel = s + '%';
      }
    }
    node.label = newLabel;
    const labelEl = document.querySelector(
      `.node-group[data-id="${uuid}"] .node-label`);
    if (labelEl) labelEl.textContent = newLabel;
  }

  function formatProbabilityValue(p) {
    const dec = (window.ProvsqlStudio && window.ProvsqlStudio.getProbDecimals)
      ? window.ProvsqlStudio.getProbDecimals()
      : 4;
    const n = Number(p);
    return Number.isFinite(n) ? n.toFixed(dec) : String(p);
  }

  // Click-to-edit on the inspector probability cell. Replaces the
  // rendered value with a number input; Enter fires POST /api/set_prob,
  // Esc and blur cancel without saving (blur-as-cancel avoids surprise
  // commits when the user clicks elsewhere mid-thought).
  function editProbability(dd) {
    const uuid = dd.dataset.probUuid;
    const current = dd.dataset.probValue;
    if (!uuid) return;
    const cur = Number(current);
    const initial = Number.isFinite(cur) ? cur : 1.0;
    dd.innerHTML =
      `<input class="cv-prob__input" type="number" min="0" max="1" `
      + `step="0.0001" value="${escapeHtml(String(initial))}">`
      + `<span class="cv-prob__msg" hidden></span>`;
    const input = dd.querySelector('input');
    const msg = dd.querySelector('.cv-prob__msg');
    input.focus();
    input.select();
    let saved = false;

    function showMsg(text, isError) {
      if (!msg) return;
      msg.textContent = text;
      msg.hidden = false;
      msg.classList.toggle('is-error', !!isError);
    }
    function restore(value) {
      const v = value != null ? value : initial;
      dd.dataset.probValue = String(v);
      dd.innerHTML = escapeHtml(formatProbabilityValue(v));
    }
    async function save() {
      if (saved) return;
      const v = Number(input.value);
      if (!Number.isFinite(v) || v < 0 || v > 1) {
        input.classList.add('is-error');
        showMsg('must be 0..1', true);
        return;
      }
      saved = true;
      input.disabled = true;
      try {
        const resp = await fetch('/api/set_prob', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ uuid, probability: v }),
        });
        if (!resp.ok) {
          const err = await resp.json().catch(() => ({}));
          input.disabled = false;
          input.classList.add('is-error');
          showMsg(err.detail || err.error || `HTTP ${resp.status}`, true);
          saved = false;
          return;
        }
        restore(v);
        updateNodeProbLabel(uuid, v);
      } catch (e) {
        input.disabled = false;
        input.classList.add('is-error');
        showMsg(e.message || 'network error', true);
        saved = false;
      }
    }
    input.addEventListener('keydown', (e) => {
      if (e.key === 'Enter')   { e.preventDefault(); save(); }
      else if (e.key === 'Escape') { e.preventDefault(); restore(); }
    });
    input.addEventListener('blur', () => {
      // Avoid restoring while save() is in flight (the disabled input
      // briefly loses focus on some browsers when network mode swaps).
      if (!saved) restore();
    });
  }

  // ─── expansion ────────────────────────────────────────────────────────

  async function expandFrontier(node, additionalDepth) {
    const root = state.scene && state.scene.root;
    if (!root) return;
    const depth = Number.isFinite(additionalDepth) ? additionalDepth : state.scene.depth;
    setStatus(null, `Expanding ${shortUuid(node.id)}…`);
    let resp;
    try {
      resp = await fetch(`/api/circuit/${encodeURIComponent(root)}/expand`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ frontier_node_uuid: node.id, additional_depth: depth }),
      });
    } catch (e) {
      showError(`Network error: ${e.message}`);
      return;
    }
    if (!resp.ok) {
      const err = await resp.json().catch(() => ({}));
      // Same actionable banner as loadCircuit's 413 path: when the
      // anchor's subgraph is too large at the requested depth, offer a
      // depth-1 retry if the depth-1 frontier fits under the cap.
      if (resp.status === 413 && err && err.error === 'circuit too large') {
        showTooLarge(err, (lowerDepth) => expandFrontier(node, lowerDepth));
        return;
      }
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
    // Depth rebase: the sub-DAG's depths are relative to the frontier
    // (sub.root is at depth 0), but state.scene's depths are relative
    // to the original root. The inspector reads node.depth, so without
    // the offset, expanded nodes report wrong depths. Anchor.depth in
    // state.scene is the absolute depth of the frontier; new nodes
    // sit `anchor.depth + n.depth` levels under the original root.
    const ddepth = (anchor.depth != null ? anchor.depth : 0)
                 - (subRoot && subRoot.depth != null ? subRoot.depth : 0);

    const known = new Set(state.scene.nodes.map(n => n.id));
    for (const n of sub.nodes) {
      if (known.has(n.id)) continue;
      state.scene.nodes.push({
        ...n,
        x: n.x + dx,
        y: n.y + dy,
        depth: (n.depth != null ? n.depth + ddepth : n.depth),
      });
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

  // Single source of truth for compiled semirings exposed in the eval
  // strip. Each entry drives the dropdown population (label + group),
  // the mapping-required check, the type-compatibility filter on the
  // mapping picker (matched against `mapping.value_base_type`), the
  // PG-version gate, and the optional hint shown next to the mapping
  // dropdown. Adding a new compiled semiring is a one-line registry
  // entry plus a matching backend dispatch.
  //
  // `types: null` means polymorphic (no filter, no expectation message).
  // `types: [...]` filters the mapping picker; an empty result yields a
  // "no compatible mappings" sentinel so the user can't run the option
  // against a mapping the kernel won't accept.
  // `acceptsEnum: true` filters the mapping picker to user-defined enum
  // carriers (matched against `mapping.is_enum`), used by sr_minmax /
  // sr_maxmin where the carrier is "any user-defined enum".
  const _NUMERIC_BASE_TYPES = [
    'smallint', 'integer', 'bigint',
    'numeric', 'real', 'double precision',
  ];
  const _INTERVAL_BASE_TYPES = [
    'tstzmultirange', 'nummultirange', 'int4multirange',
  ];
  const _COMPILED_REGISTRY = {
    // Boolean.
    'boolexpr':    { label: 'Boolean expression',        group: 'bool',
                     needsMapping: false, types: null,                hint: null,
                     optionalMapping: true },
    'boolean':     { label: 'Boolean',                    group: 'bool',
                     needsMapping: true,  types: ['boolean'],         hint: 'Expects boolean values.' },
    // Lineage. `formula` is the canonical free-polynomial expression
    // (Green-Karvounarakis-Tannen) as a circuit pretty-print; `how` is
    // the same algebra collapsed to canonical sum-of-products form,
    // making it suitable for provenance-equivalence checks across
    // syntactically different circuits.
    'formula':     { label: 'Formula',                    group: 'lin',
                     needsMapping: true,  types: null,                hint: null },
    'how':         { label: 'How-provenance',             group: 'lin',
                     needsMapping: true,  types: null,
                     hint: 'Canonical N[X] polynomial; equal circuits collapse to the same string.' },
    'why':         { label: 'Why-provenance',             group: 'lin',
                     needsMapping: true,  types: null,                hint: null },
    'which':       { label: 'Which-provenance',          group: 'lin',
                     needsMapping: true,  types: null,                hint: null },
    // Numeric / scoring. The [0, 1] constraint for Viterbi / Łukasiewicz
    // can't be enforced at type level (no PG type for "numeric in [0, 1]")
    // so the hint flags it; the kernel itself doesn't reject out-of-range
    // values, it just yields nonsense.
    'counting':    { label: 'Counting',                   group: 'num',
                     needsMapping: true,  types: _NUMERIC_BASE_TYPES, hint: 'Expects numeric values.' },
    'tropical':    { label: 'Tropical (min-plus)',        group: 'num',
                     needsMapping: true,  types: _NUMERIC_BASE_TYPES, hint: 'Expects numeric (cost) values.' },
    'viterbi':     { label: 'Viterbi (max-times)',        group: 'num',
                     needsMapping: true,  types: _NUMERIC_BASE_TYPES, hint: 'Expects numeric values in [0, 1].' },
    'lukasiewicz': { label: 'Łukasiewicz (numeric fuzzy)', group: 'num',
                     needsMapping: true,  types: _NUMERIC_BASE_TYPES, hint: 'Expects numeric values in [0, 1].' },
    // Intervals. One UI option, three kernels: the backend picks
    // sr_temporal / sr_interval_num / sr_interval_int from the mapping's
    // multirange type. PG14+ because every multirange type was added in 14.
    'interval-union': { label: 'Interval union (multirange)', group: 'iv',
                        needsMapping: true,  types: _INTERVAL_BASE_TYPES,
                        minPg: 140000,
                        hint: 'Multirange-valued (PostgreSQL 14+); selects sr_temporal / sr_interval_num / sr_interval_int by mapping type.' },
    // User-enum carrier. The bottom and top come from the enum's
    // pg_enum.enumsortorder; the kernel is polymorphic over any
    // user-defined enum, so the picker filters by `is_enum` rather
    // than a fixed type list.
    'minmax':      { label: 'Min-max (security shape)',   group: 'enum',
                     needsMapping: true,  types: null,    acceptsEnum: true,
                     hint: 'Expects a user-defined enum carrier; alternatives combine to enum-min, joins to enum-max.' },
    'maxmin':      { label: 'Max-min (enum fuzzy / trust)', group: 'enum',
                     needsMapping: true,  types: null,    acceptsEnum: true,
                     hint: 'Expects a user-defined enum carrier; alternatives combine to enum-max, joins to enum-min.' },
  };
  const _COMPILED_GROUPS = [
    ['bool', 'Boolean'],
    ['lin',  'Lineage'],
    ['num',  'Numeric'],
    ['iv',   'Intervals'],
    ['enum', 'User-enum'],
  ];

  // Custom-semiring options (encoded as `custom:<schema>.<name>`) also need a
  // mapping; see `needsMapping`. `prov-xml` and `boolexpr` accept an
  // optional mapping (used to label leaves) so the dropdown shows for
  // them too, but emptying the selection is allowed : see
  // `_OPTIONAL_MAPPING`.
  const _OPTIONAL_MAPPING = new Set(['prov-xml', 'boolexpr']);
  function needsMapping(v) {
    if (v.startsWith('custom:')) return true;
    if (_OPTIONAL_MAPPING.has(v)) return true;
    const spec = _COMPILED_REGISTRY[v];
    return !!(spec && spec.needsMapping);
  }
  function mappingOptional(v) {
    return _OPTIONAL_MAPPING.has(v);
  }

  // Gate types whose value is a *scalar* (deterministic constant, random
  // variable leaf, or arithmetic combination of either) rather than a
  // Boolean / set-valued provenance gate.  Used by the eval strip to flip
  // the dropdown between the Boolean-method menu and the scalar-method
  // menu (distribution profile, PROV-XML).  Picked by inspecting the
  // eval target's gate type via state.scene.nodes.
  const _SCALAR_GATE_TYPES = new Set(['rv', 'arith', 'value', 'mixture']);
  // Options visible when the eval target is scalar.  Everything else in
  // the <select> gets hidden + disabled for the duration of the scalar
  // focus.  PROV-XML stays because it accepts any provenance gate.
  const _SCALAR_TARGET_OPTIONS = new Set([
    'distribution-profile', 'moment', 'sample', 'prov-xml'
  ]);
  // Options visible only on scalar targets.  Hidden + disabled when the
  // eval target is Boolean / aggregate / etc., so the user can't pick a
  // semiring whose SQL kernel doesn't accept this gate type.
  const _SCALAR_ONLY_OPTIONS = new Set([
    'distribution-profile', 'moment', 'sample'
  ]);
  // Semirings that accept an optional "Condition on" gate UUID,
  // routed to provsql.rv_moment / rv_support / rv_histogram /
  // rv_sample as the `prov` argument so the result is the conditional
  // (truncated) distribution rather than the unconditional one.
  const _CONDITION_OPTIONS = new Set([
    'distribution-profile', 'moment', 'sample'
  ]);

  // Lookup the gate type of the current eval target (pinned node, else
  // the scene root) by walking state.scene.nodes.  Returns null when
  // the scene isn't loaded yet or the target is outside the rendered
  // subgraph (which can happen for a circuit that was expanded then
  // re-rooted; the strip stays runnable but the filter falls back to
  // showing every option so the user isn't locked out).
  function currentTargetGateType() {
    if (!state.scene || !state.scene.nodes) return null;
    const targetId = state.pinnedNode || state.scene.root;
    if (!targetId) return null;
    const node = state.scene.nodes.find(n => n.id === targetId);
    return node ? node.type : null;
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

  // Build the "Compiled Semirings" sub-optgroups from the registry and
  // splice them into the <select> ahead of "Custom Semirings" / "Other".
  // Idempotent: calling twice is a no-op (the anchor div is checked).
  // The registry is the single source of truth, so future compiled
  // semirings appear without touching the static HTML.
  function populateCompiledOptgroups() {
    const sel = document.getElementById('eval-semiring');
    if (!sel || sel.dataset.compiledPopulated === '1') return;
    const fragment = document.createDocumentFragment();
    for (const [groupId, groupLabel] of _COMPILED_GROUPS) {
      const og = document.createElement('optgroup');
      og.label = groupLabel;
      og.dataset.compiledGroup = groupId;
      let added = 0;
      for (const [val, spec] of Object.entries(_COMPILED_REGISTRY)) {
        if (spec.group !== groupId) continue;
        const opt = document.createElement('option');
        opt.value = val;
        opt.textContent = spec.label;
        og.appendChild(opt);
        added++;
      }
      if (added) fragment.appendChild(og);
    }
    // Inject ahead of the static "Custom Semirings" / "Other" optgroups
    // already present in the markup.
    sel.insertBefore(fragment, sel.firstChild);
    // Default the selection to the first compiled option so the strip
    // is in a runnable state on first paint, matching the old behaviour
    // where `boolexpr` was the first <option> in the select.
    if (!sel.value) sel.value = 'boolexpr';
    sel.dataset.compiledPopulated = '1';
  }

  // Compiled-semiring availability is gated by the server's PG version
  // via the `minPg` field on the registry. Read at sync time, after
  // `/api/conn` has populated `window.ProvsqlStudio.serverVersion`.
  function syncCompiledSemiringAvailability() {
    const sel = document.getElementById('eval-semiring');
    if (!sel) return;
    populateCompiledOptgroups();
    const sv = Number(window.ProvsqlStudio?.serverVersion) || 0;
    for (const [val, spec] of Object.entries(_COMPILED_REGISTRY)) {
      if (!spec.minPg) continue;
      const opt = sel.querySelector(`option[value="${val}"]`);
      if (!opt) continue;
      // 0 means "not yet known" : keep the option visible until we hear
      // back from /api/conn so the strip is usable on first paint. Once
      // the version arrives it'll be gated correctly on the next sync.
      const supported = !sv || sv >= spec.minPg;
      opt.hidden = !supported;
      opt.disabled = !supported;
      // Persist the gate so syncDropdownVisibility's later passes keep
      // unsupported options hidden even when the target-type filter
      // would otherwise reveal them.
      if (supported) delete opt.dataset.pgGated;
      else opt.dataset.pgGated = '1';
      // If the user had it selected on a stale page, fall back to the
      // first compiled semiring so the strip stays valid.
      if (!supported && sel.value === val) {
        sel.value = 'boolexpr';
        sel.dispatchEvent(new Event('change'));
      }
    }
  }
  // Expose to app.js so it can re-sync after /api/conn lands.
  window.ProvsqlStudio = window.ProvsqlStudio || {};
  window.ProvsqlStudio.syncCompiledSemiringAvailability = syncCompiledSemiringAvailability;

  function initEvalStrip() {
    const sel    = document.getElementById('eval-semiring');
    const map    = document.getElementById('eval-mapping');
    const meth   = document.getElementById('eval-method');
    const run    = document.getElementById('eval-run');
    const result = document.getElementById('eval-result');
    if (!sel || !map || !meth || !run) return;
    // Start hidden: the strip only makes sense once a circuit is
    // rendered.  refreshEvalTarget toggles it visible/hidden on every
    // scene change.
    const strip = document.getElementById('eval-strip');
    if (strip) strip.hidden = true;
    syncCompiledSemiringAvailability();

    // Includes the per-probability-method controls, the bins input for
    // distribution-profile, and the (k, central) pair for moment.
    // syncControls hides every unrelated input in one sweep, then
    // unhides whichever the current semiring needs.
    const argControls = [
      ...Object.values(_PROB_ARG_CONTROL),
      'eval-args-bins',
      'eval-args-moment-k',
      'eval-args-moment-central',
      'eval-args-sample-n',
      // The "Condition on" control is the badge + input wrapped in a
      // <span>; toggle the wrapper so the badge hides too when no
      // semiring needs the condition arg.
      'eval-args-condition-group',
    ]
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

    // Hide every <option> the current eval target can't drive, and pick
    // a sensible fallback if the user's existing selection just got
    // hidden.  Returns true iff the active sel.value was bumped to a
    // different option, so callers can decide whether to re-run the
    // full syncControls (which also wipes the result chip).
    function syncDropdownVisibility() {
      const gateType = currentTargetGateType();
      // null = scene not loaded yet (or target outside the rendered
      // subgraph); leave every option visible so the strip is usable.
      // Otherwise, scalar gates get the scalar menu, all other gates
      // get the Boolean menu.
      const isScalar = gateType != null && _SCALAR_GATE_TYPES.has(gateType);
      let firstVisible = null;
      for (const opt of sel.querySelectorAll('option')) {
        let hide;
        if (gateType == null) {
          // Indeterminate: hide nothing target-specific; respect the
          // existing PG-version gate on compiled options.
          hide = false;
        } else if (isScalar) {
          hide = !_SCALAR_TARGET_OPTIONS.has(opt.value);
        } else {
          hide = _SCALAR_ONLY_OPTIONS.has(opt.value);
        }
        // PG-version gating on compiled semirings (set by
        // syncCompiledSemiringAvailability) wins: if the option was
        // already hidden as unsupported, leave it hidden.  Track that
        // via `data-pg-gated` so we don't accidentally re-enable it.
        if (opt.dataset.pgGated === '1') opt.hidden = true;
        else opt.hidden = hide;
        opt.disabled = opt.hidden;
        if (!opt.hidden && !firstVisible) firstVisible = opt.value;
      }
      // Collapse optgroups whose options are all hidden so the dropdown
      // reads cleanly (a `<optgroup>` with zero visible options still
      // shows its label in some browsers).
      for (const og of sel.querySelectorAll('optgroup')) {
        const anyVisible = [...og.querySelectorAll('option')]
          .some(o => !o.hidden);
        og.hidden = !anyVisible;
        og.disabled = !anyVisible;
      }
      // If the active selection just got hidden, fall back to the first
      // still-visible option so the run button stays meaningful.
      const cur = sel.querySelector(`option[value="${CSS.escape(sel.value)}"]`);
      if (firstVisible && (!cur || cur.hidden)) {
        sel.value = firstVisible;
        return true;
      }
      return false;
    }

    // Wrapper called by refreshEvalTarget on pin change: re-filter the
    // dropdown, and if the value actually moved (e.g. the user pinned a
    // gate_rv after running Boolean over the root), re-run syncControls
    // so the mapping / method / args dispatch matches the new sel.value.
    // The result chip is intentionally wiped in that case because the
    // previous evaluation was against a different semiring kind and is
    // no longer meaningful.  When the value stays put, the chip is
    // preserved.
    function refilterForTarget() {
      const changed = syncDropdownVisibility();
      if (changed) syncControls();
    }
    window.ProvsqlStudio = window.ProvsqlStudio || {};
    window.ProvsqlStudio.refilterForTarget = refilterForTarget;

    function syncControls() {
      syncDropdownVisibility();
      const v = sel.value;
      map.hidden  = !needsMapping(v);
      meth.hidden = v !== 'probability';
      // Show only the args control(s) that match the current semiring,
      // hide every other one so the row stays compact.  Most semirings
      // need at most one control (the per-method picker for
      // probability, the bins input for distribution-profile); moment
      // is the exception, with both a k input and a raw/central
      // selector, so the dispatch returns a set rather than a single id.
      const wantedIds = new Set();
      if (v === 'probability') {
        const id = _PROB_ARG_CONTROL[meth.value];
        if (id) wantedIds.add(id);
      } else if (v === 'distribution-profile') {
        wantedIds.add('eval-args-bins');
      } else if (v === 'moment') {
        wantedIds.add('eval-args-moment-k');
        wantedIds.add('eval-args-moment-central');
      } else if (v === 'sample') {
        wantedIds.add('eval-args-sample-n');
      }
      if (_CONDITION_OPTIONS.has(v)) {
        wantedIds.add('eval-args-condition-group');
      }
      for (const ctrl of argControls) ctrl.hidden = !wantedIds.has(ctrl.id);
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

    // Set of value base types accepted for the mapping's `value` column
    // under the current semiring choice. Returns null when no filter
    // applies (polymorphic semirings: formula / why / which).
    //   * Custom semirings: exactly the wrapper's return type (the
    //     convention is `wrapper return type == mapping value type`,
    //     since the typed `zero`/`plus`/`times` inside provenance_evaluate
    //     pin the value column's type).
    //   * Compiled semirings: the registry's `types` list. Matched
    //     against `mapping.value_base_type` so a `numeric(10, 2)` mapping
    //     compares against `numeric` and is accepted by Counting.
    function expectedValueTypes() {
      const v = sel.value;
      if (v.startsWith('custom:')) {
        const qname = v.slice('custom:'.length);
        const c = _customs.find(x => x.qname === qname);
        return c && c.return_type ? [c.return_type] : null;
      }
      const spec = _COMPILED_REGISTRY[v];
      return spec && spec.types ? spec.types : null;
    }

    // True if the current semiring expects a user-defined enum carrier.
    // sr_minmax / sr_maxmin are polymorphic over any user enum, so the
    // picker filters by `mapping.is_enum` instead of a fixed type list.
    function expectsEnumCarrier() {
      const spec = _COMPILED_REGISTRY[sel.value];
      return !!(spec && spec.acceptsEnum);
    }

    // Render the mapping <option>s from the cached list, filtered by the
    // current semiring's expected value type set. Polymorphic semirings
    // (formula / why / which) get the full list; typed compiled and
    // custom semirings get only the type-compatible mappings, with a
    // clear empty-state if none match. The base-type comparison is
    // unparameterised (so, e.g., numeric(10, 2) matches the `numeric` slot).
    function renderMappingOptions() {
      const optional = mappingOptional(sel.value);
      if (!_mappings.length) {
        map.innerHTML = optional
          ? '<option value="">(no mapping : unlabeled tokens)</option>'
          : '<option value="">(no mappings : run create_provenance_mapping)</option>';
        map.disabled = !optional;
        return;
      }
      const expectedTypes = expectedValueTypes();
      const wantEnum = expectsEnumCarrier();
      let list;
      if (wantEnum) {
        list = _mappings.filter(m => m.is_enum);
      } else if (expectedTypes) {
        list = _mappings.filter(m => expectedTypes.includes(m.value_base_type));
      } else {
        list = _mappings;
      }
      if (!list.length) {
        const accepted = wantEnum
          ? 'a user-defined enum carrier'
          : (expectedTypes || []).join(', ');
        map.innerHTML =
          `<option value="">(no compatible mappings : expected ${escapeHtml(accepted)})</option>`;
        map.disabled = true;
        return;
      }
      map.disabled = false;
      const previousValue = map.value || '';
      // Prov-XML accepts an optional mapping : prepend a "(no mapping)"
      // sentinel so the user can explicitly export without leaf labels.
      const head = optional
        ? '<option value="">(no mapping : unlabeled tokens)</option>'
        : '';
      map.innerHTML = head + list.map(m => {
        const label = m.display_name || m.qname;
        const tagged = `${label} (${m.value_type})`;
        const title = `${m.qname} : value ${m.value_type}`;
        return `<option value="${escapeHtml(m.qname)}" title="${escapeHtml(title)}">${escapeHtml(tagged)}</option>`;
      }).join('');
      if (previousValue && [...map.options].some(o => o.value === previousValue)) {
        map.value = previousValue;
      }
    }

    // Hint text next to the mapping dropdown. For typed compiled
    // semirings the registry-defined hint flags constraints the type
    // filter can't express (e.g., "values in [0, 1]" for Viterbi /
    // Łukasiewicz, or the multirange-dispatch note for interval-union).
    // Custom semirings get a generic "Filtered to <T>" so the user
    // knows why the picker shrank.
    function updateMappingHint() {
      const hint = document.getElementById('eval-mapping-hint');
      if (!hint) return;
      if (map.hidden) {
        hint.hidden = true;
        hint.textContent = '';
        return;
      }
      const v = sel.value;
      let msg = null;
      if (v.startsWith('custom:')) {
        const expectedTypes = expectedValueTypes();
        if (expectedTypes && expectedTypes.length === 1) {
          msg = `Filtered to ${expectedTypes[0]}`;
        }
      } else {
        const spec = _COMPILED_REGISTRY[v];
        if (spec && spec.hint) msg = spec.hint;
      }
      if (msg) {
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
    // Enter inside a text / number argument field fires Run, matching
    // the form-submit convention.  Skip <select> controls (e.g. the
    // compilation method picker) where Enter natively confirms the
    // current option rather than committing the surrounding form.
    for (const ctrl of argControls) {
      if (ctrl.tagName !== 'INPUT') continue;
      ctrl.addEventListener('keydown', (ev) => {
        if (ev.key !== 'Enter') return;
        ev.preventDefault();
        if (!run.disabled) runEvaluation();
      });
    }
    // Drop the auto-preset marker as soon as the user types into the
    // "Condition on" input so a subsequent pin change within the same
    // row doesn't clobber their manual UUID.  Refresh the badge so
    // its styling flips to the muted/clickable variant (the chip stays
    // visible as a one-click "restore row prov" affordance).
    const condInput = document.getElementById('eval-args-condition');
    if (condInput) {
      condInput.addEventListener('input', () => {
        delete condInput.dataset.autoset;
        updateConditionInputBadge();
      });
    }
    // The "row prov" badge is a toggle:
    //   * active state  (value matches row prov) -> click clears the
    //     Condition input.  Useful for switching to the unconditional
    //     distribution without having to manually empty the field.
    //   * muted state   (value diverges) -> click restores the row
    //     prov.  One-click undo for a manual edit, or a re-apply
    //     after the user clicked Clear.
    const condBadge = document.getElementById('eval-args-condition-badge');
    if (condBadge) {
      condBadge.addEventListener('click', () => {
        const rowProv = state.rowProv || '';
        if (!rowProv) return;
        const cond = document.getElementById('eval-args-condition');
        if (!cond) return;
        const isActive =
          cond.dataset.autoset === '1' && cond.value.trim() === rowProv;
        if (isActive) {
          // Clear: the user wants the unconditional answer.
          cond.value = '';
          delete cond.dataset.autoset;
        } else {
          // Restore: the user pressed the muted chip after editing or
          // clearing the input.
          cond.value = rowProv;
          cond.dataset.autoset = '1';
          state.lastAutoPresetRow = rowProv;
        }
        updateConditionInputBadge();
      });
    }
    const clearBtn = document.getElementById('eval-clear');
    if (clearBtn) clearBtn.onclick = clearEvalResult;
    const copyBtn = document.getElementById('eval-copy');
    if (copyBtn) copyBtn.onclick = copyEvalResult;
    result.addEventListener('click', flipEvalResult);
    loadCustomSemirings();
    syncControls();
  }

  // Toggle the displayed precision of a probability result : the rounded
  // form is the default (driven by the Config-panel decimals setting);
  // clicking flips to full double-precision and back. Other kinds have
  // no second form, so the click is a no-op.
  function flipEvalResult() {
    const result = document.getElementById('eval-result');
    if (!result || result.dataset.flipKind !== 'prob') return;
    const expanded = result.dataset.expanded === '1';
    const next = expanded ? result.dataset.rounded : result.dataset.full;
    if (next == null) return;
    result.textContent = '= ' + next;
    result.dataset.expanded = expanded ? '' : '1';
    result.title = expanded ? 'Click to show full precision' : 'Click to show rounded value';
  }

  // Copy the just-evaluated value (or PROV-XML payload) to the clipboard.
  // The raw text is stashed on `eval-result.dataset.copy` at render time,
  // so this is independent of how the result is displayed (chip vs <pre>)
  // and skips the leading `= ` prefix the chip variant adds for legibility.
  async function copyEvalResult() {
    const result = document.getElementById('eval-result');
    const btn    = document.getElementById('eval-copy');
    const text   = result?.dataset.copy;
    if (!text || !btn) return;
    try {
      await navigator.clipboard.writeText(text);
    } catch {
      // Clipboard API blocked (insecure origin / permission) : fall back
      // to a hidden-textarea + execCommand round-trip so the action still
      // succeeds on http:// dev servers and older browsers.
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
    // Brief visual confirmation : swap the icon to a check for ~1s and
    // tint the button green via .is-copied (matches sphinx-copybutton's
    // success state, so the affordance reads the same in docs and app).
    const icon = btn.querySelector('i');
    const prev = icon ? icon.className : '';
    if (icon) icon.className = 'fas fa-check';
    btn.classList.add('is-copied');
    btn.disabled = true;
    setTimeout(() => {
      if (icon) icon.className = prev;
      btn.classList.remove('is-copied');
      btn.disabled = false;
    }, 1000);
  }

  // Wipe the result chip + bound + time. Useful in fullscreen where a
  // verbose Why / Formula output otherwise obscures the canvas with no
  // way to dismiss without re-running on a smaller circuit.
  function clearEvalResult() {
    const result = document.getElementById('eval-result');
    const bound  = document.getElementById('eval-bound');
    const time   = document.getElementById('eval-time');
    const clear  = document.getElementById('eval-clear');
    const copy   = document.getElementById('eval-copy');
    if (result) {
      result.textContent = '';
      delete result.dataset.kind;
      delete result.dataset.copy;
      delete result.dataset.full;
      delete result.dataset.rounded;
      delete result.dataset.flipKind;
      delete result.dataset.expanded;
      result.title = '';
    }
    if (bound)  bound.textContent  = '';
    if (time)   time.textContent   = '';
    if (clear)  clear.hidden = true;
    if (copy)   copy.hidden  = true;
  }

  function refreshEvalTarget() {
    const tgt   = document.getElementById('eval-target');
    const strip = document.getElementById('eval-strip');
    // Hide the whole strip when there is no scene (initial load,
    // post-error, post-clearScene).  Evaluating against nothing is
    // meaningless and the strip's controls take ~50px of vertical
    // space the user can reclaim for the SVG canvas.  The strip
    // re-appears on the next successful renderCircuit (which calls
    // back into refreshEvalTarget).
    if (strip) strip.hidden = !state.scene;
    // Re-run the semiring dropdown filter whenever the target changes
    // (pin / clear pin / scene reload): the new target may have a
    // different gate type, which flips the menu between the scalar
    // (distribution-profile + prov-xml) and the Boolean families.
    // refilterForTarget is hoisted via window.ProvsqlStudio because it
    // lives inside initEvalStrip's closure.  It re-applies the
    // scalar-vs-Boolean gate-type filter, and only re-runs syncControls
    // (which wipes the result chip) when the active selection was
    // actually bumped — so a pin change between two Boolean nodes
    // preserves the displayed evaluation.
    const refilter = window.ProvsqlStudio?.refilterForTarget;
    if (typeof refilter === 'function') refilter();
    autoPresetConditionInput();
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

  // Auto-preset the "Condition on" UUID to the row's provenance gate,
  // so `Moment` / `Distribution profile` / `Sample` evaluated against
  // any scalar inside the row's circuit yields a conditional answer
  // (truncated by the cmps the planner hook lifted out of WHERE)
  // without the user having to paste the row's provsql UUID.
  //
  // Source of truth: state.rowProv, the row's `__prov` (or
  // user-selected `provsql`) UUID, stashed by renderCircuit from the
  // data-row-prov attribute the result-table renderer stamps on every
  // clickable cell.  Falls back to scene.root for the legacy
  // "click the provsql cell" path (scene root === row's prov).
  //
  // Row-context change (clicking a different row's cell) ALWAYS
  // overwrites, even prior manual edits: a UUID pasted for row A
  // doesn't generalise to row B.  Within a single row, manual edits
  // stick (the `input` listener drops dataset.autoset; we don't
  // re-overwrite while lastAutoPresetRow matches the current row).
  function autoPresetConditionInput() {
    const cond = document.getElementById('eval-args-condition');
    if (!cond) return;
    if (!state.scene) {
      // Scene gone; drop any preset and reset row tracking.
      cond.value = '';
      delete cond.dataset.autoset;
      state.lastAutoPresetRow = null;
      updateConditionInputBadge();
      return;
    }
    // Row context: prefer the row prov stashed at scene load.  When
    // missing (legacy click path with no data-row-prov attribute, or
    // a direct loadCircuit call) fall back to scene.root iff a scalar
    // gate is pinned distinctly from the root; conditioning the
    // root on itself is meaningless.
    let target = state.rowProv;
    if (!target) {
      const pinned = state.pinnedNode;
      const root = state.scene.root;
      const node = pinned ? state.scene.nodes.find(n => n.id === pinned) : null;
      const isScalarPinned =
        node != null && _SCALAR_GATE_TYPES.has(node.type) && pinned !== root;
      if (isScalarPinned) target = root;
    }
    if (!target) {
      // No row context and no scalar pin: drop any prior auto value.
      if (cond.dataset.autoset === '1') {
        cond.value = '';
        delete cond.dataset.autoset;
      }
      state.lastAutoPresetRow = null;
      updateConditionInputBadge();
      return;
    }
    // Same row as the previous auto-preset: leave the value alone
    // (the user may have typed a manual override, which we want to
    // honour across pin changes within the same scene).
    if (state.lastAutoPresetRow === target) {
      updateConditionInputBadge();
      return;
    }
    // Row context changed: overwrite, regardless of prior autoset
    // marker.  Manual UUIDs pasted for the previous row would
    // silently condition the new row on the wrong event.
    cond.value = target;
    cond.dataset.autoset = '1';
    state.lastAutoPresetRow = target;
    updateConditionInputBadge();
  }

  // Visual cue that the "Condition on" input was auto-filled from the
  // row's provenance, with three states:
  //
  //   * no row context:           badge hidden (we don't have a row
  //                               prov to offer in the first place).
  //   * value matches row prov:   badge shown, "active" styling
  //                               (purple chip + tint on the input);
  //                               not clickable since clicking would
  //                               be a no-op.
  //   * value diverges from prov: badge shown, "muted" styling
  //                               (faded chip, no input tint); the
  //                               chip becomes a clickable button
  //                               that restores the row prov so the
  //                               user can revert a manual edit
  //                               without having to navigate away
  //                               and back.
  //
  // Called from autoPresetConditionInput on every refreshEvalTarget,
  // and from the input's `input` listener on every keystroke.
  function updateConditionInputBadge() {
    const cond = document.getElementById('eval-args-condition');
    if (!cond) return;
    const badge = document.getElementById('eval-args-condition-badge');
    const rowProv = state.rowProv || '';
    if (!badge) return;
    if (!rowProv) {
      // No row context: nothing to offer, hide the chip.
      badge.hidden = true;
      badge.classList.remove('cv-eval__cond-badge--muted');
      cond.classList.remove('is-auto-conditioned');
      return;
    }
    const matches =
      cond.dataset.autoset === '1' &&
      cond.value.trim() === rowProv;
    badge.hidden = false;
    badge.classList.toggle('cv-eval__cond-badge--muted', !matches);
    cond.classList.toggle('is-auto-conditioned', matches);
    // Tooltip updates so the affordance reads correctly in both
    // states (the badge is a toggle: active -> clear, muted -> restore).
    badge.title = matches
      ? 'Click to clear the conditioning (unconditional result).'
      : 'Click to restore the row provenance (replaces the current value).';
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
      if (!m && !mappingOptional(selValue)) {
        result.textContent = 'pick a provenance mapping';
        result.dataset.kind = 'error';
        return;
      }
      if (m) body.mapping = m;
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
    } else if (semiring === 'distribution-profile') {
      // The backend reads `arguments` as the histogram bin count.
      const bins = (document.getElementById('eval-args-bins')?.value || '').trim();
      if (bins) body.arguments = bins;
    } else if (semiring === 'moment') {
      // The backend reads `arguments` as `"k;central"` where k is a
      // non-negative integer and central is `raw` or `central`.
      const k = (document.getElementById('eval-args-moment-k')?.value || '').trim();
      const c = (document.getElementById('eval-args-moment-central')?.value || 'false').trim();
      body.arguments = `${k};${c === 'true' ? 'central' : 'raw'}`;
    } else if (semiring === 'sample') {
      const n = (document.getElementById('eval-args-sample-n')?.value || '').trim();
      if (n) body.arguments = n;
    }
    // Conditioning gate UUID, optional, accepted by every scalar
    // semiring above.  Splices into the rv_moment / rv_support /
    // rv_histogram / rv_sample `prov` arg server-side.
    if (_CONDITION_OPTIONS.has(semiring)) {
      const cond = (document.getElementById('eval-args-condition')?.value || '').trim();
      if (cond) body.condition_uuid = cond;
    }

    // Firefox: disabling a focused button moves focus to the document
    // body, which scrolls the viewport back to the top so the body's
    // "default focus position" comes into view.  Blur first so the
    // button loses focus before becoming disabled and Firefox has no
    // active focus to reassign.  Chrome doesn't exhibit this.
    if (typeof run.blur === 'function') run.blur();
    // Belt-and-braces: pin the scroll position across the run.  Even
    // with the blur above, Firefox occasionally scrolls the viewport
    // when the result panel grows tall (distribution-profile SVG,
    // sample list), presumably a focus / scroll-anchoring quirk we
    // can't isolate further.  Capture scrollY now, restore it once
    // after the synchronous DOM mutations and once on the next
    // animation frame to cover async layout passes.  Skip the
    // restore if the user themselves scrolled in the meantime
    // (heuristic: their final scrollY differs from ours by more than
    // a few px), so we don't fight a deliberate viewport change.
    const _scrollY0 = window.scrollY;
    const _restoreScroll = () => {
      const dy = Math.abs(window.scrollY - _scrollY0);
      if (dy > 4) window.scrollTo(window.scrollX, _scrollY0);
    };
    run.disabled = true;
    result.textContent = 'evaluating…';
    result.dataset.kind = 'pending';
    // Drop the previous run's copy + flip state so the copy button doesn't
    // stay armed and the click-to-flip handler doesn't reach back to a
    // stale value if this run errors before producing a fresh payload.
    delete result.dataset.copy;
    delete result.dataset.full;
    delete result.dataset.rounded;
    delete result.dataset.flipKind;
    delete result.dataset.expanded;
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
        const msg = data.detail || data.error || `HTTP ${resp.status}`;
        // Render with the same icon + ProvSQL pill + crimson banner
        // treatment the result-table errors use, instead of a bare
        // terracotta chip.  Logic mirrors app.js's renderDiag (see
        // there for the long-message <details> branch); inlined here
        // because that function lives inside the runQuery closure
        // and isn't exposed at module-load time.
        result.innerHTML = renderEvalError(msg, data.sqlstate);
        result.dataset.kind = 'error';
        return;
      }
      // PROV-XML is a multi-line export, not a scalar : render it inside
      // a scrollable <pre> instead of the inline `= value` chip. Same
      // styling as multi-line text cells in the result table.
      if (data.kind === 'xml') {
        const xmlText = data.result == null ? '' : String(data.result);
        result.innerHTML =
          `<pre class="wp-cell-pre">${escapeHtml(xmlText)}</pre>`;
        result.dataset.kind = 'xml';
        result.dataset.copy = xmlText;
        result.title = 'PROV-XML export';
      } else if (data.kind === 'distribution-profile') {
        // Inline-SVG panel: support badge + μ / σ² labels + empirical
        // histogram of the sampled distribution.  The full JSON payload
        // goes into dataset.copy so the user can grab it for further
        // analysis with the existing copy button, and is also re-used
        // by the PDF / CDF toggle to re-render without a round-trip.
        result.innerHTML = renderProfilePanel(data.result, 'pdf');
        result.dataset.kind = 'distribution-profile';
        result.dataset.copy = JSON.stringify(data.result);
        result.title = 'Distribution profile';
        wireProfileInteractions(result, data.result);
      } else if (data.kind === 'sample') {
        // Sample renders as a <details> panel with three regions:
        //   * summary: "N samples [of M]" counter + a comma-separated
        //              preview of the first PREVIEW_N values.
        //   * full:    the complete sample array (visible when the
        //              user opens the panel); monospace + wrapping
        //              so long n's stay readable.
        //   * hint:    shown only when MC's conditional acceptance
        //              rate truncated the run, pointing at the
        //              provsql.rv_mc_samples GUC.
        // The clipboard button (cv-eval__copy) is wired through
        // dataset.copy and copies the full JSON array.
        const r = data.result || {};
        const requested = r.n_requested ?? '';
        const samples = Array.isArray(r.samples) ? r.samples : [];
        const returned = r.n_returned ?? samples.length;
        const dec = (window.ProvsqlStudio && window.ProvsqlStudio.getProbDecimals)
          ? window.ProvsqlStudio.getProbDecimals() : 4;
        const fmt = v => Number.isFinite(v)
          ? Number(v.toFixed(Math.min(dec, 4))).toString()
          : String(v);
        const PREVIEW_N = 6;
        const previewVals = samples.slice(0, PREVIEW_N).map(fmt).join(', ');
        const moreCount = Math.max(0, samples.length - PREVIEW_N);
        const preview = previewVals
          ? `[${previewVals}${moreCount > 0 ? ` … +${moreCount} more` : ''}]`
          : '[]';
        const countLabel = `${returned} sample${returned === 1 ? '' : 's'}`
          + (requested && requested !== returned ? ` of ${requested}` : '');
        const fullList = samples.length
          ? `[${samples.map(fmt).join(', ')}]`
          : '[]';
        const budgetHint = (requested && returned < requested)
          ? `<div class="cv-sample-hint" role="status">`
            + `MC accepted ${returned}/${requested}.  `
            + `Raise <code>provsql.rv_mc_samples</code> in the Config panel `
            + `to widen the rejection-sampling budget.`
            + `</div>`
          : '';
        result.innerHTML =
          `<details class="cv-sample-panel">`
          + `<summary class="cv-sample-summary">`
          +   `<span class="cv-sample-count">${escapeHtml(countLabel)}</span>`
          +   `<code class="cv-sample-preview" `
          +        `title="First ${PREVIEW_N} values; click the chevron `
          +        `to see all; clipboard icon copies the JSON array.">`
          +     escapeHtml(preview)
          +   `</code>`
          + `</summary>`
          + `<pre class="cv-sample-full">${escapeHtml(fullList)}</pre>`
          + budgetHint
          + `</details>`;
        result.dataset.kind = 'sample';
        result.dataset.copy = JSON.stringify(samples);
        result.title = '';   // panel + per-element titles carry the info
      } else {
      // Show the value verbatim. Probability gets clipped to the configured
      // decimal count (default 4) for readability; the full-precision form
      // stays available via dataset.full and the click-to-flip handler.
      // Everything else is already a string from the server cast or a
      // JSON-native scalar.
      let display;
      if (data.kind === 'float' && typeof data.result === 'number') {
        const dec = (window.ProvsqlStudio && window.ProvsqlStudio.getProbDecimals)
          ? window.ProvsqlStudio.getProbDecimals()
          : 4;
        const full = String(data.result);
        display = data.result.toFixed(dec);
        result.dataset.full = full;
        result.dataset.rounded = display;
        result.dataset.flipKind = 'prob';
        result.title = 'Click to show full precision';
      } else if (data.kind === 'custom') {
        display = formatCustomValue(data.result, data.type_name);
      } else if (data.result == null) {
        display = '(null)';
      } else {
        display = String(data.result);
      }
      result.textContent = '= ' + display;
      result.dataset.kind = 'ok';
      // Copy always carries the full-precision form for probabilities so
      // the user can paste an exact value regardless of how it's
      // displayed; for other kinds, copy and display match.
      result.dataset.copy = (data.kind === 'float' && typeof data.result === 'number')
        ? String(data.result)
        : display;
      if (data.kind !== 'float') {
        result.title = data.kind === 'custom'
          ? `${data.function} → ${data.type_name}`
          : `${data.kind} value`;
      }
      }
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
      // Show the copy affordance only when there's a successful payload
      // worth copying (errors and pending states have no useful text).
      const copy = document.getElementById('eval-copy');
      if (copy) copy.hidden = !result.dataset.copy;
      // Restore the scroll position captured before the run: once
      // synchronously and once on the next animation frame, since
      // Firefox's scroll-jump can fire either during the result
      // mutation or on the subsequent layout pass.
      _restoreScroll();
      requestAnimationFrame(_restoreScroll);
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
      // Two shapes share gate_mulinput:
      //  - repair_key-style: extra is empty, info1 carries the
      //    multivalued variable's ordinal within its block.
      //  - categorical-mixture: extra holds the outcome value
      //    (float8 text), and the ordinal is irrelevant -- the
      //    mulinputs of a categorical block are unordered.
      // Surface whichever payload the mulinput actually carries.
      if (node.extra) {
        out.push({ label: 'value', value: node.extra });
      } else if (node.info1 != null) {
        out.push({ label: 'ordinal', value: node.info1 });
      }
    } else if (t === 'input' || t === 'update') {
      // info1 = source relation id (already shown as `tbl X` under the
      // node), info2 = column count. Surface column count here so the
      // inspector adds something the canvas doesn't.
      if (node.info1 != null) out.push({ label: 'relation id', value: node.info1 });
      if (node.info2 != null) out.push({ label: 'columns',     value: node.info2 });
    } else if (t === 'arith') {
      // info1 = PROVSQL_ARITH_* tag (provsql.circuit_subgraph returns it
      // as TEXT so the value is a string here); map to a name+glyph so
      // the user doesn't have to remember enum order.
      const tag = node.info1 == null ? null : Number(node.info1);
      const name = Number.isFinite(tag) ? ARITH_OP_NAME[tag] : null;
      if (name) out.push({ label: 'operator', value: name });
      else if (node.info1 != null) out.push({ label: 'operator', value: node.info1 });
    } else {
      // No type-specific translation : fall back to raw fields when
      // the value is meaningfully set. Zero is the universal "unused"
      // sentinel for these slots (gate_rv, gate_value, gate_plus,
      // gate_times, ... all leave info1 / info2 as 0), so suppress
      // the row rather than dumping a noisy "info1: 0" for every
      // gate that doesn't use those slots.
      const i1 = node.info1, i2 = node.info2;
      const meaningful = v => v != null && v !== 0 && v !== '0' && v !== '';
      if (meaningful(i1)) out.push({ label: 'info1', value: i1 });
      if (meaningful(i2)) out.push({ label: 'info2', value: i2 });
    }
    return out;
  }

  // Parse the gate_rv `extra` text encoding into {kind, params, paramNames}.
  // Mirrors src/RandomVariable.cpp's parse_distribution_spec; returns null
  // on anything we don't recognise so callers fall back to the raw text.
  function parseDistributionSpec(s) {
    if (!s) return null;
    const m = String(s).match(/^\s*([a-zA-Z]+)\s*(?::(.*))?$/);
    if (!m) return null;
    const kind = m[1].toLowerCase();
    const params = (m[2] || '')
      .split(',').map(x => Number(x.trim())).filter(x => Number.isFinite(x));
    const meta = {
      normal:      { params: 2, names: ['μ', 'σ'] },
      uniform:     { params: 2, names: ['a', 'b'] },
      exponential: { params: 1, names: ['λ'] },
      erlang:      { params: 2, names: ['k', 'λ'] },
    }[kind];
    if (!meta || params.length < meta.params) return null;
    return { kind, params: params.slice(0, meta.params), paramNames: meta.names };
  }

  // Render the analytical PDF of a parsed distribution spec into a small
  // inline SVG. Returns an HTML string suitable for insertion into the
  // inspector body. Returns "" when the spec is unrecognised so callers
  // can simply skip the preview without a special case.
  function renderRvDensity(spec) {
    if (!spec) return '';
    const W = 240, H = 100, padX = 6, padY = 6;
    const samples = 120;
    let lo, hi, pdf;
    if (spec.kind === 'normal') {
      const [mu, sigma] = spec.params;
      if (!(sigma > 0)) return '';
      lo = mu - 4 * sigma; hi = mu + 4 * sigma;
      const k = 1 / (sigma * Math.sqrt(2 * Math.PI));
      pdf = x => k * Math.exp(-0.5 * ((x - mu) / sigma) ** 2);
    } else if (spec.kind === 'uniform') {
      const [a, b] = spec.params;
      if (!(b > a)) return '';
      const margin = 0.15 * (b - a);
      lo = a - margin; hi = b + margin;
      const h = 1 / (b - a);
      pdf = x => (x >= a && x <= b) ? h : 0;
    } else if (spec.kind === 'exponential') {
      const [lam] = spec.params;
      if (!(lam > 0)) return '';
      lo = 0; hi = 6 / lam;
      pdf = x => x < 0 ? 0 : lam * Math.exp(-lam * x);
    } else if (spec.kind === 'erlang') {
      const [k, lam] = spec.params;
      if (!(k >= 1) || !(lam > 0)) return '';
      lo = 0; hi = Math.max(2 * k / lam, 6 / lam);
      // (k-1)! via gamma; k is integer in practice but we tolerate float.
      let fact = 1;
      for (let i = 2; i < k; i++) fact *= i;
      const norm = Math.pow(lam, k) / fact;
      pdf = x => x < 0 ? 0 : norm * Math.pow(x, k - 1) * Math.exp(-lam * x);
    } else {
      return '';
    }
    const xs = [], ys = [];
    for (let i = 0; i <= samples; i++) {
      const x = lo + (hi - lo) * (i / samples);
      xs.push(x);
      ys.push(pdf(x));
    }
    const yMax = Math.max(...ys, 1e-12);
    const sx = x => padX + (W - 2 * padX) * (x - lo) / (hi - lo);
    const sy = y => (H - padY) - (H - 2 * padY) * (y / yMax);
    const pts = xs.map((x, i) => `${sx(x).toFixed(1)},${sy(ys[i]).toFixed(1)}`).join(' ');
    const polygon =
      `${sx(lo).toFixed(1)},${sy(0).toFixed(1)} `
      + pts + ' '
      + `${sx(hi).toFixed(1)},${sy(0).toFixed(1)}`;
    // X-axis ticks at lo / mid / hi. The mean is the natural reference
    // line for normal / exponential / erlang and the midpoint for
    // uniform; render it as a faint dashed vertical.
    let meanLine = '';
    let mean = null;
    if (spec.kind === 'normal')      mean = spec.params[0];
    else if (spec.kind === 'uniform')     mean = (spec.params[0] + spec.params[1]) / 2;
    else if (spec.kind === 'exponential') mean = 1 / spec.params[0];
    else if (spec.kind === 'erlang')      mean = spec.params[0] / spec.params[1];
    if (mean != null && mean >= lo && mean <= hi) {
      meanLine =
        `<line x1="${sx(mean).toFixed(1)}" y1="${padY}" `
        + `x2="${sx(mean).toFixed(1)}" y2="${(H - padY).toFixed(1)}" `
        + `stroke="var(--terracotta-500)" stroke-width="1" `
        + `stroke-dasharray="3 3" opacity="0.7" />`;
    }
    const tickFmt = v => {
      if (!Number.isFinite(v)) return '';
      if (Math.abs(v) >= 1000 || (Math.abs(v) > 0 && Math.abs(v) < 0.01)) return v.toExponential(1);
      return Number(v.toFixed(3)).toString();
    };
    return `<div class="cv-rv-density" aria-label="probability density">`
      + `<svg width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" role="img">`
      + `<polygon points="${polygon}" fill="rgba(112, 82, 184, 0.18)" `
      + `         stroke="var(--purple-500)" stroke-width="1.4" />`
      + meanLine
      + `<text class="cv-rv-tick" x="${padX}" y="${H - 1}">${escapeHtml(tickFmt(lo))}</text>`
      + `<text class="cv-rv-tick" x="${W / 2}" y="${H - 1}" text-anchor="middle">`
      + `${escapeHtml(tickFmt((lo + hi) / 2))}</text>`
      + `<text class="cv-rv-tick" x="${W - padX}" y="${H - 1}" text-anchor="end">`
      + `${escapeHtml(tickFmt(hi))}</text>`
      + `</svg></div>`;
  }

  // Inline-SVG profile panel for the `distribution-profile` evaluation
  // method.  Mirrors renderRvDensity's geometry / palette but draws an
  // empirical histogram from the backend's [{bin_lo, bin_hi, count}]
  // array, with a support badge (which can extend past the sampled
  // range when the underlying distribution has unbounded support) and
  // mean / variance labels.
  // Shared geometry for the distribution-profile SVG.  Lives at module
  // scope so wireProfileInteractions can convert wheel-cursor positions
  // into the same data-space the bars are drawn in without duplicating
  // the constants in two places.
  const PROFILE_W = 280, PROFILE_H = 160;
  const PROFILE_PAD_X = 10, PROFILE_PAD_TOP = 6, PROFILE_PAD_BOTTOM = 18;
  // Zoom factor per wheel notch (1.2× in, 1/1.2× out).  Min visible
  // span is 1% of the full range so the user can't zoom into nothing.
  const PROFILE_ZOOM_STEP = 1.2;
  const PROFILE_MIN_SPAN_FRAC = 0.01;

  function readProfileView(panel) {
    if (!panel) return null;
    const lo = Number(panel.dataset.viewLo);
    const hi = Number(panel.dataset.viewHi);
    return (Number.isFinite(lo) && Number.isFinite(hi) && hi > lo)
      ? { lo, hi } : null;
  }

  // Delegated handlers for the distribution-profile panel.  Wheel and
  // dblclick are registered on `document` in the CAPTURE phase so the
  // browser sees our preventDefault before any ancestor scroll
  // listener (or its own auto-scroll path) can claim the gesture --
  // the prior bubble-phase listener on the inner SVG occasionally let
  // wheel events leak through into a page scroll when innerHTML
  // teardown briefly orphaned the listener or the cursor straddled
  // the SVG bounding rect.  Click for the PDF/CDF toggle stays on
  // resultEl since it's a discrete interaction with no scroll-side
  // effects.  Profile payload is parked on `resultEl.__profile` so
  // subsequent calls can swap data without re-wiring.
  function wireProfileInteractions(resultEl, profile) {
    resultEl.__profile = profile;
    if (!wireProfileInteractions._docWired) {
      wireProfileInteractions._docWired = true;
      document.addEventListener('wheel', _onProfileWheel,
                                { passive: false, capture: true });
      document.addEventListener('dblclick', _onProfileDblClick,
                                { capture: true });
    }
    if (!resultEl.__profileWired) {
      resultEl.__profileWired = true;
      resultEl.addEventListener('click', (ev) => {
        if (!ev.target.closest('.cv-profile-toggle')) return;
        ev.stopPropagation();
        const panel = resultEl.querySelector('.cv-profile-panel');
        if (!panel) return;
        const cur = panel.dataset.profileMode || 'pdf';
        const next = cur === 'cdf' ? 'pdf' : 'cdf';
        const view = readProfileView(panel);
        resultEl.innerHTML = renderProfilePanel(
          resultEl.__profile, next, view);
      });
    }
  }

  function _onProfileWheel(ev) {
    // Catch wheel over the WHOLE distribution-profile panel, not just
    // the inner SVG, so brief layout shifts that put the cursor over
    // the meta row (μ / σ / toggle) during re-render don't let the
    // page scroll.  Zoom only applies when the cursor is actually
    // over the SVG -- but preventDefault is unconditional inside the
    // panel.
    if (!ev.target.closest) return;
    const panel = ev.target.closest('.cv-profile-panel');
    if (!panel) return;
    ev.preventDefault();
    const svg = ev.target.closest('.cv-profile-svg svg');
    if (!svg) return;  /* over meta row: prevent scroll, don't zoom */
    const resultEl = svg.closest('.cv-eval__result');
    if (!resultEl || !resultEl.__profile) return;
    const fullLo = Number(panel.dataset.fullLo);
    const fullHi = Number(panel.dataset.fullHi);
    if (!Number.isFinite(fullLo) || !Number.isFinite(fullHi)) return;
    const view = readProfileView(panel) || { lo: fullLo, hi: fullHi };
    // Cursor → viewBox X.  getBoundingClientRect returns the CSS
    // pixel box; scale to PROFILE_W so the data-x lookup is correct
    // under any CSS scaling of the SVG element.
    const r = svg.getBoundingClientRect();
    const cxSvg = ((ev.clientX - r.left) / r.width) * PROFILE_W;
    const cxClamped = Math.max(PROFILE_PAD_X,
                                Math.min(PROFILE_W - PROFILE_PAD_X, cxSvg));
    const t = (cxClamped - PROFILE_PAD_X) /
              (PROFILE_W - 2 * PROFILE_PAD_X);          /* 0..1 */
    const dataX = view.lo + (view.hi - view.lo) * t;
    const factor = ev.deltaY < 0
      ? 1 / PROFILE_ZOOM_STEP
      : PROFILE_ZOOM_STEP;
    const fullSpan = fullHi - fullLo;
    let newSpan = (view.hi - view.lo) * factor;
    newSpan = Math.min(newSpan, fullSpan);
    newSpan = Math.max(newSpan, fullSpan * PROFILE_MIN_SPAN_FRAC);
    let newLo = dataX - t * newSpan;
    let newHi = newLo + newSpan;
    if (newLo < fullLo) { newLo = fullLo; newHi = newLo + newSpan; }
    if (newHi > fullHi) { newHi = fullHi; newLo = newHi - newSpan; }
    const mode = panel.dataset.profileMode || 'pdf';
    resultEl.innerHTML = renderProfilePanel(
      resultEl.__profile, mode, { lo: newLo, hi: newHi });
  }

  function _onProfileDblClick(ev) {
    const svg = ev.target.closest && ev.target.closest('.cv-profile-svg svg');
    if (!svg) return;
    ev.preventDefault();
    const panel = svg.closest('.cv-profile-panel');
    const resultEl = svg.closest('.cv-eval__result');
    if (!panel || !resultEl || !resultEl.__profile) return;
    const mode = panel.dataset.profileMode || 'pdf';
    resultEl.innerHTML = renderProfilePanel(resultEl.__profile, mode);
  }

  function renderProfilePanel(profile, mode, view) {
    if (!profile) return '';
    const histogram = Array.isArray(profile.histogram) ? profile.histogram : [];
    const support  = Array.isArray(profile.support) ? profile.support : [null, null];
    const expected = Number(profile.expected);
    const variance = Number(profile.variance);
    const _mode = mode === 'cdf' ? 'cdf' : 'pdf';
    // Optional viewport for x-axis zoom.  When omitted (initial render
    // or after reset), the view spans the full histogram range.  The
    // viewport carries through every re-render -- toggle, zoom, pan --
    // so the user's chosen zoom level survives mode flips.
    const _view = (view && Number.isFinite(view.lo) && Number.isFinite(view.hi)
                   && view.hi > view.lo) ? view : null;
    // Stats (μ, σ, σ²) obey the panel's "Probability decimals" setting
    // for parity with the eval strip's plain-float branch.  Falls back
    // to 4 decimals when the helper isn't available (e.g. circuit.js
    // loaded standalone in a test harness).  The exponential-fallback
    // bounds stay fixed: very-small / very-large magnitudes always
    // round-trip through scientific notation regardless of the user's
    // chosen decimal count.
    const dec = (window.ProvsqlStudio && window.ProvsqlStudio.getProbDecimals)
                ? window.ProvsqlStudio.getProbDecimals() : 4;
    const fmt = v => {
      if (v == null || !Number.isFinite(Number(v))) return String(v);
      const n = Number(v);
      if (Math.abs(n) >= 1e6 || (Math.abs(n) > 0 && Math.abs(n) < 1e-3)) {
        return n.toExponential(Math.max(2, dec - 1));
      }
      return Number(n.toFixed(dec)).toString();
    };
    // Looser formatter for axis tick labels: bin edges of an MC
    // histogram are random-sampling artefacts (the leftmost and
    // rightmost draws), so showing four decimals is precision the data
    // does not carry.  Round to roughly three significant figures
    // based on magnitude: for |x| ~ 30 that gives one decimal (33.8),
    // for |x| ~ 0.3 that gives three (0.338), and very small / large
    // values fall back to scientific notation.
    const fmtTick = v => {
      if (v == null || !Number.isFinite(Number(v))) return String(v);
      const n = Number(v);
      if (n === 0) return '0';
      const abs = Math.abs(n);
      if (abs >= 1e6 || abs < 1e-3) return n.toExponential(2);
      const dec = Math.max(0, 2 - Math.floor(Math.log10(abs)));
      return Number(n.toFixed(dec)).toString();
    };
    const fmtSupportEnd = v => {
      // Postgres serialises +/-Infinity as the strings 'Infinity'/'-Infinity'
      // through the float8 OUT params; psycopg surfaces them as the JSON
      // numbers Infinity/-Infinity, which JSON.stringify drops to null.
      // Accept either shape.
      if (v === null || v === undefined) return '∞';
      const s = String(v);
      if (s === 'Infinity' || v === Infinity) return '+∞';
      if (s === '-Infinity' || v === -Infinity) return '−∞';
      return fmt(v);
    };
    const supportLabel =
      `[${fmtSupportEnd(support[0])}, ${fmtSupportEnd(support[1])}]`;

    // Histogram geometry.  Width tracks renderRvDensity so the two
    // sit visually consistent if the user pin-compares; height is
    // taller (160 vs. the density preview's narrower band) because
    // the bar chart needs vertical room for the count axis to read,
    // and the eval strip is full-width below the toolbar anyway.
    // Constants live at module scope so wireProfileInteractions sees
    // the same coordinate frame for cursor → data-x conversion.
    const W = PROFILE_W, H = PROFILE_H, padX = PROFILE_PAD_X;
    const padTop = PROFILE_PAD_TOP, padBottom = PROFILE_PAD_BOTTOM;
    let svgInner = '';
    if (histogram.length) {
      const fullLo = Number(histogram[0].bin_lo);
      const fullHi = Number(histogram[histogram.length - 1].bin_hi);
      const lo = _view ? _view.lo : fullLo;
      const hi = _view ? _view.hi : fullHi;
      const span = (hi - lo) || 1;
      const sx = x => padX + (W - 2 * padX) * (x - lo) / span;
      const usableH = H - padTop - padBottom;
      let bars;
      const total = histogram.reduce(
        (s, b) => s + (Number(b.count) || 0), 0) || 1;
      // Tooltip text per bar.  Same readout shape in both modes:
      //   x ∈ [lo, hi]
      //   P(X ∈ bin) = p          (always)
      //   P(X ≤ hi)  = F          (CDF mode only)
      // The browser surfaces SVG <title> as a native hover tooltip
      // with a short delay; together with the .cv-profile-bars rect
      // hover style the user gets a per-bin readout without needing
      // a JS-driven overlay.
      // Per-bin probability tooltip.  The displayed percent uses
      // `dec - 2` decimals so a "Probability decimals = 4" setting
      // renders as "12.34%" (i.e. four digits of decimal probability
      // precision); clamp to 0 so small `dec` doesn't go negative.
      const pctDec = Math.max(0, dec - 2);
      const fmtPct = p => {
        if (!Number.isFinite(p)) return '?';
        if (p === 0) return '0';
        if (p < Math.pow(10, -dec)) return p.toExponential(Math.max(2, dec - 1));
        return (p * 100).toFixed(pctDec) + '%';
      };
      const tooltip = (b, cum) => {
        const lo_ = fmtTick(Number(b.bin_lo));
        const hi_ = fmtTick(Number(b.bin_hi));
        const c   = Number(b.count) || 0;
        const lines = [
          `x ∈ [${lo_}, ${hi_}]`,
          `P(X ∈ bin) = ${fmtPct(c / total)}  (n = ${c})`,
        ];
        if (_mode === 'cdf') {
          lines.push(`P(X ≤ ${hi_}) = ${fmtPct(cum / total)}`);
        }
        return escapeHtml(lines.join('\n'));
      };
      if (_mode === 'cdf') {
        // Empirical CDF as a staircase of cumulative-count bars.  Each
        // bar's height is the proportion of samples with value <= that
        // bar's right edge, so the rightmost bar reaches the top of
        // the usable area.
        let cum = 0;
        bars = histogram.map(b => {
          cum += Number(b.count) || 0;
          const x1 = sx(Number(b.bin_lo));
          const x2 = sx(Number(b.bin_hi));
          const w  = Math.max(1, x2 - x1 - 1);
          const h  = (cum / total) * usableH;
          const y  = (H - padBottom) - h;
          return `<rect x="${x1.toFixed(1)}" y="${y.toFixed(1)}" `
               + `width="${w.toFixed(1)}" height="${h.toFixed(1)}">`
               + `<title>${tooltip(b, cum)}</title></rect>`;
        }).join('');
      } else {
        // PDF: bar heights proportional to the per-bin count, scaled so
        // the tallest bin fills the usable area.
        const maxCount = histogram.reduce(
          (m, b) => Math.max(m, Number(b.count) || 0), 0) || 1;
        let cum = 0;
        bars = histogram.map(b => {
          cum += Number(b.count) || 0;
          const x1 = sx(Number(b.bin_lo));
          const x2 = sx(Number(b.bin_hi));
          const w  = Math.max(1, x2 - x1 - 1);
          const h  = ((Number(b.count) || 0) / maxCount) * usableH;
          const y  = (H - padBottom) - h;
          return `<rect x="${x1.toFixed(1)}" y="${y.toFixed(1)}" `
               + `width="${w.toFixed(1)}" height="${h.toFixed(1)}">`
               + `<title>${tooltip(b, cum)}</title></rect>`;
        }).join('');
      }
      // Mean reference line, faint dashed terracotta -- same convention
      // as renderRvDensity's mean line.  In CDF mode the line still
      // marks the expected value on the x-axis, which is independent
      // of bar interpretation.
      let meanLine = '';
      if (Number.isFinite(expected) && expected >= lo && expected <= hi) {
        const mx = sx(expected).toFixed(1);
        meanLine =
          `<line x1="${mx}" y1="${padTop}" x2="${mx}" y2="${(H - padBottom).toFixed(1)}" `
          + `stroke="var(--terracotta-500)" stroke-width="1" `
          + `stroke-dasharray="3 3" opacity="0.75" />`;
      }
      svgInner = `<g class="cv-profile-bars">${bars}</g>${meanLine}`
        + `<text class="cv-rv-tick" x="${padX}" y="${H - 4}">${escapeHtml(fmtTick(lo))}</text>`
        + `<text class="cv-rv-tick" x="${W - padX}" y="${H - 4}" text-anchor="end">`
        + `${escapeHtml(fmtTick(hi))}</text>`;
    } else {
      svgInner = `<text class="cv-rv-tick" x="${W / 2}" y="${H / 2}" text-anchor="middle">`
        + `no samples</text>`;
    }
    const ariaLabel = _mode === 'cdf'
      ? 'empirical cumulative distribution'
      : 'empirical histogram';
    // Inline cursor + touch-action on the SVG itself: the CSS
    // counterparts live in app.css but the browser's aggressive cache
    // on /static/app.css can serve a stale copy after a Studio
    // upgrade; the inline style guarantees the affordance regardless.
    const svg = `<svg width="${W}" height="${H}" viewBox="0 0 ${W} ${H}"`
      + ` style="cursor:zoom-in;touch-action:none;display:block"`
      + ` role="img" aria-label="${ariaLabel}">${svgInner}</svg>`;
    const stddev = (Number.isFinite(variance) && variance >= 0)
      ? Math.sqrt(variance) : null;
    // The toggle stores the JSON payload on the panel so the click
    // handler can re-render without re-fetching from the server.
    const toggleLabel = _mode === 'cdf' ? 'CDF' : 'PDF';
    const toggleNext  = _mode === 'cdf' ? 'PDF' : 'CDF';
    // Data attributes carry the full range and current view so the
    // wheel / dblclick handlers wired by wireProfileInteractions can
    // re-render without recomputing from the histogram array.
    const fullLoAttr = histogram.length ? Number(histogram[0].bin_lo) : 0;
    const fullHiAttr = histogram.length
                     ? Number(histogram[histogram.length - 1].bin_hi)
                     : 0;
    const viewLoAttr = _view ? _view.lo : fullLoAttr;
    const viewHiAttr = _view ? _view.hi : fullHiAttr;
    const zoomedHint = _view ? ' (double-click to reset)' : '';
    return `<div class="cv-profile-panel" aria-label="distribution profile"`
      + ` data-profile-mode="${_mode}"`
      + ` data-full-lo="${fullLoAttr}" data-full-hi="${fullHiAttr}"`
      + ` data-view-lo="${viewLoAttr}" data-view-hi="${viewHiAttr}"`
      + ` title="Scroll over the histogram to zoom${zoomedHint}">`
      + `<div class="cv-profile-meta">`
      + `<span class="cv-profile-badge" title="support interval">supp ${escapeHtml(supportLabel)}</span>`
      + `<span class="cv-profile-stat" title="expected value">μ = ${escapeHtml(fmt(expected))}</span>`
      + (stddev != null
          ? `<span class="cv-profile-stat"`
            + ` title="standard deviation (σ² = ${escapeHtml(fmt(variance))})">`
            + `σ = ${escapeHtml(fmt(stddev))}</span>`
          : `<span class="cv-profile-stat" title="variance">σ² = ${escapeHtml(fmt(variance))}</span>`)
      + `<button type="button" class="cv-profile-toggle"`
      + ` title="Switch to ${toggleNext}">${toggleLabel}</button>`
      + `</div>`
      + `<div class="cv-profile-svg cv-rv-density">${svg}</div>`
      + `</div>`;
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

  // Render a server error inside the eval-strip with the same icon +
  // ProvSQL pill + crimson .wp-error panel that the result-table banner
  // uses.  Strips the "ProvSQL: " prefix and turns it into a brand
  // badge, the way app.js's renderDiag does.  XX000 is the generic
  // internal_error SQLSTATE that provsql_error() emits, so we drop it
  // (it would add noise to every message without telling the user
  // anything actionable).
  function renderEvalError(message, sqlstate) {
    const raw = message || '';
    const m = raw.match(/^ProvSQL:\s*(.*)$/s);
    const badge = m ? '<span class="wp-srcbadge">ProvSQL</span> ' : '';
    const text  = m ? m[1] : raw;
    const tail  = (sqlstate && sqlstate !== 'XX000')
      ? ` <code>(SQLSTATE ${escapeHtml(sqlstate)})</code>`
      : '';
    return `<div class="wp-error">`
         + `<i class="fas fa-exclamation-circle"></i> `
         + `${badge}${escapeHtml(text)}${tail}`
         + `</div>`;
  }

  function shortUuid(u) {
    if (!u) return '–';
    // Match the result-table abbreviation in app.js's formatCell so the
    // two views stay visually consistent : 4 hex chars are enough for a
    // cursory same/different check, full uuids are one click away.
    return u.length > 4 ? `${u.slice(0, 4)}…` : u;
  }
})();
