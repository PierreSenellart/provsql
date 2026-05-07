Data-Modification Tracking
==========================

Beyond tracking the provenance of *query results*, ProvSQL can also
track the provenance of the rows themselves -- recording every
``INSERT``, ``UPDATE``, and ``DELETE`` so that one can later answer
"which DML statements is this row's current state due to?", roll
back individual operations with :sqlfunc:`undo`, or query the
database as it looked at a past point in time.  See
:doc:`../user/data-modification` and :doc:`../user/temporal` for
the user-facing description.

This chapter covers how the feature is *implemented*: the GUC, the
trigger machinery, the new ``update`` gate type, the
``update_provenance`` table, and how undo / time-travel are wired
on top of the same provenance circuits used for query results.

Data-modification tracking is **PostgreSQL 14+ only** because the
temporal-validity machinery (see *Time-Travel Queries* below)
relies on the ``tstzmultirange`` type, which was introduced as
part of the multirange family in PostgreSQL 14.  The temporal
validity of a row is computed by evaluating its provenance
circuit over an m-semiring whose carrier is a set of timestamp
intervals (``plus`` = union, ``times`` = intersection, ``monus``
= difference); the result of those operations is in general
*non-contiguous* -- two disjoint unioned intervals, a difference
that punches a hole in the middle of an interval, and so on --
so the carrier cannot be a plain ``tstzrange``.
``tstzmultirange`` is exactly the right type for this semiring,
and without it the whole construction does not close.  All the
SQL pieces live in ``sql/provsql.14.sql`` and are not loaded on
older PostgreSQL versions (see :doc:`build-system`).


The GUC and the Big Picture
---------------------------

The whole subsystem is gated on the ``provsql.update_provenance``
GUC, declared in :cfile:`provsql.c` and registered in
:cfunc:`_PG_init`.  When it is ``off`` (the default), the
``AFTER`` triggers installed by ``add_provenance`` short-circuit
immediately and DML statements behave normally.  When it is
``on``, every committed ``INSERT`` / ``UPDATE`` / ``DELETE`` on a
provenance-tracked table:

1. Allocates a new ``update`` gate representing the operation
   itself, with a fresh UUID.
2. Records the operation in the ``update_provenance`` housekeeping
   table (statement text, user, timestamp, validity range).
3. Combines that gate with the ``provsql`` token of every affected
   row using either ``provenance_times`` (for inserts) or
   ``provenance_monus`` (for deletes).  An update is modelled as a
   delete followed by an insert.

The end result is that each row's ``provsql`` token is a circuit
whose leaves include not only the original ``input`` gates from
``add_provenance`` but also one ``update`` leaf per DML statement
that ever touched the row.


The ``gate_update`` Gate Type
-----------------------------

A new value, ``gate_update``, is added to the
:cfunc:`gate_type` enum in :cfile:`provsql_utils.h`.  It is treated
exactly like ``gate_input`` everywhere it matters:

- :cfunc:`MMappedCircuit` and :cfunc:`GenericCircuit` allow
  probabilities to be attached to it (via :sqlfunc:`set_prob`),
  because semantically a DML operation is "another input" to the
  circuit.
- :cfunc:`GenericCircuit::evaluate` registers ``update`` gates as
  members of its ``inputs`` set, so semiring evaluators see them
  as leaves the same way they see ``input`` gates.
- :cfunc:`getBooleanCircuit` translates ``gate_update`` to a
  Boolean variable like ``gate_input``, so probability evaluation
  and Shapley computation handle them transparently.

The only thing that distinguishes an ``update`` gate from an
``input`` gate is its semantic role -- "this leaf was created by
DML, not by the original ``add_provenance``" -- and the fact that
the SQL housekeeping records (``update_provenance``) tie its UUID
back to a concrete statement.


The Triggers
------------

``add_provenance`` (in ``sql/provsql.14.sql``) installs three
``AFTER`` statement-level triggers on the target table:

.. code-block:: sql

   AFTER INSERT REFERENCING NEW TABLE AS NEW_TABLE
     EXECUTE PROCEDURE provsql.insert_statement_trigger();

   AFTER UPDATE REFERENCING OLD TABLE AS OLD_TABLE
                NEW TABLE AS NEW_TABLE
     EXECUTE PROCEDURE provsql.update_statement_trigger();

   AFTER DELETE REFERENCING OLD TABLE AS OLD_TABLE
     EXECUTE PROCEDURE provsql.delete_statement_trigger();

Each trigger:

1. Returns immediately if ``provsql.update_provenance`` is off.
2. Allocates a fresh UUID and creates a corresponding
   ``update`` gate via :sqlfunc:`create_gate`.
3. Reads the current statement text from ``pg_stat_activity`` and
   inserts a row into ``update_provenance``.
4. Sets ``provsql.update_provenance = off`` on a *local* basis to
   suppress the recursive triggering caused by the next steps.
5. Walks the affected rows (``OLD_TABLE`` / ``NEW_TABLE``) and
   rewrites each row's ``provsql`` token: ``insert`` multiplies
   the row token by the new operation gate, ``delete`` applies
   ``provenance_monus`` to remove it, and ``update`` does both
   (a monus on the old row plus a times on the new one).
6. Re-enables tracking before returning.

The temporary disabling in step 4 is essential: rewriting the
``provsql`` column of a tracked table is itself an ``UPDATE``,
which would re-trigger the same machinery and recurse forever
otherwise.

Because deletes need to keep the old row around for time-travel
queries (otherwise there would be nothing left to attach the
``monus`` token to), the delete trigger does **not** physically
delete the row -- it re-inserts a copy under a fresh ``provsql``
token whose circuit ends in a ``provenance_monus`` against the
delete gate.


The ``update_provenance`` Table
-------------------------------

``update_provenance`` is a regular PostgreSQL table created by
``sql/provsql.14.sql``.  Its schema:

.. code-block:: sql

   CREATE TABLE provsql.update_provenance (
     provsql    uuid,                            -- the update gate's UUID
     query      text,                            -- the SQL text
     query_type query_type_enum,                 -- INSERT/UPDATE/DELETE/UNDO
     username   text,                            -- session_user
     ts         timestamp DEFAULT CURRENT_TIMESTAMP,
     valid_time tstzmultirange DEFAULT
       tstzmultirange(tstzrange(CURRENT_TIMESTAMP, NULL))
   );

The ``provsql`` column is the *primary key* in spirit: it links
each row to the corresponding ``update`` gate in the circuit
store.  Once you have that token you can hand it back to
:sqlfunc:`undo` to roll the operation back, or query it via the
temporal functions to find out which rows were affected.

The ``valid_time`` column is a ``tstzmultirange``.  At the moment
an ``update`` gate is created, it is set to the half-infinite
interval :math:`[\mathit{ts}, +\infty)`: from the operation's
timestamp onward, and not before.  Crucially, ``valid_time`` is
never modified after this initial insertion -- not even by
:sqlfunc:`undo`.  Trimming the validity of a superseded
operation is *not* a property of the row in this table; it
emerges from evaluating the row's provenance circuit over the
union-of-intervals m-semiring described in *Time-Travel Queries*
below.


Undo
----

:sqlfunc:`undo` takes a token (typically read out of
``update_provenance.provsql``) and rolls the corresponding
operation back, *without* removing anything from the history.
The implementation is pure PL/pgSQL, in ``sql/provsql.14.sql``.

The trick is that the row tokens already contain the operation's
gate as a leaf; we just need to *cancel* that leaf inside every
circuit that references it.  ``undo``:

1. Allocates a new ``update`` gate ``u`` and records a
   brand-new ``UNDO`` row in ``update_provenance`` whose
   ``valid_time`` is again :math:`[\mathit{now}, +\infty)`.
2. Walks every provenance-tracked table in every schema, and for
   each row in each table calls
   ``replace_the_circuit(row_token, op_token, u)`` to rewrite
   the row's circuit.

:sqlfunc:`replace_the_circuit` (also in ``sql/provsql.14.sql``) is a
recursive PL/pgSQL function that walks a circuit and rebuilds it
with one substitution: every reference to ``op_token`` is
replaced by ``provenance_monus(op_token, u)``.  Because the
circuit is a DAG, the rewrite is structural -- it preserves
sharing within a single rewrite call.

The undo is *itself* an operation: the new ``update`` gate ``u``
becomes a leaf of every rewritten row, and undoing the undo
(yes, you can do that) follows exactly the same path.


Time-Travel Queries
-------------------

All temporal queries reduce to evaluating a row's provenance
circuit over the **union-of-intervals m-semiring**, whose
carrier is ``tstzmultirange``:

- plus (⊕) is the union of intervals, with neutral element
  :math:`\emptyset`;
- times (⊗) is the pointwise intersection, with neutral element
  the universal singleton :math:`\{(-\infty, +\infty)\}`;
- monus (⊖) is the pointwise difference (A minus B is the union,
  over :math:`a \in A` and :math:`b \in B`, of
  :math:`a \cap \overline{b}`).

This is exactly the m-semiring the PW'25 paper
(:cite:`DBLP:conf/pw/WidiaatmajaDDS25`) describes.  It is
implemented as the :cfunc:`Temporal` compiled semiring in
:cfile:`Temporal.h`, dispatched via :sqlfunc:`sr_temporal`,
which delegates the ⊕, ⊗, and ⊖ operations to PostgreSQL's
``multirange_union``, ``multirange_intersect``, and
``multirange_minus`` built-ins.

The interpretation of a gate in this semiring is simple:

- an ``input`` gate -- a plain base-table leaf -- has no
  associated time and is mapped to the ⊗-neutral
  :math:`\{(-\infty, +\infty)\}`, meaning "valid at any time";
- an ``update`` gate is mapped to its ``valid_time`` from
  ``update_provenance``, i.e.
  :math:`[\mathit{ts}, +\infty)`.

The mapping is provided by the auto-generated provenance mapping
view ``time_validity_view`` over the ``update_provenance.valid_time``
column; :sqlfunc:`get_valid_time` calls
``sr_temporal(provenance(), 'time_validity_view')`` on
the target row and returns the resulting multirange.

The reason the temporal result "just works" after an undo is
that the semiring's ⊖ does the trimming arithmetically: after
:sqlfunc:`replace_the_circuit` has rewritten ``c`` to ``c ⊖ u``, the
union-of-intervals evaluation of that subtree produces
:math:`[\mathit{ts}_c, +\infty) \ominus [\mathit{ts}_u, +\infty) = [\mathit{ts}_c, \mathit{ts}_u)`
-- exactly the finite interval during which the undone
operation was in effect.  No record in ``update_provenance`` is
ever modified.

The remaining user-facing temporal functions are thin wrappers
on top of :sqlfunc:`get_valid_time` and ordinary
``tstzmultirange`` operators:

- :sqlfunc:`timetravel` -- rows of a table valid at a given
  timestamp (tests ``@>`` containment);
- :sqlfunc:`timeslice` -- rows valid anywhere in a given
  interval (tests ``&&`` overlap);
- :sqlfunc:`history` -- full history of rows matching a key.

All four functions live entirely in PL/pgSQL.  They contain no
C code: every operation reduces to a query against the
target table plus a call to :sqlfunc:`provenance_evaluate` with
the union-of-intervals semiring.
