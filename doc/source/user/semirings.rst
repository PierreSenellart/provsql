Semiring Evaluation
====================

ProvSQL represents query provenance as a circuit of gates. A *semiring evaluation* :cite:`DBLP:conf/pods/GreenKT07` maps this circuit
to a value in a chosen semiring by:

1. Assigning a *leaf value* to each input gate via a provenance mapping.
2. Propagating these values through ``plus`` and ``times`` gates (and
   ``monus`` gates for m-semirings :cite:`DBLP:journals/japll/GeertsP10`)
   according to the semiring operations.

The built-in evaluation functions all follow the same calling convention:

.. code-block:: postgresql

    sr_<name>(provenance(), 'mapping_name')

where ``provenance()`` returns the token for the current output row and
``mapping_name`` is the name of a provenance mapping table.
Semirings are also extended with a *monus* operation, which represents
the output of non-monotone queries.

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

The mapping is optional: an input token it does not map is true, and
without a mapping :sqlfunc:`sr_boolean` tells whether the row holds in the
actual database, every input tuple present (a null-padded row of an outer
join does not when the row has a match).

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

Unlike the other ``sr_*`` functions, :sqlfunc:`sr_formula` serialises the
circuit instead of evaluating it, so it renders **every** kind of gate and
refuses none -- including the ones that carry no algebraic meaning and that
the proper semirings reject: random-variable leaves, arithmetic over them,
mixtures, ``CASE`` selections, observations and conditioning (see
:doc:`continuous-distributions`).  Ordinary arithmetic notation is used
for those, kept visually distinct from the semiring's ``⊕`` / ``⊗``:

.. code-block:: postgresql

    SELECT sr_formula((normal(0,1) + uniform(0,1))::uuid);
    -- normal(0, 1) + uniform(0, 1)

    SELECT sr_formula(mixture(0.4, normal(0,1), normal(5,1))::uuid);
    -- Bernoulli(0.4) ? normal(0, 1) : normal(5, 1)

The mapping argument is optional (such circuits have no leaf mapping at
all).  A variable leaf the mapping does not name -- every leaf when no
mapping is given, and the uncovered ones when a partial mapping is --
renders as an abbreviated UUID, the same abbreviation ProvSQL Studio
prints on the circuit's nodes:

.. code-block:: postgresql

    SELECT sr_formula(provenance(), 'witness_mapping') FROM suspects;
    -- Dave ⊗ 139dd2d7…          (Dave is in the mapping, the other tuple is not)

    SELECT sr_formula(provenance()) FROM suspects;
    -- 81ee17e5… ⊗ 139dd2d7…

This mirrors :sqlfunc:`sr_boolexpr`'s ``x<id>`` fallback.

A formula can be too large to read, or to return at all: the sum of an
aggregate over a large join has a term per pair of tuples, and a
condition on an aggregate is written as a disjunction over its possible
worlds.  :sqlfunc:`sr_formula_rows` writes the same formula as rows of
bounded size.  A sub-formula of more than ``max_size`` symbols (50 by
default) is named and given a row of its own, the first row being the
formula of the token.  A sum or product with too many terms for one row
is listed by the names of its terms, each with a row:

.. code-block:: postgresql

    CREATE TABLE total AS SELECT SUM(r.v) AS t FROM r, s;
    SELECT f.name, f.formula FROM total, sr_formula_rows(t, 'rs_map') f;
    -- #1   | #1.1+…+#1.300 (300 terms)
    -- #1.1 | (r1 ⊗ s1)*1
    -- …

A condition on an aggregate is expanded into its possible worlds only
when they are few enough for rows of that size; otherwise it is written
as the comparison itself, e.g., ``[#2 > 𝟙*2]`` with the sum as ``#2``.
The rows are computed one at a time, each reading only the part of the
circuit it writes.  Called in the select list, so that ``LIMIT`` stops it
early, it gives the first rows of a formula of any size (:sqlfunc:`plain`
keeps the ``LIMIT`` out of the provenance of ``total``):

.. code-block:: postgresql

    SELECT sr_formula_rows(t, 'rs_map') FROM total LIMIT plain(10);

In the ``FROM`` clause, PostgreSQL computes all the rows before it applies
the ``LIMIT``.

Counting Semiring (m-semiring)
-------------------------------

:sqlfunc:`sr_counting` evaluates the provenance in the *counting* m-semiring,
counting the number of derivations of the result (its multiplicity
under bag semantics):

.. code-block:: postgresql

    SELECT name, sr_counting(provenance(), 'count_mapping')
    FROM suspects;

The mapping should assign integer values (typically ``1``) to leaf tokens.
A result of a recursive query derived through a cycle has infinitely
many derivations, a count the integer result cannot hold: the evaluation
raises an error for it.  A count beyond the integer range is an error as
well.

Why-Provenance
---------------

:sqlfunc:`sr_why` returns the *why-provenance* of a result -- the set of
witnesses (sets of input tuples) that support the result:

.. code-block:: postgresql

    SELECT name, sr_why(provenance(), 'my_mapping')
    FROM mytable;

Leaf values may be bare labels (e.g., ``Alice``, treated as the singleton
witness ``{{Alice}}``) or already-structured why-provenance values
(``{}`` for zero, ``{{}}`` for one, ``{{a},{b,c}}`` for a multi-witness
set), which lets the output of one ``sr_why`` query be reused as input
to another.  The why-provenance of a result of a recursive query is
finite even on cyclic data, there being finitely many sets of input
tuples: going around a cycle again adds no new witness.

How-Provenance
---------------

:sqlfunc:`sr_how` returns the *how-provenance* of a result -- the
canonical polynomial in :math:`\mathbb{N}[X]` over the input-tuple
labels :cite:`DBLP:conf/pods/GreenKT07`.  Each derivation contributes
a monomial; coefficients count
distinct derivations of the same monomial:

.. code-block:: postgresql

    SELECT name, sr_how(provenance(), 'my_mapping')
    FROM mytable;

The result is rendered in canonical sum-of-products form, e.g.,
``2⋅Alice⋅Bob + Alice^2 + Bob^2``.  Multiplication is the dot
``⋅``; exponents use ``^k``; ``0`` and ``1`` denote the additive and
multiplicative identities.  Because the form is canonical, two
semantically-equivalent provenance circuits collapse to identical
strings, making :sqlfunc:`sr_how` suitable for provenance-aware query
equivalence (e.g., checking that two ETL pipelines produce the same
provenance, not just the same tuples).  The how-semiring is
:math:`\mathbb{N}[X]`, the universal commutative semiring for
provenance.

Leaf values may be bare labels (e.g., ``Alice``, treated as the
monomial ``Alice``), the literal ``0``, or already-structured
polynomials following the same canonical syntax as the output (e.g.,
``2⋅Alice⋅Bob^2 + 3⋅Charlie``), so the output of one ``sr_how`` query
can be reused as input to another.

Which-Provenance (Lineage)
---------------------------

:sqlfunc:`sr_which` returns the *which-provenance* (also known as
*lineage*) of a result -- a single set of input labels that contributed
to it, namely the union of all witnesses:

.. code-block:: postgresql

    SELECT name, sr_which(provenance(), 'my_mapping')
    FROM mytable;

The result is rendered as ``{a,b,c}`` for a non-empty derivation, or
``⊥`` if no derivation exists.  Compared to :sqlfunc:`sr_why`,
which-provenance is more compact (a flat set rather than a set of
sets) but loses the breakdown into individual derivations.

Leaf values may be bare labels (e.g., ``Alice``, treated as the
singleton ``{Alice}``), the literal ``⊥`` (the additive zero), or
already-structured sets (``{}`` for the multiplicative identity,
``{a,b,c}`` for a non-empty set), so the output of one ``sr_which``
query can be reused as input to another.

Regular Expressions
-------------------

:sqlfunc:`sr_regex` describes the derivations of a result as a regular
expression over the labels of its input tokens: a derivation is the word
of its labels, in the order of the query (that of the ``FROM`` clause for
a join), and alternatives are joined by union:

.. code-block:: postgresql

    SELECT e1.src, e2.dst, sr_regex(provenance(), 'label_mapping')
    FROM edges e1 JOIN edges e2 ON e1.dst = e2.src;

The result reads ``ab · bc`` for the path through the edges labelled
``ab`` then ``bc``: ``|`` is union, ``·`` concatenation, ``*`` the Kleene
star, ``ε`` the empty word (also what an empty label stands for) and
``∅`` the absence of any derivation.

Unlike the other semirings of this chapter, this multiplication does not
commute: ``ab · bc`` and ``bc · ab`` are two different words. Over a
recursive query on a cyclic graph, the regular expression is the exact
language of the derivations, a cycle appearing under a star:

.. code-block:: postgresql

    WITH RECURSIVE reach(node) AS (
        SELECT 'a'::text
      UNION
        SELECT e.dst FROM reach r JOIN edges e ON e.src = r.node
    )
    SELECT node, sr_regex(provenance(), 'label_mapping')
    FROM reach;

With edges ``a → b`` (labelled ``ab``), ``b → c`` (``bc``), ``c → a``
(``ca``) and ``a → c`` (``ac``), node ``c`` is reached by
``((ac | ab · bc) · ca)* · (ac | ab · bc)``. The recursive relation comes
first in the recursive ``FROM`` clause, so that each path reads in the
order it is followed; see :ref:`recursive-queries` for what the order
changes.

Equality of two expressions is syntactic: an alternative is merged with
an identical one, but two expressions of the same language are not
recognised as such. The semiring has no monus, and ``HAVING`` conditions
are not supported, as they have no order of the query to follow.

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

Over a recursive query on a cyclic graph, the min cost is that of the
cheapest path, and ``-Infinity`` for a vertex reached through a cycle of
negative cost.  With the optional third argument ``nonnegative => true``,
input costs are checked nonnegative and the semiring becomes *absorptive*
(:math:`\min(0, a) = 0` for :math:`a \ge 0`): a recursion on a cyclic
graph then evaluates faster, and circuits carrying the ``'absorptive'``
assumption marker are accepted (see :ref:`provsql-provenance-class`):

.. code-block:: postgresql

    WITH RECURSIVE reach(node) AS (
        SELECT 1
      UNION
        SELECT e.dst FROM edges e JOIN reach r ON e.src = r.node
    )
    SELECT node, sr_tropical(provenance(), 'cost_mapping',
                             nonnegative => true) AS min_cost
    FROM reach;

Under ``provsql.provenance = 'absorptive'`` on bounded-treewidth data,
this evaluation, like that of every absorptive semiring on this page,
is linear in the data, including hop-bounded variants, per-group minima
and directed Steiner costs; see :ref:`network-reliability-btw`.

Least Cost with Its Paths
-------------------------

:sqlfunc:`sr_tropical_paths` combines the two previous semirings: the
least cost of a derivation, as :sqlfunc:`sr_tropical`, together with the
derivations of that cost, as a regular expression over the labels, as
:sqlfunc:`sr_regex`. On a path query, these are the shortest paths. It
takes two mappings, one of costs and one of labels (an input missing from
the first costs 0, one missing from the second is the empty word), and
returns a ``cost`` and a ``paths`` column:

.. code-block:: postgresql

    WITH RECURSIVE reach(node) AS (
        SELECT 'a'::text
      UNION
        SELECT e.dst FROM reach r JOIN edges e ON e.src = r.node
    )
    SELECT node, (sr_tropical_paths(provenance(), 'cost_mapping',
                                    'label_mapping')).*
    FROM reach;

On the graph above, with costs 1 for ``ab``, 2 for ``bc``, 0 for ``ca``
and 3 for ``ac``, node ``c`` has cost 3 and paths ``ac | ab · bc``: the
two derivations of that cost. Two alternatives of the same cost keep both
their paths; a cycle of positive cost never lies on a shortest path, one of
cost zero can be followed any number of times and appears under a star, and
one of negative cost, which makes the cost unbounded, is refused. As for
:sqlfunc:`sr_regex`, the multiplication does not commute, there is no monus
and ``HAVING`` is refused.

One Least-Cost Path
-------------------

:sqlfunc:`sr_best_path` takes the same two mappings, and keeps one
derivation of least cost instead of all of them: among those, the one with
the fewest labels, then the first in lexicographic order of its labels. It
returns a ``cost`` and a ``path`` column, the labels of that derivation as
an array, in the order of the query; with no derivation, the cost is
infinite and the path ``NULL``:

.. code-block:: postgresql

    SELECT node, (sr_best_path(provenance(), 'cost_mapping',
                               'label_mapping')).*
    FROM reach;

On the same graph, node ``c`` has cost 3 and path ``{ac}``, shorter than
``ab · bc`` of the same cost. Costs must be nonnegative. The multiplication
does not commute, but the semiring has a monus (a value minus one at least
as good is :math:`\mathbb{0}`, and it is kept otherwise), so that outer
joins and ``EXCEPT`` are evaluated. As a longer path is never better than
its beginning or its end, a cycle never improves a path, and a recursion
may have the recursive relation anywhere in its ``FROM`` clause. ``HAVING``
is refused, and so are
the circuits of the ``'absorptive'`` provenance class (see
:ref:`provsql-provenance-class`), which do not keep the order of the query.

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

:sqlfunc:`union_tstzintervals` is an alias for :sqlfunc:`sr_temporal`.

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

The third argument to both functions gives the carrier enum: only its
type is used, so it is written ``NULL::`` followed by the enum. Given a
``classification_level`` enum ordered from ``unclassified`` to
``not_available``, where ``not_available`` is the top of the enum and
plays the role of the semiring 𝟘 (no derivation possible):

.. code-block:: postgresql

    SELECT create_provenance_mapping('personnel_level', 'personnel', 'classification');

    SELECT city, sr_minmax(provenance(), 'personnel_level',
                           NULL::classification_level) AS clearance
    FROM (SELECT DISTINCT city FROM personnel) t;

These two functions cover any user enum: security lattices,
fuzzy-discrete trust levels, three-valued logic, project-specific
orderings.

Subset, Clearance and Consent Semirings (m-semirings)
------------------------------------------------------

:sqlfunc:`sr_subset` evaluates the provenance over the sets of labels of
a user enum (at most 63 labels): union for ⊕, intersection for ⊗, set
difference for monus. A label stands for a world; the mapping gives the
labels (an enum array, or a single label) whose world contains each
tuple, and the result is the set of labels whose world returns the row.
Unlike :sqlfunc:`sr_minmax`, negation (``EXCEPT``, ``NOT EXISTS``) is
exact.

:sqlfunc:`sr_clearance` reads the labels as clearance levels in
increasing order: a single level in the mapping makes a tuple visible at
that level and every one above (an array is any set of levels, for
compartments). It returns the levels at which a user sees the row, which
need not be upward-closed: a row hidden at ``confidential`` by an open
investigation can reappear at ``secret``, where the resolution is
visible.

.. code-block:: postgresql

    SELECT name,
           sr_clearance(provenance(), 'clearance_map', NULL::level),
           visible_at(provenance(), 'clearance_map', 'confidential'),
           (clearance_settling(provenance(), 'clearance_map', NULL::level)).*
    FROM eligible;

:sqlfunc:`visible_at` tests one level, given as text; for
hierarchical levels, :sqlfunc:`clearance_settling` gives the level from
which visibility no longer changes up to the top, and whether the row is
visible there.

:sqlfunc:`sr_consent` reads the labels as purposes, the mapping giving
the purposes each tuple is consented for. It returns the purposes for
which the query, run over the tuples consented for that purpose, returns
the row, and whether the query over the whole database returns it.
:sqlfunc:`consented_for` keeps a row for a purpose only when both hold,
so that no purpose sees a row that exists only because a negation could
not see a tuple:

.. code-block:: postgresql

    SELECT name FROM customer c
    WHERE NOT EXISTS (SELECT * FROM purchase p WHERE p.cust = c.id)
      AND consented_for(provenance(), 'consent_map', 'marketing');

:sqlfunc:`consent_purposes` lists the purposes a row may be used for, and
:sqlfunc:`consent_conflicts` those whose answer differs from the whole
database.

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

.. note::

    :sqlfunc:`provenance_evaluate` does not support queries with
    ``HAVING`` clauses: they produce an error.
    The built-in compiled semirings (:sqlfunc:`sr_formula`,
    :sqlfunc:`sr_counting`, etc.) support aggregation and ``HAVING``,
    and are significantly faster.
    Prefer compiled semirings when available; use
    :sqlfunc:`provenance_evaluate` for semirings not covered by the
    built-in set.

Provenance Mappings
--------------------

All semiring functions take a mapping name as their second argument.
A mapping is created with :sqlfunc:`create_provenance_mapping`:

.. code-block:: postgresql

    SELECT create_provenance_mapping('mapping_name', 'table_name', 'column_name');

The mapping table has columns ``value`` and ``provenance`` (uuid). You
can also populate it manually for custom scenarios:

.. code-block:: sql

    INSERT INTO mapping_name(provenance, value)
    SELECT provenance(), my_label FROM mytable;

.. _semiring-boolean-compat:

Compatibility with Boolean-Provenance Rewriting
------------------------------------------------

When the provenance class ``provsql.provenance`` is ``'boolean'`` (see
:doc:`probabilities`), hierarchical CQs are rewritten so that their
probability is computable in linear time, and semirings whose algebra
is not Boolean-faithful refuse to evaluate the resulting circuit, with
an explicit error:

.. code-block:: text

    ERROR:  ProvSQL: provenance_evaluate_compiled: The requested
            semiring does not admit a homomorphism from Boolean
            functions; the wrapped sub-circuit was computed under a
            Boolean-provenance assumption (typically by the safe-query
            rewrite, provenance class 'boolean') and the evaluation is
            unsound under this semiring.  Re-run the query under a more
            general provenance class, or pick a Boolean-compatible
            semiring (boolean, boolexpr, formula, ...).

The following compiled semirings are Boolean-faithful and run on
any circuit, including rewritten ones: :sqlfunc:`sr_boolean`,
:sqlfunc:`sr_boolexpr`, :sqlfunc:`sr_formula`,
:sqlfunc:`sr_temporal`, :sqlfunc:`sr_interval_num`,
:sqlfunc:`sr_interval_int`, :sqlfunc:`sr_subset`,
:sqlfunc:`sr_clearance`, :sqlfunc:`sr_consent`.  The following are not, and refuse on
rewritten circuits: :sqlfunc:`sr_counting`, :sqlfunc:`sr_how`,
:sqlfunc:`sr_why`, :sqlfunc:`sr_which`, :sqlfunc:`sr_tropical`,
:sqlfunc:`sr_viterbi`, :sqlfunc:`sr_lukasiewicz`,
:sqlfunc:`sr_minmax`, :sqlfunc:`sr_maxmin`, :sqlfunc:`sr_regex`,
:sqlfunc:`sr_tropical_paths`, :sqlfunc:`sr_best_path`.

To run a refused semiring, switch
the provenance class below ``'boolean'`` (in the current session, or for
the role / database) and re-run the query; its new provenance circuit
accepts any semiring.

ProvSQL Studio's evaluation strip hides the refusing semirings from the
semiring dropdown on such circuits.
