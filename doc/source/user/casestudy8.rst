.. nb:name: cs8
.. nb:database: cs8

Case Study: ProvSQL as a Probability Calculator
===============================================

This case study uses ProvSQL as what it quietly is: an **exact,
correlation-aware probability calculator that you drive in SQL**. Classic
probability problems -- base rates, correlated events, conditional
expectation, truncated distributions -- become ordinary queries, and the
answers are computed *exactly* (not by sampling, unless you ask) and
*correlation-aware* (the provenance circuit tracks shared events, so joint
and conditional probabilities come out right without independence
assumptions or hand-rolled inclusion--exclusion).

The thread tying the problems together is the conditioning operator ``|``
(see :doc:`the chapter on probabilities <probabilities>`): once a model is
loaded, ``A | B`` reads as "``A`` given ``B``", for discrete events, for
continuous random variables, and for probabilistic aggregates alike.

The translation dictionary the calculator teaches:

.. list-table::
   :header-rows: 1
   :widths: 45 55

   * - probability
     - ProvSQL / SQL
   * - an event
     - tuple(s) with :sqlfunc:`set_prob`
   * - mutually exclusive outcomes
     - :sqlfunc:`repair_key`
   * - :math:`A \wedge B`
     - join (:sqlfunc:`provenance_times`)
   * - :math:`A \vee B`
     - ``UNION`` (:sqlfunc:`provenance_plus`)
   * - :math:`P(A)`
     - :sqlfunc:`probability_evaluate`
   * - :math:`P(A \mid B)`
     - ``A | B``
   * - a continuous quantity
     - a :sqlfunc:`random_variable`
   * - :math:`E[X]`, :math:`\mathrm{Var}[X]`
     - :sqlfunc:`expected`, :sqlfunc:`variance`
   * - :math:`E[X \mid C]`
     - ``expected(X | C)``

.. nb:skip

.. tip::

   **Follow along in your browser, no install.** This case study runs in
   full in the ProvSQL Playground: open it `as a runnable notebook
   <https://provsql.org/playground/?nb=cs8>`_, or open the bare `cs8
   database <https://provsql.org/playground/?db=cs8>`_ and type the queries
   as you read. See the :ref:`Playground note <playground-note>`.

The Scenario
------------

An epidemiology desk at a public-health agency keeps a small probabilistic
model of a screening programme and reaches for ProvSQL whenever a question
is really a probability question. Five such questions follow; each is a
recognisable textbook problem, and each is one query.

Setup
-----

.. nb:omit-begin

This case study assumes a working ProvSQL installation (see
:doc:`getting-provsql`). Download :download:`setup.sql
<../../casestudy8/setup.sql>` and load it into a fresh database:

.. code-block:: bash

    createdb cs8
    psql -d cs8 -f setup.sql

.. nb:omit-end

.. nb:setup: ../../casestudy8/setup.sql

The fixture defines three tiny probabilistic models, one per problem:

* ``screening(grp, disease, positive, p)`` -- the joint sample space of a
  diagnostic test, as four mutually-exclusive worlds of one group
  (:sqlfunc:`repair_key`): prevalence 1%, sensitivity 90%, specificity 95%.
* ``risk(id, p)`` -- three independent risk factors, two of which will be
  combined into *correlated* conditions.
* ``cases(day, region, n, p)`` -- per-day case contributions, each present
  with probability ``p``, for the aggregation problem.

The continuous problem builds its biomarker inline with
:sqlfunc:`normal`, so it needs no table.

Problem 1: The Base-Rate Fallacy
--------------------------------

A patient tests positive. What is the probability they have the disease?
The reflex answer -- the test's sensitivity, 90% -- is the base-rate
fallacy. The right answer is :math:`P(\text{disease} \mid \text{positive})`,
and with a 1% prevalence it is far lower.

We name the two events by reading their provenance off the model (a
scope-local, inert :sqlfunc:`provenance` fetch over the qualifying worlds),
then ask the calculator for the four probabilities -- including the two
conditionals, written with ``|``:

.. code-block:: postgresql

    WITH e AS (
      SELECT (SELECT provenance() FROM screening WHERE disease  GROUP BY grp) AS d,
             (SELECT provenance() FROM screening WHERE positive GROUP BY grp) AS pos)
    SELECT round(probability_evaluate(d)::numeric, 4)         AS p_disease,
           round(probability_evaluate(pos)::numeric, 4)       AS p_positive,
           round(probability_evaluate(d | pos)::numeric, 4)   AS p_disease_given_pos,
           round(probability_evaluate(pos | d)::numeric, 4)   AS p_pos_given_disease
    FROM e;

The result -- ``0.0100 | 0.0585 | 0.1538 | 0.9000`` -- is the whole lesson
in one row. ``P(positive | disease) = 0.90`` recovers the sensitivity, as
it must; but ``P(disease | positive) = 0.1538``, not 0.90: most positives
are false positives because the disease is rare. ``A | B`` computed
:math:`P(A \wedge B)/P(B)` over the shared circuit -- exactly Bayes' rule,
with no arithmetic on your part.

Problem 2: Correlation That Matters
-----------------------------------

Two clinical conditions, ``A`` and ``B``, each have a known probability.
What is the probability of *at least one*? If you reach for
:math:`1-(1-P(A))(1-P(B))`, you have assumed independence -- and here it is
wrong, because ``A`` and ``B`` share a common cause.

We build ``A = shared ∧ a1`` and ``B = shared ∧ a2`` (both depend on the
same ``shared`` factor), then compare ProvSQL's exact ``A ∨ B`` against the
independence formula:

.. code-block:: postgresql

    WITH t AS (
      SELECT (SELECT provenance() FROM risk WHERE id = 'shared') AS f,
             (SELECT provenance() FROM risk WHERE id = 'a1')     AS a1,
             (SELECT provenance() FROM risk WHERE id = 'a2')     AS a2)
    SELECT round(probability_evaluate(provenance_times(f, a1))::numeric, 4) AS p_a,
           round(probability_evaluate(provenance_times(f, a2))::numeric, 4) AS p_b,
           round(probability_evaluate(
             provenance_plus(ARRAY[provenance_times(f, a1),
                                   provenance_times(f, a2)]))::numeric, 4)  AS p_a_or_b_exact,
           round((1 - (1 - 0.3) * (1 - 0.35))::numeric, 4)                  AS p_a_or_b_naive
    FROM t;

``0.3000 | 0.3500 | 0.4400 | 0.5450``: the exact answer is **0.44**, the
independence formula gives **0.545** -- a 24% overstatement. ProvSQL gets it
right for free: because the same ``shared`` tuple is *one* input gate in
both ``A`` and ``B`` (content-addressing makes a shared base event the same
gate everywhere), the disjunction circuit performs inclusion--exclusion
itself. You never wrote :math:`P(A)+P(B)-P(A \wedge B)`.

Problem 3: Let the Cost Optimizer Choose
----------------------------------------

``probability_evaluate`` is not one algorithm but a portfolio -- exact
methods (independent factoring, the sieve / inclusion--exclusion, knowledge
compilation, tree decomposition) and sampling estimators. Called with no
method, a cost chooser picks the cheapest *exact* method that fits the
circuit; you can also name one explicitly. They agree, of course:

.. code-block:: postgresql

    SET provsql.last_eval_method = '';
    WITH t AS (
      SELECT provenance_plus(ARRAY[
               (SELECT provenance() FROM risk WHERE id = 'a1'),
               (SELECT provenance() FROM risk WHERE id = 'a2')]) AS tok)
    SELECT round(probability_evaluate(tok)::numeric, 4)                   AS p_default,
           round(probability_evaluate(tok, 'independent')::numeric, 4)   AS p_independent,
           round(probability_evaluate(tok, 'monte-carlo', '200000')::numeric, 2) AS p_monte_carlo
    FROM t;

All three columns read ``0.88`` (``a1 ∨ a2``, independent, is
:math:`1-(1-0.6)(1-0.7)`). The interesting part is *which* exact method the
chooser used for the default call; it is recorded in a GUC:

.. code-block:: postgresql

    SHOW provsql.last_eval_method;

For this tiny independent disjunction the chooser settles on a cheap exact
method rather than paying for compilation. On the correlated circuit of
Problem 2 it would choose differently; the point is that the *same* call,
``probability_evaluate(token)``, stays correct and stays cheap as the
circuits grow -- the optimizer, not you, matches method to structure.

Problem 4: A Continuous Posterior
---------------------------------

The calculator is not limited to discrete events. A lab biomarker is a
*continuous* quantity, a :sqlfunc:`random_variable`; conditioning on it is
truncation, and the conditioned distribution flows onward as a value whose
moments you can take.

Take a biomarker ``X ~ Normal(20, 5)`` and ask for its mean, and then for
its mean and variance *given* that it exceeds a referral threshold of 25 --
written with the natural predicate ``X | (X > 25)``:

.. code-block:: postgresql

    SET provsql.rv_mc_samples = 0;   -- closed-form only
    WITH r AS (SELECT normal(20, 5) AS x)
    SELECT round(expected(r.x)::numeric, 3)               AS e_x,
           round(expected(r.x | (r.x > 25))::numeric, 3)  AS e_given_referral,
           round(variance(r.x | (r.x > 25))::numeric, 3)  AS var_given_referral,
           (support(r.x | (r.x > 25))).lo                 AS support_lower
    FROM r;

``20.000 | 27.626 | 4.977 | 25``: the unconditional mean is 20, but the
posterior mean of a referred patient's biomarker is **27.626** (the
closed-form truncated-normal mean), its variance shrinks to 4.977, and its
support starts at the threshold 25. ``X | (X > 25)`` is itself a
``random_variable`` -- it could be stored in a column and re-queried; here
we read its moments directly.

Problem 5: Conditional Expectation of an Aggregate
--------------------------------------------------

Finally, the third carrier: a probabilistic *aggregate*. Each row of
``cases`` contributes to its region's total only when present, so the total
is a random quantity. Its expectation, and its expectation *conditioned* on
an observation, are again one query.

We materialise the per-region totals (each a probabilistic
:sqlfunc:`agg_token`), then ask for the expected total in the North, and the
expected total *given* that the high-count day actually occurred:

.. code-block:: postgresql

    CREATE TABLE casesum AS SELECT region, sum(n) AS total FROM cases GROUP BY region;

    SELECT cs.region,
           round(expected(cs.total)::numeric, 2) AS e_total,
           round(expected(cs.total
                   | (SELECT provenance() FROM cases WHERE n = 4))::numeric, 2)
             AS e_total_given_highday
    FROM casesum cs
    WHERE cs.region = 'North';

``North | 3.50 | 5.50``: unconditionally the North expects
:math:`0.5\cdot 3 + 0.5\cdot 4 = 3.5` cases; given that the ``n = 4`` day is
confirmed, the conditional expectation rises to ``5.50`` (a certain 4 plus
the still-uncertain 3). Conditioning composed through the aggregate exactly
as it did through the discrete events and the continuous biomarker.

What Made This Work
-------------------

Five textbook problems, five one-line queries, three carriers (discrete
events, a continuous random variable, a probabilistic aggregate), and one
operator -- ``|`` -- meaning the same thing throughout. Two properties did
the heavy lifting:

* **Exactness with a cost optimizer.** ``probability_evaluate`` returns the
  exact probability and picks the cheapest method that fits; you never chose
  an algorithm or accepted sampling error unless you asked for it.
* **Correlation for free.** Content-addressing makes a shared base event the
  same gate in every circuit, so joint, disjoint, and conditional
  probabilities are right without independence assumptions -- the difference
  between 0.44 and 0.545 in Problem 2, and between 3.5 and 5.5 in Problem 5.

The model lives in the database (``add_provenance`` + ``set_prob`` +
``repair_key``), so it persists and is queried -- and updated -- across
sessions, next to the data it is about. That is the case for ProvSQL as a
probability calculator: not a toy sample space, but a real query language
over real, indexed, persistent data, doing exact probability arithmetic.
