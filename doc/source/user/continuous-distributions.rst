Continuous Distributions
=========================

ProvSQL extends the probabilistic-database setting from discrete
Bernoulli inputs (see :doc:`probabilities`) to **continuous random
variables**. Columns can carry distributions such as ``normal(μ, σ)``,
``uniform(a, b)``, or ``exponential(λ)``; arithmetic and comparisons
apply to them; ``WHERE``, ``JOIN`` and ``UNION`` work on
random-variable columns as ordinary SQL; and ``expected``,
``variance``, ``moment``, ``quantile``, ``support``, ``rv_sample`` and
``rv_histogram`` query the resulting distributions, optionally
conditioned on filter predicates. Conditioning a random variable --
``x | (x > k)``, which truncates and renormalises its distribution --
uses the same ``|`` operator that conditions discrete events and
aggregates; see :doc:`conditioning`.

Introduction
------------

A *random-variable column* stores, in each row, a token referring to a
probability distribution instead of a single value. The token has type
``random_variable`` (a provenance token, castable to ``uuid``) and fits
in any ``CREATE TABLE``:

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
``add_provenance`` adds a Boolean provenance token for each *row*, so
tuple-level uncertainty (a row that may or may not be present, see
:doc:`probabilities`) combines with random-variable events in the same
query.

The remainder of this chapter uses this sensors example as a running
motivator. Each row carries a different kind of noise:

- sensor ``1`` is a calibrated unit with Gaussian measurement error
  centred at ``2.5``;
- sensor ``2`` is a cheap unit whose reading is uniformly distributed
  between ``1`` and ``3``;
- sensor ``3`` is a drift-prone unit whose reading is exponentially
  distributed with rate ``0.4``.

Filtering against a numeric threshold makes each row's presence
depend on the event that its reading satisfies the filter:

.. code-block:: postgresql

    SELECT id FROM sensor_readings WHERE reading > 2;
    -- id 1, 2, 3 selected with respective probabilities
    --   1 - Φ((2 - 2.5) / 0.5)  ≈ 0.84   (Normal CDF)
    --   (3 - 2) / (3 - 1)       =  0.50   (Uniform CDF)
    --   exp(-0.4 · 2)           ≈ 0.45   (Exponential survival)

These probabilities are read with :sqlfunc:`probability` over
:sqlfunc:`provenance` (see *Probabilistic Queries* below).

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

These families are enumerated into a categorical distribution through
:sqlfunc:`categorical_from_log_pmf` (computed in log space, stable at
large parameters; also usable directly for a custom pmf). Moments,
quantiles, and comparisons -- including exact ``=`` / ``<>`` point
masses -- are exact over the enumerated support; an infinite support is
truncated at a ``1e-15`` relative-mass tail, and a support of more than
``10000`` outcomes raises an error. Degenerate parameters
(``poisson(0)``, ``binomial(n, 1)``…) give an :sqlfunc:`as_random`
constant.

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

The full list of registered parameterised families is introspectable
with :sqlfunc:`rv_families`, which returns one row per family with its
name token, parameter count, conventional parameter symbols, and a
short display label (:doc:`ProvSQL Studio <studio>` reads it to display
distributions).

Arithmetic on Random Variables
------------------------------

The arithmetic operators ``+``, ``-``, ``*``, ``/``, ``^`` and unary
``-`` are declared on ``(random_variable, random_variable)`` and
return a new ``random_variable``. Mixing scalars and random variables
works through the implicit casts above:

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
``^ 0.5``) apply per draw. They allow generative constructions of
dependent joints (``2 * u ^ 0.25`` is the inverse-CDF recipe for a
marginal of a triangular joint density) and log/exp transforms as in
log-normal models. Two domain rules apply; a violating draw raises an
error at evaluation time:

- ``ln(x)`` requires the argument's support to be non-negative; a
  negative draw raises (a draw of exactly ``0`` yields ``-Infinity``).
- ``x ^ p`` with a **non-integer** exponent likewise requires a
  non-negative base; a negative base draw raises, with the fix in the
  message (``pow(greatest(x, 0), p)`` for the clamped branch). Integer
  exponents are total: ``x ^ 2`` works for any ``x``.

``expected`` / ``variance`` / ``quantile`` over a nonlinear transform
are evaluated by Monte Carlo, except for ``exp`` of a normal (a
lognormal) and ``ln`` of a lognormal (a normal), whose moments and
quantiles are exact. Constant subexpressions are computed exactly, and
:sqlfunc:`support` propagates sound intervals through ``^`` / ``ln`` /
``exp``, so comparisons decided by the support stay exact.

Arithmetic builds an expression without evaluating it. The value is
computed only when queried, via :sqlfunc:`expected`,
:sqlfunc:`variance`, :sqlfunc:`moment`, :sqlfunc:`probability`,
:sqlfunc:`rv_sample`, or :sqlfunc:`rv_histogram`: exactly where the
shape allows (such as a sum of independent normals), otherwise by
Monte Carlo. See *Exact vs. Sampled Answers* below.

Comparison operators ``<``, ``<=``, ``=``, ``<>``, ``>=``, ``>``
on ``(random_variable, random_variable)`` have type ``boolean``; in a
query they become probabilistic events (see *Probabilistic Queries*
below).

Order Statistics: greatest / least, min / max
---------------------------------------------

Order statistics over random variables come in two shapes.

The **same-row** form takes several random variables and returns
their pointwise maximum or minimum:

.. code-block:: postgresql

    -- Three independent U(0,1) columns in one row.
    CREATE TABLE d AS
      SELECT uniform(0,1) AS x, uniform(0,1) AS y, uniform(0,1) AS z;

    SELECT expected(greatest(x, y, z)) FROM d;   -- 0.75  (= 3/4)
    SELECT expected(least(x, y, z))    FROM d;   -- 0.25  (= 1/4)
    SELECT variance(greatest(x, y, z)) FROM d;   -- 0.0375 (Beta(3,1))

The SQL ``GREATEST`` / ``LEAST`` syntax accepts ``random_variable``
arguments in queries ProvSQL rewrites; the functions
``provsql.greatest(variadic random_variable[])`` /
``provsql.least(...)`` do the same and also work outside such queries.
``NULL`` arguments are ignored, as in the built-in. ``greatest(x, x, y)``
is ``greatest(x, y)`` and ``greatest(x)`` is ``x``; two *independent*
draws of the same distribution remain distinct variables.

The **aggregate** form extends ``min`` / ``max`` to random variables,
like the ``sum`` / ``avg`` / ``product`` aggregates:

.. code-block:: postgresql

    WITH s(r) AS (VALUES (uniform(0,1)), (uniform(0,1)), (uniform(0,1)))
    SELECT expected(max(r)), expected(min(r)) FROM s;   -- 0.75, 0.25

A row absent in a world contributes ``-inf`` to ``max`` and ``+inf``
to ``min``, so it cannot perturb the extremum; an empty group itself
is SQL ``NULL``, as standard SQL ``MIN`` / ``MAX`` report.

Monte Carlo evaluation keeps operands that share random variables
correlated. Where the operands are independent and identically
distributed, the mean is **exact**:
``E[max]`` of ``n`` i.i.d. ``U(a,b)`` is ``a + (b-a)·n/(n+1)``,
``E[min]`` is ``a + (b-a)/(n+1)``; i.i.d. exponentials give
``E[min] = 1/(nλ)`` and ``E[max] = H_n/λ``. Ordering or de-duplicating
a ``random_variable`` directly (``ORDER BY rv``, ``DISTINCT rv``) is
meaningless -- a random variable is a distribution, not a scalar -- and
raises a clear error pointing at the order-statistic constructors.

CASE Over Random Variables
--------------------------

A searched ``CASE`` whose ``WHEN`` guards are random-variable
comparisons and whose branches are random variables is itself a random
variable: in each draw, the value of the first branch whose guard
holds, else the ``ELSE`` default:

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

Numeric branches must be cast explicitly (``ELSE as_random(0)`` or
``ELSE 0::random_variable``): PostgreSQL does not apply the implicit
numeric casts to ``CASE`` branches, although it does to operator
arguments, so ``pm25 - 35`` needs no cast. ``abs``, ``clamp``, ReLU and
other piecewise transforms are written this way. The functions
``rv_case`` (returning a ``random_variable``) and ``provenance_case``
build the same construct from a
``[guard₁, value₁, …, guardₖ, valueₖ, default]`` UUID array. The guards
and branches see one consistent draw, so correlations through shared
variables are preserved.

Moments (``expected`` / ``variance`` / ``moment``) of a ``CASE`` are
computed in **closed form**, exact even under
``SET provsql.rv_mc_samples = 0``, for these shapes:

- a **piecewise function of one random variable** (guards compare it to
  constants, branches are affine in it): ``abs`` / ``clamp`` / ReLU and
  the like, integrated over the branch intervals;
- a **two-way min / max** (``CASE WHEN x >= y THEN x ELSE y``), and more
  generally a sequence of guards that computes the **max or min of
  several** random variables.

Other multi-variable ``CASE`` shapes are evaluated by Monte Carlo (or
raise under ``rv_mc_samples = 0``). A ``CASE`` passed to a
set-returning function (``support`` / ``rv_sample``) in the ``FROM``
clause must be materialised first
(``CREATE TABLE ... AS SELECT CASE ...``), as for the aggregates.

Probabilistic Queries
---------------------

Filter predicates, joins, and unions on ``random_variable`` columns are
written as ordinary SQL:

.. code-block:: postgresql

    SELECT id, provenance() AS prov
    FROM sensor_readings
    WHERE reading > 2;

The comparison ``reading > 2`` becomes part of each row's provenance.
The query returns one row per source row whose random-variable event
is satisfiable; its probability is obtained with:

.. code-block:: postgresql

    SELECT id, probability(provenance()) AS p
    FROM sensor_readings
    WHERE reading > 2;
    --  id |    p
    -- ----+--------
    --   1 | 0.8413
    --   2 | 0.5000
    --   3 | 0.4493

Comparisons between two random-variable columns work the same way. A
``JOIN`` condition on random-variable columns becomes part of the
joined row's provenance, and ``UNION ALL`` over relations with
random-variable columns combines the source rows' provenance as for
any tracked table.

A comparison can also be projected: ``SELECT x > y`` returns the
event's token (a ``uuid``), and the ``probability(<predicate>)``
overload computes the probability of an event written in infix form:

.. code-block:: postgresql

    SELECT x > y FROM d;                        -- the event uuid
    SELECT probability(x > y) FROM d;           -- 0.5
    SELECT probability(x > y AND x < z) FROM d; -- 0.1667  (ordering y<x<z)

``probability`` is also a short alias of
:sqlfunc:`probability_evaluate` on a ``uuid``, and the spelling
preferred throughout this chapter. Over a purely
deterministic Boolean it is total -- ``probability(1 > 0)`` is ``1``,
``probability(region = 'north')`` is a per-row ``0`` / ``1`` -- so it
works on definite events too, even with ``provsql.active`` off. The
predicate overload exists only under the name ``probability``, not
``probability_evaluate``.

Two comparison events can be conditioned with the same ``|`` operator
that conditions a random variable (:doc:`conditioning`): ``(A) | (B)``
reads "``A`` given ``B``" and evaluates the correlation-aware
:math:`\Pr(A \wedge B) / \Pr(B)`. The result is an event token (a
``uuid``), usable as a :sqlfunc:`probability` argument, a projected
column, or the left operand of a further ``|``:

.. code-block:: postgresql

    -- x ~ Normal(1500, 400); {x >= 2000} ⊂ {x >= 1000}
    SELECT probability((x >= 2000) | (x >= 1000)) FROM d;  -- 0.1181

The joint is *not* the product of the marginals: both comparisons share
the variable ``x``, so ``Pr(x >= 2000 ∧ x >= 1000) = Pr(x >= 2000)``.
Comparisons against constants on a single distribution are resolved
through its CDF, so the answer is exact regardless of
``provsql.rv_mc_samples``, including ``0``. Correlated events with no
closed form, such as comparisons over composite expressions like
``x + y > z``, need Monte Carlo; with ``provsql.rv_mc_samples = 0``
such a query raises an error instead of returning the
independent-product approximation.

Configuration of the Monte Carlo Sampler
-----------------------------------------

Two settings control Monte Carlo evaluation. See
:doc:`configuration` for the full configuration reference.

``provsql.monte_carlo_seed`` (default: ``-1``)
    Seed of the random generator. The default ``-1`` gives
    non-deterministic sampling. Any other value (including ``0``) is
    used as a literal seed and makes every Monte Carlo result
    reproducible across runs, for both discrete and continuous
    sampling.

``provsql.rv_mc_samples`` (default: ``10000``)
    Sample count used by :sqlfunc:`expected`, :sqlfunc:`variance`,
    :sqlfunc:`moment`, :sqlfunc:`rv_histogram`, and
    :sqlfunc:`rv_sample` under conditioning, when no closed form
    applies. Set to ``0`` to disable Monte Carlo entirely: such calls
    then raise an error.

The sample count of ``probability(..., 'monte-carlo', 'n')`` is
independent: it is the third argument, passed as a string like every
other :sqlfunc:`probability` parameter.

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

Setting ``provsql.rv_mc_samples = 0`` makes a query raise an error
when no closed form applies: this asserts that a query is answered
exactly.

``provsql.simplify_on_load`` (default: ``on``) simplifies a circuit
when it is loaded, for every consumer (semiring evaluation, Monte
Carlo, ``view_circuit``, PROV export, ProvSQL Studio). Turn it off
only to inspect the raw circuit.

Moments, Quantiles, and Support
-------------------------------

Six polymorphic functions compute moments, quantiles, and supports;
they accept ``random_variable``, plain ``uuid``, ``numeric``, and
``agg_token`` inputs (:sqlfunc:`quantile` accepts only
``random_variable`` and plain numeric input).

:sqlfunc:`expected` ``(input [, prov [, method [, arguments]]])``
    Expectation ``E[input | prov]``; for an ``agg_token``, the
    expectation of the aggregate over the possible worlds. Without
    ``prov`` (default ``gate_one()``), the unconditional expectation.

:sqlfunc:`variance` ``(input [, prov [, method [, arguments]]])``
    Variance ``Var[input | prov]``, in closed form when available,
    otherwise by Monte Carlo.

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
    support edges. Exact for a bare random variable (continuous or
    categorical), including under conditioning that reduces to an
    interval. Compound expressions use the empirical Monte Carlo
    quantile with ``percentile_cont``-style interpolation. Plain
    numeric input is its own quantile (a Dirac). Plain ``uuid`` and
    ``agg_token`` inputs are not supported.

    .. code-block:: postgresql

        SELECT quantile(posterior, 0.025) AS lower_95,
               quantile(posterior, 0.5)   AS median,
               quantile(posterior, 0.975) AS upper_95
        FROM model_posteriors WHERE param = 'mu_revenue';

:sqlfunc:`support` ``(input [, prov [, method [, arguments]]])``
    Support interval ``[lo, hi]``. For a ``random_variable``, the
    interval obtained by interval arithmetic over the expression,
    narrowed by the bounds that ``prov`` imposes; for plain numeric
    input, the point ``[c, c]``; for an ``agg_token``, the
    closed-form support of the aggregate.

Three further functions take ``random_variable`` arguments from the
*same row* (they are not aggregates over a group of rows):

:sqlfunc:`stddev` ``(x [, prov])``
    Standard deviation ``sqrt(Var[x | prov])``.

:sqlfunc:`covariance` ``(x, y [, prov])``
    Covariance ``E[xy | prov] − E[x | prov]·E[y | prov]``.
    Arguments built from disjoint sets of random variables give an
    exact ``0``; arguments sharing random variables are computed
    analytically where every factor has a closed form, otherwise as
    the sample covariance of jointly drawn ``(x, y)`` pairs.

:sqlfunc:`correlation` ``(x, y [, prov])``
    Pearson correlation, the covariance normalised by the two
    standard deviations. Returns ``NULL`` when either standard
    deviation is zero (a constant argument). All moments are
    evaluated under the same conditioning event ``prov``.

.. code-block:: postgresql

    -- shared drift leaf: both sensors move together
    SELECT correlation(drift + noise_a, drift + noise_b) FROM s;

Three information-theoretic functions (all in nats):

:sqlfunc:`entropy` ``(x [, prov])``
    Entropy ``H(x)``: Shannon entropy for a discrete distribution
    (a categorical, a discrete count, a constant -- a point mass has
    entropy ``0``), differential entropy for a continuous one
    (computed exactly, including for mixtures of independent
    components such as :sqlfunc:`gmm`). Arithmetic expressions and
    the conditional form (``prov``) are estimated from a Monte Carlo
    histogram, so they need ``provsql.rv_mc_samples > 0``.

:sqlfunc:`kl` ``(p, q)``
    Kullback-Leibler divergence ``KL(P ‖ Q)``, exact via the
    defining sum (discrete-discrete) or integral
    (continuous-continuous). ``Infinity`` when ``P`` is not
    absolutely continuous with respect to ``Q``: an outcome of
    ``P`` that ``Q`` gives zero mass, mismatched kinds, or a region
    of ``P``'s support where ``Q``'s density (under)flows to zero.
    Both arguments must have closed-form densities; arithmetic
    expressions raise an error.

:sqlfunc:`mutual_information` ``(x, y)``
    Mutual information ``I(x; y)``: exactly ``0`` for arguments built
    from disjoint sets of random variables, ``H(x)`` for a discrete
    variable paired with itself (``Infinity`` for a continuous one),
    and otherwise an estimate from a 2-D histogram of joint Monte
    Carlo draws (needs ``provsql.rv_mc_samples > 0``).

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
other shapes use Monte Carlo.

Conditional Inference
---------------------

The functions above accept an optional ``prov uuid`` argument that
conditions the result on the provenance event ``prov``. In a tracked
query, every ``WHERE`` filter on a random-variable column is part of
the row's provenance, so passing the :sqlfunc:`provenance`
pseudo-column conditions on the filter:

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
for the families with closed-form truncated moments: Normal
(Mills-ratio truncation), Uniform (truncated support), Exponential
(memorylessness), Log-normal, Weibull, Pareto, and Beta; other shapes
are estimated by Monte Carlo. If the conditioning event is rare, few
samples may be accepted within the ``provsql.rv_mc_samples`` budget,
and a ``NOTICE`` suggests increasing it (an error under
``provsql.rv_mc_samples = 0``).

Passing ``gate_one()`` (the default) as ``prov`` gives the
unconditional moment, at no extra cost.

Sampling and Histograms
-----------------------

Two functions return raw and binned samples.

:sqlfunc:`rv_sample` ``(token, n [, prov])`` ``RETURNS SETOF float8``
    Draws ``n`` samples of the value ``token``, conditioned on the
    provenance event ``prov`` (unconditional by default). The value
    and the event are drawn jointly, so random variables they share
    stay correlated.

    When ``token`` is a bare Uniform, Normal, Exponential,
    Log-normal, Weibull, Pareto, or Logistic random variable and the
    event reduces to an interval constraint on it, the truncated
    distribution is sampled directly: exactly ``n`` samples are
    returned, even for a narrow tail like ``X > 9.5`` over
    ``U(0, 10)``.

    Otherwise ``provsql.rv_mc_samples`` draws are attempted; a
    ``NOTICE`` is emitted when fewer than ``n`` satisfy the event,
    and the function returns the samples that did.

:sqlfunc:`rv_histogram` ``(token, bins [, prov])`` ``RETURNS jsonb``
    Empirical histogram of the same value as
    :sqlfunc:`rv_sample`, returned as a JSON array of
    ``{bin_lo, bin_hi, count}`` objects. The number of bins is
    ``bins`` (default ``30``); the bin range covers the observed
    ``[min, max]`` of the draws; the sample count is
    ``provsql.rv_mc_samples``. Pin ``provsql.monte_carlo_seed``
    for reproducibility.

    ``token`` must be a scalar value: a constant (single bin), a
    random variable, an arithmetic expression, a mixture, or an
    aggregate; a conditioned ``X | C`` gives the histogram of its
    conditional distribution. Any other token raises an error. The
    direct truncated sampling of :sqlfunc:`rv_sample` applies here
    too.

Example, drawing 200 samples from the sensor-1 reading conditioned on
``reading > 2.5``:

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
coin is shared: a coin given as a provenance token (``uuid``) makes
several mixtures pick the same side in each draw, while a scalar
probability creates a fresh coin per call:

.. code-block:: postgresql

    -- Mint a shared coin: a fresh gate_input token pinned to
    -- probability 0.3.
    CREATE TEMP TABLE coin(p uuid);
    INSERT INTO coin VALUES (public.uuid_generate_v4());
    SELECT create_gate((SELECT p FROM coin), 'input');
    SELECT set_prob((SELECT p FROM coin), 0.3);

    -- Two mixtures coupled through the shared coin: they always
    -- pick the same side per Monte-Carlo iteration.
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

Two ``categorical(probs, outcomes)`` calls with the same arrays
produce two *independent* categorical draws. A categorical with a
single positive-probability outcome is the constant
:sqlfunc:`as_random` of that outcome.

.. _continuous-aggregation:

Aggregation Over Random Variables
---------------------------------

Three aggregates lift the standard arithmetic aggregates from
deterministic scalars to ``random_variable`` columns:

:sqlfunc:`sum` ``(random_variable)`` ``RETURNS random_variable``
    Provenance-weighted sum
    :math:`\sum_i \mathbf{1}\{\varphi_i\} \cdot X_i`. An empty group
    is SQL ``NULL`` (as standard SQL ``SUM``).

:sqlfunc:`avg` ``(random_variable)`` ``RETURNS random_variable``
    Provenance-weighted average
    :math:`(\sum_i \mathbf{1}\{\varphi_i\} \cdot X_i) /
    (\sum_i \mathbf{1}\{\varphi_i\})`. An empty group is SQL ``NULL``
    (as standard SQL ``AVG``).

:sqlfunc:`product` ``(random_variable)`` ``RETURNS random_variable``
    Provenance-weighted product
    :math:`\prod_{i : \varphi_i} X_i`: rows with false provenance
    contribute ``1``. An empty group is SQL ``NULL``, as for the
    other aggregates.

.. note::

   ``AVG`` returns ``NaN`` (the floating-point ``0/0``, not an error)
   when every row's provenance is false. If you need ``NULL`` on
   such groups, filter by ``probability(provenance()) > 0``
   before averaging.

``COUNT`` over a tracked ``random_variable`` column counts rows as
for any tracked table.

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
    Population / sample standard deviation.

:sqlfunc:`percentile_cont` ``(fraction) WITHIN GROUP (ORDER BY random_variable)`` ``RETURNS random_variable``
    The SQL-standard continuous percentile as an order statistic
    over the group: in each Monte Carlo draw, the values of the rows
    present in that world are sorted and linearly interpolated at
    the fraction. Requires provenance-tracked input: on an
    untracked table it raises the error for ordering a
    ``random_variable``.

Each of these statistics is a ``random_variable`` whose moments
(:sqlfunc:`expected`, :sqlfunc:`variance`…) are estimated by Monte
Carlo (there is no closed form), so set
``provsql.rv_mc_samples > 0``. Do not confuse the *aggregate*
:sqlfunc:`corr` (one value per group of rows) with the *same-row
scalar readouts* :sqlfunc:`covariance` / :sqlfunc:`correlation` /
:sqlfunc:`stddev`, which take two RV expressions from a single row
and return ``double precision``.

Latent variables and posterior inference
----------------------------------------

A distribution parameter may itself be a **random variable** (or an
``agg_token`` cast to ``uuid``). The parameter is then a *latent*
variable and the result a **compound (hierarchical) distribution**,
for instance a Normal whose mean is drawn from a broad prior:

.. code-block:: postgresql

    -- M ~ Normal(0, 10);  X ~ Normal(M, 1):  a hierarchical model.
    SELECT expected(normal(normal(0, 10), 1));

Most constructors have overloads accepting a random variable in each
parameter position (``normal(random_variable, float8)``,
``normal(float8, random_variable)``,
``normal(random_variable, random_variable)``, and likewise for
``logistic``, ``uniform``, ``exponential``, ``gamma``, ``lognormal``,
``weibull``, ``pareto``, ``beta``, ``inverse_gamma`` and
``inverse_gaussian``; ``erlang``, ``chi_squared`` and ``wald`` keep
literal-only forms). A call with literal parameters is the plain
numeric constructor.

The **discrete** families accept a latent rate or success probability
through ``poisson(random_variable)``, ``geometric(random_variable)``,
``binomial(integer, random_variable)`` and the ``negative_binomial``
overloads (e.g., ``poisson(120 * R)`` or ``binomial(50, 40.0 / N)``).
These are sampled like the continuous ones, while the literal
``poisson(λ)`` / ``binomial(n, p)`` remain exact categoricals. Their
pmf is used as the likelihood when they are observed, which gives the
discrete conjugate updates (Gamma-Poisson, Beta-Binomial) below.

The **mean** of a compound distribution is exact (no Monte Carlo, even
with ``provsql.rv_mc_samples = 0``) whenever the family's mean is
affine in its parameters (Normal ``μ``, Uniform ``(a+b)/2``,
inverse-Gaussian ``μ``, Poisson ``λ``), since
``E[X] = E[mean(θ)] = mean(E[θ])`` by linearity of expectation, with no
independence assumption. It stays exact through ``+``/``-``/scaling
and mixtures; a nonlinear dependency (a product of shared variables, a
family with nonlinear mean such as Exponential ``1/λ``) uses Monte
Carlo.

Other moments of compound distributions are estimated by Monte Carlo
(set ``provsql.rv_mc_samples > 0``). A latent **shared** by several
distributions correlates them: two ``normal(M, 1)`` over the *same*
``M`` are positively correlated, a way to model correlation without a
multivariate distribution.

.. note::

   A drawn parameter outside a family's domain (a sampled scale or
   rate ``≤ 0``) raises an error. Put a positive-support prior on such
   a parameter (e.g., ``gamma`` / ``lognormal``).

**Posterior inference (likelihood weighting).** Conditioning a latent on
an observed value is *posterior inference*, written as a **conditional
equality**: ``X | (Y = c)`` observes that the distribution ``Y`` took
the value ``c``. A single observation is written like truncation
conditioning (:doc:`conditioning`); for a table of observations, the
prefix ``|`` operator (:sqlfunc:`given`) produces per-row evidence that
:sqlfunc:`and_agg` combines into one evidence token, passed as the
conditioning argument of any function above:

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

The posterior is computed by **self-normalised importance sampling**:
latents are drawn from the prior and each draw is weighted by the
densities of the observations. An *inequality* event (``Y > c``, a
truncation) weighs a draw ``0`` or ``1``, a point equality ``Y = c``
by the pdf (the **pmf** for a discrete distribution), and both can be
combined in the same evidence. :sqlfunc:`expected`,
:sqlfunc:`variance`, :sqlfunc:`moment`, :sqlfunc:`quantile` and
:sqlfunc:`rv_sample` all accept such evidence; the posterior
predictive is ``rv_sample`` on a new distribution that reuses the
latent.

**Exact conjugate posteriors.** When the model matches a classic
conjugate prior/likelihood pair (the latent is a bare distribution,
and every observation binds a datum to a distribution with the latent
in one parameter and literal other parameters), the posterior is
computed in **closed form**: exact, deterministic, and available with
Monte Carlo disabled (``provsql.rv_mc_samples = 0``), with the same
queries. The recognised pairs:

.. list-table::
   :header-rows: 1
   :widths: 40 30 30

   * - Observed leaf (latent slot)
     - Prior
     - Posterior
   * - ``normal(θ, σ)`` (mean)
     - ``normal``
     - Normal (precision-weighted)
   * - ``lognormal(θ, σ)`` (log-scale location)
     - ``normal``
     - Normal (update at ``ln d``)
   * - ``exponential(θ)`` (rate)
     - ``gamma``
     - Gamma
   * - ``poisson(θ)`` (rate)
     - ``gamma``
     - Gamma
   * - ``gamma(k₀, θ)`` / ``erlang(k₀, θ)`` (rate)
     - ``gamma``
     - Gamma
   * - ``binomial(n, θ)`` (success probability)
     - ``beta``
     - Beta
   * - ``geometric(θ)`` (success probability)
     - ``beta``
     - Beta
   * - ``negative_binomial(r, θ)`` (success probability)
     - ``beta``
     - Beta
   * - ``uniform(0, θ)`` (upper bound)
     - ``pareto``
     - Pareto
   * - ``pareto(xₘ, θ)`` (tail shape)
     - ``gamma``
     - Gamma

Because each observation updates the *running* posterior, mixed
likelihoods sharing one conjugate prior compose: a Gamma-prior rate
observed through interleaved Poisson counts and Exponential gaps stays
Gamma. The closed form is the exact value of what importance sampling
estimates. Any other shape (a latent entering the observed
distribution through arithmetic, a prior on a Normal's ``σ``, a
Boolean event combined with the observations, several latents in one
evidence set) uses importance sampling, with the diagnostics below.

.. note::

   A continuous point event ``Y = c`` has probability zero as a
   *selection* (in a ``WHERE`` clause it matches nothing), but as a
   *conditioning* event it is the observation of ``Y`` at ``c``.
   ``Y`` must be a **bare distribution**: observing a derived quantity
   (``(X + Y) = d``) is not supported, and the observations must share
   the latent within one query.

**Marginal likelihood and diagnostics.** :sqlfunc:`evidence` returns the
marginal likelihood ``P(data)`` (the mean importance weight, the same
quantity conditioning computes as ``P(C)``). When many observations
constrain one latent, the weights concentrate and the *effective sample
size* (ESS) of the posterior collapses; a ``WARNING`` is emitted when
the ESS falls below ``provsql.ess_warn_fraction`` of the accepted
draws. Increase ``provsql.rv_mc_samples``; one latent observed through
many rows calls for sequential Monte Carlo instead.

**Explaining the posterior (Shapley over observations).**
:sqlfunc:`shapley_observe` returns the Shapley value of each observation
over a posterior moment, answering *"which observation most shifted my
posterior"*. The values sum to the shift from prior to posterior, and a
dominant outlier gets the largest-magnitude value. The computation is
exact over subsets of observations, so it is limited to 12
observations.

Studio Integration
------------------

ProvSQL Studio (:doc:`studio`) has three Circuit-mode features for
continuous distributions:

- **Distribution profile**: ``μ`` and ``σ²`` with a histogram, a
  PDF/CDF toggle, per-bar tooltips, and wheel zoom (computed with
  :sqlfunc:`rv_histogram`).
- **Conditioning input**: clicking a result cell puts the row's
  provenance into the *Condition on* input, so every subsequent
  moment, sample or histogram is conditional. Toggle the
  *Conditioned by* badge off to get the unconditional answer.
- **Simplified-circuit rendering**: the circuit is shown as simplified
  under ``provsql.simplify_on_load``.

Limitations
-----------

The following are not supported:

- ``EXCEPT`` and ``SELECT DISTINCT`` on relations that carry
  ``random_variable`` columns.
- Where-provenance combined with random variables.
- Editing distributions in Studio.
