/* profile.js -- the Distribution-profile panel (histogram / PDF / CDF
   of a random variable or aggregate, with the analytical-curve overlay,
   wheel zoom, double-click reset and the PDF/CDF toggle), shared by the
   Circuit-mode evaluation strip (circuit.js) and notebook evaluation cells
   (notebook.js).  A plain <script> loaded before app.js; it exposes
   ProvsqlStudio.renderProfilePanel(profile, mode, view) -> HTML string and
   ProvsqlStudio.wireProfileInteractions(hostEl, profile), which marks
   hostEl as a .cv-profile-host so the document-level wheel / dblclick
   handlers find the payload to re-render. */
(function () {
  'use strict';

  function escapeHtml(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;');
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
    resultEl.classList.add('cv-profile-host');
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
    const resultEl = svg.closest('.cv-profile-host');
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
    const resultEl = svg.closest('.cv-profile-host');
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
    /* Hoisted so the data-full-lo / data-full-hi panel attrs below
     * pick up any frame extension done inside the histogram block
     * (pure-discrete shapes widen to the analytical curves' range).
     * The zoom handler reads these to clamp newLo/newHi, so without
     * the hoist a mode toggle or zoom-out would silently snap back
     * to the raw histogram extent. */
    let fullLo = 0, fullHi = 0;
    if (histogram.length) {
      fullLo = Number(histogram[0].bin_lo);
      fullHi = Number(histogram[histogram.length - 1].bin_hi);
      /* Whenever the backend ships an analytical curves payload, take
       * the union of the histogram extent and the curves' x-range as
       * the chart frame.  shape_x_range already pads ±10% around the
       * underlying support (and unions branch ranges for mixtures),
       * so this:
       *  - pure-discrete shapes (Dirac, categorical, nested mixtures
       *    of those): shows the flat staircase before the first and
       *    after the last stem, which the MC histogram alone collapses
       *    onto the outermost outcomes;
       *  - mixed continuous + discrete: shows Dirac discs that sit at
       *    or past the empirical histogram's edge (e.g. Dirac at 0 in
       *    a Uniform(0,1) mixture would otherwise be flush against the
       *    left margin);
       *  - degenerate histograms (Dirac alone): replaces the zero-
       *    width frame with a sensible padded window.
       * Without curves we keep the histogram frame, plus a ±1 fallback
       * if the histogram is degenerate. */
      const aCurves = profile.analytical_curves;
      const cdfArr = aCurves && Array.isArray(aCurves.cdf) ? aCurves.cdf : null;
      if (cdfArr && cdfArr.length >= 2) {
        const cLo = Number(cdfArr[0].x);
        const cHi = Number(cdfArr[cdfArr.length - 1].x);
        if (Number.isFinite(cLo) && Number.isFinite(cHi) && cLo < cHi) {
          if (fullLo < fullHi) {
            fullLo = Math.min(fullLo, cLo);
            fullHi = Math.max(fullHi, cHi);
          } else {
            fullLo = cLo;
            fullHi = cHi;
          }
        }
      } else if (!(fullLo < fullHi)) {
        const c = fullLo;
        fullLo = c - 1;
        fullHi = c + 1;
      }
      const lo = _view ? _view.lo : fullLo;
      const hi = _view ? _view.hi : fullHi;
      const span = (hi - lo) || 1;
      const sx = x => padX + (W - 2 * padX) * (x - lo) / span;
      const usableH = H - padTop - padBottom;
      let bars;
      const total = histogram.reduce(
        (s, b) => s + (Number(b.count) || 0), 0) || 1;
      // Hoisted out of the mode dispatch so the analytical-curve overlay
      // below can scale PDF density to count-scaled bar heights.
      const maxCount = histogram.reduce(
        (m, b) => Math.max(m, Number(b.count) || 0), 0) || 1;
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
        // the tallest bin fills the usable area.  `maxCount` is hoisted
        // above the dispatch so the analytical-curve overlay can reuse it.
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
      // Analytical-curve overlay.  When the backend's rv_analytical_curves
      // returned a {pdf, cdf} payload (closed-form shape: bare gate_rv with
      // a recognised family, possibly truncated), draw the matching curve
      // over the histogram bars.  In PDF mode we scale the density into
      // the count-axis frame so the curve rides over the bars at the
      // same scale; in CDF mode we map the cumulative probability into
      // the staircase's [0, usableH] frame.  Falls back to no overlay
      // when the payload is absent (NULL on the SQL side -> null here)
      // so non-closed-form shapes (gate_arith composites, mixtures,
      // categoricals) render histogram-only without a special case.
      let overlayPath = '';
      const curves = profile.analytical_curves;
      const curveArr = curves
                       && Array.isArray(curves[_mode === 'cdf' ? 'cdf' : 'pdf'])
                     ? curves[_mode === 'cdf' ? 'cdf' : 'pdf'] : null;
      /* Pure-discrete shape (Dirac, categorical, or any nested
       * mixture of those without a continuous arm): the CDF is a
       * staircase, not a smooth function.  Render it as horizontal
       * segments joined by vertical jumps so the jump at each
       * outcome lands on the stem instead of being interpolated
       * across the two surrounding x-samples. */
      const discreteCdf = _mode === 'cdf'
                       && curves
                       && !Array.isArray(curves.pdf)
                       && Array.isArray(curves.stems);
      if (discreteCdf && Array.isArray(curves.stems) && curves.stems.length) {
        /* Pure-discrete CDF: build the staircase directly from the
         * stems so the jumps land exactly on the outcome values
         * (sampled-cdf rendering would smear each jump across the
         * surrounding sample interval and the final segment would
         * stop one sample short of x_hi).  Stems outside the current
         * viewport still contribute to the cumulative mass: stems
         * before lo preload `cum` so the path enters the view at the
         * correct height (matching the empirical bars, which also
         * track cum across the full histogram, not just the visible
         * bins); stems after hi are ignored, the staircase exits at
         * the last in-view cum. */
        const baseY = (H - padBottom);
        const allStems = curves.stems.slice()
          .map(s => ({ x: Number(s.x), p: Number(s.p) }))
          .filter(s => Number.isFinite(s.x) && Number.isFinite(s.p))
          .sort((a, b) => a.x - b.x);
        if (allStems.length) {
          let cum = 0;
          for (const st of allStems) if (st.x < lo) cum += st.p;
          const seg = [];
          seg.push(
            `${sx(lo).toFixed(1)},`
            + `${(baseY - Math.min(1, cum) * usableH).toFixed(1)}`);
          for (const st of allStems) {
            if (st.x < lo || st.x > hi) continue;
            const px = sx(st.x).toFixed(1);
            const yPrev = (baseY - Math.min(1, cum) * usableH).toFixed(1);
            seg.push(`${px},${yPrev}`);
            cum += st.p;
            const yCur = (baseY - Math.min(1, cum) * usableH).toFixed(1);
            seg.push(`${px},${yCur}`);
          }
          const yEnd = (baseY - Math.min(1, cum) * usableH).toFixed(1);
          seg.push(`${sx(hi).toFixed(1)},${yEnd}`);
          overlayPath =
            `<path class="cv-profile-overlay" d="M${seg.join(' L')}" `
            + `fill="none" stroke="var(--terracotta-500)" `
            + `stroke-width="1.5" opacity="0.9" />`;
        }
      } else if (curveArr && curveArr.length >= 2) {
        // Equal-width histogram: bin width = (full range) / nbins.
        // The backend builds the histogram with constant bin width
        // (compute_support's lo..hi divided into `bins` cells), so
        // averaging the first cell's width is equivalent and avoids a
        // separate reduction.
        const binW = (Number(histogram[0].bin_hi)
                      - Number(histogram[0].bin_lo)) || 1;
        const pts = [];
        for (const pt of curveArr) {
          const xv = Number(pt.x);
          const pv = Number(pt.p);
          if (!Number.isFinite(xv) || !Number.isFinite(pv)) continue;
          /* Clip to the current x-axis view so a curve that extends
           * past the histogram (e.g. unconditional Normal's mu ± 4σ
           * window vs. a tighter empirical bin range) does not
           * overhang the SVG. */
          if (xv < lo || xv > hi) continue;
          const px = sx(xv);
          let py;
          if (_mode === 'cdf') {
            /* Staircase maps cum/total in [0, 1] to height usableH.
             * The analytical CDF is already in [0, 1] over the
             * truncation, so a direct linear map matches. */
            const c = Math.max(0, Math.min(1, pv));
            py = (H - padBottom) - c * usableH;
          } else {
            /* Bar height is (count / maxCount) * usableH.  The
             * expected count in a bin of width binW is total · binW
             * · pdf(midpoint).  Substituting count = total · binW · p:
             *   bar_h_frac = total · binW · p / maxCount.
             * Same scale as the bars, so a correctly-modelled
             * histogram lays right under the curve. */
            const scaled = (total * binW * pv) / maxCount;
            py = (H - padBottom) - Math.max(0, scaled) * usableH;
          }
          pts.push(`${px.toFixed(1)},${py.toFixed(1)}`);
        }
        if (pts.length >= 2) {
          overlayPath =
            `<path class="cv-profile-overlay" d="M${pts.join(' L')}" `
            + `fill="none" stroke="var(--terracotta-500)" `
            + `stroke-width="1.5" opacity="0.9" />`;
        }
      }
      /* Point-mass overlay.  When the backend payload carries
       * stems:[{x, p}], render each as a vertical line capped by a
       * small disc: Dirac (as_random) or categorical roots, or
       * Dirac arms inside a Bernoulli mixture.  PDF mode only; CDF
       * mode already conveys the same information through the
       * staircase jumps that shape_cdf bakes into the curve. */
      let stemsGroup = '';
      const stemArr = curves && Array.isArray(curves.stems) ? curves.stems : null;
      if (stemArr && stemArr.length && _mode !== 'cdf') {
        const baseY = (H - padBottom);
        const parts = [];
        for (const st of stemArr) {
          const xv = Number(st.x);
          const pv = Number(st.p);
          if (!Number.isFinite(xv) || !Number.isFinite(pv)) continue;
          if (xv < lo || xv > hi) continue;
          /* Match the bar scale.  Bars are scaled count/maxCount, and
           * the expected count at a stem's x is pv·total.  Using
           *   stem_h_frac = pv · total / maxCount
           * puts the disc at the same height as the bar that the
           * sampler builds at that x, so a perfectly-modelled
           * categorical / Dirac has its disc landing right on top
           * of its bar. */
          const scaled = (pv * total) / maxCount;
          const h = Math.max(0, Math.min(1, scaled)) * usableH;
          const px = sx(xv).toFixed(1);
          const py = (baseY - h).toFixed(1);
          const tip = `x = ${escapeHtml(String(xv))}\nP(X = x) = ${fmtPct(pv)}`;
          parts.push(
            `<line x1="${px}" y1="${baseY.toFixed(1)}" `
            + `x2="${px}" y2="${py}" />`
            + `<circle cx="${px}" cy="${py}" r="4">`
            + `<title>${tip}</title></circle>`
          );
        }
        if (parts.length) {
          stemsGroup =
            `<g class="cv-profile-stems" stroke="var(--terracotta-500)" `
            + `stroke-width="1.5" fill="var(--terracotta-500)">${parts.join('')}</g>`;
        }
      }
      svgInner = `<g class="cv-profile-bars">${bars}</g>${overlayPath}${stemsGroup}${meanLine}`
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
    // Entropy headline stat (nats): Shannon for a discrete root,
    // differential for a continuous one.  The backend sends null when
    // the entropy evaluator declines the shape (guarded savepoint), so
    // the tile simply doesn't render there.
    // null (declined) must not read as 0 nats: Number(null) is 0.
    const entropy = profile.entropy == null ? NaN : Number(profile.entropy);
    const entropySpan = Number.isFinite(entropy)
      ? `<span class="cv-profile-stat"`
        + ` title="entropy in nats (Shannon for a discrete distribution,`
        + ` differential for a continuous one; Monte Carlo plug-in`
        + ` estimate for composite or conditioned shapes)">`
        + `H = ${escapeHtml(fmt(entropy))}</span>`
      : '';
    // The toggle stores the JSON payload on the panel so the click
    // handler can re-render without re-fetching from the server.
    const toggleLabel = _mode === 'cdf' ? 'CDF' : 'PDF';
    const toggleNext  = _mode === 'cdf' ? 'PDF' : 'CDF';
    // Data attributes carry the full range and current view so the
    // wheel / dblclick handlers wired by wireProfileInteractions can
    // re-render without recomputing from the histogram array.
    const fullLoAttr = fullLo;
    const fullHiAttr = fullHi;
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
      + entropySpan
      + `<button type="button" class="cv-profile-toggle"`
      + ` title="Switch to ${toggleNext}">${toggleLabel}</button>`
      + `</div>`
      + `<div class="cv-profile-svg cv-rv-density">${svg}</div>`
      + `</div>`;
  }

  // Sample panel: a <details> whose summary shows "N samples [of M]" and a
  // preview of the first values, whose body lists them all, plus a hint
  // when conditional rejection sampling accepted fewer than requested.
  function renderSamplePanel(result) {
    const r = result || {};
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
    return `<details class="cv-sample-panel">`
      + `<summary class="cv-sample-summary">`
      +   `<span class="cv-sample-count">${escapeHtml(countLabel)}</span>`
      +   `<code class="cv-sample-preview" `
      +        `title="First ${PREVIEW_N} values; click the chevron `
      +        `to see all.">`
      +     escapeHtml(preview)
      +   `</code>`
      + `</summary>`
      + `<pre class="cv-sample-full">${escapeHtml(fullList)}</pre>`
      + budgetHint
      + `</details>`;
  }

  window.ProvsqlStudio = window.ProvsqlStudio || {};
  window.ProvsqlStudio.renderProfilePanel = renderProfilePanel;
  window.ProvsqlStudio.wireProfileInteractions = wireProfileInteractions;
  window.ProvsqlStudio.renderSamplePanel = renderSamplePanel;
})();
