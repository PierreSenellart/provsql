Continuous Distributions
========================

This page describes the architecture of ProvSQL's continuous
random-variable surface: the on-disk gates, the per-family
``Distribution`` class hierarchy and its rule registries, the SQL
composite type, the planner-hook rewriter's classifier, the Monte
Carlo sampler, the RangeCheck / AnalyticEvaluator / Expectation
chain, the HybridEvaluator's simplifier and island decomposer, the
conditional-inference path, the aggregate dispatch, and the Studio
rendering hooks. The user-facing description lives in
:doc:`../user/continuous-distributions`.

Gate Types
----------

Four gate types back the continuous surface, appended to the
:cfunc:`gate_type` enum in :cfile:`provsql_utils.h` before the
``gate_invalid`` sentinel, with no renumbering of the existing
values. The companion ``provenance_gate`` SQL enum in
``sql/provsql.common.sql`` mirrors the C enum identically. (The
conditioning marker ``gate_conditioned`` is shared with the
Boolean surface and described in :doc:`probability-evaluation`;
its interaction with the moment evaluators is covered in
*Conditional Evaluation* below.)

``gate_rv``
    Random-variable leaf. The gate's ``extra`` blob carries the
    distribution as text, ``<family>:<p1>[,<p2>]`` — e.g.
    ``"normal:2.5,0.5"`` for ``Normal(2.5, 0.5)`` or
    ``"exponential:2"`` for ``Exponential(2)``. The family token
    is *not* an enum: it is resolved at parse time against the
    distribution registry (see *The Distribution Class Hierarchy*
    below), so the set of valid blobs grows with the registered
    families — currently ``normal``, ``uniform``, ``exponential``,
    ``erlang``, ``gamma``, ``lognormal``, ``weibull``, ``pareto``,
    ``beta``.

    Categorical random variables share no ``gate_rv`` encoding;
    they are encoded as a block of ``gate_mulinput`` gates
    under a ``gate_mixture`` (see below). The discrete count
    families (Poisson, Binomial, …) are SQL-level constructors
    over that categorical encoding, not gate types.

``gate_arith``
    ``N``-ary arithmetic over scalar children. The operator tag
    lives in ``info1`` of the gate's ``GateInformation``:
    ``provsql_arith_op`` is ``PLUS = 0``, ``TIMES = 1``,
    ``MINUS = 2``, ``DIV = 3``, ``NEG = 4``, ``MAX = 5``,
    ``MIN = 6``, ``POW = 7``, ``LN = 8``, ``EXP = 9``.
    ``MAX`` / ``MIN`` are the n-ary order statistics behind
    ``greatest`` / ``least`` and the ``max`` /
    ``min`` aggregates. ``POW`` (binary, real branch only) and
    ``LN`` carry evaluation-time domain guards (see the sampler
    section); ``EXP`` is total. The enum is append-only: the
    values are persisted on disk and must not be renumbered.

``gate_mixture``
    Probabilistic mixture. The wire vector is
    ``[p, x, y]`` for a Bernoulli mixture (with ``p`` a Boolean
    gate and ``x``, ``y`` scalar RV roots) or
    ``[key, mul_1, …, mul_n]`` for a categorical block (with
    ``key`` a fresh ``gate_input`` anchoring the block and
    each ``mul_i`` a ``gate_mulinput`` carrying the
    outcome value in its ``extra`` and its probability via
    :sqlfunc:`set_prob`).

``gate_case``
    N-ary guarded selection over scalar children, with first-match
    semantics: the wire vector is
    ``[guard_1, value_1, …, guard_k, value_k, default]`` (odd
    length ``2k + 1``), and the gate's value is the ``value_i`` of
    the first guard (a Boolean event, typically a ``gate_cmp``)
    that holds, else the ``default``. It carries data only in its
    wires — no ``info`` / ``extra``, following the
    ``gate_conditioned`` precedent. Minted by
    ``provenance_case`` (and its ``random_variable``
    wrapper ``rv_case``), the target of the planner hook's
    lowering of SQL ``CASE`` over ``random_variable`` branches.
    It is a real arm in the MC sampler, in RangeCheck (support =
    union of the value branches), and in the Expectation
    footprint; like every measure-carrier gate it is refused by
    the general ``sr_*`` semirings (a guarded selection is not a
    semiring operation).

In addition, ``gate_value`` gains a *float8 mode*: the
``extra`` blob is parsed via ``extract_constant_double``
(:cfile:`having_semantics.cpp`) rather than the existing
``extract_constant_C`` integer-only path. Both paths coexist
so ``gate_value`` covers both the deterministic-numeric
mode used in HAVING sub-circuits and the
random-variable-constant mode used by :sqlfunc:`as_random`.

The Distribution Class Hierarchy
--------------------------------

``src/distributions/`` holds the per-family class hierarchy that
every evaluator dispatches through; no evaluator names a family.
The abstract interface is ``distributions/Distribution.h``;
the registries live in ``distributions/Distribution.cpp``;
each family is one self-contained implementation file
(``normal.cpp``, ``uniform.cpp``, ``exponential.cpp``,
``erlang.cpp``, ``gamma.cpp``, ``lognormal.cpp``,
``weibull.cpp``, ``pareto.cpp``, ``beta.cpp``) sharing only the
internal header ``DistributionCommon.h``.

A ``Distribution`` is a transient per-family view constructed
from a parsed spec by ``makeDistribution``. Its interface
groups into:

- **identity**: ``family()`` returns the interned
  ``DistributionFamily`` descriptor (name token, parameter count,
  display label, parameter symbols, factory) — descriptor-pointer
  equality *is* family identity; there is no family enum anywhere;
- **closed-form moments**: ``mean()``, ``variance()``,
  ``rawMoment(k)``, plus the optional ``truncatedRawMoment(lo,
  hi, k)`` and ``iidOrderStatMean(n, isMax)``;
- **density / distribution**: ``pdf(x)``, ``cdf(x)`` — a family
  that has no closed form returns NaN (the *NaN-as-undecided*
  contract: callers treat NaN as "fall back", never as a value);
  the optional ``quantile(p)`` returns ``nullopt`` when there is
  no elementary inverse;
- **ranges**: ``support()``, ``integrationRange()`` (a finite
  quadrature window), ``plotRange()`` (for Studio's density
  previews);
- **sampling**: ``sample(rng)``, plus the optional
  ``sampleTruncated(rng, lo, hi, n)`` for rejection-free
  conditioned draws;
- **structure**: ``affine(a, b)`` (the family's image under
  ``a·X + b``, or ``nullptr`` when the image leaves the family),
  ``asDirac()`` (degenerate point masses), and ``serialise()``
  (the on-disk ``extra`` text, inverse of
  :cfunc:`parse_distribution_spec`).

Alongside the family table, four *rule registries* capture the
pairwise closed forms, all keyed by family-name tokens and all
populated at static initialisation:

- the **comparator registry** maps a family pair to a
  ``P(X < Y)`` closed form, consulted by ``comparatorPairLess``
  (used by the AnalyticEvaluator; a miss falls through to the
  quadrature described below);
- the **sum-closure registry** maps a family pair to a rule
  folding a list of ``a·Z + b`` terms into a single distribution,
  consulted by ``closePlusTerms`` (used by the HybridEvaluator;
  this is how a same-rate Exponential / Erlang chain folds into
  one Erlang, and any linear combination of independent normals
  into one normal);
- the **product-closure registry** does the same for TIMES wires
  via ``closeProductFactors`` (independent lognormals multiply in
  log space);
- the **transform registry** maps a ``(transform, family)`` pair
  — transform names are the opcode-free strings ``"ln"`` /
  ``"exp"`` — to the image distribution, consulted by
  ``closeTransform`` (``exp(Normal) → Lognormal``,
  ``ln(Lognormal) → Normal``).

``numericQuantile`` (same file) is the family-agnostic fallback
inverse CDF: a monotone bisection of ``cdf()`` over the family's
``integrationRange``, used whenever ``quantile()`` declines
(Erlang, Gamma, Beta).

A family file self-registers: it defines one static
``DistributionFamily`` descriptor plus a
``DistributionFamilyRegistrar`` (and any
comparator/closure/product/transform registrar objects), all of
which run at static initialisation. **Adding a family is one new
self-registering** ``src/distributions/<name>.cpp`` **plus its
SQL constructor** in ``sql/provsql.common.sql`` — no shared
header, enum, parser, or evaluator is touched, and every readout
(moments, quantiles, sampling, comparisons, Studio rendering)
picks the family up through the registries.

``DistributionSpec`` (:cfile:`RandomVariable.h`) is the parsed
form of a ``gate_rv``'s ``extra`` blob: an interned
family-descriptor pointer plus up to two parameters.
:cfunc:`parse_distribution_spec` (:cfile:`RandomVariable.cpp`)
splits the blob on ``:``, resolves the token through the family
registry, and parses the comma-separated parameters per the
descriptor's arity; ``serialise()`` is its inverse. The
:cfunc:`analytical_mean` / ``analytical_variance`` /
``analytical_raw_moment`` helpers there are thin wrappers over
``makeDistribution(spec)->…``.

Not every family implements every optional capability, and the
evaluators degrade per capability, not per family: exact
truncated moments need ``truncatedRawMoment`` (Normal, Uniform,
Exponential, Lognormal, Weibull, Pareto, Beta); rejection-free
conditioned sampling needs ``sampleTruncated`` (the same list
minus Beta); elementary quantiles need ``quantile()`` (Normal —
a Beasley-Springer-Moro start polished to machine precision by
two Newton steps — Uniform, Exponential, Lognormal, Weibull,
Pareto; the rest bisect); closed-form i.i.d. order-statistic
means need ``iidOrderStatMean`` (Uniform, Exponential, Weibull,
Pareto). A missing capability falls through to quadrature,
bisection, or Monte Carlo as appropriate.

SQL Surface
-----------

The type ``random_variable`` is a thin wrapper around the UUID of
the provenance gate behind the variable: the UUID is the single
source of truth, and every downstream evaluator (``MonteCarloSampler``,
``AnalyticEvaluator``, ``Expectation``, ``RangeCheck``,
``HybridEvaluator``) dispatches on the gate it points at, parsing
the distribution from the gate's ``extra`` blob. Its IO functions
live in :cfile:`random_variable_type.c`; the C++-side parsing
helpers live in :cfile:`RandomVariable.cpp` (see the previous
section).

Constructors are PL/pgSQL functions in
``sql/provsql.common.sql``: :sqlfunc:`normal`,
:sqlfunc:`uniform`, :sqlfunc:`exponential`, :sqlfunc:`erlang`,
:sqlfunc:`gamma` (with :sqlfunc:`chi_squared` sugar; an integer
shape routes through :sqlfunc:`erlang`), :sqlfunc:`lognormal`,
:sqlfunc:`weibull` (``k = 1`` routes through
:sqlfunc:`exponential`), :sqlfunc:`pareto`, :sqlfunc:`beta`
(``Beta(1,1)`` routes through :sqlfunc:`uniform`),
:sqlfunc:`categorical`, :sqlfunc:`mixture` (two overloads), and
:sqlfunc:`as_random` (three numeric overloads via the
``double precision`` form). They validate parameters and mint
the appropriate gate via :sqlfunc:`create_gate`, ``set_extra``,
:sqlfunc:`set_prob`, ``set_infos``.

The discrete count families are pure-SQL constructors over the
categorical encoding: :sqlfunc:`poisson`, :sqlfunc:`binomial`,
:sqlfunc:`geometric`, :sqlfunc:`hypergeometric`, and
:sqlfunc:`negative_binomial` each enumerate their pmf by a
log-space recurrence (numerically stable at large parameters, no
``lgamma`` dependency) into
:sqlfunc:`categorical_from_log_pmf`, which subtracts the maximum
log-mass, drops outcomes below a ``1e-15`` relative tail,
renormalises, and calls :sqlfunc:`categorical`.
``categorical_from_log_pmf`` is itself public — the "arbitrary
user-defined discrete density" surface. Infinite supports are
truncated at the same relative tail; a 10000-outcome cap raises
with the suggested continuous approximation; degenerate
parameters route through :sqlfunc:`as_random`.

:sqlfunc:`rv_families` (:cfile:`rv_families.cpp`) exposes the
family registry as a SRF — one row per registered family with
its on-disk name token, parameter count, parameter-symbol array,
and display label. UI clients (Studio) read it so newly added
families render without a client release.

The fresh-randomness constructors (every distribution
constructor that mints an anonymous gate) are ``VOLATILE`` to
prevent constant-folding under ``STABLE`` or ``IMMUTABLE`` from
collapsing two independent draws into a single shared gate. The
deterministic constructors (:sqlfunc:`as_random` and the
three-argument ``mixture(p uuid, x random_variable,
y random_variable)`` overload, both of which mint a v5-derived
UUID keyed on their inputs) are ``IMMUTABLE``.

Arithmetic operators ``+ - * / -`` on
``(random_variable, random_variable)`` resolve to
``random_variable_plus`` and siblings, each a one-line SQL
function that calls ``provenance_arith`` with the appropriate
``provsql_arith_op`` tag. The transform surface follows the same
pattern: the ``^`` operator and :sqlfunc:`pow` / ``power``
resolve to ``random_variable_pow`` (opcode ``POW``),
:sqlfunc:`ln` and :sqlfunc:`exp` to their opcodes, and
:sqlfunc:`sqrt` is pure ``x ^ 0.5`` sugar with no opcode of its
own. ``greatest`` / ``least`` (quoted — they shadow
keywords) build ``MAX`` / ``MIN`` gates over their variadic
arguments, de-duplicating identical child gates first and
collapsing a single survivor to itself. Comparison operators
``< <= = <> >= >`` resolve to placeholder procedures that
*raise* if executed; the planner hook intercepts every such
``OpExpr`` and rewrites it before the executor sees it (see the
classifier section below).

Implicit casts ``integer → random_variable``,
``numeric → random_variable``, ``double precision →
random_variable`` are declared explicitly so that
``WHERE rv > 2`` and ``WHERE 2.5 > rv`` resolve uniformly via the
``(rv, rv)`` operator declarations — and so a scalar exponent in
``x ^ 0.5`` lifts to the ``(rv, rv)`` operator.

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

Two further rewrites piggyback on the same hook: an SQL ``CASE``
whose result branches are ``random_variable`` lowers to
``rv_case`` (a ``gate_case`` whose guards are the lifted
comparison events), and a call to the Boolean
:sqlfunc:`probability` overload whose argument carries a
probabilistic comparison is rewritten to
:sqlfunc:`probability_evaluate` over the comparison's event
token (a purely deterministic argument falls through to the SQL
body, ``predicate::integer::double precision``).

A short-cut handles the corner case of ``WHERE rv > 2`` on a
query that touches no provenance-tracked relation: there is
nothing to conjoin into, so the rewriter synthesises a
single-row FROM-less SELECT to host the gate_cmp, and the
result is a circuit that :sqlfunc:`probability_evaluate` reads
directly.

Monte Carlo Sampler
-------------------

The sampler implementation lives in
:cfile:`MonteCarloSampler.cpp`. The entry point
:cfunc:`monteCarloRV` runs ``N`` iterations over a
:cfunc:`GenericCircuit`; per iteration it draws every reachable
``gate_rv`` leaf once (memoised in ``scalar_cache_``) and
evaluates every reachable ``gate_input`` Bernoulli once
(memoised in ``bool_cache_``). The two per-iteration caches
ensure that shared leaves are correctly coupled within an
iteration. A third, cross-iteration cache (``dist_cache_``)
holds the per-gate ``Distribution`` object, constructed once via
``makeDistribution`` and reused for every draw, so the
blob-parsing and factory cost is paid once per gate rather than
once per sample.

The RNG is ``std::mt19937_64``, seeded from the
``provsql.monte_carlo_seed`` GUC: ``-1`` seeds from
``std::random_device``; any other value (including ``0``) is
used as a literal seed for reproducibility. The same RNG drives
the Bernoulli and continuous (gate_rv) sampling paths, so a
pinned seed reproduces both the discrete and continuous
components of a circuit's randomness.

``Sampler::evalScalar`` is the scalar dispatcher: it knows
how to sample ``gate_rv`` (via ``Distribution::sample``),
``gate_value`` (float8 mode parsed via
``extract_constant_double``), ``gate_arith`` (recursing on
children and combining per ``info1``, including ``MAX`` /
``MIN`` folds — shared base RVs stay coupled through the caches,
so ``max(x, y)`` with correlated ``x``, ``y`` is sampled
jointly), ``gate_mixture`` (sampling the Boolean selector once
via ``evalBool``, then recursing into the chosen branch), and
``gate_case`` (evaluating guards via ``evalBool`` and values via
``evalScalar`` in the same iteration; the first true guard wins,
else the default wire). The ``gate_agg`` arm calls back into the
aggregate evaluator with the per-iteration sampled values; this
is what unlocks HAVING+RV under Monte Carlo.

The transform opcodes carry *evaluation-time domain guards*: a
negative draw flowing into ``LN``, or a negative base drawn
together with a non-integer exponent under ``POW``, raises an
actionable error naming the ``greatest(x, 0)`` clamp — never a
silently dropped NaN, which would bias the estimate. Integer
exponents are total; ``ln(0)`` legitimately yields ``-∞``; NaN
operands (from an upstream guard-free source) propagate.

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

Interval propagation covers all ten arith opcodes: ``MAX`` /
``MIN`` take the corner max / min of the child intervals;
``EXP`` and ``LN`` map interval endpoints through the monotone
function (``LN`` clamping the interval at zero from below);
``POW`` over a non-negative base does corner analysis on the
``(base, exponent)`` box, and widens to all-real when the base
interval crosses zero. This keeps :sqlfunc:`support` readouts
and interval-decidable comparisons exact through the transform
surface. A ``gate_case``'s interval is the union of its value
branches (odd wires plus the default).

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

:cfile:`AnalyticEvaluator.cpp` computes the exact probability of
a decidable ``gate_cmp``. :cfunc:`runAnalyticEvaluator` snapshots
every ``gate_cmp`` in the circuit, calls ``tryAnalyticDecide``
per comparator, and on a non-NaN result collapses the gate to a
Bernoulli ``gate_input`` with that probability.
``tryAnalyticDecide`` recognises three shapes:

- **RV vs constant** (either order): ``cdfDecide`` evaluates the
  family CDF at the constant — ``F(c)`` for ``<`` / ``<=``,
  ``1 − F(c)`` for the mirrored operators. Equality on a
  continuous distribution is RangeCheck's job (below).
- **categorical vs constant**: ``categoricalDecide`` sums
  :sqlfunc:`get_prob` over the mulinputs satisfying the
  predicate; being discrete, ``=`` / ``<>`` are decided here,
  exactly.
- **RV vs RV** (two bare leaves): ``rvVsRvDecide`` consults the
  comparator registry through ``comparatorPairLess``. Same-family
  closed forms are registered for Normal (via the difference
  normal), Uniform, Exponential, Lognormal, Weibull (same
  shape), and Pareto (any parameter pair — the comparison the
  heavy-tailed quadrature grid handles worst). On a registry
  miss — including every *mixed-family* pair — the generic
  fallback ``quadraturePairLess`` computes
  :math:`P(X < Y) = \int (1 - F_Y(t))\, f_X(t)\, dt` by
  composite Simpson quadrature (4000 panels) over X's
  ``integrationRange``. A NaN anywhere (no finite integration
  window, a declined pdf/cdf) leaves the comparator to the Monte
  Carlo net.

Equality and inequality on continuous distributions collapse in
``RangeCheck``: ``X = X`` is identically true (a zero-width
interval identity), ``X = Y`` for any two sub-circuits of which
at least one has purely continuous support is identically false.
``hasOnlyContinuousSupport`` (in :cfile:`RangeCheck.cpp`) is the
predicate behind the second case: a recursive walk that returns
true on ``gate_rv`` leaves, on ``gate_arith`` whose every wire is
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
recursive :sqlfunc:`probability_evaluate` call) also bail. The
sum-product subsumes the disjoint-Dirac case as its boundary
(empty intersection ⇒ ``P(X = Y) = 0``).

Analytical Moment Evaluator
---------------------------

:cfile:`Expectation.cpp` implements the closed-form moment
evaluator for continuous-RV circuits. It is *not* a
``Semiring`` subclass: the
:cfunc:`provenance_evaluate_compiled_internal` dispatcher
special-cases ``semiring == "expectation"`` and calls
:cfunc:`compute_expectation` directly on the
``GenericCircuit``, bypassing the template-based
``GenericCircuit::evaluate<S>`` machinery used by the proper
semirings. The same entry point is reached by
:sqlfunc:`expected` over a ``random_variable`` and by the
``rv_moment`` C helper.

The algorithm runs analytical moment computation per distribution
at leaves (``Distribution::mean`` / ``variance`` /
``rawMoment``), then propagates through ``gate_arith`` by
closed-form rules:

- ``E[X + Y] = E[X] + E[Y]`` (always),
- ``E[X − Y] = E[X] − E[Y]`` (always),
- ``E[a · X] = a · E[X]`` (when one operand is a constant),
- ``E[X · Y] = E[X] · E[Y]`` (only when ``X`` and ``Y`` are
  structurally independent),
- ``Var[X + Y] = Var[X] + Var[Y]`` (independent),
- etc.

Divergent moments are reported honestly: a family whose k-th
moment does not exist (a Pareto with ``α ≤ k``) returns
``Infinity``, never an estimate.

The ``MAX`` / ``MIN`` order statistics get their own mean rules:
when the children are independent bare RVs of the same family
and parameters, ``Distribution::iidOrderStatMean`` supplies the
closed form (Uniform, Exponential, Weibull, Pareto implement
it); otherwise ``mixedOrderStatMean`` integrates the layer-cake
identities :math:`E[\max] = lo + \int (1 - \prod_i F_i)` /
:math:`E[\min] = lo + \int \prod_i (1 - F_i)` by composite
Simpson quadrature over a window covering every child's support
— still exact-grade for independent bare-RV children of *any*
family mix. Shared leaves, non-leaf children, or an undefined
CDF fall through to MC, as do order-statistic variances and
higher moments.

Two read-only registry consultations extend the closed-form
reach without rewriting the circuit (safe under shared-RV
identity): ``transform_image`` maps ``LN`` / ``EXP`` gates over
a single bare RV through the transform registry, and
``product_image`` maps a TIMES over independent same-family
leaves through the product registry; both the moment and the
quantile evaluators then read the image distribution's closed
forms directly.

Structural independence is detected via a per-evaluation
``FootprintCache`` that memoises, per gate, the set of base
``gate_rv`` leaves reachable from it (a ``gate_case``
contributes all its wires, like a ``gate_arith``). Two gates
whose footprints are disjoint are independent; the cache speeds
up the check from quadratic to linear by sharing the leaf-set
computation across the recursion.

When no closed form applies, :cfunc:`compute_expectation` falls
back to a Monte-Carlo estimate using ``MonteCarloSampler``. The
sample count is ``provsql.rv_mc_samples``; setting it to ``0``
turns the fallback into an exception so callers that need
analytical answers can detect the silent fallback.

Quantiles
^^^^^^^^^

The polymorphic :sqlfunc:`quantile` dispatcher routes a
``random_variable`` to ``rv_quantile``, whose C implementation
``compute_quantile`` (:cfile:`Expectation.cpp`) dispatches on
the root shape: a ``gate_value`` returns its constant; a bare
``gate_rv`` goes to ``analytic_dist_quantile``; a categorical
mixture takes the generalised inverse
:math:`F^{-1}(p) = \min\{v : F(v) \ge p\}` over its enumerated
outcomes; an ``LN`` / ``EXP`` / foldable ``TIMES`` root reads
its registry image (see above); anything else — and any
conditioned shape outside the truncated closed form — draws
``provsql.rv_mc_samples`` samples and interpolates the empirical
quantile with the same type-7 linear interpolation PostgreSQL's
``percentile_cont`` uses.

``analytic_dist_quantile`` handles the edges and truncation
uniformly: ``p ≤ 0`` / ``p ≥ 1`` return the (possibly truncated)
support edges; under interval conditioning the probability is
rescaled to :math:`u = F(lo) + p\,(F(hi) - F(lo))` before
inverting (a conditioning mass below ``1e-12`` falls to MC); the
inversion itself tries ``Distribution::quantile`` first and
``numericQuantile`` bisection second.

Covariance, correlation, standard deviation
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The same-row second-moment readouts are pure SQL over the moment
evaluator: :sqlfunc:`covariance` ``(x, y [, prov])`` is
``expected(x·y) − expected(x)·expected(y)``, :sqlfunc:`stddev`
is ``sqrt(variance(x))``, and :sqlfunc:`correlation` divides the
covariance by the product of standard deviations (``NULL`` on a
degenerate denominator via ``NULLIF``). Exactness rides on the
``FootprintCache`` path: structurally independent arguments give
an exact ``0`` covariance because ``E[XY]`` factors; shared
leaves make ``x·y`` a shared-leaf product whose mean is
correlation-aware (closed-form where the product rules apply, MC
otherwise). The optional ``prov`` conditioning argument is
threaded to every ``expected`` / ``variance`` call, so all
moments are computed under the same event.

HybridEvaluator
---------------

:cfile:`HybridEvaluator.cpp` is the orchestrator. Given a
circuit, it runs:

1. **Universal peephole pass** (:cfunc:`runHybridSimplifier`)
   that folds family-preserving combinations into a single leaf.
   The fold rules are registry-driven: ``try_sum_closure``
   collects a PLUS gate's wires as ``a·Z + b`` terms and consults
   the sum-closure registry (any linear combination of
   independent normals and constants into a single normal; a
   same-rate Exponential / Erlang chain into ``Erlang(Σk, λ)``;
   same-rate Gammas into a Gamma); ``try_product_closure``
   consults the product registry (independent lognormals fold in
   log space); ``try_transform_closure`` consults the transform
   registry (``exp(Normal) → Lognormal``, ``ln(Lognormal) →
   Normal`` — so a chain like ``exp(N_1 + N_2)`` collapses to
   one lognormal leaf). Scalar shifts, scalings, and negations
   of a single RV fold through ``Distribution::affine``; a
   family whose affine image leaves the family declines the fold
   and the shape stays a ``gate_arith`` (shifting or negating an
   exponential, for instance, flips or displaces its support).
   Around the registry rules sit the structural
   canonicalisations: MINUS-to-PLUS (so subtraction shapes flow
   through the same PLUS pipeline), DIV-to-TIMES for division by
   a constant, shift-and-scale pushed through mixtures and
   categoricals, single-child arith roots, semiring-identity
   drops (``gate_one`` in TIMES, ``gate_zero`` in PLUS…), and
   constant folding of deterministic subtrees — including
   ``POW`` / ``LN`` / ``EXP`` / ``MAX`` / ``MIN`` over
   constants, with domain-violating constants deliberately left
   unfolded so the sampler's guard fires. The pass is
   invariant-preserving: every transformation produces a
   semantically equivalent circuit. Out of scope: combinations
   with no registered rule — the sum of two *distinct* uniforms
   (triangular, not uniform), differing-rate exponential sums,
   min / max of RVs (only their means have closed forms, see
   above); these shapes stay as ``gate_arith`` and the MC
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

2. **Island decomposition** (:cfunc:`runHybridDecomposer`)
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
:sqlfunc:`central_moment`, :sqlfunc:`quantile`,
:sqlfunc:`support`, :sqlfunc:`rv_sample`, and
:sqlfunc:`rv_histogram` all accept an optional ``prov uuid
DEFAULT gate_one()`` argument. When ``prov`` resolves to anything
other than ``gate_one()``, evaluation routes through the
joint-circuit loader.

``getJointCircuit`` (:cfile:`MMappedCircuit.cpp`)
builds a multi-rooted BFS over the union of the reachable gates
from both ``input`` and ``prov`` so shared ``gate_rv``
leaves between the two are loaded into a single
:cfunc:`GenericCircuit` and consequently couple correctly in the
Monte Carlo sampler's per-iteration caches. This is the
*shared-atom coupling invariant*: a conditioning event ``prov``
is only meaningful relative to the random variables it
references, and those must be the same leaves the moment's
evaluator sees.

The closed-form table for truncated (interval-conditioned)
moments is a per-family capability, not a hard-coded list: a
family implementing ``Distribution::truncatedRawMoment`` gets
exact conditional moments (Normal via the Mills-ratio formula
and integration by parts, Uniform trivially on the intersected
interval, Exponential by memorylessness plus the lower
incomplete gamma, Lognormal, Weibull, Pareto — the latter via
tail self-similarity — and Beta via the incomplete-beta ratio).
Families that decline (Erlang, Gamma) fall through to the MC
path below. Extending the truncated surface to a new family
means implementing ``truncatedRawMoment`` (and, for exact
conditioned sampling, ``sampleTruncated``) in that family's
file — no detection or dispatch site changes.

For shapes outside the closed-form table, the conditional moment
is estimated by rejection sampling at ``provsql.rv_mc_samples``;
:sqlfunc:`rv_sample` emits a ``NOTICE`` (and
:sqlfunc:`rv_histogram` / :sqlfunc:`expected` raise) when the
acceptance rate drops below the requested ``n`` within the
budget, so the caller can either widen the budget or loosen the
conditioning.

``matchTruncatedSingleRv`` (in :cfile:`RangeCheck.cpp`) is the
single-RV shape-detection helper used by the moment surface
(``try_truncated_closed_form`` in :cfile:`Expectation.cpp`), the
quantile evaluator, and the rejection-free sampler
(:cfunc:`try_truncated_closed_form_sample`). It
runs the four common gates -- ``gate_rv`` root check,
``parse_distribution_spec``, ``collectRvConstraints``, and the
empty-intersection / ``gate_zero`` event guards -- so the
supported-shape set stays in sync between moments, quantiles,
and sampling.

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
``conditional_raw_moment`` / ``conditional_central_moment``
(:cfile:`Expectation.cpp`) calls it after
``try_truncated_closed_form`` returns ``nullopt`` and raises a
"conditioning event is infeasible" error directly when the
predicate fires, avoiding a 100 000-sample MC round whose
acceptance probability is exactly zero.

:sqlfunc:`rv_sample` and :sqlfunc:`rv_histogram` share
``MonteCarloSampler::try_truncated_closed_form_sample``: a
direct exact-sampling fast path that fires on a bare ``gate_rv``
with an interval-extractable event whenever the family
implements ``Distribution::sampleTruncated`` (Uniform draws
``U(lo, hi)`` on the intersected truncation; Exponential
one-sided uses memorylessness (``X | X > c = c + Exp(λ)``),
two-sided uses inverse-CDF via ``std::log1p`` / ``std::expm1``
for numerical accuracy near the support boundary; Normal uses
the inverse-CDF transform; Lognormal, Weibull, and Pareto invert
their exact quantiles). The fast path delivers exactly ``n``
samples with 100% acceptance even for tight tail events that
would otherwise starve the rejection budget. Families without
``sampleTruncated`` (Erlang, Gamma, Beta) and ``gate_arith``
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
a bare ``gate_rv`` of *any* registered family — the plot window,
density, and distribution come from ``Distribution::plotRange``
/ ``pdf`` / ``cdf``, so a new family gets curves for free; a
family that declines pdf/cdf on the grid (NaN) returns NULL —
plus ``as_random(c)`` Diracs, categorical mixtures, and
Bernoulli mixtures over any recursively-matched shape, all four
arms accepting a non-trivial conditioning event. ``gate_arith``
composites return NULL; the frontend renders histogram-only in
those cases.

Before matching, :sqlfunc:`rv_analytical_curves` runs
:cfunc:`runHybridSimplifier` so the curves see the same folded tree
that :sqlfunc:`simplified_circuit_subgraph` exposes to Studio's circuit
view: ``c·Exp(λ)`` folded to ``Exp(λ/c)``, sums of independent
normals folded to a single normal, ``exp(Normal)`` folded to its
lognormal image, ``c + mixture(p, X, Y)``
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
invoked from :cfunc:`CircuitFromMMap::applyLoadTimeSimplification`
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
``provsql.hybrid_evaluation`` GUC, which gates :cfunc:`runHybridSimplifier`
inside the probability and view paths where the simplifier owns
the rewritten subtree.

Aggregate Dispatch
------------------

The :sqlfunc:`sum`, :sqlfunc:`avg`, :sqlfunc:`product`,
:sqlfunc:`max`, and :sqlfunc:`min` aggregates over
``random_variable`` all share ``sum_rv_sfunc`` as their
state-transition function (a ``uuid[]`` accumulator) and an
``INITCOND = '{}'`` so the FFUNC runs even on an empty group.

The row-absence identity is baked into each per-row contribution
*upstream*, by the planner hook: ``make_rv_aggregate_expression``
wraps the per-row argument in ``rv_aggregate_semimod`` — a
:sqlfunc:`mixture` ``(prov_i, X_i, as_random(identity))`` — with
the identity dispatched per aggregate: ``0`` for :sqlfunc:`sum`
(the two-argument form), and, through the three-argument
identity-parameterised form, ``1`` for :sqlfunc:`product`,
``-Infinity`` for :sqlfunc:`max`, ``+Infinity`` for
:sqlfunc:`min` — a row absent in a world must not perturb the
fold, so it contributes the fold's identity. :sqlfunc:`avg` is
rewritten at the same site into the "AVG = SUM / COUNT"
identity, ``rv_sum_or_null(rv_aggregate_semimod(prov, x)) /
sum(rv_aggregate_indicator(prov))``: the numerator is the usual
provenance-weighted sum (``rv_sum_or_null`` differs from
:sqlfunc:`sum` only in returning ``NULL`` on an empty group so
the division propagates standard ``AVG`` semantics), and the
denominator sums per-row ``mixture(prov_i, 1, 0)`` indicators —
the count of *included* rows as a random variable.

With the identities pre-baked, the FFUNCs are plain folds with
no gate inspection: a single ``gate_arith`` root (``PLUS`` /
``TIMES`` / ``MAX`` / ``MIN``, the extremum pair through the
shared ``extremum_rv_ffunc``) over the accumulated UUIDs, a
singleton group collapsing to its single child, and per-aggregate
empty-group identities (``as_random(0)`` for :sqlfunc:`sum`,
``as_random(1)`` for :sqlfunc:`product`, ``as_random(∓∞)`` for
the extrema, SQL ``NULL`` for :sqlfunc:`avg`). On an
*untracked* call (no provenance to weight by) the direct
aggregates still work: every row is unconditionally present and
the raw per-row RVs are folded as-is.

The dispatch is keyed on ``aggtype`` (the aggregate's result
type OID) rather than ``aggfnoid`` so the same routing works for
any future RV-returning aggregate.

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
The continuous gate types map to:

- one entry per type in the server-side JSON serialiser at
  ``studio/provsql_studio/circuit.py`` (the ``label`` and
  ``children`` fields, plus the distribution-blob parsing for
  the per-leaf inline glyph);
- one branch in the client renderer at
  ``studio/provsql_studio/static/circuit.js`` to pick the
  ``node--rv`` / ``node--arith`` / ``node--mixture`` /
  ``node--case`` class and to emit the correct edge labels
  (``p`` / ``x`` / ``y`` for mixtures; the ``provsql_arith_op``
  glyph for arith, with ``pow(x, 0.5)`` rendered as ``√``;
  guard / value / default for ``gate_case``, whose node glyph is
  ``⇢``);
- a Circuit-mode fetch that consumes the
  :sqlfunc:`simplified_circuit_subgraph` SRF (a thin
  ``compute_simplified_circuit_subgraph`` wrapper that runs
  the universal peephole pass on a sub-BFS and returns the
  result as ``jsonb``), with the ``provsql.simplify_on_load``
  Config-panel toggle switching between the raw and folded
  views.

The RV-family rendering is registry-driven end to end: Studio
reads :sqlfunc:`rv_families` for each family's display label and
parameter symbols, and fetches its density preview as a
server-computed grid from :sqlfunc:`rv_analytical_curves` — so a
newly registered family renders correctly with no Studio change.

The eval-strip *Distribution profile*, *Sample*, *Moment*, and
*Support* entries call :sqlfunc:`rv_histogram`,
:sqlfunc:`rv_sample`, ``rv_moment`` and
``rv_support`` directly. The *Condition on* row-prov
auto-preset is a client-side feature: clicking a result cell
stamps the row's provenance UUID into the input and toggles the
*Conditioned by* badge active. Manual edits stick within a row;
row navigation resets the input to the new row's prov.
