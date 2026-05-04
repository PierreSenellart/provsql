/* ProvSQL Circuit Visualizer
   Renders provenance circuits as DAGs with auto-layout (layered, top-down,
   sinks at top). Each query carries a hand-built circuit so the layout is
   deterministic and pedagogically clear. */

// ─── data: 7-tuple personnel relation ─────────────────────────────────
const PERSONNEL = [
  {id:1, name:'John',     position:'Director',     city:'New York', uuid:'a1f0…1d1e'},
  {id:2, name:'Paul',     position:'Janitor',      city:'New York', uuid:'b2c4…7a3f'},
  {id:3, name:'Dave',     position:'Analyst',      city:'Paris',    uuid:'c8e1…9b22'},
  {id:4, name:'Ellen',    position:'Field agent',  city:'Berlin',   uuid:'d44a…0e0c'},
  {id:5, name:'Magdalen', position:'Double agent', city:'Paris',    uuid:'e9c2…f1a8'},
  {id:6, name:'Nancy',    position:'HR',           city:'Paris',    uuid:'f1b8…2d4e'},
  {id:7, name:'Susan',    position:'Analyst',      city:'Berlin',   uuid:'a7e3…6c91'},
];

const leaf = (i) => ({type:'leaf', id:`l${i}`, idx:i, label:`p${i}`, uuid:PERSONNEL[i-1].uuid, info:`personnel #${i} · ${PERSONNEL[i-1].name}`});
const one  = ()   => ({type:'one',  id:`one_${Math.random().toString(36).slice(2,6)}`, label:'⊤'});
const tms  = (id, ch) => ({type:'times', id, label:'×', children:ch});
const pls  = (id, ch) => ({type:'plus',  id, label:'+', children:ch});
const eq   = (id, ch) => ({type:'eq',    id, label:'=', children:ch});

/* Each query: SQL, result table, and per-result-row a circuit (top→bottom).
   Circuits are flat node lists with parent→child edges; we layout below. */
const QUERIES = [
  {
    name: 'Distinct',
    sql: 'SELECT DISTINCT city FROM personnel',
    cols: ['city'],
    rows: [
      { cells: ['New York'],
        circuit: pls('+_ny', [leaf(1), leaf(2)]) },
      { cells: ['Paris'],
        circuit: pls('+_paris', [leaf(3), leaf(5), leaf(6)]) },
      { cells: ['Berlin'],
        circuit: pls('+_berlin', [leaf(4), leaf(7)]) },
    ],
  },
  {
    name: 'Self-join',
    sql: "SELECT DISTINCT P1.city\nFROM personnel P1 JOIN personnel P2\n  ON P1.city = P2.city\nWHERE P1.id < P2.id",
    cols: ['city'],
    rows: [
      // New York: pair (1,2)
      { cells: ['New York'],
        circuit: tms('×_ny', [leaf(1), leaf(2)]) },
      // Paris: (3,5) ∨ (3,6) ∨ (5,6)
      { cells: ['Paris'],
        circuit: pls('+_paris', [
          tms('×_35', [leaf(3), leaf(5)]),
          tms('×_36', [leaf(3), leaf(6)]),
          tms('×_56', [leaf(5), leaf(6)]),
        ]) },
      // Berlin: (4,7)
      { cells: ['Berlin'],
        circuit: tms('×_bn', [leaf(4), leaf(7)]) },
    ],
  },
  {
    name: 'Union',
    sql: "SELECT city FROM personnel\nUNION\nSELECT '5' FROM personnel",
    cols: ['city'],
    rows: [
      { cells: ['New York'], circuit: pls('+_ny', [leaf(1), leaf(2)]) },
      { cells: ['Paris'],    circuit: pls('+_paris', [leaf(3), leaf(5), leaf(6)]) },
      { cells: ['Berlin'],   circuit: pls('+_berlin', [leaf(4), leaf(7)]) },
      { cells: ['5'],        circuit: pls('+_five', PERSONNEL.map((_,i)=>leaf(i+1))) },
    ],
  },
  {
    name: 'Group by',
    sql: 'SELECT city, COUNT(*)\nFROM personnel\nGROUP BY city',
    cols: ['city','count'],
    rows: [
      { cells: ['New York','2'], circuit: pls('+_ny', [leaf(1), leaf(2)]) },
      { cells: ['Paris','3'],    circuit: pls('+_paris', [leaf(3), leaf(5), leaf(6)]) },
      { cells: ['Berlin','2'],   circuit: pls('+_berlin', [leaf(4), leaf(7)]) },
    ],
  },
];

// ─── state ─────────────────────────────────────────────────────────────
let state = {
  qIdx: 1,        // start on Self-join — most interesting circuit
  rowIdx: 1,      // Paris
  showUuids: false,
  showFormula: true,
  zoom: 1,
  pan: {x:0, y:0},
  pinnedNode: null,
};

// ─── layout: layered top-down ──────────────────────────────────────────
/* Walk the tree, assign each node a depth (root = 0) and an x-slot.
   Use Reingold-Tilford-ish: leaves get sequential x positions,
   internal nodes get the centroid of their children. */
function layout(root, opts = {}) {
  const hSpacing = opts.hSpacing || 80;
  const vSpacing = opts.vSpacing || 90;

  const nodes = [];     // flattened with x,y
  const edges = [];     // {from, to}
  let leafCounter = 0;

  function walk(node, depth) {
    const flat = { ...node, depth };
    nodes.push(flat);
    if (!node.children || node.children.length === 0) {
      flat.x = leafCounter++ * hSpacing;
    } else {
      const childPositions = node.children.map(c => walk(c, depth + 1));
      // x = centroid of children
      flat.x = childPositions.reduce((s, c) => s + c.x, 0) / childPositions.length;
      node.children.forEach(c => edges.push({ from: flat.id, to: c.id }));
    }
    flat.y = depth * vSpacing;
    return flat;
  }
  walk(root, 0);

  // normalize: shift so min-x = padding
  const xs = nodes.map(n => n.x);
  const minX = Math.min(...xs), maxX = Math.max(...xs);
  const ys = nodes.map(n => n.y);
  const minY = Math.min(...ys), maxY = Math.max(...ys);
  const padding = 50;
  nodes.forEach(n => { n.x = n.x - minX + padding; n.y = n.y - minY + padding; });
  return {
    nodes, edges,
    width: maxX - minX + padding * 2,
    height: maxY - minY + padding * 2,
  };
}

// ─── render ────────────────────────────────────────────────────────────
const svg = document.getElementById('circuit');
const edgeLayer = document.getElementById('circuit-edges');
const nodeLayer = document.getElementById('circuit-nodes');

function svgEl(tag, attrs = {}) {
  const el = document.createElementNS('http://www.w3.org/2000/svg', tag);
  for (const k in attrs) el.setAttribute(k, attrs[k]);
  return el;
}

function render() {
  const q = QUERIES[state.qIdx];
  const row = q.rows[state.rowIdx];
  const lay = layout(row.circuit);

  edgeLayer.innerHTML = '';
  nodeLayer.innerHTML = '';

  // edges
  for (const e of lay.edges) {
    const from = lay.nodes.find(n => n.id === e.from);
    const to = lay.nodes.find(n => n.id === e.to);
    const path = svgEl('path', {
      class: 'edge',
      d: `M ${from.x} ${from.y + 22} C ${from.x} ${from.y + 50}, ${to.x} ${to.y - 50}, ${to.x} ${to.y - 22}`,
      'data-from': e.from, 'data-to': e.to,
    });
    edgeLayer.appendChild(path);
  }

  // nodes
  for (const n of lay.nodes) {
    const g = svgEl('g', { class: `node-group node--${n.type}`, 'data-id': n.id, transform: `translate(${n.x},${n.y})` });
    let shape;
    if (n.type === 'leaf' || n.type === 'one') {
      shape = svgEl('circle', { class: 'node-shape', r: 22 });
    } else {
      shape = svgEl('circle', { class: 'node-shape', r: 22 });
    }
    g.appendChild(shape);
    const label = svgEl('text', { class: 'node-label', y: 1 });
    label.textContent = n.label;
    g.appendChild(label);
    if (n.type === 'leaf') {
      const meta = svgEl('text', { class: 'node-meta', y: 38 });
      meta.textContent = state.showUuids ? n.uuid : `tuple ${n.idx}`;
      g.appendChild(meta);
    }
    g.addEventListener('click', (e) => { e.stopPropagation(); pinNode(n.id, lay); });
    g.addEventListener('mouseenter', () => highlightSubtree(n.id, lay, true));
    g.addEventListener('mouseleave', () => { if (!state.pinnedNode) highlightSubtree(n.id, lay, false); });
    nodeLayer.appendChild(g);
  }

  // viewbox: fit content with margin, applying zoom/pan
  const w = lay.width, h = lay.height;
  fitView(w, h);

  // formula
  document.getElementById('formula-expr').innerHTML = state.showFormula ? formulaHtml(row.circuit) : '<span style="opacity:.5">(hidden)</span>';
  document.getElementById('formula-strip').style.display = state.showFormula ? 'flex' : 'none';

  // re-apply pin if same node still exists
  if (state.pinnedNode && lay.nodes.find(n => n.id === state.pinnedNode)) {
    highlightSubtree(state.pinnedNode, lay, true, true);
  } else {
    state.pinnedNode = null;
    closeInspector();
  }
}

function fitView(w, h) {
  const vbW = Math.max(w, 400);
  const vbH = Math.max(h, 300);
  // apply zoom + pan
  const cx = vbW / 2 + state.pan.x;
  const cy = vbH / 2 + state.pan.y;
  const halfW = vbW / (2 * state.zoom);
  const halfH = vbH / (2 * state.zoom);
  svg.setAttribute('viewBox', `${cx - halfW} ${cy - halfH} ${halfW * 2} ${halfH * 2}`);
}

// ─── highlight ─────────────────────────────────────────────────────────
function descendants(id, lay) {
  const out = new Set([id]);
  const stack = [id];
  while (stack.length) {
    const cur = stack.pop();
    for (const e of lay.edges) {
      if (e.from === cur && !out.has(e.to)) {
        out.add(e.to); stack.push(e.to);
      }
    }
  }
  return out;
}

function highlightSubtree(id, lay, on, pinned = false) {
  const set = descendants(id, lay);
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

function pinNode(id, lay) {
  if (state.pinnedNode === id) {
    state.pinnedNode = null;
    highlightSubtree(id, lay, false);
    closeInspector();
  } else {
    state.pinnedNode = id;
    document.querySelectorAll('.node-group').forEach(g => g.classList.remove('is-active','is-pinned'));
    document.querySelectorAll('.edge').forEach(p => p.classList.remove('is-active'));
    highlightSubtree(id, lay, true, true);
    openInspector(id, lay);
  }
}

// ─── inspector ─────────────────────────────────────────────────────────
function openInspector(id, lay) {
  const node = lay.nodes.find(n => n.id === id);
  if (!node) return;
  const inspector = document.getElementById('inspector');
  const title = document.getElementById('inspector-title');
  const body = document.getElementById('inspector-body');
  inspector.classList.add('is-open');

  const typeNames = { times:'Times gate (×)', plus:'Plus gate (+)', eq:'Equality gate (=)', leaf:'Leaf — base tuple', one:'One — semiring identity' };
  title.textContent = typeNames[node.type] || 'Node';

  let html = '<dl>';
  html += `<dt>type</dt><dd>${node.type}</dd>`;
  html += `<dt>gate id</dt><dd>${node.id}</dd>`;
  if (node.type === 'leaf') {
    const p = PERSONNEL[node.idx - 1];
    html += `<dt>uuid</dt><dd>${node.uuid}</dd>`;
    html += `<dt>relation</dt><dd>personnel</dd>`;
    html += `<dt>tuple</dt><dd>(${p.id}, '${p.name}', '${p.position}', '${p.city}')</dd>`;
  } else {
    const sub = lay.edges.filter(e => e.from === id);
    html += `<dt>fan-in</dt><dd>${sub.length}</dd>`;
  }
  html += '</dl>';
  if (node.type === 'times') html += '<p>Conjunction: this gate evaluates to ⊤ only when every child does. Used for joins and conjunctive WHERE clauses.</p>';
  else if (node.type === 'plus')  html += '<p>Disjunction: evaluates to ⊤ if any child does. Used for UNION and projection-induced merges.</p>';
  else if (node.type === 'leaf')  html += '<p>A base tuple. Its presence or absence in the database determines whether this branch contributes.</p>';
  body.innerHTML = html;
}
function closeInspector() {
  document.getElementById('inspector').classList.remove('is-open');
}
document.getElementById('inspector-close').addEventListener('click', () => {
  if (state.pinnedNode) {
    document.querySelectorAll('.node-group').forEach(g => g.classList.remove('is-pinned'));
    document.querySelectorAll('.edge').forEach(p => p.classList.remove('is-active'));
    state.pinnedNode = null;
  }
  closeInspector();
});

// ─── formula rendering ─────────────────────────────────────────────────
function formulaHtml(node) {
  if (node.type === 'leaf') return `<span class="leaf">${node.label}</span>`;
  if (node.type === 'one')  return `<span class="one">⊤</span>`;
  const op = node.type === 'times' ? '<span class="op-times">×</span>' :
             node.type === 'plus'  ? '<span class="op-plus">+</span>' : '<span class="op-eq">=</span>';
  const inner = node.children.map(formulaHtml).join(op);
  return `(${inner})`;
}

// ─── side panels: query list + result table ────────────────────────────
function renderQueryList() {
  const list = document.getElementById('querylist');
  list.innerHTML = QUERIES.map((q, i) => `
    <button class="cv-query ${i === state.qIdx ? 'is-active' : ''}" data-q="${i}">
      <span class="cv-query__name">${q.name}</span>
      <span class="cv-query__sql">${q.sql.split('\n')[0]}</span>
    </button>
  `).join('');
  list.querySelectorAll('.cv-query').forEach(btn => {
    btn.addEventListener('click', () => {
      state.qIdx = +btn.dataset.q;
      state.rowIdx = 0;
      state.pinnedNode = null;
      renderAll();
    });
  });
}
function renderResultTable() {
  const q = QUERIES[state.qIdx];
  document.getElementById('sql-display').textContent = q.sql;
  document.getElementById('result-head').innerHTML = q.cols.map(c => `<th>${c}</th>`).join('');
  document.getElementById('result-body').innerHTML = q.rows.map((r, i) => `
    <tr class="${i === state.rowIdx ? 'is-selected' : ''}" data-r="${i}">
      ${r.cells.map(c => `<td>${c}</td>`).join('')}
    </tr>
  `).join('');
  document.querySelectorAll('#result-body tr').forEach(tr => {
    tr.addEventListener('click', () => {
      state.rowIdx = +tr.dataset.r;
      state.pinnedNode = null;
      renderAll();
    });
  });
  const row = q.rows[state.rowIdx];
  document.getElementById('selected-label').textContent = row.cells.join(' · ');
  document.getElementById('circuit-title').textContent = `Provenance Circuit · ${row.cells.join(', ')}`;
  document.getElementById('circuit-sub').textContent = `Derivation of result row ${state.rowIdx + 1} of ${q.rows.length} for "${q.name}".`;
}
function renderAll() {
  renderQueryList();
  renderResultTable();
  render();
}

// ─── toolbar ───────────────────────────────────────────────────────────
document.getElementById('tool-zoom-in').addEventListener('click', () => { state.zoom = Math.min(2.5, state.zoom * 1.2); render(); });
document.getElementById('tool-zoom-out').addEventListener('click', () => { state.zoom = Math.max(0.4, state.zoom / 1.2); render(); });
document.getElementById('tool-zoom-fit').addEventListener('click', () => { state.zoom = 1; state.pan = {x:0,y:0}; render(); });
document.getElementById('tool-show-uuids').addEventListener('click', (e) => {
  state.showUuids = !state.showUuids;
  e.currentTarget.setAttribute('aria-pressed', state.showUuids);
  render();
});
document.getElementById('tool-show-formula').addEventListener('click', (e) => {
  state.showFormula = !state.showFormula;
  e.currentTarget.setAttribute('aria-pressed', state.showFormula);
  render();
});

// pan via drag
let dragging = false, dragStart = null;
svg.addEventListener('mousedown', (e) => {
  dragging = true;
  dragStart = { x: e.clientX, y: e.clientY, panX: state.pan.x, panY: state.pan.y };
});
window.addEventListener('mousemove', (e) => {
  if (!dragging) return;
  const dx = (e.clientX - dragStart.x) / state.zoom;
  const dy = (e.clientY - dragStart.y) / state.zoom;
  state.pan.x = dragStart.panX - dx;
  state.pan.y = dragStart.panY - dy;
  render();
});
window.addEventListener('mouseup', () => { dragging = false; });
// background click clears selection
svg.addEventListener('click', () => {
  if (state.pinnedNode) {
    document.querySelectorAll('.node-group').forEach(g => g.classList.remove('is-pinned'));
    document.querySelectorAll('.edge').forEach(p => p.classList.remove('is-active'));
    state.pinnedNode = null;
    closeInspector();
  }
});

// init
renderAll();
