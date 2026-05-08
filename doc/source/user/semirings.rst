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

ProvSQL Studio's :ref:`evaluation strip <studio-circuit-eval-strip>`
drives every compiled and custom semiring interactively: pick a
semiring and a provenance mapping in the dropdown, click :guilabel:`Run`,
and the result lands inline. The rest of this chapter is the SQL
reference for the same operations.

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
semiring, returning a human-readable propositional formula. The mapping
argument is optional: with one, leaves are labelled by the mapping's
``value`` column; without one, leaves are rendered as bare ``x<id>``
placeholders.

.. code-block:: postgresql

    SELECT name, sr_boolexpr(provenance(), 'my_mapping')
    FROM mytable;

    SELECT name, sr_boolexpr(provenance())
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

Leaf values may be bare labels (e.g. ``Alice``, treated as the singleton
witness ``{{Alice}}``) or already-structured why-provenance values
(``{}`` for zero, ``{{}}`` for one, ``{{a},{b,c}}`` for a multi-witness
set), which lets the output of one ``sr_why`` query be reused as input
to another.

How-Provenance
---------------

:sqlfunc:`sr_how` returns the *how-provenance* of a result – the
canonical polynomial in :math:`\mathbb{N}[X]` over the input-tuple
labels (Green, Karvounarakis & Tannen, *Provenance Semirings*,
PODS'07).  Each derivation contributes a monomial; coefficients count
distinct derivations of the same monomial:

.. code-block:: postgresql

    SELECT name, sr_how(provenance(), 'my_mapping')
    FROM mytable;

The result is rendered in canonical sum-of-products form, e.g.
``2⋅Alice⋅Bob + Alice^2 + Bob^2``.  Multiplication is the dot
``⋅``; exponents use ``^k``; ``0`` and ``1`` denote the additive and
multiplicative identities.  Because the form is canonical, two
semantically-equivalent provenance circuits collapse to identical
strings, making :sqlfunc:`sr_how` suitable for provenance-aware query
equivalence (e.g. checking that two ETL pipelines produce the same
provenance, not just the same tuples).  The how-semiring is
:math:`\mathbb{N}[X]`, the universal commutative semiring for
provenance.

Leaf values may be bare labels (e.g. ``Alice``, treated as the
monomial ``Alice``), the literal ``0``, or already-structured
polynomials following the same canonical syntax as the output (e.g.
``2⋅Alice⋅Bob^2 + 3⋅Charlie``), so the output of one ``sr_how`` query
can be reused as input to another.

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

Leaf values may be bare labels (e.g. ``Alice``, treated as the
singleton ``{Alice}``), the literal ``⊥`` (the additive zero), or
already-structured sets (``{}`` for the multiplicative identity,
``{a,b,c}`` for a non-empty set), so the output of one ``sr_which``
query can be reused as input to another.

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
truth (:math:`0.7 \otimes_{\text{Ł}} 1 = 0.7`) and avoids the
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

Numeric and Integer Interval-Union Semirings (m-semirings)
-----------------------------------------------------------

:sqlfunc:`sr_interval_num` and :sqlfunc:`sr_interval_int` are the
numeric and integer counterparts of :sqlfunc:`sr_temporal`: the same
interval-union algebra (union for ⊕, intersection for ⊗, set
difference for monus) over ``nummultirange`` and ``int4multirange``
respectively.

.. code-block:: postgresql

    SELECT name, sr_interval_num(provenance(), 'sensor_validity_mapping')
    FROM mytable;

    SELECT cite, sr_interval_int(provenance(), 'page_range_mapping')
    FROM mytable;

Typical use cases:

- ``sr_interval_num``: measurement-validity provenance in scientific
  data integration (a sensor reading or model coefficient is valid
  only over a parameter range; joins compute jointly-valid ranges).
- ``sr_interval_int``: page-range or line-range provenance in
  scholarly / source-code corpora ("supported by pages [12,18] of
  doc A and pages [3,5] ∪ [40,42] of doc B").

Both require PostgreSQL ≥ 14.

Min-Max and Max-Min Semirings (m-semirings)
--------------------------------------------

:sqlfunc:`sr_minmax` and :sqlfunc:`sr_maxmin` evaluate the provenance
in the min-max and max-min m-semirings over an arbitrary user-defined
PostgreSQL ``ENUM`` type. The carrier order comes from
``pg_enum.enumsortorder``; bottom and top are derived automatically.

- :sqlfunc:`sr_minmax`: ``⊕ = min``, ``⊗ = max``, zero is the top of
  the enum, one is the bottom. The *security* shape: alternative
  derivations combine to the least sensitive label, joins combine to
  the most sensitive label.
- :sqlfunc:`sr_maxmin`: dual ``⊕ = max``, ``⊗ = min``. The *fuzzy* /
  availability / trust shape: alternatives combine to the most
  permissive label, joins combine to the strictest label.

The third argument to both functions is a sample value of the carrier
enum, used only for type inference; its value is ignored. Given a
``classification_level`` enum ordered from ``unclassified`` to
``not_available``, where ``not_available`` is the top of the enum and
plays the role of the semiring 𝟘 (no derivation possible):

.. code-block:: postgresql

    SELECT create_provenance_mapping('personnel_level', 'personnel', 'classification');

    SELECT city, sr_minmax(provenance(), 'personnel_level',
                           'unclassified'::classification_level) AS clearance
    FROM (SELECT DISTINCT city FROM personnel) t;

This is the compiled replacement for the hand-rolled access-control
semiring previously documented as the *security semiring*: a single
implementation covers any user enum (security lattices, fuzzy-discrete
trust levels, three-valued logic, project-specific orderings).

.. _custom-semirings:

Custom Semirings with :sqlfunc:`provenance_evaluate`
------------------------------------------------------

Advanced users can define custom semirings in SQL and evaluate them
using :sqlfunc:`provenance_evaluate`.  The function takes a provenance token,
a mapping table, a one element, and the names of aggregate functions
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
functions with a two-argument state transition function. The monus and
delta operations are plain functions.

As a worked example, consider the *capability* semiring over the
``bit(2)`` carrier interpreted as the diamond lattice
:math:`\{00, 01, 10, 11\}` (e.g., ``(can_read, can_write)``).
``⊕ = |`` (bitwise OR) combines alternative derivations permissively;
``⊗ = &`` (bitwise AND) combines joins restrictively;
``⊖ = a & ~b`` (bitwise AND-NOT) is the Boolean difference monus;
``δ`` is the identity, so an aggregated group carries the OR of the
capabilities of its supporting rows (e.g., a Paris group built from a
read-only and a write-only row is annotated ``B'11'``, while a group
built from two read-only rows stays ``B'01'``). Zero is ``B'00'``,
one is ``B'11'``:

.. code-block:: postgresql

    CREATE FUNCTION cap_or  (state bit(2), v bit(2)) RETURNS bit(2) IMMUTABLE
      LANGUAGE SQL AS $$ SELECT state | v $$;
    CREATE FUNCTION cap_and (state bit(2), v bit(2)) RETURNS bit(2) IMMUTABLE
      LANGUAGE SQL AS $$ SELECT state & v $$;
    CREATE FUNCTION cap_minus(a bit(2), b bit(2))    RETURNS bit(2) IMMUTABLE
      LANGUAGE SQL AS $$ SELECT a & ~b $$;
    CREATE FUNCTION cap_delta(v bit(2)) RETURNS bit(2) IMMUTABLE
      LANGUAGE SQL AS $$ SELECT v $$;

    CREATE AGGREGATE cap_plus (bit(2)) (
      sfunc=cap_or,  stype=bit(2), initcond='00');
    CREATE AGGREGATE cap_times(bit(2)) (
      sfunc=cap_and, stype=bit(2), initcond='11');

    SELECT name,
           provenance_evaluate(provenance(), 'capability_mapping',
                               B'11'::bit(2),
                               'cap_plus', 'cap_times', 'cap_minus', 'cap_delta')
    FROM mytable;


This is a commutative m-semiring on the four-element Boolean
lattice :math:`B^2`; the lattice is partial (the two middle elements
are incomparable), so it is not subsumed by :sqlfunc:`sr_minmax` or
:sqlfunc:`sr_maxmin`. Additional examples can be found in the test
suite: :download:`test/sql/capability.sql <../../../test/sql/capability.sql>`
(the capability semiring above) and
:download:`test/sql/formula.sql <../../../test/sql/formula.sql>`
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
