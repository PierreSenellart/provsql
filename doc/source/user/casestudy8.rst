.. nb:name: cs8
.. nb:database: cs8

Case Study: ProvSQL as a Probability Calculator
===============================================

This case study uses ProvSQL as an **exact, correlation-aware probability
calculator that you drive in SQL**. Classic probability problems -- base
rates, correlated events, conditional expectation, truncated distributions,
denial constraints -- become ordinary queries, and the answers are computed
*exactly* (not by
sampling, unless you ask) and *correlation-aware* (the provenance circuit
tracks shared events, so joint and conditional probabilities come out right
without independence assumptions or hand-rolled `inclusion--exclusion <https://en.wikipedia.org/wiki/Inclusion%E2%80%93exclusion_principle>`_).

The thread tying the problems together is the conditioning operator ``|``:
once a model is loaded, ``A | B`` reads as "``A`` given ``B``", for discrete
events, for continuous random variables, and for probabilistic aggregates
alike.

.. nb:skip

.. tip::

   **Best run as a notebook.** This case study is meant to be run
   interactively in :doc:`ProvSQL Studio <studio>` in notebook mode -- try
   it in the ProvSQL Playground, no install, `as a runnable notebook
   <https://provsql.org/playground/?nb=cs8>`_ and step through the cells.
   See the :ref:`Playground note <playground-note>`.

The Scenario
------------

An epidemiology desk at a public-health agency keeps a small probabilistic
model of a screening programme and reaches for ProvSQL whenever a question
needs probabilistic evaluation. Five such questions follow; each is a
recognisable textbook problem, and we work through each one step by step,
building its model and then asking the calculator.

.. nb:omit-begin

This case study assumes a working ProvSQL installation (see
:doc:`getting-provsql`). It needs no setup script: each problem creates its
own little model as part of the story. Start a fresh database and enable
the extension once,

.. code-block:: bash

    createdb cs8
    psql -d cs8

.. code-block:: postgresql

    CREATE EXTENSION IF NOT EXISTS provsql CASCADE;
    SET search_path TO public, provsql;

then paste the blocks below as you read.

.. nb:omit-end

.. nb:md: It is recommended to run this case study in ProvSQL Studio or the ProvSQL Playground, in notebook mode. There is no separate setup script: each problem creates its own small model below, so run the cells from top to bottom. Every model first drops and recreates its tables, so **Run All is safe to repeat**: nothing accumulates.

Problem 1: The Base-Rate Fallacy
--------------------------------

Screening tests are imperfect, but a good one is quite accurate. Ours
catches 90% of true cases (its `sensitivity <https://en.wikipedia.org/wiki/Sensitivity_and_specificity>`_) and correctly clears 95% of
healthy people (its `specificity <https://en.wikipedia.org/wiki/Sensitivity_and_specificity>`_); the disease itself is rare, affecting
1% of the population.

A patient tests positive. What is the probability they actually have the
disease? The tempting answer -- "around 90%, since the test is that
accurate" -- is wrong, and the mistake is common enough to have a name: the
`base-rate fallacy <https://en.wikipedia.org/wiki/Base_rate_fallacy>`_. It
forgets how rare the disease is. Among the 99% who are healthy, even a 5%
false-positive rate produces a flood of positives, and that flood swamps
the genuine cases -- so most positive results are in fact false alarms. The
quantity we really want, :math:`\Pr(\text{disease} \mid \text{positive})`,
comes out far below 90%. Let us compute it exactly.

The model is the *joint sample space*: the four possible worlds for one
screened patient -- diseased or healthy, each crossed with a positive or
negative test. We build it in small steps. First, keep the test's three
numbers in one place, named once and easy to change:

.. code-block:: postgresql

    DROP TABLE IF EXISTS params CASCADE;
    CREATE TABLE params(prevalence float, sensitivity float, specificity float);
    INSERT INTO params VALUES (0.01, 0.90, 0.95);

Now spell out the four worlds. Each world's probability is the prevalence
of its health state times the test's rate in that state -- the chain rule,
written once per world:

.. code-block:: postgresql

    DROP TABLE IF EXISTS screening CASCADE;
    CREATE TABLE screening(disease boolean, positive boolean, p float);
    INSERT INTO screening
    SELECT true,  true,  prevalence       * sensitivity        FROM params  -- true positive
    UNION ALL
    SELECT true,  false, prevalence       * (1 - sensitivity)  FROM params  -- false negative
    UNION ALL
    SELECT false, true,  (1 - prevalence) * (1 - specificity)  FROM params  -- false positive
    UNION ALL
    SELECT false, false, (1 - prevalence) * specificity        FROM params; -- true negative

Have a look: each row carries the probability of its world, and the four
sum to 1.

.. code-block:: postgresql

    SELECT * FROM screening;

These four worlds are *mutually exclusive*: for any one patient exactly one
of them is the true state of affairs. :sqlfunc:`repair_key` is how ProvSQL
records that -- it turns a set of rows into a *block of mutually-exclusive
alternatives* (a "repair" of a key), exactly one of which holds at a time.
Passing an *empty* key makes the whole table one such block, and
:sqlfunc:`set_prob` then gives each world the probability in its ``p``
column:

.. code-block:: postgresql

    SELECT repair_key('screening', '');
    SELECT disease, positive, set_prob(provenance(), p) FROM screening;

The model is loaded. Now let us *see* what a probabilistic event looks like
to ProvSQL. Every row a query returns over a provenance-tracked table
carries a *provenance token* in a hidden ``provsql`` column: a handle to
the little circuit explaining how that row came to be. Ask for the distinct
values of ``positive``:

.. code-block:: postgresql

    SELECT DISTINCT positive FROM screening;

Two rows come back, ``true`` and ``false``, each with a token. **Click the
token on the** ``true`` **row.** You stay in Notebook mode; Studio just adds
a cell below showing that token's circuit -- a single *plus* gate joining
the two mutually-exclusive worlds in which the test reads positive (the
diseased true positive and the healthy false positive). That little circuit
*is* the event "the test is positive".

On that circuit cell, press :fa:`bolt` **Evaluate** to add an evaluation
cell bound to the circuit above. Leave the scheme on **Probability** and
press :fa:`play` **Run**: ProvSQL walks the circuit and returns about
**0.0585** -- a positive result is uncommon.

That click-path is one SQL query. :sqlfunc:`probability_evaluate` takes a
token and returns its probability, and :sqlfunc:`provenance` hands back the
token of a query's rows -- so the same number comes from one cell:

.. code-block:: postgresql

    SELECT probability_evaluate(provenance())
    FROM (SELECT DISTINCT positive FROM screening) AS s
    WHERE positive;

The question we actually care about is `conditional <https://en.wikipedia.org/wiki/Conditional_probability>`_: given a positive
test, how likely is the disease? ProvSQL writes "``A`` given ``B``" as
``A | B``. We name both events -- diseased and positive -- and ask for the
two conditionals (``GROUP BY ()`` collapses each event's worlds into a
single token, just as ``DISTINCT`` did above):

.. code-block:: postgresql

    WITH e AS (
      SELECT (SELECT provenance() FROM screening WHERE disease  GROUP BY ()) AS d,
             (SELECT provenance() FROM screening WHERE positive GROUP BY ()) AS pos)
    SELECT probability_evaluate(pos | d) AS pos_given_disease,
           probability_evaluate(d | pos) AS disease_given_pos
    FROM e;

The first number is about 0.9: :math:`\Pr(\text{positive} \mid
\text{disease})` recovers the test's sensitivity, as it must. The second is
the punchline -- :math:`\Pr(\text{disease} \mid \text{positive})` is only
about **0.15**, not 0.9. Most positive results really are false alarms,
exactly as the base-rate fallacy warned. ProvSQL computed :math:`\Pr(A
\wedge B) / \Pr(B)` -- `Bayes' rule <https://en.wikipedia.org/wiki/Bayes%27_theorem>`_ -- over the shared circuit, with no
arithmetic on your part.

Problem 2: Correlation That Matters
-----------------------------------

Risk factors rarely act alone. Obesity, for instance, raises the risk of
*both* type 2 diabetes and hypertension. Because the two conditions share
that common cause, they are `correlated
<https://en.wikipedia.org/wiki/Correlation>`_: a patient who has one is more
likely to have the other. Suppose we want the probability that a patient
develops *at least one* of the two. Treating them as `independent
<https://en.wikipedia.org/wiki/Independence_%28probability_theory%29>`_
overestimates it -- let us see by how much.

.. note::

   This model -- the factors, the probabilities, and the "obesity plus one
   extra trigger" rule for each condition -- is invented and deliberately
   oversimplified to illustrate correlation. It is not medical guidance.

Model three independent base factors: obesity (the shared cause) and one
extra trigger for each condition, each present with its own probability.
:sqlfunc:`add_provenance` makes the table provenance-tracked (one
independent token per row) and :sqlfunc:`set_prob` reads each row's
probability from its ``p`` column:

.. code-block:: postgresql

    DROP TABLE IF EXISTS risk CASCADE;
    CREATE TABLE risk(factor text, p float);
    INSERT INTO risk VALUES
      ('obesity',            0.5),   -- the shared common cause
      ('insulin_resistance', 0.6),   -- extra trigger for diabetes
      ('high_salt',          0.7);   -- extra trigger for hypertension
    SELECT add_provenance('risk');
    SELECT factor, set_prob(provenance(), p) FROM risk;

A patient develops **diabetes** when they are *both* obese and
insulin-resistant. In SQL "both" is a join -- ask for the two facts at once
-- and ProvSQL records the result's provenance as the *product* of their
tokens (a :sqlfunc:`provenance_times`, formed for you). Its probability:

.. code-block:: postgresql

    SELECT probability_evaluate(provenance())
    FROM risk o, risk x
    WHERE o.factor = 'obesity' AND x.factor = 'insulin_resistance';

0.3 (that is :math:`0.5 \cdot 0.6`). **Hypertension** is the same query with
``high_salt`` in place of ``insulin_resistance`` -- 0.35. Now "diabetes
*or* hypertension": match a patient who is obese and has *either* trigger,
and fold the two matches into one event with ``GROUP BY ()``. The provenance
of a set of alternatives is their *sum* (a :sqlfunc:`provenance_plus`), so
this single query *is* the "at least one condition" event. We do not even
need to select anything in particular -- the provsql token rides along in
every result row, so a constant label is enough to read:

.. code-block:: postgresql

    SELECT 'diabetes or hypertension' AS event
    FROM risk o, risk x
    WHERE o.factor = 'obesity'
      AND x.factor IN ('insulin_resistance', 'high_salt')
    GROUP BY ();

**Click the token to see its circuit.** ``obesity`` appears as a *single*
gate feeding both branches, not two separate copies: ProvSQL addresses
every gate by its content, so the shared cause is literally the same node
everywhere -- and that overlap is exactly what an independence assumption
ignores. Press :fa:`bolt` **Evaluate** and :fa:`play` **Run**: about
**0.44**.

Compare that with the independence estimate, plain arithmetic
:math:`1 - (1 - \Pr(\text{diabetes}))(1 - \Pr(\text{hypertension}))`:

.. code-block:: postgresql

    SELECT 1 - (1 - 0.3) * (1 - 0.35) AS independence_estimate;

**0.545** -- a 24% overstatement. ProvSQL's 0.44 is correct because the
disjunction circuit accounted for the shared obesity on its own. You never
had to write the inclusion--exclusion correction :math:`\Pr(A) + \Pr(B) -
\Pr(A \wedge B)`.

The same shared gate also lets us turn the question around and *condition* on
it. Problem 1 conditioned one event on another with the **binary** ``A | B``;
the **unary** ``| B`` (the prefix form of :sqlfunc:`given`) conditions a
*whole query's rows at once* -- a directive in the ``SELECT`` list that is
stripped from the output, leaving each row's provenance conditioned on ``B``.
Condition every factor on obesity being present:

.. code-block:: postgresql

    SELECT factor,
           round(probability_evaluate(provenance())::numeric, 4) AS p_given_obesity
    FROM (
      SELECT factor, | (SELECT provenance() FROM risk WHERE factor = 'obesity')
      FROM risk
    ) s
    ORDER BY factor;

``obesity`` comes back at **1.0** -- an event given itself is certain -- while
``insulin_resistance`` (0.6) and ``high_salt`` (0.7) are **unchanged** from
their priors: they share no gate with obesity, so conditioning on it tells us
nothing about them. The same content-addressed circuit that made the
disjunction correct makes the conditional correct.

See what conditioning built. Select one conditioned tuple on its own and
**click its** ``provsql`` **token**:

.. code-block:: postgresql

    SELECT factor, | (SELECT provenance() FROM risk WHERE factor = 'obesity')
    FROM risk WHERE factor = 'insulin_resistance';

You stay in Notebook mode; Studio adds a circuit cell below showing a ``∣``
(*conditioned*) gate with three labelled children -- the **target** event
(``insulin_resistance``), the **evidence** (``obesity``), and their **joint**
``target ⊗ evidence``. Its probability is read straight off that gate as
:math:`\Pr(\text{target} \wedge \text{evidence}) / \Pr(\text{evidence})` --
Bayes' rule over the shared circuit, the same ratio Problem 1 used. The ``|``
operator, in both its binary and unary forms (with the function spellings
:sqlfunc:`cond` and :sqlfunc:`given`), is documented in
:doc:`the conditioning chapter <conditioning>`.

Problem 3: The Right Method, Chosen for You
-------------------------------------------

A probability can be computed from a circuit in many ways -- some exact
(factoring out independent parts, `inclusion--exclusion
<https://en.wikipedia.org/wiki/Inclusion%E2%80%93exclusion_principle>`_,
`knowledge compilation <https://en.wikipedia.org/wiki/Knowledge_compilation>`_,
`tree decomposition <https://en.wikipedia.org/wiki/Tree_decomposition>`_),
some by random sampling. You rarely choose: the evaluation strip (and
:sqlfunc:`probability_evaluate` under it) picks the cheapest method that
*fits the circuit* -- and "fits" matters, because not every method works on
every circuit.

Re-run the "at least one condition" query from Problem 2 and click its
token to bring the circuit back up:

.. code-block:: postgresql

    SELECT 'diabetes or hypertension' AS event
    FROM risk o, risk x
    WHERE o.factor = 'obesity'
      AND x.factor IN ('insulin_resistance', 'high_salt')
    GROUP BY ();

Press :fa:`bolt` **Evaluate**, leave the method on its default, and
:fa:`play` **Run**: 0.44, and the strip notes *via possible-worlds* -- the
exact method the chooser judged cheapest here. Now change the **method**
dropdown in the strip and re-run:

* `Monte-Carlo <https://en.wikipedia.org/wiki/Monte_Carlo_method>`_ works on
  *any* circuit, but only approximately: it samples at random, so its
  estimate lands near 0.44, not exactly on it. Give it an accuracy target in
  the strip's arguments box -- ``eps=0.1, delta=0.05`` -- and it reports the
  guarantee it met: the interval the true probability lies within, at the
  requested confidence (95% here).
* ``independent`` *refuses* -- ``Not an independent circuit`` -- because it
  assumes no input is shared between the parts it multiplies, and here
  ``obesity`` feeds both conditions (the shared gate in that circuit).

That refusal is the point. The chooser never offered ``independent`` for
this circuit; the bare default stays correct as your circuits gain
structure, and a method that does not fit is refused, never quietly wrong.

Problem 4: A Continuous Quantity
--------------------------------

Every event so far was discrete -- true or false. But a lab `biomarker
<https://en.wikipedia.org/wiki/Biomarker>`_ is a *continuous* number, and
ProvSQL represents one as a ``random_variable``: a
value that is not a single number but a whole distribution. Conditioning on
it works exactly as ``|`` did for discrete events, and the result is again
a distribution, whose mean, variance, and range you can read off with
:sqlfunc:`expected`, :sqlfunc:`variance`, and :sqlfunc:`support`.

We want exact, closed-form answers, not sampled ones. In the **Config**
panel, set ``provsql.rv_mc_samples`` to 0: that turns off the Monte-Carlo
fallback, so ProvSQL either answers a continuous query in closed form or
tells you it cannot, instead of silently sampling. The panel applies the
setting to every cell you run (outside Studio, the equivalent is
``SET provsql.rv_mc_samples = 0``).

Build a biomarker that follows a `Normal distribution <https://en.wikipedia.org/wiki/Normal_distribution>`_ with mean 20 and
standard deviation 5, and look at it:

.. code-block:: postgresql

    SELECT normal(20, 5) AS biomarker;

One row comes back with a ``random_variable`` token. **Click it**, then on
the circuit cell press :fa:`external-link-alt` **Circuit mode** (this leaves
the notebook; you will come back through the :fa:`book-open` **Notebook**
tab in the mode switcher at the top). In Circuit mode, press :fa:`play`
**Run** on the evaluation strip: for a continuous variable the default
evaluation is its *distribution profile*, and Studio draws the bell curve
of Normal(20, 5), centred on 20. Have a look, then return through the
**Notebook** tab -- the notebook reopens where you left it.

Its `mean <https://en.wikipedia.org/wiki/Expected_value>`_ is no surprise:

.. code-block:: postgresql

    SELECT expected(normal(20, 5));

20. Now suppose a patient is *referred* -- their biomarker came back above a
threshold of 25. Conditioning the variable on that event, ``x | (x > 25)``,
chops off everything below 25 and renormalises; the result is itself a
``random_variable``. Its mean is no longer 20:

.. code-block:: postgresql

    WITH r AS (SELECT normal(20, 5) AS x)
    SELECT expected(x | (x > 25)) FROM r;

About **27.6** -- a referred patient's biomarker is expected well above the
threshold, not at the population average. The conditioned variable has a
spread and a range too:

.. code-block:: postgresql

    WITH r AS (SELECT normal(20, 5) AS x)
    SELECT variance(x | (x > 25))     AS variance,
           (support(x | (x > 25))).lo AS lowest_value
    FROM r;

The `variance <https://en.wikipedia.org/wiki/Variance>`_ has shrunk to about 5 (from 25 for the unconditioned Normal),
and the lowest value the conditioned biomarker can take is exactly 25 --
`truncation <https://en.wikipedia.org/wiki/Truncated_normal_distribution>`_ moved the floor up to the threshold.

We took those moments one at a time, but the conditioned biomarker is a
single object -- a ``random_variable`` you can hand onward, store, or
inspect whole. Print it:

.. code-block:: postgresql

    WITH r AS (SELECT normal(20, 5) AS x)
    SELECT (x | (x > 25)) AS referred_biomarker
    FROM r;

**Click its token, open** :fa:`external-link-alt` **Circuit mode, and press**
:fa:`play` **Run.** Its distribution profile is the *truncated* bell curve
-- everything below 25 cut away, the rest renormalised -- with its support,
mean, and variance reported alongside: the same conditioned variable you
just took moments of, now seen as a whole distribution.

Problem 5: A Probabilistic Total
--------------------------------

The last carrier is an *aggregate*. When the rows you add up are themselves
uncertain, their total is a random quantity, and ProvSQL tracks it as an
``agg_token`` -- the aggregate counterpart of a provenance token.

Each row of ``cases`` reports a day's case count for a region, present only
with some probability:

.. code-block:: postgresql

    DROP TABLE IF EXISTS cases CASCADE;
    CREATE TABLE cases(day int, region text, n int, p float);
    INSERT INTO cases VALUES (1, 'North', 3, 0.5), (1, 'North', 4, 0.5), (1, 'South', 2, 0.8);
    SELECT add_provenance('cases');
    SELECT day, region, n, p, set_prob(provenance(), p) FROM cases;

Sum each region's counts with an ordinary ``GROUP BY``. Because the inputs
are uncertain, each total comes out as an ``agg_token`` rather than a plain
number -- look at the per-region totals:

.. code-block:: postgresql

    DROP TABLE IF EXISTS casesum CASCADE;
    CREATE TABLE casesum AS SELECT region, sum(n) AS total FROM cases GROUP BY region;
    SELECT * FROM casesum;

The North has two possible contributions, 3 and 4, each present with
probability 0.5. Its *expected* total -- the average over all the ways the
days could turn out -- is:

.. code-block:: postgresql

    SELECT expected(total) FROM casesum WHERE region = 'North';

3.5 (that is :math:`0.5 \cdot 3 + 0.5 \cdot 4`). Now condition on an
observation: suppose we *know* the high-count day (``n = 4``) really
happened. The expected total *given* that:

.. code-block:: postgresql

    SELECT expected(total | (SELECT provenance() FROM cases WHERE n = 4))
    FROM casesum WHERE region = 'North';

5.5 -- a certain 4 plus the still-uncertain 3 (worth :math:`0.5 \cdot 3 =
1.5` on average). The ``|`` operator conditioned the aggregate exactly as it
conditioned the discrete events in Problem 1 and the continuous variable in
Problem 4.

Like the conditioned biomarker, the conditioned total is a value in its own
right -- here an ``agg_token``. Select it directly to get its token:

.. code-block:: postgresql

    SELECT total | (SELECT provenance() FROM cases WHERE n = 4) AS conditioned_total
    FROM casesum WHERE region = 'North';

**Click that token to see its circuit** -- the same ``∣`` gate as before,
now over the aggregate and the evidence. In :fa:`external-link-alt`
**Circuit mode**, the evaluation strip defaults to the *distribution profile*;
switch it to the **moment** evaluator (order *k* = 1, *raw*) and press
:fa:`play` **Run**: its expected value comes back as **5.5**, the same answer
as above. A moment of an aggregate is computed *exactly* -- by enumerating the
rows' contributions and weighting each by its probability -- so it needs no
sampling and is unaffected by the ``provsql.rv_mc_samples = 0`` you set in
Problem 4 (its full distribution and individual samples, by contrast, are
estimated by Monte Carlo, and would need sampling switched back on).

Problem 6: Ruling Worlds Out
----------------------------

:sqlfunc:`repair_key` in Problem 1 imposed one kind of constraint -- *at most
one* row of a key group is real. Many real rules are not keys but relations
between *pairs* of rows: "no two reported doses of one vaccine fall within 21
days", "no two outbreak cases at one site are reported within the incubation
window". No key declaration captures these. They are **denial constraints** --
a query describing a forbidden pattern -- and ProvSQL conditions on their
*non-occurrence* with the event-negation operator ``!``
(:sqlfunc:`provenance_not`).

An immunization registry merges dose reports from several sources, so each
reported dose is only *probably* a real administration. A data-quality rule
says two doses of the same vaccine must be at least 21 days apart. Here are
four uncertain dose reports for one patient -- the date each was administered
and the probability it is genuine. The Mar 14 report sits close to two others:
it is within 21 days of both Mar 4 (10 days earlier) and Mar 28 (14 days
later).

.. code-block:: postgresql

    DROP TABLE IF EXISTS doses CASCADE;
    CREATE TABLE doses(id int, administered date, p float);
    INSERT INTO doses VALUES
      (1, '2024-03-04', 0.5),
      (2, '2024-03-14', 0.5),
      (3, '2024-03-28', 0.5),
      (4, '2024-04-30', 0.8);
    SELECT add_provenance('doses');
    SELECT id, administered, p, set_prob(provenance(), p) FROM doses;

The forbidden pattern -- "some two doses are fewer than 21 days apart" -- is an
ordinary self-join. Materialise it, collapsing all the witnessing pairs into a
single ``DISTINCT`` row: that one row's *provenance* is the violation event
``W``, "the record has a too-close pair". Two pairs qualify -- (Mar 4, Mar 14)
and (Mar 14, Mar 28) -- and they *share* the Mar 14 dose, so ``W`` is not a
simple product of independent pairs; ProvSQL tracks the shared gate and gets
the overlap right (the same correlation-awareness as Problem 2). So
``provenance()`` over the ``violation`` table is ``W``, and ``!provenance()`` is
the complementary "valid record" event:

.. code-block:: postgresql

    DROP TABLE IF EXISTS violation;
    CREATE TEMP TABLE violation AS
      SELECT DISTINCT 1
      FROM doses a JOIN doses b
        ON a.id < b.id AND abs(a.administered - b.administered) < 21;

    SELECT probability_evaluate(provenance())  AS p_violation,
           probability_evaluate(!provenance()) AS p_valid
    FROM violation;

The clash has probability 0.375, so a valid record (``!provenance()``) has
probability 0.625.

Now condition each dose on the record being valid -- one row per dose. Prior and
posterior are the *same* row token, ``provenance()``, evaluated two ways:
unconditioned, and conditioned on ``!W``. Each row's own provenance stays the
dose itself; the violation event is pulled in by an inert
``(SELECT provenance() FROM violation)`` -- naming ``W`` once, without coupling
it into the row's lineage:

.. code-block:: postgresql

    SELECT d.id,
           probability_evaluate(provenance()) AS prior,
           probability_evaluate(provenance() | !(SELECT provenance() FROM violation)) AS posterior
    FROM doses d
    ORDER BY d.id;

Same prior, different posterior. A valid record is evidence against exactly the
doses the constraint could have caught, in proportion to how implicated each is:
the Mar 14 dose (dose 2), which would clash with *either* neighbour, drops the
furthest -- from 0.5 to **0.2** -- while doses 1 and 3, each in only one of the
two possible violations, drop to **0.4**; dose 4, far from the rest and in no
possible clash, is untouched at **0.8**. The constraint here was an arbitrary
query: ``!`` turns any forbidden pattern into evidence, the way
:sqlfunc:`repair_key` turns a key into mutual exclusion, but without being
limited to keys.

Recap
-----

The six problems used one operator, ``|``, with a single meaning
throughout -- conditional probability, :math:`\Pr(A \mid B) = \Pr(A \wedge
B) / \Pr(B)` -- over three kinds of value: discrete events (Problems 1-3
and 6), a continuous ``random_variable`` (Problem 4), and a probabilistic
aggregate
``agg_token`` (Problem 5). A few mechanics recurred:

* Each model was built and stored in the database. :sqlfunc:`add_provenance`
  registers a table for tuple-independent tracking and :sqlfunc:`set_prob`
  attaches a probability to each row; :sqlfunc:`repair_key` is the alternative
  registration, making the rows of a key group mutually exclusive outcomes,
  and the negation operator ``!`` conditions on the non-occurrence of an
  arbitrary forbidden pattern (Problem 6) -- a denial constraint beyond keys.
  The model is ordinary SQL data: it persists, and is queried and updated,
  across sessions.
* Provenance is recorded per result row as a circuit of gates, and equal
  sub-expressions are the same gate. A base event shared between two queries
  is therefore one node, so joint, disjoint, and conditional probabilities
  come out consistent without assuming independence. That is the gap between
  ProvSQL's 0.44 and the independence estimate 0.545 in Problem 2.
* :sqlfunc:`probability_evaluate` returns an exact probability and selects the
  evaluation method itself; Studio's evaluation strip reports which one ran.
  Sampling appears only where no exact method applies (the Monte-Carlo paths
  in Problems 4 and 5), and then with a stated error guarantee.

None of this required leaving SQL: the questions were ordinary queries, and
the probabilistic answers were functions applied to their provenance.
