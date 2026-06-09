.. _conditioning:

Conditioning
============

ProvSQL can compute **conditional** probabilities, distributions, and
expectations: the answer to a query *given* that some other event is known
to hold. The single entry point is the conditioning operator ``|``, read as
"given": ``A | B`` means "``A`` given ``B``". It works identically over the
three carriers ProvSQL tracks -- discrete events, continuous
``random_variable`` values, and probabilistic ``agg_token`` aggregates --
and it is **exact** and **correlation-aware**: the conditional is computed
from the shared provenance circuit, so events that overlap are accounted for
without any independence assumption or hand-written
`inclusion--exclusion <https://en.wikipedia.org/wiki/Inclusion%E2%80%93exclusion_principle>`_.

This chapter documents the operator and its variants. For the underlying
probability machinery see :doc:`probabilities`, for the continuous surface
see :doc:`continuous-distributions`, and for a worked, end-to-end tour see
:doc:`casestudy8`.

What conditioning means
-----------------------

For two Boolean events ``A`` and ``B``, ``A | B`` denotes the conditional
probability in the textbook sense, `Bayes' rule
<https://en.wikipedia.org/wiki/Bayes%27_theorem>`_:

.. math::

   \Pr(A \mid B) = \frac{\Pr(A \wedge B)}{\Pr(B)}.

ProvSQL realises this by building a terminal *conditioned gate* over the two
provenance tokens, whose probability evaluators read as exactly that ratio.
Because gates are addressed by content, a base tuple shared between ``A`` and
``B`` is **literally the same input gate** in both circuits, so the joint
:math:`\Pr(A \wedge B)` is computed over the real overlap -- the conditional
is correct even when ``A`` and ``B`` are correlated.

Two conventions follow from the definition:

* **Conditioning on a certain or absent event is a no-op.** ``A | B`` returns
  ``A`` unchanged when ``B`` is the certain event or is ``NULL``
  (:math:`\Pr(A \mid \text{true}) = \Pr(A)`).
* **Nested conditioning folds** as a sequential `Bayesian update
  <https://en.wikipedia.org/wiki/Bayesian_inference>`_: ``(A | B) | C`` is
  the same as ``A | (B ∧ C)``. The conditioned gate never nests; it stays one
  level deep with the evidence accumulated.

The operator family
-------------------

``|`` comes in a binary and a unary (prefix) form, each accepting either a
provenance token or a Boolean predicate on the right.

Binary ``value | evidence`` -- conditioning a value
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The binary operator conditions **one value** -- a discrete event token, a
``random_variable``, or an ``agg_token`` -- on an evidence event, and
returns a new conditioned value of the same carrier. The right operand is
either a provenance token,

.. code-block:: postgresql

    -- P(disease | positive) over a shared screening model
    SELECT probability_evaluate(disease | positive)
    FROM (SELECT (SELECT provenance() FROM screening WHERE disease  GROUP BY ()) AS disease,
                 (SELECT provenance() FROM screening WHERE positive GROUP BY ()) AS positive) e;

or a **Boolean predicate** built from ``random_variable`` / ``agg_token``
comparisons, which the planner lifts into an evidence gate for you:

.. code-block:: postgresql

    -- a biomarker conditioned on exceeding a referral threshold
    WITH r AS (SELECT normal(20, 5) AS x)
    SELECT expected(x | (x > 25)) FROM r;

The result of ``value | evidence`` is **terminal**: a conditioned value may
only be conditioned further, never combined into a larger ``plus`` /
``times`` / ``monus`` / aggregate gate.

The function spelling of the binary operator is :sqlfunc:`cond`
``(target, evidence)``, interchangeable with ``target | evidence``.

Unary ``| evidence`` -- conditioning a whole tuple
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Written as a term in the ``SELECT`` list (with no left operand), ``| evidence``
is a **whole-tuple directive**: it conditions the *output provenance of every
row* of the query on the evidence, then is stripped from the visible
projection. The function spelling is :sqlfunc:`given` ``(evidence)``,
the same as the prefix ``| evidence``.

.. code-block:: postgresql

    -- condition every output row on "obesity is present", in one directive
    SELECT factor, probability_evaluate(provenance()) AS p_given_obesity
    FROM (
      SELECT factor, | (SELECT provenance() FROM risk WHERE factor = 'obesity')
      FROM risk
    ) s
    ORDER BY factor;

Here the ``| (...)`` term contributes no column; instead each surviving row's
provenance becomes ``provenance() | <evidence>``. Over the correlation model
of :doc:`case study 8 <casestudy8>` this returns ``1`` for ``obesity`` itself
(an event given itself is certain) and each independent factor's prior
unchanged (``insulin_resistance`` ``0.6``, ``high_salt`` ``0.7``).

The evidence is evaluated **per output row** and may reference the row's own
columns, so each tuple can be conditioned on its **own** evidence -- the
typical use being a correlated sub-select:

.. code-block:: postgresql

    -- each patient's row conditioned on that same patient testing positive
    SELECT p.id, p.name,
           | (SELECT provenance() FROM tests t
              WHERE t.patient_id = p.id AND t.result = 'positive')
    FROM patients p;

The unary predicate form ``| (predicate)`` works the same way, with the
evidence written as a Boolean combination of ``random_variable`` /
``agg_token`` comparisons.

.. note::

   The unary :sqlfunc:`given` / ``|`` directive conditions the **whole-tuple
   existence event** (the row's provenance token), not the value of any
   ``random_variable`` or ``agg_token`` column you happen to select. To
   condition a *value* -- to truncate a distribution, say -- use the binary
   ``value | evidence`` form. The unary directive is only accepted in a plain
   per-row ``SELECT``; it is rejected on an aggregated, grouped, ``DISTINCT``,
   or set-operation (``UNION`` / ``EXCEPT`` …) query, where the individual
   tokens should be conditioned with the binary ``|`` instead.

Negating an event -- ``! event``
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The prefix ``!`` operator (function spelling :sqlfunc:`provenance_not`
``(event)``) is the **complement** of a Boolean provenance event: ``!x`` holds
in exactly
the worlds where ``x`` does not, so ``probability_evaluate(!x)`` is
:math:`1 - \Pr(x)`. Unlike the conditioned gate, ``!`` is an ordinary
m-semiring expression -- Boolean negation, :math:`\mathbb{1} \ominus x`
underneath -- so it composes freely under ``times`` / ``plus``; the one thing
it refuses is a conditioned (terminal) token.

Its natural use with conditioning is a **denial constraint**: restricting a
query to the worlds where some forbidden pattern *does not* occur. The
violation event ``W`` is just an ordinary query -- no hand-built gates --
aggregated to a single token with ``provenance() ... GROUP BY ()``, and the
query is conditioned on its negation, ``Q | !W``:

.. code-block:: postgresql

    -- P(booking 1 is present | no two overlapping bookings of the same room)
    WITH w AS (
      SELECT provenance() AS violation        -- W = "some overlapping pair exists"
      FROM bookings a JOIN bookings b
        ON a.id < b.id AND a.room = b.room
           AND a.lo < b.hi AND b.lo < a.hi
      GROUP BY ())
    SELECT probability_evaluate(
             (SELECT provenance() FROM bookings WHERE id = 1) | !w.violation)
    FROM w;

The constraint can be any query: a forbidden pattern expressed as a query
becomes a denial constraint by conditioning on the negation of "the pattern
occurs", so ``!W`` is the event "no violation" and ``Q | !W`` restricts ``Q``
to exactly the worlds the constraint admits. ``!`` is also useful on its own, wherever the
complement of an event is wanted (``probability_evaluate(!a)``,
``a AND NOT b`` as ``times(a, !b)``).

The three carriers
------------------

Discrete events
~~~~~~~~~~~~~~~~

A Boolean provenance token stands for the event "this row exists". Conditioning
one token on another yields a conditional probability:

.. code-block:: postgresql

    -- base-rate fallacy: a positive screening test, P(disease | positive)
    SELECT probability_evaluate(disease | positive) AS disease_given_pos
    FROM (SELECT (SELECT provenance() FROM screening WHERE disease  GROUP BY ()) AS disease,
                 (SELECT provenance() FROM screening WHERE positive GROUP BY ()) AS positive) e;
    --  ≈ 0.1538

Random variables
~~~~~~~~~~~~~~~~~

Conditioning a ``random_variable`` produces another ``random_variable`` -- the
conditional distribution -- whose mean, variance, and range you read with
:sqlfunc:`expected`, :sqlfunc:`variance`, and :sqlfunc:`support`:

.. code-block:: postgresql

    WITH r AS (SELECT normal(20, 5) AS x)
    SELECT expected(x | (x > 25))     AS cond_mean,      -- ≈ 27.6
           variance(x | (x > 25))     AS cond_variance,  -- ≈ 5  (was 25)
           (support(x | (x > 25))).lo AS lowest_value    -- 25  (truncated floor)
    FROM r;

Conditioning on a threshold predicate `truncates
<https://en.wikipedia.org/wiki/Truncated_normal_distribution>`_ the
distribution and renormalises it; the result is a value in its own right that
you can select, store, or hand onward. See :doc:`continuous-distributions`
for the closed-form truncation table (Normal, Uniform, Exponential) and the
Monte-Carlo fallback for other shapes.

Probabilistic aggregates
~~~~~~~~~~~~~~~~~~~~~~~~~~

When the rows being aggregated are themselves uncertain, the total is an
``agg_token``. Conditioning it on an observation gives a conditional
expectation:

.. code-block:: postgresql

    -- expected regional total, given that the high-count day really happened
    SELECT expected(total | (SELECT provenance() FROM cases WHERE n = 4))
    FROM casesum WHERE region = 'North';

The aggregate-specific spelling :sqlfunc:`expected` ``(aggregate, condition)``
(see :doc:`probabilities`) is the same operation; the ``|`` operator is the
uniform way to write it across all three carriers.

In ProvSQL Studio
-----------------

Studio's :ref:`evaluation strip <studio-circuit-eval-strip>` exposes
conditioning interactively: the :guilabel:`Condition on` input takes an
evidence provenance UUID, auto-presetting to a clicked row's own provenance,
with an adjacent :guilabel:`Conditioned by` badge that lights up while the
result is being conditioned on it -- click the badge to toggle the
conditioning off (an unconditional result) and back on. Distribution
profiles, moments, and probabilities all honour it,
so the truncated histogram of a conditioned ``random_variable`` and the
conditional mean of an ``agg_token`` are visible in the canvas. See
:doc:`studio` for the panel and :doc:`case study 6 <casestudy6>` for it in
use.

.. seealso::

   - :doc:`probabilities` -- the probability methods and the
     ``expected(aggregate, condition)`` form.
   - :doc:`continuous-distributions` -- conditioning ``random_variable``
     values and the truncation table.
   - :doc:`casestudy8` -- a five-problem tour driving the ``|`` operator
     across all three carriers.
