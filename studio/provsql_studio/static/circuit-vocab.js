/* circuit-vocab.js -- shared, DOM-free "circuit vocabulary" for the two
   provenance-DAG renderers: the interactive Circuit-mode canvas (circuit.js)
   and the compact notebook snapshot painter (notebook.js).  Both consume this
   single source, so they never drift on which wires get positional labels,
   what those labels say (mixture p/x/y, conditioned A/B, mobius coefficients,
   case guard/value/default, rv parameter symbols), or how an oversized node
   label is shrunk to fit.  Every function here is pure: (scene data) ->
   string / number, no DOM and no module state; the scene is passed in so the
   rv registry (scene.rv_families) and sibling wires (scene.edges/nodes) are
   available without a global. */
(function () {
  'use strict';

  // Gates whose child order matters, so their wires get positional labels.
  // cmp's lhs/rhs, monus's minuend/subtrahend, agg (order-sensitive fns),
  // arith (MINUS/DIV), mixture, conditioned, mobius, case, and a parametric
  // rv's parameter wires.
  const ORDERED_GATES = new Set(['cmp', 'monus', 'agg', 'arith', 'mixture',
                                 'conditioned', 'mobius', 'case', 'rv']);
  // = and <> are commutative; lhs/rhs digits add noise for those, as they
  // would for SUM/COUNT/etc.  The strict comparators keep the digits.
  const COMMUTATIVE_AGG = new Set(['sum', 'count', 'min', 'max', 'avg']);
  const COMMUTATIVE_CMP = new Set(['=', '<>', '!=']);
  // PROVSQL_ARITH_* tags whose result depends on argument order: 2 = MINUS,
  // 3 = DIV.  PLUS / TIMES commute; NEG has a single child.
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

  // gate_mixture wires.  Distinguish the categorical form structurally rather
  // than by wire count: a bimodal categorical (two outcomes) has only three
  // wires ([key, mul_1, mul_2]) yet is still categorical, while a classic
  // 3-wire mixture is [p_input, x_scalar, y_scalar].  The discriminator is the
  // types of the non-first wires -- all gate_mulinput in the categorical form.
  function mixtureEdgeLabel(parent, child_pos, scene) {
    if (scene && scene.edges && scene.nodes) {
      const nodes_by_id = {};
      for (const n of scene.nodes) nodes_by_id[n.id] = n;
      const children = scene.edges
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
          // Only the key wire has a distinguished role; the mulinputs are
          // unordered outcomes, so suppress their labels (return null).
          return child_pos === 1 ? 'key' : null;
        }
      }
    }
    // Classic 3-wire mixture: [p, x, y].
    return ({ 1: 'p', 2: 'x', 3: 'y' })[child_pos] ?? String(child_pos);
  }

  // gate_conditioned wires: the target A and the evidence B, plus -- for
  // discrete (uuid|uuid) conditioning -- their joint A/\B (so the circuit shows
  // P(A|B) = P(A/\B)/P(B)).  The rv|uuid / agg|uuid forms have just the two.
  function conditionedEdgeLabel(parent, child_pos) {
    return ({ 1: 'A', 2: 'B', 3: 'A∧B' })[child_pos] ?? null;
  }

  // gate_mobius wires: each child is one element of the inclusion-exclusion
  // expansion, carrying an integer coefficient.  The coefficients live in the
  // gate's extra, keyed by child UUID ("uuid:coeff ..." plus an optional
  // "L:<uuid>" naming the literal lineage child); render its signed coefficient.
  function mobiusEdgeLabel(parent, child_pos, scene) {
    if (!parent.extra) return null;
    const coeffs = {};
    let lineage = null;
    for (const tok of parent.extra.split(/\s+/)) {
      if (!tok) continue;
      if (tok.startsWith('L:')) { lineage = tok.slice(2); continue; }
      const i = tok.lastIndexOf(':');
      if (i > 0) coeffs[tok.slice(0, i)] = tok.slice(i + 1);
    }
    if (!(scene && scene.edges)) return null;
    const edge = scene.edges.find(
      e => e.from === parent.id && e.child_pos === child_pos);
    if (!edge) return null;
    if (lineage && edge.to === lineage) return 'lineage';
    const c = coeffs[edge.to];
    if (c == null) return null;
    const n = Number(c);
    if (Number.isFinite(n))
      return (n < 0 ? '−' : '+') + Math.abs(n);   // U+2212 minus sign
    return String(c);
  }

  // gate_case wires (1-based child_pos): [guard_1, value_1, ..., guard_k,
  // value_k, default].  The final (highest-position) wire is the ELSE default;
  // the rest alternate odd = guard, even = value.
  function caseEdgeLabel(parent, child_pos, scene) {
    let maxPos = child_pos;
    if (scene && scene.edges)
      for (const e of scene.edges)
        if (e.from === parent.id && e.child_pos > maxPos) maxPos = e.child_pos;
    if (child_pos === maxPos) return 'default';
    const i = Math.ceil(child_pos / 2);
    return (child_pos % 2 === 1) ? `guard ${i}` : `value ${i}`;
  }

  // gate_rv wires: a parametric (latent / compound) leaf wires one or more of
  // its distribution parameters as tokens.  The extra "kind:$0[,$1]" marks a
  // wired slot; the edge from wire (child_pos-1) fills parameter slot j -- label
  // it with that family's parameter symbol (mu / sigma / lambda ...) from the
  // scene's rv_families registry.  A bare (non-parametric) rv has no wires.
  function rvEdgeLabel(parent, child_pos, scene) {
    if (!parent.extra) return null;
    const m = String(parent.extra).match(/^\s*([a-zA-Z_]+)\s*:(.*)$/);
    if (!m) return null;
    const kind = m[1].toLowerCase();
    const slots = m[2].split(',').map(x => x.trim());
    const j = slots.indexOf('$' + (child_pos - 1));   // wire index = child_pos-1
    if (j < 0) return null;
    const reg = ((scene && scene.rv_families) || {})[kind];
    const names = reg && reg.param_names;
    return (names && names[j]) || null;
  }

  // The positional wire label for a parent gate's child.  Returns a string, or
  // null to suppress the label entirely (categorical-mixture mulinput wires, an
  // unmatched conditioned / mobius / rv wire).  Ordered gates without a
  // dedicated rule (cmp / monus / agg / arith) get the bare digit.
  function edgePosLabel(parent, child_pos, scene) {
    switch (parent.type) {
      case 'mixture':     return mixtureEdgeLabel(parent, child_pos, scene);
      case 'conditioned': return conditionedEdgeLabel(parent, child_pos);
      case 'mobius':      return mobiusEdgeLabel(parent, child_pos, scene);
      case 'case':        return caseEdgeLabel(parent, child_pos, scene);
      case 'rv':          return rvEdgeLabel(parent, child_pos, scene);
      default:            return String(child_pos);
    }
  }

  // Shrink a node label wider than the usable node diameter: given the current
  // font size (px), the measured label width, and the max width, return the
  // font size that fits, floored at 5.5px for legibility (unchanged when it
  // already fits).  The caller does the DOM measurement and applies the result.
  function fitLabelFontSize(currentPx, measuredWidth, maxW) {
    if (!(measuredWidth > maxW)) return currentPx;
    return Math.max(5.5, currentPx * maxW / measuredWidth);
  }

  window.ProvsqlCircuitVocab = {
    ORDERED_GATES,
    shouldLabelChildren,
    edgePosLabel,
    fitLabelFontSize,
  };
})();
