# Draft release notes for ProvSQL Studio 1.1.0

Staging file. The maintainer hand-merges this into
`studio/CHANGELOG.md` at tag time. Delete after release.

## [1.1.0]

Companion release for ProvSQL extension 1.5.0. Adds first-class
rendering and inspection for the continuous-distribution gate
family, conditional-inference workflow, simplified-circuit
rendering, and a new evaluator strip group for distribution
profile / sample / moment / support.

### Highlights

- **New gate-type renderers**: `gate_rv` leaves with
  distribution-kind glyphs (*N* / *U* / *E* / *Γ*); `gate_arith`
  with the operator glyph drawn from `info1`; `gate_mixture`
  with three labelled edges (`p` / `x` / `y` for Bernoulli
  mixtures, `key` plus one mulinput edge per outcome for
  categorical blocks) and the Bernoulli probability rendered
  inline in the parent circle.
- **Distribution profile evaluator**: new entry under the
  *Distribution* optgroup of the eval strip. Header stats
  (`μ`, `σ²`), inline-SVG histogram, PDF/CDF toggle, per-bar
  tooltip with `σ`-markers, wheel zoom. Backed server-side by
  the new C entry point `rv_histogram`.
- **Sample evaluator**: second *Distribution* entry, drawing
  conditional samples via `rv_sample(token, n, prov)`. Renders
  as a `<details>` panel with a six-value inline preview and a
  "show full list" expander; when the MC acceptance rate
  truncates the run below the requested `n`, surfaces an
  actionable hint pointing at `provsql.rv_mc_samples`.
- **Moment evaluator**: third *Distribution* entry exposing
  `moment` / `central_moment` with `k` and raw/central
  selectors.
- **Support evaluator**: fourth *Distribution* entry showing the
  closed-form `support` interval (or its conservative
  all-real fallback).
- **Conditioning**: `Condition on` text input on the eval strip
  with row-provenance auto-preset (clicking a result-table cell
  stamps the row's provenance UUID). Toggleable
  `Conditioned by:` badge underneath; clicking the active badge
  clears the conditioning and reverts to the unconditional
  answer, clicking the muted badge restores the row prov.
  Manual edits stick within a row and reset on row navigation.
- **Simplified-circuit rendering**: Circuit mode honours the new
  `provsql.simplify_on_load` extension GUC and renders the
  in-memory simplified graph via the
  `simplified_circuit_subgraph` SRF; the Config-panel toggle
  switches between the raw, gate-creation view and the
  simplified, evaluation-time view.
- **Config-panel rows**: new sliders/toggles for
  `monte_carlo_seed`, `rv_mc_samples`, and `simplify_on_load`,
  matching the existing Provenance-section pattern.
- **Anonymous-input rendering**: anonymous `gate_input`
  probabilities render as a percentage in the node circle;
  tracked-table input gates continue to render `ι` per the
  existing iota convention.
- **Categorical and mixture inspector**: node-inspector entries
  for the two new gate shapes; single-outcome categoricals
  collapse to `as_random` in the simplifier.

### Demo and tests

- `studio/scripts/demo_continuous.{sh,py}` fixture loader for
  the sensors / air-quality narrative used in
  `doc/source/user/casestudy6.rst`.
- Playwright e2e at `studio/tests/e2e/test_continuous.py`
  covering `gate_rv` / `gate_arith` rendering, the
  *Distribution profile* evaluator, the *Sample* panel, the
  conditioning auto-preset, and a Monte-Carlo
  `p ∈ (0, 1)` smoke test on the sensors fixture.

### Compatibility

Minimum required extension version: **1.5.0**. See the
[compatibility matrix](https://provsql.org/docs/user/studio.html#compatibility)
in the user guide for the full table.
