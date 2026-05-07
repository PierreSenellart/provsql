Semiring Evaluation
====================

ProvSQL represents query provenance as a circuit of gates. A *semiring evaluation* :cite:`DBLP:conf/pods/GreenKT07` maps this circuit
to a value in a chosen semiring by:

1. Assigning a *leaf value* to each input gate via a provenance mapping.
2. Propagating these values through ``plus`` and ``times`` gates (and
   ``monus`` gates for m-semirings :cite:`DBLP:journals/japll/GeertsP10`)
   according to the semiring operations.

The built-in evaluation functions all follow the same calling convention::

    sr_<name>(provenance(), 'mapping_name')

where ``provenance()`` returns the token for the current output row and
``mapping_name`` is the name of a provenance mapping table.
Semirings are also extended with a *monus* operation allowing to
represent the output of non-monotone queries.

Boolean Semiring
-----------------

:sqlfunc:`sr_boolean` evaluates the provenance in the Boolean semiring
``({true, false}, ∨, ∧, false, true)``, answering the question "was this
result derivable at all, depending on the Boolean mapping of input
provenance tokens?":

.. code-block:: postgresql

    SELECT name, sr_boolean(provenance(), 'my_mapping')
    FROM mytable;

Boolean-Expression Semiring
-----------------------------

:sqlfunc:`sr_boolexpr` evaluates the provenance in the *Boolean-expression*
semiring, returning a human-readable propositional formula:

.. code-block:: postgresql

    SELECT name, sr_boolexpr(provenance(), 'my_mapping')
    FROM mytable;

This is used as the basis for probability computation.


Symbolic Representation (as a Formula)
----------------------------------------

:sqlfunc:`sr_formula` returns a symbolic representation of the provenance
as a human-readable formula using symbols ``⊕`` and ``⊗``:

.. code-block:: postgresql

    SELECT name, sr_formula(provenance(), 'witness_mapping')
    FROM suspects;

For example, a result supported by tuples labelled ``a`` and ``b`` through
a join would show as ``a⊗b``; one that could come from either ``a`` or
``b`` would show as ``a⊕b``.

Counting Semiring (m-semiring)
-------------------------------

:sqlfunc:`sr_counting` evaluates the provenance in the *counting* m-semiring,
counting the number of distinct supporting inputs:

.. code-block:: postgresql

    SELECT name, sr_counting(provenance(), 'count_mapping')
    FROM suspects;

The mapping should assign integer values (typically ``1``) to leaf tokens.

Why-Provenance
---------------

:sqlfunc:`sr_why` returns the *why-provenance* of a result – the set of
minimal witnesses (sets of input tuples) that support the result:

.. code-block:: postgresql

    SELECT name, sr_why(provenance(), 'my_mapping')
    FROM mytable;

Which-Provenance (Lineage)
---------------------------

:sqlfunc:`sr_which` returns the *which-provenance* (also known as
*lineage*) of a result – a single set of input labels that contributed
to it, namely the union of all witnesses:

.. code-block:: postgresql

    SELECT name, sr_which(provenance(), 'my_mapping')
    FROM mytable;

The result is rendered as ``{a,b,c}`` for a non-empty derivation, or
``⊥`` if no derivation exists.  Compared to :sqlfunc:`sr_why`,
which-provenance is more compact (a flat set rather than a set of
sets) but loses the breakdown into individual derivations.

Tropical Semiring (m-semiring)
-------------------------------

:sqlfunc:`sr_tropical` evaluates the provenance in the *tropical* (min-plus)
m-semiring ``(ℝ ∪ {+∞}, min, +, +∞, 0)``, returning the cost of the
cheapest derivation:

.. code-block:: postgresql

    SELECT name, sr_tropical(provenance(), 'cost_mapping')
    FROM mytable;

The mapping should assign ``float8`` cost values to leaf tokens; use
``'Infinity'::float8`` to encode the additive identity. This is useful
for shortest-path or least-cost provenance, where ``plus`` selects the
cheaper alternative and ``times`` accumulates cost along a derivation.

Viterbi Semiring (m-semiring)
------------------------------

:sqlfunc:`sr_viterbi` evaluates the provenance in the *Viterbi* (max-times)
m-semiring ``([0,1], max, ×, 0, 1)``, returning the probability of the
most likely derivation:

.. code-block:: postgresql

    SELECT name, sr_viterbi(provenance(), 'prob_mapping')
    FROM mytable;

The mapping should assign ``float8`` probability values in :math:`[0,1]`
to leaf tokens. Unlike :sqlfunc:`probability_evaluate`, which marginalises
over derivations, the Viterbi semiring keeps only the single most likely
derivation, making it suitable for *most-probable-explanation* style
queries.

Łukasiewicz Fuzzy Semiring (m-semiring)
----------------------------------------

:sqlfunc:`sr_lukasiewicz` evaluates the provenance in the *Łukasiewicz*
fuzzy m-semiring ``([0,1], max, ⊗_Ł, 0, 1)``, where the
multiplicative operation is the Łukasiewicz t-norm
:math:`a \otimes_{\text{Ł}} b = \max(a + b - 1, 0)`:

.. code-block:: postgresql

    SELECT name, sr_lukasiewicz(provenance(), 'evidence_mapping')
    FROM mytable;

The mapping should assign ``float8`` graded-truth values in
:math:`[0,1]` to leaf tokens. Compared to :sqlfunc:`sr_viterbi` (which
multiplies probabilities), the Łukasiewicz t-norm preserves crisp
truth — :math:`0.7 \otimes_{\text{Ł}} 1 = 0.7` — and avoids the
near-zero collapse of long product chains. This makes it the standard
choice for fuzzy graded conjunctions where inputs are degrees of
evidence rather than independent probabilities.

Temporal (Interval-Union) Semiring (m-semiring)
------------------------------------------------

:sqlfunc:`sr_temporal` evaluates the provenance in the *temporal*
(interval-union) m-semiring over PostgreSQL ``tstzmultirange`` values.
Addition is multirange union, multiplication is intersection, monus is
set difference; the additive identity is ``'{}'`` and the multiplicative
identity is ``'{(,)}'`` (the universal range):

.. code-block:: postgresql

    SELECT entity_id, sr_temporal(provenance(), 'validity_mapping')
    FROM mytable;

This is the compiled counterpart of :sqlfunc:`union_tstzintervals`. The
two compute the same quantity for plain SELECT-FROM-WHERE-GROUP BY
queries, but :sqlfunc:`sr_temporal` additionally supports HAVING clauses,
aggregation, and where-provenance, which the PL/pgSQL evaluator skips.

Requires PostgreSQL ≥ 14 (for ``tstzmultirange``).

Security Semiring
------------------

The security semiring assigns security-level labels to tuples and
propagates them through queries according to a lattice. It is implemented
using :sqlfunc:`provenance_evaluate` with custom aggregates for the
semiring plus and times operations. For example, given a type
``classification_level`` (an enum ordered from ``unclassified`` to
``top_secret``):

.. code-block:: postgresql

    -- Define the semiring operations
    CREATE FUNCTION security_plus_state(state classification_level,
                                        level classification_level)
      RETURNS classification_level LANGUAGE SQL IMMUTABLE AS $$
        SELECT LEAST(state, level)
    $$;

    CREATE FUNCTION security_times_state(state classification_level,
                                         level classification_level)
      RETURNS classification_level LANGUAGE SQL IMMUTABLE AS $$
        SELECT GREATEST(state, level)
    $$;

    CREATE AGGREGATE security_plus(classification_level) (
      sfunc = security_plus_state, stype = classification_level,
      initcond = 'unavailable'
    );
    CREATE AGGREGATE security_times(classification_level) (
      sfunc = security_times_state, stype = classification_level,
      initcond = 'unclassified'
    );

    -- Evaluate the security level of a query result
    SELECT create_provenance_mapping('personnel_level', 'personnel', 'classification');

    SELECT city, provenance_evaluate(provenance(), 'personnel_level',
                                     'unclassified'::classification_level,
                                     'security_plus', 'security_times')
    FROM (SELECT DISTINCT city FROM personnel) t;

.. _custom-semirings:

Custom Semirings with :sqlfunc:`provenance_evaluate`
------------------------------------------------------

Advanced users can define custom semirings in SQL and evaluate them
using :sqlfunc:`provenance_evaluate`.  The function takes a provenance token,
a mapping table, a zero element, and the names of aggregate functions
implementing the semiring operations:

.. code-block:: postgresql

    provenance_evaluate(
        token UUID,           -- provenance token to evaluate
        token2value regclass, -- mapping table (token → value)
        element_one,          -- identity element (type determines the semiring value type)
        plus_function,        -- name of the ⊕ aggregate
        times_function,       -- name of the ⊗ aggregate
        monus_function,       -- name of the ⊖ function (optional, for m-semirings)
        delta_function        -- name of the δ function (optional, for δ-semirings)
    )

The plus and times operations must be defined as PostgreSQL aggregate
functions with a two-argument state transition function.  The monus and
delta operations are plain functions.  See the security semiring example
above for a complete illustration.  Additional examples can be found in
the test suite: :download:`test/sql/security.sql <../../../test/sql/security.sql>`
(security semiring) and :download:`test/sql/formula.sql <../../../test/sql/formula.sql>`
(symbolic representation as a formula).

For queries involving aggregation (``GROUP BY``), use
:sqlfunc:`aggregation_evaluate` instead, which additionally takes the names
of an aggregate finalization function and a semimodule operation:

.. code-block:: postgresql

    aggregation_evaluate(
        token,                -- aggregate result (agg_token)
        token2value,          -- mapping table
        agg_final_function,   -- finalization function for the aggregate
        agg_function,         -- aggregate function for group values
        semimod_function,     -- semimodule scalar multiplication
        element_one,          -- identity element
        plus_function,
        times_function,
        monus_function,       -- optional
        delta_function        -- optional
    )

See :download:`test/sql/aggregation.sql <../../../test/sql/aggregation.sql>`
for a complete example of :sqlfunc:`aggregation_evaluate` usage.

.. note::

    :sqlfunc:`provenance_evaluate` and :sqlfunc:`aggregation_evaluate`
    are PL/pgSQL functions that traverse the provenance circuit
    recursively.  They do not support ``cmp`` gates introduced by
    ``HAVING`` clauses; queries with ``HAVING`` will produce an error.
    The built-in compiled semirings (:sqlfunc:`sr_formula`,
    :sqlfunc:`sr_counting`, etc.) are implemented in C, support all
    gate types including ``HAVING``, and are significantly faster.
    Prefer compiled semirings when available; use
    :sqlfunc:`provenance_evaluate` for semirings not covered by the
    built-in set.

Provenance Mappings
--------------------

All semiring functions take a mapping name as their second argument.
A mapping is created with :sqlfunc:`create_provenance_mapping`:

.. code-block:: postgresql

    SELECT create_provenance_mapping('mapping_name', 'table_name', 'column_name');

The mapping table has columns ``token`` (uuid) and ``value``. You can
also populate it manually for custom scenarios:

.. code-block:: sql

    INSERT INTO mapping_name(token, value)
    SELECT provenance(), my_label FROM mytable;
