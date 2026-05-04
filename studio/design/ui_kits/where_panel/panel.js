/* Where Panel interactions
   Mirrors the upstream PHP/JS: hovering a result cell highlights every
   source cell whose where-provenance contributed to it. We map each
   result cell's data-sources (semicolon-separated id list) onto cells
   in the source tables. */

(function () {
  const resultBody = document.getElementById('result-body');

  function setSourceState(ids, on) {
    ids.forEach(id => {
      const el = document.getElementById(id);
      if (el) el.classList.toggle('is-source', on);
    });
  }

  resultBody.addEventListener('mouseover', (e) => {
    const cell = e.target.closest('.wp-result__cell');
    if (!cell) return;
    cell.classList.add('is-hover');
    const ids = (cell.dataset.sources || '').split(';').filter(Boolean);
    setSourceState(ids, true);
  });
  resultBody.addEventListener('mouseout', (e) => {
    const cell = e.target.closest('.wp-result__cell');
    if (!cell) return;
    cell.classList.remove('is-hover');
    const ids = (cell.dataset.sources || '').split(';').filter(Boolean);
    setSourceState(ids, false);
  });

  // Example query buttons
  document.querySelectorAll('.wp-btn--ex').forEach(btn => {
    btn.addEventListener('click', () => {
      document.getElementById('request').value = btn.dataset.q;
    });
  });
})();

/* Mock query runner.
   The real where_panel posts the query to PHP and re-renders. Here we
   recognise the three example queries and swap the result table; for
   anything else we leave the existing (DISTINCT city) result in place
   and flash the editor. */
function runQuery(ev) {
  ev.preventDefault();
  const q = document.getElementById('request').value.trim().replace(/;$/, '').toLowerCase();
  const body = document.getElementById('result-body');
  const head = document.querySelector('#t-result thead tr');
  const count = document.getElementById('result-count');
  const time = document.getElementById('result-time');

  let cols, rows;
  if (q === 'select * from personnel') {
    cols = ['id','name','position','city','classification'];
    rows = [
      ['1','John','Director','New York',['unclassified']],
      ['2','Paul','Janitor','New York',['restricted']],
      ['3','Dave','Analyst','Paris',['confidential']],
      ['4','Ellen','Field agent','Berlin',['secret']],
      ['5','Magdalen','Double agent','Paris',['top_secret']],
      ['6','Nancy','HR','Paris',['restricted']],
      ['7','Susan','Analyst','Berlin',['secret']],
    ].map((r, i) => r.map((v, j) => ({
      v: Array.isArray(v) ? `<span class="wp-pill wp-pill--${v[0]}">${v[0]}</span>` : v,
      sources: `personnel:${i+1}:${j+1}`
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
      [{v:'New York', sources:'personnel:1:4;personnel:2:4'}, {v:'2', sources:'personnel:1:4;personnel:2:4'}],
      [{v:'Paris',    sources:'personnel:3:4;personnel:5:4;personnel:6:4'}, {v:'3', sources:'personnel:3:4;personnel:5:4;personnel:6:4'}],
      [{v:'Berlin',   sources:'personnel:4:4;personnel:7:4'}, {v:'2', sources:'personnel:4:4;personnel:7:4'}],
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
