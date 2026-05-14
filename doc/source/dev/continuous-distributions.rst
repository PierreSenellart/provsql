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

The type ``random_variable`` is a thin wrapper around the UUID of
the provenance gate behind the variable: the UUID is the single
source of truth, and every downstream evaluator (``MonteCarloSampler``,
``AnalyticEvaluator``, ``Expectation``, ``RangeCheck``,
``HybridEvaluator``) dispatches on the gate it points at, parsing
the distribution from the gate's ``extra`` blob. Its IO functions
live in :cfile:`random_variable_type.c`; the C++-side helpers
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
zero-width interval identity), ``X = Y`` for any two sub-circuits
of which at least one has a continuous distribution is identically
false. ``RangeCheck::hasOnlyContinuousSupport`` is the predicate
behind the second case: a recursive walk that returns true on
``gate_rv`` leaves, on ``gate_arith`` whose every wire is
continuous, and on Bernoulli mixtures whose two branches are both
continuous; false on ``gate_value`` (Dirac), on categorical
mixtures (point masses at each outcome), and on Boolean / agg
gates. The widened test catches heterogeneous-rate exponential
sums (``Exp(λ_1) + Exp(λ_2)`` with ``λ_1 ≠ λ_2``, no Erlang
closure), products of two independent continuous RVs, and mixed
``gate_arith`` composites that the simplifier cannot fold to a
single ``gate_rv`` -- their equality predicate would otherwise
have flowed all the way down to the MC marginalisation only to
return 0 in finite precision anyway.

When neither side is purely continuous, a second analytical path
in ``RangeCheck`` fires: ``collectDiracMassMap`` extracts the
``(value → mass)`` map from each side (recursing into
categoricals and Bernoulli mixtures of ``as_random`` /
``gate_value`` branches), and the cmp resolves exactly via the
independent-Dirac sum-product

  ``P(X = Y) = Σ_{v ∈ M_X ∩ M_Y} M_X(v) · M_Y(v)``.

Continuous components on either side contribute zero by
measure-zero arguments (continuous vs Dirac and continuous vs
continuous), so they need not appear in the sum. Independence is
required for the factoring to hold; ``collectRandomLeaves`` walks
both sides for the union of ``gate_rv`` + ``gate_input`` leaves
and the shortcut bails on any overlap (e.g. two mixtures sharing
a Bernoulli ``p_token``). Bernoulli mixtures whose ``p_token`` is
a compound Boolean (whose static probability would require a
recursive ``probability_evaluate`` call) also bail. The
sum-product subsumes the disjoint-Dirac case as its boundary
(empty intersection ⇒ ``P(X = Y) = 0``).

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
   same rate (into an Erlang), affine combinations of a single
   uniform (``a·U(p, q) + c`` into ``U(a·p + c, a·q + c)`` with
   bounds reordered when ``a < 0``), closed-form negation of a
   bare Normal or Uniform (``-N(μ, σ)`` into ``N(-μ, σ)``,
   ``-U(a, b)`` into ``U(-b, -a)``), MINUS-to-PLUS canonicalisation
   so subtraction shapes flow through the same PLUS pipeline,
   DIV-to-TIMES canonicalisation for division by a constant
   (``X / c`` rewritten as ``(1/c) · X`` so the existing
   normal-family and uniform-family scaling rules apply),
   shift-and-scale of a single normal through mixtures and
   categoricals, single-child arith roots, semiring-identity drops
   (``gate_one`` in TIMES, ``gate_zero`` in PLUS, …). The pass is
   invariant-preserving: every transformation produces a
   semantically equivalent circuit. Out of scope: subtraction
   between two distinct continuous RVs of the same family
   (``U - U`` is triangular, not uniform), shifting an exponential
   or Erlang (``Exp + c`` has the wrong support), and negating an
   exponential or Erlang (``-Exp`` flips the support to
   ``(-∞, 0]``); these shapes stay as ``gate_arith`` and the MC
   sampler handles them per-iteration.

   *Peephole* is borrowed from compiler engineering (McKeeman,
   CACM 8(7), 1965): a small sliding window over consecutive
   instructions / gates, a fixed list of local pattern ->
   replacement rules, iterated to a fixed point. Each rule here
   looks at one ``gate_arith`` plus its immediate children,
   never further, matching the original scope. Contrast with
   ``RangeCheck`` (:cfile:`RangeCheck.cpp`), which propagates a
   data-flow fact (the support interval) through the whole
   circuit, and with the island decomposer below, which uses a
   global union-find over base-RV footprints.

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

``matchTruncatedSingleRv`` (in :cfile:`RangeCheck.cpp`) is the
single-RV shape-detection helper used by the moment surface
(``Expectation::try_truncated_closed_form``) and the
rejection-free sampler
(``MonteCarloSampler::try_truncated_closed_form_sample``). It
runs the four common gates -- ``gate_rv`` root check,
``parse_distribution_spec``, ``collectRvConstraints``, and the
empty-intersection / ``gate_zero`` event guards -- so the
supported-shape set stays in sync between moments and sampling
and adding a fifth (e.g. truncated Erlang via the regularised
incomplete gamma) only touches one detection site.

``matchClosedFormDistribution`` (same file) generalises the
single-RV matcher to the four-arm variant
``std::variant<TruncatedSingleRv, DiracShape, CategoricalShape,
BernoulliMixtureShape>`` consumed by
:sqlfunc:`rv_analytical_curves`. The variant covers, in addition
to the bare-RV case, ``as_random(c)`` Diracs (``gate_value``
roots), categorical-form ``gate_mixture`` roots, and classic
Bernoulli ``gate_mixture`` roots over any recursively-matched
shape. Conditioning is honoured uniformly across all four arms:
non-trivial events are routed through ``collectRvConstraints``
to extract a ``[lo, hi]`` interval on the root variable, then
``truncateShape`` is applied recursively -- bare RVs intersect
their bounds and renormalise via the truncated CDF; Diracs are
kept iff the value falls inside the interval (otherwise the
event is infeasible); categoricals drop outcomes outside the
interval and renormalise surviving masses; Bernoulli mixtures
recursively truncate their arms and reweight the Bernoulli by
the ratio of arm masses
(:math:`\pi' = \pi Z_L / (\pi Z_L + (1-\pi) Z_R)`), with the
arm masses computed by ``shape_mass`` (a parallel recursive
pass that integrates the unconditional CDF over the interval).
An arm with zero post-truncation mass is eliminated and the
mixture degenerates to the surviving arm.

``eventIsProvablyInfeasible`` (also :cfile:`RangeCheck.cpp`) is
the conditional-moment dispatcher's pre-MC short-circuit:
``gate_zero`` events are detected for every root type, and
``gate_rv`` roots additionally surface
``collectRvConstraints``-empty intersections. The dispatcher in
``Expectation::conditional_raw_moment`` /
``conditional_central_moment`` calls it after
``try_truncated_closed_form`` returns ``nullopt`` and raises a
"conditioning event is infeasible" error directly when the
predicate fires, avoiding a 100 000-sample MC round whose
acceptance probability is exactly zero.

:sqlfunc:`rv_sample` and :sqlfunc:`rv_histogram` share
``MonteCarloSampler::try_truncated_closed_form_sample``: a
direct exact-sampling fast path that fires on the same shape as
the moment surface (bare ``gate_rv`` of Uniform / Normal /
Exponential with an interval-extractable event). Uniform draws
``U(lo, hi)`` on the intersected truncation; Exponential
one-sided uses memorylessness (``X | X > c = c + Exp(λ)``),
two-sided uses inverse-CDF via ``std::log1p`` / ``std::expm1``
for numerical accuracy near the support boundary; Normal uses
inverse-CDF transform with ``std::erf`` for the forward CDF and
the Beasley-Springer-Moro rational approximation for the
inverse. The fast path delivers exactly ``n`` samples with 100%
acceptance even for tight tail events that previously starved
the rejection budget. Erlang truncation and ``gate_arith``
composite roots fall through to MC rejection unchanged.

:sqlfunc:`rv_analytical_curves` (in :cfile:`RvAnalyticalCurves.cpp`)
exposes the closed-form PDF, CDF, and discrete stems as sampled
data for ProvSQL Studio's Distribution profile overlay. Returns
NULL when the root sub-circuit is not a closed-form shape, so
callers can dispatch it in parallel with :sqlfunc:`rv_histogram`
without a structural pre-check. The payload has three optional
fields:

* ``pdf`` -- evenly-spaced ``{x, p}`` samples of the continuous
  density. Absent when the shape has no continuous component
  (pure Dirac, pure categorical, or nested mixture of those).
* ``cdf`` -- same grid as ``pdf``, cumulative probability.
  Always emitted (well-defined for any supported shape; a pure-
  discrete shape produces a staircase, a continuous shape a
  smooth curve, and a mixed shape a smooth curve with jumps at
  the stem positions).
* ``stems`` -- ``{x, p}`` point masses produced by Dirac roots,
  categorical roots, or Dirac / categorical arms inside a
  Bernoulli mixture. Bernoulli weights propagate down the path
  (a Dirac inside ``mixture(0.3, X, c)`` appears at ``(c, 0.7)``).

The supported shape set is the union of
``matchClosedFormDistribution`` 's variant arms (see above):
bare ``gate_rv`` of Normal / Uniform / Exponential /
integer-shape Erlang, ``as_random(c)`` Diracs, categorical
mixtures, and Bernoulli mixtures over any recursively-matched
shape -- all four arms accept a non-trivial conditioning event.
``gate_arith`` composites and non-integer Erlang shapes return
NULL; the frontend renders histogram-only in those cases.

Before matching, :sqlfunc:`rv_analytical_curves` runs
``runHybridSimplifier`` so the curves see the same folded tree
that ``simplified_circuit_subgraph`` exposes to Studio's circuit
view: ``c·Exp(λ)`` folded to ``Exp(λ/c)``, sums of independent
normals folded to a single normal, ``c + mixture(p, X, Y)``
pushed inside the mixture, etc. Without this pass a circuit
that displays as a single ``Exp(0.5)`` node would still be
seen by the matcher as a ``gate_arith`` composite of
``value(2)`` and ``gate_rv:Exp(1)`` and would silently fall
back to histogram-only.

Truncation under a bare RV normalises the PDF by
:math:`Z = \text{CDF}(\text{hi}) - \text{CDF}(\text{lo})` and
rescales the CDF to ``[0, 1]`` over the conditioning interval.
Under a Bernoulli mixture the truncated PDF is
:math:`f_{M|A}(x) = (\pi \cdot Z_L \cdot f_{L|A}(x) +
(1-\pi) \cdot Z_R \cdot f_{R|A}(x)) / (\pi Z_L + (1-\pi) Z_R)`
with the per-arm normalisers
:math:`Z_L, Z_R` computed by ``shape_mass``. Under a categorical
the conditional masses are :math:`p_i \cdot
\mathbb{1}\{v_i \in A\} / \sum_j p_j \cdot \mathbb{1}\{v_j \in A\}`.
A Dirac is invariant under any feasible event.

The load-time pass ``runConstantFold`` (in ``HybridEvaluator.cpp``,
invoked from ``CircuitFromMMap::applyLoadTimeSimplification``
alongside ``runRangeCheck`` and ``foldSemiringIdentities``)
folds deterministic ``gate_arith`` subtrees to ``gate_value``
at load time. This lifts the common parser shape
``arith(NEG, value:c)`` (produced when SQL parses
``-c::random_variable`` as ``-(c::random_variable)``) into a
clean ``value:-c``, so ``asRvVsConstCmp`` and friends recognise
the comparator's constant side without callers having to
parenthesise. The pass runs only the constant-fold rule from the
hybrid simplifier, never the family closures or identity drops,
because the result is always a ``gate_value`` that carries no
random identity, so no shared-RV coupling is decoupled by the
rewrite. The family closures stay behind the separate
``provsql.hybrid_evaluation`` GUC, which gates ``runHybridSimplifier``
inside the probability and view paths where the simplifier owns
the rewritten subtree.

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
