Continuous Distributions
=========================

ProvSQL extends the probabilistic-database setting from discrete
Bernoulli inputs (see :doc:`probabilities`) to **first-class
continuous random variables**. Columns can carry distributions such
as ``Normal(μ, σ)``, ``Uniform(a, b)``, or ``Exponential(λ)``;
arithmetic and comparison work natively; the planner rewrites
``WHERE``, ``JOIN`` and ``UNION`` on random-variable columns
transparently; and ``expected``, ``variance``, ``moment``,
``support``, ``rv_sample`` and ``rv_histogram`` query the resulting
distributions, conditioning on filter predicates when asked.

.. note::

   Continuous-distribution support requires
   ``shared_preload_libraries = 'provsql'`` in ``postgresql.conf``;
   the planner hook performs all of the rewrites described below
   transparently. Discrete probabilities (:doc:`probabilities`) and
   continuous random variables coexist in the same circuit and the
   same query.

Introduction
------------

A *random-variable column* stores, in each row, a token referring to a
probability distribution rather than a single value. The token is a
composite ``random_variable`` (a UUID plus a cached scalar for
deterministic constants) that fits in any ``CREATE TABLE``:

.. code-block:: postgresql

    CREATE TABLE sensor_readings(
      id      int PRIMARY KEY,
      reading random_variable);

    INSERT INTO sensor_readings VALUES
      (1, provsql.normal(2.5, 0.5)),
      (2, provsql.uniform(1, 3)),
      (3, provsql.exponential(0.4));

    SELECT add_provenance('sensor_readings');

The remainder of this chapter uses this sensors example as a running
motivator. Each row carries a different kind of noise:

- sensor ``1`` is a calibrated unit with Gaussian measurement error
  centred at ``2.5``;
- sensor ``2`` is a cheap unit whose reading is uniformly distributed
  between ``1`` and ``3``;
- sensor ``3`` is a drift-prone unit whose reading is exponentially
  distributed with rate ``0.4``.

Filtering against a numeric threshold, the planner rewrites the
``WHERE`` clause into a conditioning event on each row's reading:

.. code-block:: postgresql

    SELECT id FROM sensor_readings WHERE reading > 2;
    -- id 1, 2, 3 selected with respective probabilities
    --   1 - Φ((2 - 2.5) / 0.5)  ≈ 0.84   (Normal CDF)
    --   (3 - 2) / (3 - 1)       =  0.50   (Uniform CDF)
    --   exp(-0.4 · 2)           ≈ 0.45   (Exponential survival)

The numeric value of these probabilities is recovered through the
provenance circuit (see :sqlfunc:`probability_evaluate` and
:sqlfunc:`provenance`); the query itself is written and read as
ordinary SQL.

Distribution Constructors
-------------------------

The constructors below all return a ``random_variable``. Each call
mints a fresh, independent random variable: two calls to
``provsql.normal(0, 1)`` produce two uncorrelated draws, not the
same draw. Use :sqlfunc:`mixture` (below) when you need
shared underlying randomness.

:sqlfunc:`normal` ``(mu, sigma)``
    ``Normal(μ, σ)`` with mean ``μ`` and standard deviation ``σ``.
    Both arguments must be finite; ``σ`` must be non-negative. The
    degenerate ``σ = 0`` case is silently routed through
    :sqlfunc:`as_random` to share the constant-token gate.

:sqlfunc:`uniform` ``(a, b)``
    ``Uniform[a, b]`` with bounds ``a ≤ b``. Both bounds must be
    finite. The degenerate ``a = b`` case is routed through
    :sqlfunc:`as_random`.

:sqlfunc:`exponential` ``(lambda)``
    ``Exponential(λ)`` with rate ``λ > 0`` and mean ``1/λ``. There
    is no degenerate form: ``λ = 0`` raises.

:sqlfunc:`erlang` ``(k, lambda)``
    ``Erlang(k, λ)`` is the sum of ``k`` independent
    ``Exponential(λ)`` draws (equivalently, the Gamma with integer
    shape). It is materialised as a single ``gate_rv`` so the
    closed-form CDF and moments fire directly, rather than the
    sampler having to draw ``k`` exponentials per iteration. The
    degenerate ``k = 1`` case is routed through
    :sqlfunc:`exponential` to share its gate.

:sqlfunc:`categorical` ``(probs, outcomes)``
    Discrete distribution over the values in ``outcomes`` with the
    corresponding probabilities in ``probs``. Both arrays must have
    the same length, every probability must be in ``[0, 1]``, and
    the probabilities must sum to ``1`` within ``1e-9``. A
    single-outcome categorical reduces to
    :sqlfunc:`as_random` at construction.

:sqlfunc:`mixture` ``(p, x, y)`` *(two overloads)*
    Bernoulli-weighted choice between two random variables: with
    probability ``p`` the result samples ``x``, otherwise ``y``.

    - The first overload takes ``p`` as the UUID of an existing
      Boolean gate (``input`` / ``mulinput`` / ``plus`` / ``times``
      / ``cmp`` / ``…``). Two mixtures that share the same ``p``
      token are *coupled*: they sample the same underlying coin per
      Monte-Carlo iteration. Use this overload when you want a
      family of mixtures that all switch together.
    - The second overload takes ``p`` as a plain ``double
      precision`` probability in ``[0, 1]`` and mints a fresh
      anonymous ``gate_input`` with that probability under the
      hood. Each call mints a new coin, so two
      ``mixture(0.5, X, Y)`` calls draw independently.

:sqlfunc:`as_random` ``(c)``
    Lift a numeric constant into a deterministic ``random_variable``
    (a Dirac point mass at ``c``). Three overloads exist
    (``double precision``, ``integer``, ``numeric``) and the same
    constants share a UUID. ``c = -0.0`` is canonicalised to
    ``+0.0`` so the two zeros refer to the same gate.

Implicit casts ``integer → random_variable``, ``numeric →
random_variable`` and ``double precision → random_variable``
are installed. Writing ``WHERE reading > 2`` works without an
explicit ``provsql.as_random(2)`` wrapper.

Arithmetic on Random Variables
------------------------------

The four arithmetic operators ``+``, ``-``, ``*``, ``/`` and unary
``-`` are declared on ``(random_variable, random_variable)`` and
return a fresh ``random_variable`` whose underlying gate is a
``gate_arith`` over the operand UUIDs. Mixing scalars and random
variables resolves through the implicit casts above:

.. code-block:: postgresql

    -- All of these are well-typed random_variable expressions.
    SELECT reading + 1            FROM sensor_readings;
    SELECT 2 * reading - 0.5      FROM sensor_readings;
    SELECT reading / reading      FROM sensor_readings;
    SELECT -reading               FROM sensor_readings;

The arithmetic operators are *structural*: they record the
operation in the circuit without evaluating it. Evaluation happens
later, when the value is queried via
:sqlfunc:`expected`, :sqlfunc:`variance`, :sqlfunc:`moment`,
:sqlfunc:`probability_evaluate`, :sqlfunc:`rv_sample`, or
:sqlfunc:`rv_histogram`. Two paths exist:

- **Closed-form**, when ProvSQL's hybrid evaluator recognises a
  family-preserving combination: a sum of independent normals is
  another normal; a scalar shift, scale, or negation of a normal
  preserves the family; the sum of ``k`` i.i.d. exponentials with
  the same rate is an Erlang; a linear combination of disjoint
  random variables has closed-form mean and variance. The result
  is exact.
- **Monte Carlo fallback**, when no closed form applies, e.g. a
  product of two non-trivial random variables. The sampler draws
  independent values from each leaf, evaluates the arithmetic
  expression per iteration, and aggregates the results. See
  *Configuration of the Monte Carlo sampler* below for the
  controlling GUCs.

Comparison operators ``<``, ``<=``, ``=``, ``<>``, ``>=``, ``>``
on ``(random_variable, random_variable)`` return ``boolean``
syntactically, but the planner hook intercepts every such
operator at planning time and rewrites it into a conditioning
event on the row's provenance. End users never invoke the
comparison procedures directly; the rewriter routes them through
``gate_cmp``.

Probabilistic Queries
---------------------

Filter predicates, joins, and unions on ``random_variable``
columns are rewritten transparently into operations on the
provenance circuit. The user writes ordinary SQL:

.. code-block:: postgresql

    SELECT id, provenance() AS prov
    FROM sensor_readings
    WHERE reading > 2;

The rewriter recognises ``reading > 2`` as a comparison on an
RV column, mints a ``gate_cmp`` for the comparison, and conjoins
its UUID into the row's ``provsql`` column. Querying the result
returns one row per source row whose underlying random-variable
event is satisfiable; the corresponding probability is recovered
through:

.. code-block:: postgresql

    SELECT id, probability_evaluate(provenance()) AS p
    FROM sensor_readings
    WHERE reading > 2;
    --  id |    p
    -- ----+--------
    --   1 | 0.8413
    --   2 | 0.5000
    --   3 | 0.4493

Comparisons between two random-variable columns work the same
way, with the rewriter conjoining a ``gate_cmp`` whose two children
are the two operand gates. ``JOIN`` predicates on RV columns
follow the standard ProvSQL rewriting, with the join condition
contributing a ``gate_cmp`` to the joined row's provenance.
``UNION ALL`` over RV-bearing relations produces the natural
``gate_plus`` over the two source rows' provenance.

Configuration of the Monte Carlo Sampler
-----------------------------------------

Two GUCs control the Monte Carlo fallback path. See
:doc:`configuration` for the full configuration reference.

``provsql.monte_carlo_seed`` (default: ``-1``)
    Seed for ``std::mt19937_64``. The default ``-1`` seeds from
    ``std::random_device`` for non-deterministic sampling. Any
    other value (including ``0``) is used as a literal seed and
    makes every Monte-Carlo result reproducible across runs and
    across the Bernoulli / continuous sampling paths.

``provsql.rv_mc_samples`` (default: ``10000``)
    Default sample count used by analytical fallbacks
    (:sqlfunc:`expected`, :sqlfunc:`variance`, :sqlfunc:`moment`,
    :sqlfunc:`rv_histogram`, :sqlfunc:`rv_sample` under
    conditioning) when they cannot decompose a sub-circuit and
    must fall back to Monte Carlo. Set to ``0`` to disable the
    fallback entirely: callers will raise rather than sample.

The sample count for ``probability_evaluate(..., 'monte-carlo',
n)`` is independent and explicit in the third argument.

Closed-Form Evaluation
----------------------

Three pieces work together to keep evaluation analytical where
possible:

- **RangeCheck** propagates support intervals through ``gate_arith``
  and tests every ``gate_cmp`` against the propagated interval. A
  comparator that is decidable from the support alone collapses
  to a Bernoulli ``gate_input`` with probability ``0`` or ``1``;
  the rest of the circuit sees a plain Boolean leaf. Joint
  ``WHERE`` clauses are intersected per random variable: ``reading
  > 1 AND reading < 3`` constrains a single normal once and runs
  the conjunction as one analytic CDF call.
- **AnalyticEvaluator** computes the exact CDF of a single
  distribution's ``gate_cmp`` (e.g. ``Normal > 2``,
  ``Uniform <= 1.5``, ``Exponential >= λ⁻¹``) via the standard
  CDFs of the supported families. Equality and inequality on
  continuous distributions collapse correctly (``X = X`` is
  identically true, ``X <> X`` identically false).
- **HybridEvaluator** simplifies the in-memory circuit before
  evaluation: linear closure on normals (``a·X + b·Y + c`` is a
  single normal when ``X``, ``Y`` are independent normals), i.i.d.
  exponentials sum to Erlang, ``c·X``-style shifts and scales
  thread through mixtures and categoricals, single-child arith
  roots and semiring identities collapse.

``provsql.simplify_on_load`` (default: ``on``) folds the universal
peephole pass at the moment a circuit is read into memory, so
every downstream consumer (semiring evaluators, Monte Carlo,
``view_circuit``, PROV export, ProvSQL Studio) sees the
simplified form. Toggle it off only to inspect raw gate-creation
structure for debugging.

Moments and Support
-------------------

Five polymorphic dispatchers cover the moment surface; they accept
``random_variable``, plain ``uuid``, ``numeric``, and ``agg_token``
inputs and dispatch internally.

:sqlfunc:`expected` ``(input [, prov [, method [, arguments]]])``
    Expectation ``E[input | prov]``. For a ``random_variable``,
    runs the ``Expectation`` semiring with structural-independence
    detection on ``gate_arith TIMES``; for an ``agg_token``,
    evaluates the discrete expectation over the gate's underlying
    inclusion-indicator world. Defaults to the unconditional
    expectation when ``prov`` is omitted (the default is
    ``gate_one()``).

:sqlfunc:`variance` ``(input [, prov [, method [, arguments]]])``
    Variance ``Var[input | prov]``. The ``random_variable`` path
    computes the central moment of order two analytically when
    the closed form is available, falling back to Monte Carlo
    otherwise.

:sqlfunc:`moment` ``(input, k [, prov [, method [, arguments]]])``
    Raw moment ``E[input^k | prov]``. ``k`` must be a non-negative
    integer. ``k = 0`` returns ``1``; ``k = 1`` is equivalent to
    :sqlfunc:`expected`.

:sqlfunc:`central_moment` ``(input, k [, prov [, method [, arguments]]])``
    Central moment ``E[(input − E[input | prov])^k | prov]``.
    ``k = 0`` returns ``1``; ``k = 1`` returns ``0``; ``k = 2`` is
    equivalent to :sqlfunc:`variance`.

:sqlfunc:`support` ``(input [, prov [, method [, arguments]]])``
    Support interval ``[lo, hi]``. For a ``random_variable``,
    propagates each leaf's support through ``gate_arith`` via
    interval arithmetic and intersects per-variable bounds from
    ``prov``; for plain numeric input, returns the degenerate
    point ``[c, c]``; for an ``agg_token``, returns the
    closed-form support of the aggregation function.

End-to-end on the sensors fixture:

.. code-block:: postgresql

    SELECT id,
           expected(reading)   AS mean,
           variance(reading)   AS var,
           support(reading)    AS supp
    FROM sensor_readings;

The expectation, variance, and support of ``provsql.normal(2.5,
0.5)`` come out exactly as ``2.5``, ``0.25``, and
``(-Infinity, +Infinity)``; the uniform's as ``2``, ``1/3``, and
``(1, 3)``; the exponential's as ``2.5``, ``6.25``, and
``(0, +Infinity)``.

**Structural-independence shortcuts.** Sums of independent random
variables have exact expectation and variance; products of
random variables with disjoint *footprints* (the set of base
``gate_rv`` leaves reachable from each operand) have exact
expectation (``E[XY] = E[X]·E[Y]``). The hybrid evaluator detects
both shapes through a per-evaluation ``FootprintCache``; otherwise
it falls back to Monte Carlo with budget
``provsql.rv_mc_samples``.

Conditional Inference
---------------------

The moment dispatchers above all accept an optional ``prov uuid``
argument that conditions the moment on the provenance event
``prov``. The natural source of ``prov`` in a tracked query is
the :sqlfunc:`provenance` pseudo-column: every ``WHERE`` filter
on a random-variable column has already been lifted into the
row's provenance, so passing ``provenance()`` conditions on the
filter:

.. code-block:: postgresql

    SELECT id,
           expected(reading, provenance()) AS cond_mean,
           variance(reading, provenance()) AS cond_var
    FROM sensor_readings
    WHERE reading > 2;

For sensor ``1`` (``Normal(2.5, 0.5)`` truncated to ``> 2``),
the conditional mean is the textbook Mills-ratio formula
``μ + σ · φ(α) / (1 − Φ(α))`` with ``α = (2 − μ)/σ``; for sensor
``2`` (``Uniform[1, 3]`` truncated to ``> 2``), the conditional
distribution is ``Uniform[2, 3]`` with mean ``2.5``; for sensor
``3`` (``Exponential(0.4)`` truncated to ``> 2``), the
memoryless property gives conditional mean ``2 + 1/0.4 = 4.5``.

Three closed-form paths are wired:

- **Normal**, truncated to any one-sided or two-sided interval,
  via the Mills-ratio formula and integration by parts.
- **Uniform**, on the intersection of the support and the
  conditioning interval (mean and variance trivial in closed
  form).
- **Exponential**, by memorylessness when the conditioning event
  is a lower bound, and by truncation to a finite interval.

When no closed form applies, the joint circuit between ``input``
and ``prov`` is loaded with shared ``gate_rv`` leaves correctly
coupled, and the conditional moment is estimated by rejection
sampling. The sample count is ``provsql.rv_mc_samples``; if
fewer than ``n`` accepted samples land within the budget
(because the conditioning event is rare), a ``NOTICE`` is
emitted suggesting the user widen the budget. Setting
``provsql.rv_mc_samples = 0`` turns the notice into an error.

Passing ``gate_one()`` (the default) as ``prov`` is equivalent to
the unconditional moment, so an unconditional call has no extra
cost.

Sampling and Histograms
-----------------------

Two functions expose raw and binned samples for inspection or
downstream analytics.

:sqlfunc:`rv_sample` ``(token, n [, prov])`` ``RETURNS SETOF float8``
    Draw up to ``n`` accepted Monte-Carlo samples from the scalar
    sub-circuit rooted at ``token``, conditioning on the
    provenance event ``prov`` (defaulting to unconditional). The
    function is a set-returning function. Shared ``gate_rv``
    leaves between ``token`` and ``prov`` are loaded into a
    single joint circuit so the conditioning event's draw and the
    value's draw share their per-iteration state.

    A ``NOTICE`` is emitted when the acceptance rate yields fewer
    than ``n`` accepted samples within the
    ``provsql.rv_mc_samples`` budget; the SRF returns whatever
    samples were accepted so the caller can proceed with a
    smaller batch.

:sqlfunc:`rv_histogram` ``(token, bins [, prov])`` ``RETURNS jsonb``
    Empirical histogram of the same scalar sub-circuit as
    :sqlfunc:`rv_sample`, returned as a JSON array of
    ``{bin_lo, bin_hi, count}`` objects. The number of bins is
    ``bins`` (default ``30``); the bin range covers the observed
    ``[min, max]`` of the draws; the sample count is
    ``provsql.rv_mc_samples``. Pin ``provsql.monte_carlo_seed``
    for reproducibility.

    Accepted root gates are the scalar ones: ``gate_value``
    (single bin), ``gate_rv``, and ``gate_arith``. Any other gate
    kind raises.

Example, drawing 200 samples from the truncated sensor-1 reading:

.. code-block:: postgresql

    SET provsql.monte_carlo_seed = 42;
    SELECT s
    FROM sensor_readings sr,
         LATERAL provsql.rv_sample(
                   provsql.random_variable_uuid(sr.reading),
                   200,
                   sr.provsql) AS t(s)
    WHERE sr.id = 1;

Mixtures and Categorical Random Variables
------------------------------------------

Probabilistic *mixtures* and *categorical* random variables are the
discrete-by-mixture side of the surface.

A **Bernoulli mixture** selects between two random variables based
on a Boolean coin. The two overloads of :sqlfunc:`mixture`
differ in whether the coin is shared:

.. code-block:: postgresql

    -- Two mixtures coupled through a shared coin: they always
    -- pick the same side per Monte-Carlo iteration.
    WITH coin AS (
      SELECT provsql.create_input_gate(0.3) AS p)
    SELECT
      provsql.mixture((SELECT p FROM coin),
                      provsql.normal(0, 1),
                      provsql.normal(10, 1))   AS shared_a,
      provsql.mixture((SELECT p FROM coin),
                      provsql.uniform(-1, 1),
                      provsql.uniform(9, 11))  AS shared_b;

    -- Two ad-hoc mixtures: each mints its own fresh coin.
    SELECT
      provsql.mixture(0.3, provsql.normal(0, 1),
                           provsql.normal(10, 1)) AS independent_a,
      provsql.mixture(0.3, provsql.uniform(-1, 1),
                           provsql.uniform(9, 11)) AS independent_b;

A **categorical** random variable assigns explicit probabilities
to a list of outcomes:

.. code-block:: postgresql

    -- 0 with probability 0.2, 1 with probability 0.5, 2 with 0.3
    SELECT provsql.categorical(
             ARRAY[0.2, 0.5, 0.3]::double precision[],
             ARRAY[0, 1, 2]::double precision[]);

Categoricals share UUIDs through a v5 hash of their wires, so two
``provsql.categorical(probs, outcomes)`` calls with the same
arrays produce the same gate. Calls with the same arrays but
distinct fresh ``gate_input`` keys (in the underlying block
encoding) remain independent; in practice the constructor
mints a fresh key per call, so two calls with the same arrays
produce two *independent* categorical draws.

.. note::

   The simplifier does **not** auto-collapse a cascade of Dirac
   mixtures into a single categorical: that conversion is
   reserved for explicit user calls to
   :sqlfunc:`categorical`. If you want a categorical, ask
   for one; if you build a tower of mixtures, the circuit keeps
   the tower shape so its structural sharing remains intact.

Aggregation Over Random Variables
---------------------------------

Three aggregates lift the standard arithmetic aggregates from
deterministic scalars to ``random_variable`` columns:

:sqlfunc:`sum` ``(random_variable)`` ``RETURNS random_variable``
    Provenance-weighted sum
    :math:`\sum_i \mathbf{1}\{\varphi_i\} \cdot X_i`, materialised
    as a single ``gate_arith PLUS`` over the per-row mixture
    gates. The empty-group identity is :sqlfunc:`as_random`
    ``(0)`` (the additive identity).

:sqlfunc:`avg` ``(random_variable)`` ``RETURNS random_variable``
    Provenance-weighted average
    :math:`(\sum_i \mathbf{1}\{\varphi_i\} \cdot X_i) /
    (\sum_i \mathbf{1}\{\varphi_i\})`, materialised as a single
    ``gate_arith DIV`` over two ``gate_arith PLUS`` subtrees. The
    empty-group identity is SQL ``NULL`` (matching standard SQL
    ``AVG``).

:sqlfunc:`product` ``(random_variable)`` ``RETURNS random_variable``
    Provenance-weighted product
    :math:`\prod_{i : \varphi_i} X_i`, materialised as a
    ``gate_arith TIMES`` over per-row mixtures whose else-branch
    is :sqlfunc:`as_random` ``(1)`` (the multiplicative
    identity, so rows with false provenance contribute ``1``).
    The empty-group identity is :sqlfunc:`as_random`
    ``(1)``.

The aggregates lower to *semimodule-of-mixtures* shape via
``rv_aggregate_semimod``: each per-row argument ``X_i`` is
wrapped in ``provsql.mixture(prov_i, X_i, as_random(identity))``
so the aggregate's effective semantics become the natural lift of
the semimodule-provenance machinery to RV-valued semimodules. The
``INITCOND = '{}'`` convention on all three aggregates guarantees
the FFUNC runs even on an empty group, which is what lets the
empty-group identity be controlled per aggregate.

.. note::

   ``AVG`` returns ``NaN`` when every row's provenance is false
   (zero divided by zero). The numerator and denominator are
   structurally correct; the result is the natural floating-point
   ``0/0`` rather than an error. If you need ``NULL`` on empty
   effective groups, filter by ``probability_evaluate(provenance())
   > 0`` before averaging.

The following aggregates over ``random_variable`` are not yet
supported: ``MIN``, ``MAX``, ``stddev``, ``covar_pop``,
percentile aggregates. ``COUNT`` over a tracked
``random_variable`` column goes through the standard ``COUNT``
path on the ``provsql`` UUID column.

Studio Integration
------------------

ProvSQL Studio (:doc:`studio`) surfaces three Circuit-mode features
specifically for continuous distributions:

- **Distribution profile**: ``μ`` and ``σ²`` headline stats with an
  inline-SVG histogram, a PDF/CDF toggle, per-bar tooltip, and
  wheel zoom. Backed server-side by :sqlfunc:`rv_histogram`.
- **Conditioning input** with row-provenance auto-preset: clicking
  a result cell stamps the row's provenance into the
  *Condition on* input so every subsequent moment, sample or
  histogram evaluates the conditional shape automatically. Toggle
  the *Conditioned by* badge off to fall back to the unconditional
  answer.
- **Simplified-circuit rendering** driven by
  ``provsql.simplify_on_load`` so the in-memory peephole-folded
  graph is what you see.

See :doc:`studio` for the full feature surface.

Out of Scope / Open Follow-ups
------------------------------

The following are deliberately out of scope at the time of
writing and tracked as separate follow-ups:

- ``EXCEPT`` and ``SELECT DISTINCT`` on relations that carry
  ``random_variable`` columns.
- ``MIN``, ``MAX``, percentile aggregates over
  ``random_variable``; the broader covariance family
  (``covar_pop``, ``stddev`` …) and other Tier B aggregates.
- Where-provenance crossed with random variables (the
  column-level tracking layered on top of an RV-bearing query is
  not yet defined).
- An in-Studio distribution editor.
