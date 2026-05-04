/* ProvSQL Studio — entry script.
   Wires the shared chrome (mode switcher, example buttons, query form) plus
   the where-mode sidebar (relation tables + hover-highlight), with the same
   mock data as the design bundle so the static preview keeps working before
   Stage 2 hooks up the Flask backend. Circuit-mode wiring is a placeholder
   here; Stage 3 lazy-loads circuit.js into the sidebar. */

(function () {
  /* ──────── Where mode: relations sidebar + hover-highlight ──────── */

  // Bundle's hardcoded personnel rows, lifted from the where_panel index.
  // Stage 2 replaces this with GET /api/relations.
  const MOCK_PERSONNEL = {
    name: 'personnel',
    columns: ['id','name','position','city','classification'],
    meta: '7 tuples · 5 cols · provsql tagged',
    rows: [
      ['1','John','Director','New York',          {pill:'unclassified'}],
      ['2','Paul','Janitor','New York',           {pill:'restricted'}],
      ['3','Dave','Analyst','Paris',              {pill:'confidential'}],
      ['4','Ellen','Field agent','Berlin',        {pill:'secret'}],
      ['5','Magdalen','Double agent','Paris',     {pill:'top_secret'}],
      ['6','Nancy','HR','Paris',                  {pill:'restricted'}],
      ['7','Susan','Analyst','Berlin',            {pill:'secret'}],
    ],
  };

  const mode = document.body.classList.contains('mode-circuit') ? 'circuit' : 'where';

  // Reflect current mode on the switcher (which itself is just two anchors).
  document.querySelectorAll('.ps-modeswitch__btn').forEach(btn => {
    btn.classList.toggle('is-active', btn.dataset.mode === mode);
  });

  // Example-query buttons paste into the textarea; works in either mode.
  document.querySelectorAll('.wp-btn--ex').forEach(btn => {
    btn.addEventListener('click', () => {
      document.getElementById('request').value = btn.dataset.q;
    });
  });

  if (mode === 'where') setupWhereMode();
  else                  setupCircuitMode();

  function setupWhereMode() {
    renderRelation(MOCK_PERSONNEL);
    const body = document.getElementById('result-body');
    body.addEventListener('mouseover', (e) => onResultHover(e, true));
    body.addEventListener('mouseout',  (e) => onResultHover(e, false));
    // Default initial result (DISTINCT city) so the page isn't empty.
    runQuery({ preventDefault() {} });
  }

  function renderRelation(rel) {
    const body = document.getElementById('sidebar-body');
    body.innerHTML = `
      <section class="wp-relation">
        <header class="wp-relation__hdr">
          <h3 class="wp-relation__name">${rel.name}</h3>
          <span class="wp-relation__meta">${rel.meta}</span>
        </header>
        <div class="wp-table-wrap">
          <table class="wp-table" id="t-${rel.name}">
            <thead><tr>${rel.columns.map(c => `<th>${c}</th>`).join('')}</tr></thead>
            <tbody>
              ${rel.rows.map((r, ri) => `
                <tr data-row="${ri+1}">
                  ${r.map((v, ci) => {
                    const id = `${rel.name}:${ri+1}:${ci+1}`;
                    const html = (v && typeof v === 'object' && v.pill)
                      ? `<span class="wp-pill wp-pill--${v.pill}">${v.pill}</span>`
                      : v;
                    return `<td id="${id}">${html}</td>`;
                  }).join('')}
                </tr>
              `).join('')}
            </tbody>
          </table>
        </div>
      </section>`;
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
})();

/* Global mock query runner — invoked by the inline onsubmit. Stage 2 replaces
   this with a real POST /api/exec. */
function runQuery(ev) {
  ev.preventDefault();
  if (document.body.classList.contains('mode-circuit')) return false;

  const q = document.getElementById('request').value.trim().replace(/;$/, '').toLowerCase();
  const head  = document.getElementById('result-head');
  const body  = document.getElementById('result-body');
  const count = document.getElementById('result-count');
  const time  = document.getElementById('result-time');

  let cols, rows;
  if (q === 'select * from personnel') {
    cols = ['id','name','position','city','classification'];
    rows = [
      ['1','John','Director','New York','unclassified'],
      ['2','Paul','Janitor','New York','restricted'],
      ['3','Dave','Analyst','Paris','confidential'],
      ['4','Ellen','Field agent','Berlin','secret'],
      ['5','Magdalen','Double agent','Paris','top_secret'],
      ['6','Nancy','HR','Paris','restricted'],
      ['7','Susan','Analyst','Berlin','secret'],
    ].map((r, i) => r.map((v, j) => ({
      v: j === 4 ? `<span class="wp-pill wp-pill--${v}">${v}</span>` : v,
      sources: `personnel:${i+1}:${j+1}`,
    })));
  } else if (q.startsWith("select city from personnel union")) {
    cols = ['city'];
    rows = [
      [{v:'New York', sources:'personnel:1:4;personnel:2:4'}],
      [{v:'Paris',    sources:'personnel:3:4;personnel:5:4;personnel:6:4'}],
      [{v:'Berlin',   sources:'personnel:4:4;personnel:7:4'}],
      [{v:'5',        sources:'personnel:1:1;personnel:2:1;personnel:3:1;personnel:4:1;personnel:5:1;personnel:6:1;personnel:7:1'}],
    ];
  } else if (q.startsWith('select city, count')) {
    cols = ['city','count'];
    rows = [
      [{v:'New York', sources:'personnel:1:4;personnel:2:4'},
       {v:'2',        sources:'personnel:1:4;personnel:2:4'}],
      [{v:'Paris',    sources:'personnel:3:4;personnel:5:4;personnel:6:4'},
       {v:'3',        sources:'personnel:3:4;personnel:5:4;personnel:6:4'}],
      [{v:'Berlin',   sources:'personnel:4:4;personnel:7:4'},
       {v:'2',        sources:'personnel:4:4;personnel:7:4'}],
    ];
  } else { /* default: distinct city */
    cols = ['city'];
    rows = [
      [{v:'New York', sources:'personnel:1:4;personnel:2:4'}],
      [{v:'Paris',    sources:'personnel:3:4;personnel:5:4;personnel:6:4'}],
      [{v:'Berlin',   sources:'personnel:4:4;personnel:7:4'}],
    ];
  }

  head.innerHTML = cols.map(c => `<th>${c}</th>`).join('');
  body.innerHTML = rows.map(r =>
    `<tr>${r.map(c => `<td class="wp-result__cell" data-sources="${c.sources}">${c.v}</td>`).join('')}</tr>`
  ).join('');
  count.textContent = rows.length;
  time.textContent = (8 + Math.floor(Math.random() * 18));
  return false;
}
