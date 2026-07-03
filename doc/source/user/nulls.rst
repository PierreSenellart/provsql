NULL Values
============

SQL's NULL comes with famously subtle semantics: predicates over NULLs
follow a three-valued logic, set operations switch to a different notion
of equality, aggregates skip NULL inputs, and queries that look
equivalent stop being equivalent the moment a NULL appears. ProvSQL
tracks provenance *through* these rules: the rewritten query returns
what vanilla PostgreSQL returns, and the provenance and probabilities it
computes are correct with respect to SQL's own NULL semantics. This
chapter states the rules ProvSQL follows and the few places where its
behavior is a deliberate, documented choice.

What NULL Means Here
---------------------

ProvSQL implements the *SQL-operational* reading of NULL: a NULL is a
value like any other, predicates over it evaluate to *unknown* per SQL's
three-valued logic (3VL), and a row whose condition is unknown is not an
answer. ProvSQL does **not** interpret NULL as an unknown quantity
ranging over possible values (the "incomplete information" reading);
NULL stays NULL in every possible world.

Two rules pin the semantics down:

* **Selection rule.** Every condition is evaluated under SQL's 3VL on
  the actual data; a tuple whose condition evaluates to *unknown* or
  *false* is annotated with the semiring **zero**, exactly like an
  absent tuple. Unknown never becomes "maybe" in the annotation, and
  never silently becomes "true".
* **Possible-worlds criterion.** For Boolean provenance (and everything
  built on it: probabilities, Shapley values, knowledge compilation), a
  tuple's annotation must satisfy: the tuple is in the query answer over
  a world *W* -- evaluated under SQL's own semantics, NULLs included --
  exactly when *W* satisfies the tuple's Boolean provenance.

Behavior by Construct
----------------------

**WHERE, JOIN, and HAVING predicates.** PostgreSQL itself evaluates
deterministic predicates, 3VL included; ProvSQL only combines the tokens
of rows that survive. ``WHERE b > 5`` drops rows with a NULL ``b``, a
NULL join key matches nothing, and their tokens simply never enter the
circuit.

**IN and EXISTS.** ``x IN (SELECT …)`` with no true match makes the row
a non-answer whether the comparison is false or unknown -- that is SQL's
own top-level rule, and the annotation is zero either way.
``EXISTS`` / ``NOT EXISTS`` only ever ask whether a *true* match exists,
so they compose correctly with NULLs as-is.

**NOT IN and quantified comparisons (``op ALL``).** Negation is where
NULLs bite: ``x NOT IN Q`` is *unknown* -- hence not an answer -- as
soon as ``Q`` contains a NULL (or ``x`` is NULL and ``Q`` is non-empty).
ProvSQL's rewriting accounts for this: the removal condition it builds
treats an unknown comparison like a match, so a subquery row with a NULL
removes the outer row in every world containing it. Consequently,
``NOT IN``, ``NOT EXISTS``, and ``EXCEPT`` -- equivalent on NULL-free
data -- get genuinely different provenance on data with NULLs, matching
their different SQL answers. (See Step 12 of
:doc:`case study 5 <casestudy5>` for the three side by side.)

**Set operations, DISTINCT, GROUP BY.** SQL deliberately switches to
*syntactic* equality here: two NULLs are the same value.
``DISTINCT`` and ``GROUP BY`` collapse NULL keys into one group whose
annotation is the ⊕ of all contributing tokens; ``UNION`` merges NULL
rows from both sides; ``EXCEPT`` matches NULL rows on the two sides
against each other (its antijoin uses ``IS NOT DISTINCT FROM``
matching). ``INTERSECT`` is not supported and fails with an explicit
error.

**Aggregates.** ``sum`` / ``avg`` / ``min`` / ``max`` skip NULL inputs;
``count(col)`` counts non-NULL values while ``count(*)`` counts rows;
an all-NULL (or empty) group yields a NULL aggregate whose formula
readout is NULL as well. The same holds for aggregates over
``random_variable`` columns: a NULL cell contributes to neither the sum
nor ``avg``'s count.

**HAVING over a NULL aggregate.** A comparison against a NULL aggregate
never passes -- neither on the actual instance (the group's annotation
is zero) nor in any possible world where the aggregate comes out NULL
(a world with no contributing row). ``HAVING <agg> IS [NOT] NULL`` is
fully supported across possible worlds: ``sum(b) IS NULL`` holds in
exactly the worlds where the group exists but no non-NULL-valued row is
present.

**Outer joins.** The supported shape -- a two-relation
LEFT/RIGHT/FULL JOIN between tracked arms, with no outer reference to
the join -- is lowered into its matched and NULL-padded arms with
correct (monus) provenance for the padding. An outer join whose
null-padded side is entirely *untracked* is also fine as-is: which rows
are padded is then deterministic. Any other outer join with a tracked
relation on a null-padded side is refused with an explicit error rather
than silently mis-tracked.

**Comparisons on NULL random variables.** A comparison involving a NULL
``random_variable`` -- a NULL constant or a NULL cell -- is unknown in
every world: the row is annotated zero (probability 0). ``IS NULL`` on
a ``random_variable`` column is an ordinary deterministic predicate.
(See Step 19 of :doc:`case study 6 <casestudy6>`.)

Zero-Annotated Rows May Stay Visible
-------------------------------------

A row whose annotation is the semiring zero is equivalent to an absent
row, and ProvSQL does not always spend the effort of filtering such rows
out: rewritten queries may return rows that vanilla SQL does not, whose
annotation evaluates to zero. Typical examples are the antijoin arm of
a difference (a row removed *on this instance* but present in worlds
where its remover is absent -- exactly what makes its probability
meaningful) and ``HAVING`` groups that fail the predicate. Deciding
zero-ness in general requires evaluating the provenance and is
semiring-relative, so visible-but-zero is the deliberate default.

When the vanilla result set is wanted, filter explicitly with
:sqlfunc:`present`:

.. code-block:: postgresql

    SELECT * FROM (...) t WHERE provsql.present(provenance());

``present(token)`` is true exactly when the row is in the answer the
query has without provenance tracking (the circuit evaluated in the
Boolean semiring with every leaf true). Its general form,
:sqlfunc:`nonzero`, tests the annotation against a
semiring's zero: ``'boolean'`` as above, ``'counting'`` for bag
multiplicity, and -- with ``semiring`` omitted -- a *universal* zero
test that is false only when the annotation is provably zero in every
(m-)semiring under every leaf valuation, so filtering on it can never
contradict any downstream evaluation. Both keep rows they cannot prove
zero (and untracked rows, whose NULL token reads as the neutral 1);
the cost is one circuit evaluation per candidate row.

NULL Provenance Tokens
-----------------------

Data NULLs are not the only NULLs around: a *provenance token* slot can
itself be NULL, and each combinator maps a NULL token to its own neutral
element:

.. list-table::
   :header-rows: 1
   :widths: 30 20 50

   * - Combinator
     - NULL token reads as
     - Situation that produces it
   * - ``provenance_times`` (⊗)
     - 1 (neutral)
     - untracked source in a join
   * - ``provenance_plus`` (⊕)
     - 0 (neutral)
     - padded/absent row's slot in a disjunction
   * - ``provenance_monus`` (⊖, right side)
     - 0 (neutral)
     - antijoin with no matching right rows
   * - ``provenance_delta`` (δ)
     - 1
     - untracked source

The load-bearing rule: **a NULL token never means "false"**. Whenever a
condition is unknown or fails, ProvSQL's rewriting produces an explicit
zero gate (for instance, ``provenance_cmp`` returns ``gate_zero``
when an operand is NULL) rather than letting a NULL token reach ⊗, where
it would read as *certainly true*.

Unset Probabilities Are Certain
--------------------------------

An input token that was never given a probability with
:sqlfunc:`set_prob` is treated as **certain** (probability 1.0) by
:sqlfunc:`probability_evaluate` and everything downstream. This is the
intended convention -- in typical deployments most tuples are
deterministic and only a few tables are probabilistic -- but note the
API asymmetry: :sqlfunc:`get_prob` returns NULL for such a token
("never set"), while evaluation uses 1.0.

``EXCEPT ALL``: the Documented Bag Difference
----------------------------------------------

Plain ``EXCEPT`` (set semantics) is a full SQL-conformance target. For
``EXCEPT ALL``, ProvSQL deliberately implements a *different multiset
difference* than SQL: a left row is removed entirely whenever a
syntactically equal right row exists, rather than cancelling copies
one-for-one (SQL's ``max(0, m − n)``). The reason is provenance
tractability: SQL's semantics assigns no canonical provenance to an
individual surviving copy (which left copy is cancelled by which right
copy is an arbitrary pairing), whereas the chosen semantics is definable
row by row. The two coincide after duplicate elimination. Matching is
syntactic in both readings: a NULL row on the right removes NULL rows on
the left.

Empty Groups in Evaluated Worlds
---------------------------------

The value-aware evaluators (Monte Carlo sampling, ``HAVING`` world
enumeration, the moment readouts) must give each aggregate a value in
worlds where its group has no contributing row. The conventions:

* A ``HAVING`` comparison never passes in a world where the aggregate is
  NULL (no contributing row for ``sum``/``avg``/``min``/``max``).
* The moment readouts (:sqlfunc:`expected` and friends) over
  aggregate-valued arguments are **conditional on the aggregate being
  defined**, returning NULL only when the aggregate is defined with
  probability 0. ``sum`` and ``count`` are defined in every world (an
  empty group's sum is the real value 0 there); ``min``/``max``/``avg``
  are defined exactly when some contributing row is present.
