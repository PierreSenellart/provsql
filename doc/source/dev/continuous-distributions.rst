Continuous Distributions
========================

This page describes the architecture of ProvSQL's continuous
random-variable surface: the on-disk gates, the SQL composite
type, the planner-hook rewriter's classifier, the Monte Carlo
sampler, the RangeCheck / AnalyticEvaluator / Expectation chain,
the HybridEvaluator's simplifier and island decomposer, the
conditional-inference path, the aggregate dispatch, and the
Studio rendering hooks. The user-facing description lives in
:doc:`../user/continuous-distributions`.

Gate Types
----------

Three gate types are appended to the :cfunc:`gate_type` enum in
:cfile:`provsql_utils.h`, before the ``gate_invalid``
sentinel, with no renumbering of the existing values. The
companion ``provenance_gate`` SQL enum in
``sql/provsql.common.sql`` mirrors the C enum identically.

``gate_rv``
    Random-variable leaf. The gate's ``extra`` blob carries the
    distribution kind and parameters as text, parsed at load
    time:

    - ``"normal:μ,σ"`` for ``Normal(μ, σ)``;
    - ``"uniform:a,b"`` for ``Uniform[a, b]``;
    - ``"exponential:λ"`` for ``Exponential(λ)``;
    - ``"erlang:k,λ"`` for ``Erlang(k, λ)``.

    Categorical random variables share no ``gate_rv`` encoding;
    they are encoded as a block of ``gate_mulinput`` gates
    under a ``gate_mixture`` (see below).

``gate_arith``
    ``N``-ary arithmetic over scalar children. The operator tag
    lives in ``info1`` of the gate's ``GateInformation``:
    ``provsql_arith_op`` is ``PLUS = 0``, ``TIMES = 1``,
    ``MINUS = 2``, ``DIV = 3``, ``NEG = 4``. The enum is
    append-only: the values are persisted on disk and must not
    be renumbered.

``gate_mixture``
    Probabilistic mixture. The wire vector is
    ``[p, x, y]`` for a Bernoulli mixture (with ``p`` a Boolean
    gate and ``x``, ``y`` scalar RV roots) or
    ``[key, mul_1, …, mul_n]`` for a categorical block (with
    ``key`` a fresh ``gate_input`` anchoring the block and
    each ``mul_i`` a ``gate_mulinput`` carrying the
    outcome value in its ``extra`` and its probability via
    :sqlfunc:`set_prob`).

In addition, ``gate_value`` gains a *float8 mode*: the
``extra`` blob is parsed via ``extract_constant_double``
(:cfile:`having_semantics.cpp`) rather than the existing
``extract_constant_C`` integer-only path. Both paths coexist
so ``gate_value`` covers both the deterministic-numeric
mode used in HAVING sub-circuits and the
random-variable-constant mode used by :sqlfunc:`as_random`.

SQL Surface
-----------

The composite type ``random_variable`` mirrors the ``agg_token``
pattern (UUID + cached scalar). Its IO functions live in
:cfile:`random_variable_type.c`; the C++-side helpers
(``RandomVariable::parseExtra``, ``RandomVariable::mean``
and so on) live in :cfile:`RandomVariable.cpp` and are
the parsers consumed by every downstream evaluator.

Constructors are plpgsql functions in
``sql/provsql.common.sql``:
:sqlfunc:`normal`, :sqlfunc:`uniform`,
:sqlfunc:`exponential`, :sqlfunc:`erlang`,
:sqlfunc:`categorical`,
:sqlfunc:`mixture` (two overloads), and
:sqlfunc:`as_random` (three numeric overloads via the
``double precision`` form). They validate parameters and mint
the appropriate gate via :sqlfunc:`create_gate`,
``set_extra``, :sqlfunc:`set_prob`,
``set_infos``. Each constructor is ``VOLATILE`` to
prevent constant-folding under ``STABLE`` or ``IMMUTABLE`` from
collapsing two independent draws into a single shared gate.

Arithmetic operators ``+ - * / -`` on
``(random_variable, random_variable)`` resolve to
``random_variable_plus`` and siblings, each a
one-line SQL function that calls ``provenance_arith``
with the appropriate ``provsql_arith_op`` tag. Comparison
operators ``< <= = <> >= >`` resolve to placeholder procedures
that *raise* if executed; the planner hook intercepts every such
``OpExpr`` and rewrites it before the executor sees it (see the
classifier section below).

Implicit casts ``integer → random_variable``,
``numeric → random_variable``, ``double precision →
random_variable`` are declared explicitly so that
``WHERE rv > 2`` and ``WHERE 2.5 > rv`` resolve uniformly via the
``(rv, rv)`` operator declarations.

Planner-Hook Rewriting
----------------------

The transformation that lifts ``WHERE`` and join predicates on
``random_variable`` columns into the row's provenance circuit
lives in the same :cfunc:`provsql_planner` hook in
:cfile:`provsql.c` that handles deterministic
provenance tracking and the :cfunc:`agg_token` HAVING surface.

The central walker is :cfunc:`migrate_probabilistic_quals`. It
walks every qual in the input query and routes each into one of
four mutually-exclusive classes (the ``qual_class`` enum):

- ``QUAL_PURE_AGG``: the qual is built only from ``agg_token``
  comparators (the pre-existing HAVING pathway).
- ``QUAL_PURE_RV``: the qual is built only from
  ``random_variable`` comparators.
- ``QUAL_DETERMINISTIC``: the qual contains no probabilistic
  comparator and stays in the WHERE clause as ordinary SQL.
- A short tail of *mixed-error* classes flagged so the rewriter
  raises a clean diagnostic rather than producing a malformed
  circuit (e.g. a qual that conjoins a ``random_variable``
  comparator and an ``agg_token`` comparator in the same node).

For ``QUAL_PURE_RV`` quals, the rewriter mints a
``gate_cmp`` per comparator and conjoins its UUID into the
row's provenance via ``provenance_times``. The
comparator's float8-comparator OID is recovered via
``random_variable_cmp_oid``. The original
``OpExpr`` is dropped from the WHERE so the executor never reaches
the placeholder procedure.

For ``QUAL_PURE_AGG`` quals, the existing HAVING pathway
(:cfunc:`make_aggregation_expression`, dispatched on
``aggtype``) is reused with one extension: when the aggregate's
result type is ``OID_TYPE_RANDOM_VARIABLE``, the rewriter routes
through ``make_rv_aggregate_expression`` so the per-row
argument is wrapped in ``rv_aggregate_semimod`` (a
:sqlfunc:`mixture` over the row's provenance and the
identity for the aggregate, see *Aggregate Dispatch* below).

A short-cut handles the corner case of ``WHERE rv > 2`` on a
query that touches no provenance-tracked relation: there is
nothing to conjoin into, so the rewriter synthesises a
single-row FROM-less SELECT to host the gate_cmp, and the
result is a circuit that :sqlfunc:`probability_evaluate` reads
directly.

Monte Carlo Sampler
-------------------

The sampler implementation lives in
:cfile:`MonteCarloSampler.cpp`. A single
``MonteCarloSampler::run`` runs ``N`` iterations over an
:cfunc:`GenericCircuit`; per iteration it draws every reachable
``gate_rv`` leaf once (memoised in ``rv_cache_``) and
evaluates every reachable ``gate_input`` Bernoulli once
(memoised in ``bool_cache_``). The two caches ensure that
shared leaves are correctly coupled within an iteration.

The RNG is ``std::mt19937_64``, seeded from the
``provsql.monte_carlo_seed`` GUC: ``-1`` seeds from
``std::random_device``; any other value (including ``0``) is
used as a literal seed for reproducibility. The same RNG drives
the Bernoulli and continuous (gate_rv) sampling paths, so a
pinned seed reproduces both the discrete and continuous
components of a circuit's randomness.

``Sampler::evalScalar`` is the scalar dispatcher: it knows
how to sample ``gate_rv``, ``gate_value`` (float8 mode
parsed via ``extract_constant_double``),
``gate_arith`` (recursing on children and combining per
``info1``), and ``gate_mixture`` (sampling the Boolean
selector once via ``evalBool``, then recursing into the
chosen branch). The ``gate_agg`` arm calls back into the
aggregate evaluator with the per-iteration sampled values; this
is what unlocks HAVING+RV under Monte Carlo.

``Sampler::evalBool`` is the Boolean dispatcher: it walks the
Boolean wrappers (plus / times / monus / cmp / input / mulinput /
project / eq), and treats ``gate_delta`` as transparent: the
gate exists for the structural δ-semiring algebra but adds
no event to the rv_* event walker. The same transparency is
asserted in ``walkAndConjunctIntervals`` so the AND-conjunct
pass that backs RangeCheck behaves consistently.

RangeCheck
----------

:cfile:`RangeCheck.cpp` propagates support intervals
through ``gate_arith`` and tests every ``gate_cmp``
against the propagated interval. A comparator that is decidable
from the support alone (e.g. a Normal restricted to
``x > μ + 10σ``: the support of the LHS is
:math:`(-\infty, +\infty)` but every realisation is
overwhelmingly to the right of the RHS; or a bounded uniform
``x > b``: the support cap is ``b``, so the cmp is identically
false) collapses to a Bernoulli ``gate_input`` with
probability ``0`` or ``1``, transparent to every downstream
consumer.

The AND-conjunction pass ``walkAndConjunctIntervals`` walks a
``WHERE`` clause's conjunction and intersects the per-RV intervals
across conjuncts before running the per-comparator decision.
``reading > 1 AND reading < 3`` thus constrains a single normal
once, with the analytic CDF call evaluating both endpoints in a
single pass.

``compute_support`` is exposed as ``rv_support`` for
SQL-side use (:sqlfunc:`support` polymorphically dispatches on
type and routes ``random_variable`` here).

AnalyticEvaluator
-----------------

:cfile:`AnalyticEvaluator.cpp` computes the exact
probability of a ``gate_cmp`` whose two children are a
single-distribution scalar and a constant (or two
single-distribution scalars whose joint distribution is
analytically tractable, e.g. two independent normals via
``X − Y ∼ Normal(μ_X − μ_Y, σ_X² + σ_Y²)``).

The kernel is ``std::erf`` for the standard-normal CDF;
``std::log1p`` / ``std::expm1`` for the exponential CDF;
linear arithmetic for the uniform CDF; the regularised lower
incomplete gamma for the Erlang CDF. Equality and inequality on
continuous distributions collapse correctly: ``X = X`` is
identically true (handled in ``RangeCheck`` as a
zero-width interval identity), ``X = c`` for a continuous ``X``
is identically false.

Expectation Semiring
--------------------

:cfile:`Expectation.cpp` implements the
``"expectation"`` compiled semiring entry point (registered in
the FLOAT block of :cfile:`provenance_evaluate_compiled.cpp`;
add new entries there for sister semirings). It runs analytical
moment computation per distribution at leaves, then propagates
through ``gate_arith`` by closed-form rules:

- ``E[X + Y] = E[X] + E[Y]`` (always),
- ``E[X − Y] = E[X] − E[Y]`` (always),
- ``E[a · X] = a · E[X]`` (when one operand is a constant),
- ``E[X · Y] = E[X] · E[Y]`` (only when ``X`` and ``Y`` are
  structurally independent),
- ``Var[X + Y] = Var[X] + Var[Y]`` (independent),
- etc.

Structural independence is detected via a per-evaluation
``FootprintCache`` that memoises, per gate, the set of base
``gate_rv`` leaves reachable from it. Two gates whose
footprints are disjoint are independent; the cache speeds up the
check from quadratic to linear by sharing the leaf-set
computation across the recursion.

When no closed form applies, ``Expectation`` falls back to a
Monte-Carlo estimate using ``MonteCarloSampler``. The sample
count is ``provsql.rv_mc_samples``; setting it to ``0`` turns the
fallback into an exception so callers that need analytical
answers can detect the silent fallback.

HybridEvaluator
---------------

:cfile:`HybridEvaluator.cpp` is the orchestrator. Given a
circuit, it runs:

1. **Universal peephole pass** (``HybridEvaluator::simplify``)
   that folds family-preserving combinations into a single leaf:
   linear combinations of independent normals (``a·X + b·Y + c``
   into a single normal), sums of i.i.d. exponentials with the
   same rate (into an Erlang), shift-and-scale of a single normal
   through mixtures and categoricals, single-child arith roots,
   semiring-identity drops (``gate_one`` in TIMES,
   ``gate_zero`` in PLUS, …). The pass is invariant-preserving:
   every transformation produces a semantically equivalent
   circuit.

2. **Island decomposition** (``HybridEvaluator::decompose``)
   that splits a multi-cmp query into independent islands
   (connected components on a union-find over base-RV
   footprints). A single-cmp island marginalises to a Bernoulli
   ``gate_input`` via ``AnalyticEvaluator``. A multi-cmp
   island whose cmps share base RVs is enumerated via the joint
   table (the joint distribution of the shared base RVs is
   evaluated explicitly).

3. **Monotone-shared-scalar fast path** for the common shape of
   a single ``gate_rv`` shared across multiple monotone
   comparators (typical of range queries on a single column):
   the joint event reduces to an interval on the underlying
   scalar and one analytical CDF call per endpoint.

4. **Universal semiring-identity collapse** after RangeCheck
   has decided every decidable cmp.

The simplifier is gated by ``provsql.simplify_on_load`` for the
universal pass run at load time, and by the debug-only
``provsql.hybrid_evaluation`` (``GUC_NO_SHOW_ALL``) for the
in-evaluator hybrid path. End users have no reason to flip
``hybrid_evaluation``; it exists for developer A/B against the
unfolded path and as a bisection knob.

Conditional Evaluation
----------------------

:sqlfunc:`expected`, :sqlfunc:`variance`, :sqlfunc:`moment`,
:sqlfunc:`central_moment`, :sqlfunc:`support`,
:sqlfunc:`rv_sample`, and :sqlfunc:`rv_histogram` all accept an
optional ``prov uuid DEFAULT gate_one()`` argument. When ``prov``
resolves to anything other than ``gate_one()``, evaluation routes
through the joint-circuit loader.

``getJointCircuit`` (:cfile:`MMappedCircuit.cpp`)
builds a multi-rooted BFS over the union of the reachable gates
from both ``input`` and ``prov`` so shared ``gate_rv``
leaves between the two are loaded into a single
:cfunc:`GenericCircuit` and consequently couple correctly in the
Monte Carlo sampler's ``rv_cache_``. This is the *shared-atom
coupling invariant*: a conditioning event ``prov`` is only
meaningful relative to the random variables it references, and
those must be the same leaves the moment's evaluator sees.

The closed-form table is exhaustive for the single-distribution
shapes:

- **Normal**, truncated to any one-sided or two-sided interval,
  via the Mills-ratio formula and integration by parts.
- **Uniform** on the intersection of the support and the
  conditioning interval (mean and variance trivial).
- **Exponential** by memorylessness on a lower bound, plus
  truncation to a finite interval via the lower incomplete gamma.

For shapes outside the closed-form table, the conditional moment
is estimated by rejection sampling at ``provsql.rv_mc_samples``;
:sqlfunc:`rv_sample` emits a ``NOTICE`` (and
:sqlfunc:`rv_histogram` / :sqlfunc:`expected` raise) when the
acceptance rate drops below the requested ``n`` within the
budget, so the caller can either widen the budget or loosen the
conditioning.

Aggregate Dispatch
------------------

The :sqlfunc:`sum`, :sqlfunc:`avg`, and
:sqlfunc:`product` aggregates over ``random_variable``
all share ``sum_rv_sfunc`` as their state-transition
function (a ``uuid[]`` accumulator) and an
``INITCOND = '{}'`` so the FFUNC runs even on an empty group.
The per-aggregate FFUNCs differ in how they fold the accumulated
UUIDs into a final ``random_variable`` root:

- ``sum_rv_ffunc`` builds a single
  ``gate_arith`` ``PLUS`` over the per-row mixtures (or
  returns the single child for a singleton group, or
  :sqlfunc:`as_random` ``(0)`` for an empty group).
- ``avg_rv_ffunc`` constructs a parallel denominator
  array: for each mixture in the state, it pulls out the
  per-row provenance gate (the mixture's first child) and builds
  a matching ``mixture(prov_i, as_random(1), as_random(0))`` so
  the denominator sums to the count of *included* rows, then
  divides via a ``gate_arith`` ``DIV``. Empty group returns
  SQL ``NULL`` to match standard ``AVG``.
- ``product_rv_ffunc`` re-walks each mixture in the state
  and patches its else-branch from :sqlfunc:`as_random`
  ``(0)`` to :sqlfunc:`as_random` ``(1)`` (going through
  :sqlfunc:`mixture` rather than
  :sqlfunc:`create_gate` so the v5 hash of mixtures
  sharing the same ``(prov_i, X_i, as_random(1))`` triple
  collides correctly), then builds a
  ``gate_arith`` ``TIMES`` root.

The planner hook routes RV-returning aggregates through
``make_rv_aggregate_expression`` so each per-row argument
becomes :sqlfunc:`mixture` ``(prov_i, X_i,
provsql.as_random(0))`` *before* the SFUNC sees it; the
:sqlfunc:`avg` and :sqlfunc:`product` FFUNCs unpack this shape
to recover ``prov_i``. The dispatch is keyed on ``aggtype``
(the aggregate's result type OID) rather than ``aggfnoid`` so
the same routing works for any future RV-returning aggregate.

An earlier design considered an *M-polymorphic ``gate_agg``*
that would carry the full semimodule lift directly. We rejected
it because the mixture-of-mixtures shape composes through every
existing gate_arith / gate_mixture rule, while a new
M-polymorphic gate would have required a parallel evaluation
path in every analytical evaluator. The semimodule-of-mixtures
shape reuses what's there.

Studio Rendering
----------------

ProvSQL Studio's circuit canvas is class-based: every node
carries a ``node--<type>`` CSS class derived from the gate kind,
and the stylesheet at ``studio/provsql_studio/static/app.css``
gives each class its colour, glyph, and inline-text layout.
Adding the three new gate types meant:

- one entry per type in the server-side JSON serialiser at
  ``studio/provsql_studio/circuit.py`` (the ``label`` and
  ``children`` fields, plus the distribution-blob parsing for
  the per-leaf inline glyph);
- one branch in the client renderer at
  ``studio/provsql_studio/static/circuit.js`` to pick the
  ``node--rv`` / ``node--arith`` / ``node--mixture`` class and
  to emit the correct edge labels (``p`` / ``x`` / ``y`` for
  mixtures, the ``provsql_arith_op`` glyph for arith);
- a Circuit-mode fetch that consumes the
  :sqlfunc:`simplified_circuit_subgraph` SRF (a thin
  ``compute_simplified_circuit_subgraph`` wrapper that runs
  the universal peephole pass on a sub-BFS and returns the
  result as ``jsonb``), with the ``provsql.simplify_on_load``
  Config-panel toggle switching between the raw and folded
  views.

The eval-strip *Distribution profile*, *Sample*, *Moment*, and
*Support* entries call :sqlfunc:`rv_histogram`,
:sqlfunc:`rv_sample`, ``rv_moment`` and
``rv_support`` directly. The *Condition on* row-prov
auto-preset is a client-side feature: clicking a result cell
stamps the row's provenance UUID into the input and toggles the
*Conditioned by* badge active. Manual edits stick within a row;
row navigation resets the input to the new row's prov.
