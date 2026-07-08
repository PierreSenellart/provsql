Continuous Distributions
=========================

ProvSQL extends the probabilistic-database setting from discrete
Bernoulli inputs (see :doc:`probabilities`) to **first-class
continuous random variables**. Columns can carry distributions such
as ``normal(μ, σ)``, ``uniform(a, b)``, or ``exponential(λ)``;
arithmetic and comparison work natively; the planner rewrites
``WHERE``, ``JOIN`` and ``UNION`` on random-variable columns
transparently; and ``expected``, ``variance``, ``moment``,
``quantile``, ``support``, ``rv_sample`` and ``rv_histogram`` query the
resulting distributions, conditioning on filter predicates when asked. Conditioning a
random variable -- ``x | (x > k)``, which truncates and renormalises its
distribution -- uses the same ``|`` operator that conditions discrete events
and aggregates; see :doc:`conditioning`.

Introduction
------------

A *random-variable column* stores, in each row, a token referring to a
probability distribution rather than a single value. The token is a
``random_variable``, a thin wrapper around the UUID of a provenance
gate, that fits in any ``CREATE TABLE``:

.. code-block:: postgresql

    CREATE TABLE sensor_readings(
      id      int PRIMARY KEY,
      reading random_variable);

    INSERT INTO sensor_readings VALUES
      (1, normal(2.5, 0.5)),
      (2, uniform(1, 3)),
      (3, exponential(0.4));

    SELECT add_provenance('sensor_readings');

The :sqlfunc:`add_provenance` call is *optional*. A ``random_variable``
column is already a provenance token, so every query in this chapter --
the comparisons, the moments, the conditioning -- works without it.
What ``add_provenance`` adds is a Boolean provenance token for each
*row*, so ordinary tuple-level uncertainty (a row that may or may not be
present) composes with the random-variable events in the same circuit:
it is the bridge between discrete ProvSQL provenance
(:doc:`probabilities`) and random variables, and lets the two coexist in
a single query. The call is kept here to show that interface.

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
provenance circuit (see :sqlfunc:`probability` and
:sqlfunc:`provenance`); the query itself is written and read as
ordinary SQL.

Distribution Constructors
-------------------------

The constructors below each return a ``random_variable``; every call mints a
fresh, independent variable (use :sqlfunc:`mixture` when two draws must share
underlying randomness). The tables give each family's support, what is
computed in closed form, and any closure (sum / product / min stability); the
Reference column links to Wikipedia.

**Continuous parametric**

.. list-table::
   :header-rows: 1
   :widths: 24 12 10 24 30

   * - Distribution
     - Reference
     - Support
     - Closed form
     - Notes
   * - :sqlfunc:`normal` ``(mu, sigma)``
     - `Normal <https://en.wikipedia.org/wiki/Normal_distribution>`__
     - ``R``
     - moments, CDF, quantiles, truncated moments; **sum-closed**
     - ``sigma = 0`` -> Dirac via :sqlfunc:`as_random`
   * - :sqlfunc:`uniform` ``(a, b)``
     - `Uniform <https://en.wikipedia.org/wiki/Continuous_uniform_distribution>`__
     - ``[a, b]``
     - all exact
     - ``a = b`` -> Dirac
   * - :sqlfunc:`exponential` ``(lambda)``
     - `Exponential <https://en.wikipedia.org/wiki/Exponential_distribution>`__
     - ``[0, inf)``
     - all exact; **sum-closed** (Erlang)
     - mean ``1/lambda``; ``lambda = 0`` raises
   * - :sqlfunc:`erlang` ``(k, lambda)``
     - `Erlang <https://en.wikipedia.org/wiki/Erlang_distribution>`__
     - ``[0, inf)``
     - moments, CDF; same-rate **sum-closed**
     - sum of ``k`` ``Exp(lambda)``; ``k = 1`` -> :sqlfunc:`exponential`
   * - :sqlfunc:`gamma` ``(k, lambda)``
     - `Gamma <https://en.wikipedia.org/wiki/Gamma_distribution>`__
     - ``(0, inf)``
     - moments, CDF (lower incomplete gamma); same-rate **sum-closed**
     - mean ``k/lambda``; integer ``k`` -> :sqlfunc:`erlang`
   * - :sqlfunc:`chi_squared` ``(k)``
     - `Chi-squared <https://en.wikipedia.org/wiki/Chi-squared_distribution>`__
     - ``(0, inf)``
     - via gamma
     - sugar for ``gamma(k/2, 0.5)``
   * - :sqlfunc:`lognormal` ``(mu, sigma)``
     - `Log-normal <https://en.wikipedia.org/wiki/Log-normal_distribution>`__
     - ``(0, inf)``
     - moments, CDF, quantiles, truncated moments; **product-closed**
     - ``exp``/``ln`` bridges to :sqlfunc:`normal`; ``sigma = 0`` -> Dirac
   * - :sqlfunc:`logistic` ``(mu, s)``
     - `Logistic <https://en.wikipedia.org/wiki/Logistic_distribution>`__
     - ``R``
     - mean, variance, CDF (sigmoid), quantiles
     - the logit link; mean ``mu``
   * - :sqlfunc:`weibull` ``(k, lambda)``
     - `Weibull <https://en.wikipedia.org/wiki/Weibull_distribution>`__
     - ``[0, inf)``
     - moments, CDF, quantiles, truncated moments; **min-stable**
     - scale ``lambda``; ``k = 1`` -> :sqlfunc:`exponential`
   * - :sqlfunc:`pareto` ``(xm, alpha)``
     - `Pareto <https://en.wikipedia.org/wiki/Pareto_distribution>`__
     - ``[xm, inf)``
     - moments, CDF, quantiles, truncated moments (any params); **min-stable**
     - heavy tail; divergent moments reported as ``Infinity``
   * - :sqlfunc:`beta` ``(alpha, beta)``
     - `Beta <https://en.wikipedia.org/wiki/Beta_distribution>`__
     - ``[0, 1]``
     - moments, CDF (incomplete beta), truncated moments
     - ``beta(1,1)`` -> :sqlfunc:`uniform`
   * - :sqlfunc:`inverse_gamma` ``(alpha, beta)``
     - `Inverse-gamma <https://en.wikipedia.org/wiki/Inverse-gamma_distribution>`__
     - ``(0, inf)``
     - moments, CDF (upper incomplete gamma)
     - ``1/Gamma``; divergent moments reported as ``Infinity``
   * - :sqlfunc:`inverse_gaussian` / :sqlfunc:`wald` ``(mu, lambda)``
     - `Inverse Gaussian <https://en.wikipedia.org/wiki/Inverse_Gaussian_distribution>`__
     - ``(0, inf)``
     - moments (all finite), CDF (via ``Phi``)
     - Brownian first-passage; ratio-``lambda/mu^2`` **sum-closed**

**Discrete parametric**

Enumerated into a categorical gate via :sqlfunc:`categorical_from_log_pmf`
(log-space, stable at large parameters; itself usable for a custom pmf).
Moments, quantiles, and comparisons -- including exact ``=`` / ``<>`` point
masses -- are exact over the enumerated support; an infinite support is
truncated at a ``1e-15`` relative-mass tail, and a ``10000``-outcome cap
raises. Degenerate parameters (``poisson(0)``, ``binomial(n, 1)``, ...) route
through :sqlfunc:`as_random`.

.. list-table::
   :header-rows: 1
   :widths: 30 14 12 36

   * - Distribution
     - Reference
     - Support
     - Notes
   * - :sqlfunc:`poisson` ``(lambda)``
     - `Poisson <https://en.wikipedia.org/wiki/Poisson_distribution>`__
     - ``0, 1, ...``
     - rate ``lambda``
   * - :sqlfunc:`binomial` ``(n, p)``
     - `Binomial <https://en.wikipedia.org/wiki/Binomial_distribution>`__
     - ``0..n``
     - ``n`` trials, success ``p``
   * - :sqlfunc:`geometric` ``(p)``
     - `Geometric <https://en.wikipedia.org/wiki/Geometric_distribution>`__
     - ``1, 2, ...``
     - number of trials (support starts at 1)
   * - :sqlfunc:`negative_binomial` ``(r, p)``
     - `Negative binomial <https://en.wikipedia.org/wiki/Negative_binomial_distribution>`__
     - ``0, 1, ...``
     - failures before the ``r``-th success; real ``r > 0``
   * - :sqlfunc:`hypergeometric` ``(pop_n, k_marked, n)``
     - `Hypergeometric <https://en.wikipedia.org/wiki/Hypergeometric_distribution>`__
     - ``0..n``
     - draws without replacement

**Nonparametric and structured**

.. list-table::
   :header-rows: 1
   :widths: 34 14 12 34

   * - Distribution
     - Reference
     - Support
     - Notes
   * - :sqlfunc:`as_random` ``(c)``
     - `Degenerate <https://en.wikipedia.org/wiki/Degenerate_distribution>`__
     - ``{c}``
     - Dirac point mass; also the implicit numeric -> ``random_variable`` casts
   * - :sqlfunc:`categorical` ``(probs, outcomes)``
     - `Categorical <https://en.wikipedia.org/wiki/Categorical_distribution>`__
     - finite
     - all exact; ``probs`` sum to 1 within ``1e-9``
   * - :sqlfunc:`mixture` ``(p, x, y)``
     - `Mixture <https://en.wikipedia.org/wiki/Mixture_distribution>`__
     - union of arms
     - ``p`` as a gate UUID (coupled coin) or a ``[0,1]`` scalar (fresh coin)
   * - :sqlfunc:`gmm` ``(weights, means, stddevs)``
     - `Mixture model <https://en.wikipedia.org/wiki/Mixture_model>`__
     - ``R``
     - Gaussian mixture; exact moments and sampling; comparisons ride Monte Carlo
   * - :sqlfunc:`empirical_samples` ``(samples)``
     - `Empirical <https://en.wikipedia.org/wiki/Empirical_distribution_function>`__
     - sample values
     - ecdf via categorical; exact moments/quantiles; <= 10000 distinct
   * - :sqlfunc:`empirical_cdf` ``(grid, cdf)``
     - `Empirical <https://en.wikipedia.org/wiki/Empirical_distribution_function>`__
     - ``[grid_1, grid_n]``
     - piecewise-linear CDF; exact moments/sampling; comparisons ride Monte Carlo

Implicit casts ``integer → random_variable``, ``numeric →
random_variable`` and ``double precision → random_variable``
are installed. Writing ``WHERE reading > 2`` works without an
explicit ``as_random(2)`` wrapper.

The full list of registered parameterized families is introspectable
with :sqlfunc:`rv_families`, which returns one row per family with its
name token, parameter count, conventional parameter symbols, and a
short display label. UI clients (such as :doc:`ProvSQL Studio
<studio>`'s circuit inspector) read it so that parameterized families
added to the extension render without a client upgrade.

Arithmetic on Random Variables
------------------------------

The arithmetic operators ``+``, ``-``, ``*``, ``/``, ``^`` and unary
``-`` are declared on ``(random_variable, random_variable)`` and
return a fresh ``random_variable`` whose underlying gate is a
``gate_arith`` over the operand UUIDs. Mixing scalars and random
variables resolves through the implicit casts above:

.. code-block:: postgresql

    -- All of these are well-typed random_variable expressions.
    SELECT reading + 1            FROM sensor_readings;
    SELECT 2 * reading - 0.5      FROM sensor_readings;
    SELECT -reading               FROM sensor_readings;
    SELECT reading ^ 0.25         FROM sensor_readings;
    SELECT r1.reading / r2.reading
      FROM sensor_readings r1, sensor_readings r2
     WHERE r1.id < r2.id;

Beyond the operators, the nonlinear transforms :sqlfunc:`pow` /
:sqlfunc:`power` (function spellings of the ``^`` operator),
:sqlfunc:`ln`, :sqlfunc:`exp`, and :sqlfunc:`sqrt` (pure sugar for
``^ 0.5``) apply per draw. They unlock
generative constructions of dependent joints -- ``2 * u ^ 0.25`` is the
inverse-CDF recipe for a marginal of a triangular joint density -- and
the log/exp bridges used by log-normal-style models. Two domain rules
apply, enforced at evaluation time with actionable errors rather than
silently dropped draws (which would bias the estimate):

- ``ln(x)`` requires the argument's support to be non-negative; a
  negative draw raises (a draw of exactly ``0`` yields ``-Infinity``).
- ``x ^ p`` with a **non-integer** exponent likewise requires a
  non-negative base; a negative base draw raises, with the fix in the
  message (``pow(greatest(x, 0), p)`` for the clamped branch). Integer
  exponents are total: ``x ^ 2`` works for any ``x``.

Moments have no linearity to push through a nonlinear map, so
``expected`` / ``variance`` / ``quantile`` over a transform evaluate by
Monte Carlo -- except where a family registers a closed-form image:
``exp`` of a normal is a lognormal and ``ln`` of a lognormal is a
normal, so those moments and quantiles are exact. Constant subtrees
fold exactly, and :sqlfunc:`support` propagates sound intervals through
``^`` / ``ln`` / ``exp``, so support-decidable comparisons stay exact.

The arithmetic operators are *structural*: they build the circuit
without evaluating it. The value is computed only when queried, via
:sqlfunc:`expected`, :sqlfunc:`variance`, :sqlfunc:`moment`,
:sqlfunc:`probability`, :sqlfunc:`rv_sample`, or
:sqlfunc:`rv_histogram` -- exactly where the shape allows (a
family-preserving combination such as a sum of independent normals),
otherwise by Monte Carlo. See *Exact vs. Sampled Answers* below.

Comparison operators ``<``, ``<=``, ``=``, ``<>``, ``>=``, ``>``
on ``(random_variable, random_variable)`` return ``boolean``
syntactically, but the planner hook intercepts every such
operator at planning time and rewrites it into a conditioning
event on the row's provenance. End users never invoke the
comparison procedures directly; the rewriter routes them through
``gate_cmp``.

Order Statistics: greatest / least, min / max
---------------------------------------------

Order statistics over random variables come in two shapes, both
lowering to one ``gate_arith`` node with a ``MAX`` or ``MIN`` opcode.

The **same-row** form takes several random variables and returns
their pointwise maximum or minimum:

.. code-block:: postgresql

    -- Three independent U(0,1) columns in one row.
    CREATE TABLE d AS
      SELECT uniform(0,1) AS x, uniform(0,1) AS y, uniform(0,1) AS z;

    SELECT expected(greatest(x, y, z)) FROM d;   -- 0.75  (= 3/4)
    SELECT expected(least(x, y, z))    FROM d;   -- 0.25  (= 1/4)
    SELECT variance(greatest(x, y, z)) FROM d;   -- 0.0375 (Beta(3,1))

The bare ``GREATEST`` / ``LEAST`` SQL grammar is lifted over
``random_variable`` arguments by the planner hook (a fixed
``provsql.greatest(variadic random_variable[])`` /
``provsql.least(...)`` constructor is available too, and is the only
form outside a planner-hook-rewritten query). ``NULL`` arguments are
ignored, matching the built-in. Being max / min, they are
**idempotent**: ``greatest(x, x, y)`` de-duplicates to
``greatest(x, y)`` -- the same circuit gate -- and ``greatest(x)``
collapses to ``x``. (Two *independent* draws of the same distribution
are distinct gates and are not merged.)

The **aggregate** form promotes ``min`` / ``max`` to RV-aware versions,
exactly like the ``sum`` / ``avg`` / ``product`` aggregates:

.. code-block:: postgresql

    WITH s(r) AS (VALUES (uniform(0,1)), (uniform(0,1)), (uniform(0,1)))
    SELECT expected(max(r)), expected(min(r)) FROM s;   -- 0.75, 0.25

The empty-group identity is ``-inf`` for ``max`` and ``+inf`` for
``min`` (the extremum counterparts to ``sum``'s ``0`` and
``product``'s ``1``).

Evaluation is Monte-Carlo-correct out of the box (the sampler takes
``std::max`` / ``std::min`` over the jointly-drawn children, so shared
base random variables stay coupled). Where the operands are
independent and identically distributed, the mean is **exact**:
``E[max]`` of ``n`` i.i.d. ``U(a,b)`` is ``a + (b-a)·n/(n+1)``,
``E[min]`` is ``a + (b-a)/(n+1)``; i.i.d. exponentials give
``E[min] = 1/(nλ)`` and ``E[max] = H_n/λ``. Ordering or de-duplicating
a ``random_variable`` directly (``ORDER BY rv``, ``DISTINCT rv``) is
meaningless -- a random variable is a distribution, not a scalar -- and
raises a clear error pointing at the order-statistic constructors.

CASE Over Random Variables
--------------------------

A searched ``CASE`` whose ``WHEN`` guards are random-variable
comparisons and whose branches are random variables is lowered into a
``gate_case`` guarded selection (the value of the first guard that
holds, else the ``ELSE`` default):

.. code-block:: postgresql

    -- max, written as a CASE (equals greatest(x, y, z))
    SELECT expected(CASE WHEN x >= y AND x >= z THEN x
                         WHEN y >= z            THEN y
                         ELSE z END) FROM d;             -- 0.75

    -- abs:  E|N(0,1)| = sqrt(2/pi)
    SELECT expected(CASE WHEN n >= 0 THEN n ELSE -n END)
      FROM (SELECT normal(0,1) AS n) t;                  -- 0.7979

    -- ReLU: E[max(N,0)] = 1/sqrt(2*pi)
    SELECT expected(CASE WHEN n >= 0 THEN n ELSE as_random(0) END)
      FROM (SELECT normal(0,1) AS n) t;                  -- 0.3989

Numeric branches must be lifted explicitly -- ``ELSE as_random(0)``
or ``ELSE 0::random_variable``: PostgreSQL resolves ``CASE`` branch
types within a single type *category* and does not consult the
implicit numeric casts across categories (those do fire for operator
arguments, so ``pm25 - 35`` needs no annotation). This
subsumes ``abs`` / ``clamp`` / ReLU and other monotone piecewise
transforms as ``CASE`` sugar. The lowering targets
``rv_case``, a thin ``random_variable`` wrapper over
``provenance_case`` -- which mints the ``gate_case`` from a
``[guard₁, value₁, …, guardₖ, valueₖ, default]`` UUID array --
and both are callable directly when assembling circuits by hand.
Correlations through shared leaves are always preserved: the guards and
branches see one consistent draw.

Moments (``expected`` / ``variance`` / ``moment``) of a ``CASE`` are
returned in **closed form**, without Monte Carlo, for the common shapes
-- so they are exact even under ``SET provsql.rv_mc_samples = 0``:

- a **piecewise function of one random variable** (guards compare it to
  constants, branches are affine in it): ``abs`` / ``clamp`` / ReLU and
  the like, integrated over the branch intervals;
- a **two-way min / max** (``CASE WHEN x >= y THEN x ELSE y``), and more
  generally a first-match tournament that computes the **max or min of
  several** random variables (recognised as an order statistic).

Other multi-variable ``CASE`` shapes fall back to Monte Carlo (or raise
under ``rv_mc_samples = 0``). A ``CASE`` fed to a set-returning consumer
(``support`` / ``rv_sample``) in the ``FROM`` clause must be
materialised first (``CREATE TABLE ... AS SELECT CASE ...``), the same
pattern the aggregates use.

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

    SELECT id, probability(provenance()) AS p
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

A comparison is also a first-class **value** wherever it is
projected. ``SELECT x > y`` surfaces the event's ``gate_cmp`` token
(a ``uuid``) rather than raising, and the ``probability(<predicate>)``
overload asks for the probability of an event with the natural infix
grammar:

.. code-block:: postgresql

    SELECT x > y FROM d;                        -- the event uuid
    SELECT probability(x > y) FROM d;           -- 0.5
    SELECT probability(x > y AND x < z) FROM d; -- 0.1667  (ordering y<x<z)

``probability`` is also a short alias of
:sqlfunc:`probability_evaluate` on a ``uuid``, and the spelling
preferred throughout this chapter. Over a purely
deterministic Boolean it is total -- ``probability(1 > 0)`` is ``1``,
``probability(region = 'north')`` is a per-row ``0`` / ``1`` -- so it
works on definite events too, even with ``provsql.active`` off. (The
predicate overload lives only on the short ``probability`` name; a
Boolean overload of ``probability_evaluate`` would make a
``uuid``-as-text literal ambiguous.)

Two comparison events can be conditioned with the same ``|`` operator
that conditions a random variable (:doc:`conditioning`): ``(A) | (B)``
reads "``A`` given ``B``" and evaluates the correlation-aware
:math:`\Pr(A \wedge B) / \Pr(B)`. Because both comparisons are written
in place they are statically ``boolean``-typed, so the whole ``(A) |
(B)`` resolves to a first-class event token (a ``uuid``), usable as a
:sqlfunc:`probability` argument, a projected column, or a further
``|``:

.. code-block:: postgresql

    -- x ~ Normal(1500, 400); {x >= 2000} ⊂ {x >= 1000}
    SELECT probability((x >= 2000) | (x >= 1000)) FROM d;  -- 0.1181

The joint is *not* the product of the marginals: both comparisons share
the ``x`` leaf, so ``Pr(x >= 2000 ∧ x >= 1000) = Pr(x >= 2000)``. For a
group of comparisons against constants on a single distribution the
joint is resolved analytically (through the CDF), so the answer is exact
regardless of ``provsql.rv_mc_samples`` -- including ``0``. A genuinely
correlated joint with no closed form (comparisons between two random
variables that share a leaf) needs Monte Carlo; with
``provsql.rv_mc_samples = 0`` such a query raises rather than silently
returning the independent-product approximation.

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

The sample count for ``probability(..., 'monte-carlo',
'n')`` is independent and explicit in the third argument (the
sample count is passed as a string, like every other
:sqlfunc:`probability` parameter).

Exact vs. Sampled Answers
-------------------------

Wherever the shape of a query allows, ProvSQL answers it in closed
form -- exactly, with no sampling. This covers the family-preserving
combinations above (sums of normals, i.i.d. exponentials to Erlang,
affine transforms…), comparisons resolved from a distribution's CDF
(``normal > 2``, ``uniform <= 1.5``) or decided from the support alone
(``reading > 1 AND reading < 3`` on a single normal), same-family
comparisons between two variables (``probability(x > y)`` for two
i.i.d. uniforms is exactly ``0.5``; ``probability(a < b)`` for
``a ~ Exp(2), b ~ Exp(3)`` is exactly ``0.4``), i.i.d. order-statistic
means (``expected(greatest(x, y, z))`` of three uniforms is exactly
``0.75``), and conditioning on a comparison (``E[X | X > Y] = 2/3`` for
uniforms). Everything else falls back to Monte Carlo (see
*Configuration of the Monte Carlo Sampler* above).

Setting ``provsql.rv_mc_samples = 0`` forces the analytic path and
raises rather than sampling when no closed form applies -- the way to
assert that a query is answered exactly.

``provsql.simplify_on_load`` (default: ``on``) applies a peephole
simplification when a circuit is read into memory, so every consumer
(semiring evaluators, Monte Carlo, ``view_circuit``, PROV export,
ProvSQL Studio) sees the simplified form. Toggle it off only to
inspect the raw circuit for debugging.

Moments, Quantiles, and Support
-------------------------------

Six polymorphic dispatchers cover the moment / quantile surface; they
accept ``random_variable``, plain ``uuid``, ``numeric``, and
``agg_token`` inputs and dispatch internally.

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

:sqlfunc:`quantile` ``(input, p [, prov])``
    p-quantile (inverse CDF)
    ``F⁻¹(p) = min{x : P(input ≤ x | prov) ≥ p}`` for
    ``p ∈ [0, 1]`` -- medians, percentiles, Value-at-Risk, credible
    intervals. ``p = 0`` / ``p = 1`` return the (possibly infinite)
    support edges. Exact for a bare random variable -- each family's
    elementary inverse CDF where one exists, a monotone bisection of
    the closed-form CDF otherwise (Erlang, Gamma) -- and for
    categorical distributions (generalised inverse); conditioning
    that reduces to an interval event truncates in closed form.
    Compound expressions fall back to the empirical Monte Carlo
    quantile with ``percentile_cont``-style interpolation (backed by
    the :sqlfunc:`rv_quantile` C entry point). Plain numeric input is
    its own quantile (a Dirac). ``agg_token`` input is not yet
    supported.

    .. code-block:: postgresql

        SELECT quantile(posterior, 0.025) AS lower_95,
               quantile(posterior, 0.5)   AS median,
               quantile(posterior, 0.975) AS upper_95
        FROM model_posteriors WHERE param = 'mu_revenue';

:sqlfunc:`support` ``(input [, prov [, method [, arguments]]])``
    Support interval ``[lo, hi]``. For a ``random_variable``,
    propagates each leaf's support through ``gate_arith`` via
    interval arithmetic and intersects per-variable bounds from
    ``prov``; for plain numeric input, returns the degenerate
    point ``[c, c]``; for an ``agg_token``, returns the
    closed-form support of the aggregation function.

Three derived readouts complete the same-row second-moment
surface (these take ``random_variable`` arguments from the *same
row* -- they are not aggregates over a group of rows):

:sqlfunc:`stddev` ``(x [, prov])``
    Standard deviation ``sqrt(Var[x | prov])``.

:sqlfunc:`covariance` ``(x, y [, prov])``
    Covariance ``E[xy | prov] − E[x | prov]·E[y | prov]``.
    Structurally independent arguments (disjoint base-RV
    footprints) give an exact ``0``; arguments sharing leaves are
    correlation-aware, analytically where the product has a
    closed form and by Monte Carlo otherwise.

:sqlfunc:`correlation` ``(x, y [, prov])``
    Pearson correlation, the covariance normalised by the two
    standard deviations. Returns ``NULL`` when either standard
    deviation is zero (a degenerate, constant argument). All
    moments are evaluated under the same conditioning event
    ``prov``.

.. code-block:: postgresql

    -- shared drift leaf: both sensors move together
    SELECT correlation(drift + noise_a, drift + noise_b) FROM s;

Three information-theoretic readouts (all in nats) complete the
surface:

:sqlfunc:`entropy` ``(x [, prov])``
    Entropy ``H(x)``: Shannon entropy for a discrete distribution
    (a categorical, a discrete count, a constant -- a point mass has
    entropy ``0``), differential entropy for a continuous one
    (exact quadrature of ``−f ln f``, including through
    independent-arm mixture trees such as :sqlfunc:`gmm`'s).
    Arithmetic composites and the conditional form (``prov``) are
    estimated by a Monte Carlo histogram plug-in, so they need
    ``provsql.rv_mc_samples > 0``.

:sqlfunc:`kl` ``(p, q)``
    Kullback-Leibler divergence ``KL(P ‖ Q)``, exact via the
    defining sum (discrete-discrete) or integral
    (continuous-continuous). ``Infinity`` when ``P`` is not
    absolutely continuous with respect to ``Q``: an outcome of
    ``P`` that ``Q`` gives zero mass, mismatched kinds, or a region
    of ``P``'s support where ``Q``'s density (under)flows to zero.
    Both arguments must resolve to closed-form densities;
    arithmetic composites raise an error.

:sqlfunc:`mutual_information` ``(x, y)``
    Mutual information ``I(x; y)``: exactly ``0`` for structurally
    independent arguments (disjoint base-RV footprints), ``H(x)``
    for a discrete variable paired with itself (``Infinity`` for a
    continuous one), and a 2-D histogram plug-in estimate over
    coupled joint Monte Carlo draws for a genuinely correlated
    pair (needs ``provsql.rv_mc_samples > 0``).

.. code-block:: postgresql

    SELECT kl(posterior, prior)           AS information_gain,
           entropy(posterior)             AS residual_uncertainty,
           mutual_information(x, x + eps) AS shared_information
    FROM model;

End-to-end on the sensors fixture:

.. code-block:: postgresql

    SELECT id,
           expected(reading)   AS mean,
           variance(reading)   AS var,
           support(reading)    AS supp
    FROM sensor_readings;

The expectation, variance, and support of ``normal(2.5,
0.5)`` come out exactly as ``2.5``, ``0.25``, and
``(-Infinity, +Infinity)``; the uniform's as ``2``, ``1/3``, and
``(1, 3)``; the exponential's as ``2.5``, ``6.25``, and
``(0, +Infinity)``.

**Independence shortcuts.** Sums of independent random variables
have exact expectation and variance, and products of independent
random variables have exact expectation (``E[XY] = E[X]·E[Y]``);
other shapes fall back to Monte Carlo.

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

For sensor ``1`` (``normal(2.5, 0.5)`` truncated to ``> 2``),
the conditional mean is the textbook Mills-ratio formula
``μ + σ · φ(α) / (1 − Φ(α))`` with ``α = (2 − μ)/σ``; for sensor
``2`` (``uniform[1, 3]`` truncated to ``> 2``), the conditional
distribution is ``uniform[2, 3]`` with mean ``2.5``; for sensor
``3`` (``exponential(0.4)`` truncated to ``> 2``), the
memoryless property gives conditional mean ``2 + 1/0.4 = 4.5``.

Conditioning on a one- or two-sided interval is exact in closed form
for Normal (Mills-ratio truncation), Uniform (truncated support), and
Exponential (memorylessness); other shapes are estimated by Monte
Carlo. If the conditioning event is rare, fewer than ``n`` samples may
be accepted within the ``provsql.rv_mc_samples`` budget, and a
``NOTICE`` suggests widening it (or an error under
``provsql.rv_mc_samples = 0``).

Passing ``gate_one()`` (the default) as ``prov`` is equivalent to
the unconditional moment, so an unconditional call has no extra
cost.

Sampling and Histograms
-----------------------

Two functions expose raw and binned samples for inspection or
downstream analytics.

:sqlfunc:`rv_sample` ``(token, n [, prov])`` ``RETURNS SETOF float8``
    Draw ``n`` samples from the scalar sub-circuit rooted at
    ``token``, conditioning on the provenance event ``prov``
    (defaulting to unconditional). The function is a
    set-returning function. Shared ``gate_rv`` leaves between
    ``token`` and ``prov`` are loaded into a single joint circuit
    so the conditioning event's draw and the value's draw share
    their per-iteration state.

    When the root is a bare ``gate_rv`` of a supported family
    (Uniform / Normal / Exponential) and the event reduces to an
    interval constraint on it, the conditional distribution is
    sampled directly in closed form (uniform on the truncated
    interval; memoryless shift for exponential one-sided tails;
    inverse-CDF transform for two-sided exponential and normal).
    100% acceptance: exactly ``n`` samples are returned even when
    the event is a tight tail like ``X > 9.5`` over ``U(0, 10)``
    that would degrade the rejection budget.

    Otherwise the rejection path runs: ``provsql.rv_mc_samples``
    iterations attempt to satisfy the event; a ``NOTICE`` is
    emitted when fewer than ``n`` accept, and the SRF returns
    whatever samples were accepted so the caller can proceed with
    a smaller batch.

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

    The same closed-form truncated sampler as :sqlfunc:`rv_sample`
    applies when the shape qualifies, so a tight ``provsql.rv_mc_samples``
    budget no longer fails with ``conditional MC accepted 0 of N``
    on conditioning events that the closed-form path can handle.

Example, drawing 200 samples from the truncated sensor-1 reading
(conditioned on ``reading > 2.5``, which the planner lifts into
the row's provenance as a ``gate_cmp``):

.. code-block:: postgresql

    SET provsql.monte_carlo_seed = 42;
    SELECT s
    FROM (SELECT reading, provenance() AS prov
            FROM sensor_readings
           WHERE id = 1 AND reading > 2.5) q,
         LATERAL rv_sample(q.reading::uuid, 200, q.prov) AS t(s);

Mixtures and Categorical Random Variables
------------------------------------------

The two overloads of :sqlfunc:`mixture` differ in whether the Boolean
coin is shared -- a coupled coin (a gate UUID) makes several mixtures
pick the same side per draw, while a scalar mints a fresh coin per
call:

.. code-block:: postgresql

    -- Two mixtures coupled through a shared coin: they always
    -- pick the same side per Monte-Carlo iteration.
    WITH coin AS (
      SELECT create_input_gate(0.3) AS p)
    SELECT
      mixture((SELECT p FROM coin),
              normal(0, 1),
              normal(10, 1))   AS shared_a,
      mixture((SELECT p FROM coin),
              uniform(-1, 1),
              uniform(9, 11))  AS shared_b;

    -- Two ad-hoc mixtures: each mints its own fresh coin.
    SELECT
      mixture(0.3, normal(0, 1),
                   normal(10, 1)) AS independent_a,
      mixture(0.3, uniform(-1, 1),
                   uniform(9, 11)) AS independent_b;

A :sqlfunc:`categorical` assigns explicit probabilities to its
outcomes:

.. code-block:: postgresql

    -- 0 with probability 0.2, 1 with probability 0.5, 2 with 0.3
    SELECT categorical(
             ARRAY[0.2, 0.5, 0.3]::double precision[],
             ARRAY[0, 1, 2]::double precision[]);

Each ``categorical(probs, outcomes)`` call mints a fresh block
anchor, so two calls with the same arrays produce two
*independent* categorical draws. (Exception: a single-outcome
categorical, where exactly one entry of ``probs`` is positive,
collapses to :sqlfunc:`as_random` of the corresponding outcome at
construction time; two such calls with the same outcome value
then share the v5-keyed ``as_random`` gate.)

.. note::

   The simplifier does **not** auto-collapse a cascade of Dirac
   mixtures into a single categorical: that conversion is
   reserved for explicit user calls to
   :sqlfunc:`categorical`. If you want a categorical, ask
   for one; if you build a tower of mixtures, the circuit keeps
   the tower shape so its structural sharing remains intact.

.. _continuous-aggregation:

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

.. note::

   ``AVG`` returns ``NaN`` when every row's provenance is false
   (zero divided by zero). The numerator and denominator are
   structurally correct; the result is the natural floating-point
   ``0/0`` rather than an error. If you need ``NULL`` on empty
   effective groups, filter by ``probability(provenance())
   > 0`` before averaging.

``COUNT`` over a tracked ``random_variable`` column goes through
the standard ``COUNT`` path on the ``provsql`` UUID column.

The SQL-standard second-moment statistic aggregates are also lifted to
``random_variable`` rows, with the same provenance-weighted semantics
(a row absent in a world drops out of every sum, the count, and the
percentile member set):

:sqlfunc:`covar_pop` / :sqlfunc:`covar_samp` ``(random_variable, random_variable)`` ``RETURNS random_variable``
    Population / sample covariance of the row pairs,
    :math:`S_{XY}/N - (S_X/N)(S_Y/N)` and
    :math:`(S_{XY} - S_X S_Y / N)/(N-1)` over the
    indicator-weighted power sums. Rows with either side ``NULL``
    are skipped (standard SQL); a world with :math:`N = 0` (or
    :math:`N = 1` for the sample form) evaluates to ``NaN``, the
    undefined-world convention the moment estimators skip.

:sqlfunc:`corr` ``(random_variable, random_variable)`` ``RETURNS random_variable``
    Pearson correlation
    :math:`\mathrm{covar\_pop} / \sqrt{v_X\,v_Y}` (a zero-variance
    world yields ``NaN``, matching SQL's ``NULL`` for a
    zero-stddev input).

:sqlfunc:`stddev_pop` / :sqlfunc:`stddev_samp` ``(random_variable)`` ``RETURNS random_variable``
    Population / sample standard deviation (the variance is clamped
    at ``0`` before the square root, so floating-point error can
    never trip the ``pow`` domain guard).

:sqlfunc:`percentile_cont` ``(fraction) WITHIN GROUP (ORDER BY random_variable)`` ``RETURNS random_variable``
    The SQL-standard continuous percentile as an order statistic
    over the group: per Monte Carlo draw, the values of the rows
    present in that world are sorted and linearly interpolated at
    the fraction. Requires provenance-tracked input: on an
    untracked table the input sort has no meaningful order over
    distributions and raises the usual ordering diagnostic (the
    same behaviour as an un-rewritten ``GREATEST`` /
    ``LEAST``).

The statistics distribute over the possible worlds — the result is a
``random_variable`` whose moments (:sqlfunc:`expected`,
:sqlfunc:`variance`…) are estimated by Monte Carlo (there is no
closed form for these compound circuits), so set
``provsql.rv_mc_samples > 0``. Do not confuse the *aggregate*
:sqlfunc:`corr` (one value per group of rows) with the *same-row
scalar readouts* :sqlfunc:`covariance` / :sqlfunc:`correlation` /
:sqlfunc:`stddev`, which take two RV expressions from a single row
and return ``double precision``.

Latent variables and posterior inference
----------------------------------------

A distribution parameter may itself be a **random variable** (or an
``agg_token`` cast to ``uuid``) rather than a concrete number. The
parameter is then a *latent* variable and the leaf a **compound
(hierarchical) distribution** -- for instance a Normal whose mean is
drawn from a broad prior:

.. code-block:: postgresql

    -- M ~ Normal(0, 10);  X ~ Normal(M, 1):  a hierarchical model.
    SELECT expected(normal(normal(0, 10), 1));

Every constructor gains token-accepting overloads for each parameter
position (``normal(random_variable, float8)``,
``normal(float8, random_variable)``,
``normal(random_variable, random_variable)``, and likewise for
``uniform``, ``exponential``, ``gamma``, ``lognormal``, ``weibull``,
``pareto``, ``beta``, ``inverse_gamma`` and ``inverse_gaussian``). A
literal call still resolves to the plain numeric constructor, so the
common case is unchanged.

The **discrete** families join in through ``poisson(random_variable)``
and ``binomial(integer, random_variable)`` (a latent rate / success
probability, e.g. ``poisson(120 * R)`` or ``binomial(50, 40.0 / N)``).
A latent parameter cannot be enumerated into a categorical at
construction, so these build a sampled leaf like the continuous ones;
the literal ``poisson(λ)`` / ``binomial(n, p)`` still return the exact
categorical. Their pmf supplies the likelihood weight when such a leaf
is observed, which makes the classic discrete conjugate updates
(Gamma-Poisson, Beta-Binomial) available to the inference engine below.

The **mean** of a compound leaf is exact (no Monte Carlo, and it works
even with ``provsql.rv_mc_samples = 0``) whenever the family's mean is
affine in its parameters -- Normal ``μ``, Uniform ``(a+b)/2``,
inverse-Gaussian ``μ``, Poisson ``λ`` -- since
``E[X] = E[mean(θ)] = mean(E[θ])`` by linearity of expectation, with no
independence assumption. This composes with the ordinary linearity of
``+``/``-``/scaling and mixtures, so ``E[·]`` stays exact over affine
transforms and mixtures of tractable leaves; a genuinely nonlinear
coupling (a product of shared variables, a nonlinear-mean family like
Exponential ``1/λ``) is where it falls back to Monte Carlo.

Compound leaves have no constant-parameter closed form, so their
moments are estimated by Monte Carlo (set ``provsql.rv_mc_samples >
0``). A latent **shared** across several leaves couples them: two
``normal(M, 1)`` leaves over the *same* ``M`` are positively correlated,
which is the informal way to reproduce a correlation without a
multivariate primitive.

.. note::

   A drawn parameter that violates a family's support (a sampled scale
   or rate ``≤ 0``) raises a specific error rather than being silently
   dropped -- dropping it would truncate the prior and bias every moment.
   Put a positive-support prior on such a parameter (e.g. ``gamma`` /
   ``lognormal``).

**Posterior inference (likelihood weighting).** Conditioning a latent on
an observed value is *posterior inference*, written with the natural
**conditional-equality** form: ``X | (Y = c)`` observes that the leaf
``Y`` took the value ``c``. For a single observation it reads exactly like
truncation conditioning (:doc:`conditioning`), and for a table of
observations the prefix ``|`` operator (:sqlfunc:`given`) produces the
per-row evidence that :sqlfunc:`and_agg` folds into one evidence token,
passed as the conditioning argument of any readout:

.. code-block:: postgresql

    -- Single observation: posterior of mu given normal(mu, 1) = 8.
    WITH model AS (SELECT normal(0, 10) AS mu)
    SELECT expected(mu | (normal(mu, 1) = 8)) FROM model;

    -- A table of observations x_i ~ Normal(mu, 1); posterior mean/variance:
    WITH model AS (SELECT normal(0, 10) AS mu)
    SELECT expected(mu, ev), variance(mu, ev)
    FROM   model,
    LATERAL (SELECT and_agg(| (normal(mu, 1) = x)) AS ev
             FROM (VALUES (8.0), (10.0), (12.0)) AS obs(x)) e;

The engine is **self-normalised importance sampling**: latents are drawn
from the prior and each draw is weighted by the observations' densities
at the data. It is the continuous generalisation of the rejection-based
conditioning of :doc:`conditioning` -- a Boolean *inequality* event
(``Y > c``, a truncation) contributes a ``0/1`` weight, a point equality
``Y = c`` contributes a pdf weight (a **pmf** weight for a discrete leaf),
through the same evidence conjunction. All of :sqlfunc:`expected`,
:sqlfunc:`variance`, :sqlfunc:`moment`, :sqlfunc:`quantile` and
:sqlfunc:`rv_sample` gain posteriors with no surface change; the posterior
predictive is ``rv_sample`` on a fresh leaf that reuses the latent.
Conjugate models are recovered numerically to MC tolerance -- Normal-Normal,
Gamma-Poisson (a ``poisson(R)`` leaf), Beta-Binomial (a ``binomial(n, p)``
leaf).

.. note::

   A continuous point event ``Y = c`` is measure-zero as a Boolean
   *selection* (in a ``WHERE`` clause it matches nothing), but as a
   *conditioning* event it is the well-defined observation of ``Y`` at
   ``c`` -- the disintegration, computed by likelihood weighting. ProvSQL
   routes the two readings apart automatically. ``Y`` must be a **bare
   distribution leaf**: observing a derived quantity (``(X + Y) = d``)
   needs a change-of-variables density and is out of scope, and the
   observations must share the latent through one query so the weight and
   the value see the same draw.

**Marginal likelihood and diagnostics.** :sqlfunc:`evidence` returns the
marginal likelihood ``P(data)`` (the mean importance weight -- the same
quantity conditioning computes as ``P(C)``). When many observations pin
one latent, the weights concentrate and the posterior *effective sample
size* collapses; a ``WARNING`` fires once the ESS falls below
``provsql.ess_warn_fraction`` of the accepted draws (raise
``provsql.rv_mc_samples``, or defer to sequential Monte Carlo for the
relational regime of one latent and many rows).

**Explaining the posterior (Shapley over observations).** Because the
importance weight is a product of per-observation density factors,
dropping an observation is dropping one factor: the Shapley value of each
observation over a posterior moment answers *"which observation most
shifted my posterior"* directly. :sqlfunc:`shapley_observe` returns each
observation's attribution (the values sum to the prior→posterior shift);
a dominant outlier gets the largest-magnitude value. It is exact over the
observation subsets, so it is capped at 12 observations.

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
- Where-provenance crossed with random variables (the
  column-level tracking layered on top of an RV-bearing query is
  not yet defined).
- An in-Studio distribution editor.
