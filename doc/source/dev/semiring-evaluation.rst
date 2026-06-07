Semiring Evaluation
===================

ProvSQL evaluates provenance circuits over arbitrary (m-)semirings.
This page explains the semiring interface, walks through two
existing implementations, gives a step-by-step guide for adding a
new compiled semiring, and discusses the *symbolic representation*
semirings used by tests and tutorials.

.. note::

   This page is about **compiled semirings** -- semirings
   implemented in |cpp| and invoked through
   :cfunc:`provenance_evaluate_compiled` (via SQL wrappers such as
   :sqlfunc:`sr_boolean` or :sqlfunc:`sr_counting`).  Compiled
   semirings are the preferred option when performance matters.

   ProvSQL also exposes an SQL-level mechanism,
   :sqlfunc:`provenance_evaluate`, that lets users assemble a
   semiring directly in SQL by supplying ``plus``, ``times``,
   ``monus`` and ``delta`` functions along with a zero and a one
   element.  That path requires no |cpp| code or recompilation, but
   is much slower and limited to what the SQL type system can
   express.  It is described in :doc:`../user/semirings` and is
   **not** what this page covers.


The ``Semiring<V>`` Interface
-----------------------------

All semirings inherit from the abstract template class
``semiring::Semiring<V>`` defined in :cfile:`Semiring.h`.
The template parameter ``V`` is the *carrier type* (e.g., ``bool``,
``unsigned``, ``std::string``).

Required Methods (Pure Virtual)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Every semiring must override the following pure-virtual methods:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Method
     - Semantics
   * - ``zero()``
     - Additive identity :math:`\mathbb{0}`.
   * - ``one()``
     - Multiplicative identity :math:`\mathbb{1}`.
   * - ``plus(vector<V>)``
     - Additive operation :math:`v_0 \oplus v_1 \oplus \cdots`.
       An empty vector should return ``zero()``.
   * - ``times(vector<V>)``
     - Multiplicative operation :math:`v_0 \otimes v_1 \otimes \cdots`.
       An empty vector should return ``one()``.
   * - ``monus(x, y)``
     - M-semiring difference :math:`x \ominus y`.
   * - ``delta(x)``
     - Delta operator :math:`\delta(x)`.

``zero``, ``one``, ``plus``, and ``times`` are load-bearing: every
evaluator traverses the circuit applying them.  ``monus`` and
``delta`` are only exercised by specific SQL features --
``EXCEPT`` / ``EXCEPT ALL`` for ``monus``, and ``GROUP BY`` with
aggregates (without ``HAVING``) for ``delta``.  A semiring that
does not sensibly implement one of them can throw
:cfunc:`SemiringException` from its override; evaluation will then
fail *only* for queries that actually use the corresponding gate,
and all other queries remain evaluable.

.. note::

   The algebraic axioms a new semiring should satisfy are
   machine-checked in the ProvSQL Lean 4 library.  The
   `Provenance.SemiringWithMonus
   <https://provsql.org/lean-docs/Provenance/SemiringWithMonus.html>`_
   module defines the ``SemiringWithMonus`` typeclass and proves
   the key monus identities -- ``monus_smallest``
   (:math:`a \ominus b` is the least :math:`c` such that
   :math:`a \le b + c`), ``monus_self``, ``zero_monus``,
   ``monus_add``, ``add_monus`` -- plus the characterisation of
   idempotent m-semirings (``idempotent_iff_add_monus``,
   ``plus_is_join``).  The typeclass also carries the
   :math:`\delta` operator as a field, with three axioms --
   `delta_zero
   <https://provsql.org/lean-docs/Provenance/SemiringWithMonus.html#SemiringWithMonus.delta_zero>`_
   (:math:`\delta(0) = 0`),
   `delta_natCast_pos
   <https://provsql.org/lean-docs/Provenance/SemiringWithMonus.html#SemiringWithMonus.delta_natCast_pos>`_
   (:math:`\delta(n) = 1` for positive :math:`n`), and
   `delta_regrouping
   <https://provsql.org/lean-docs/Provenance/SemiringWithMonus.html#SemiringWithMonus.delta_regrouping>`_
   (:math:`\delta\bigl(\sum_i \delta(a_i)\bigr) = \delta\bigl(\sum_i a_i\bigr)`
   for any multiset of annotations).  The third axiom makes
   grouped aggregation invariant under partition coarsening
   and strengthens idempotence; the derived
   `delta_one
   <https://provsql.org/lean-docs/Provenance/SemiringWithMonus.html#delta_one>`_
   (:math:`\delta(1) = 1`) and
   `delta_idempotent
   <https://provsql.org/lean-docs/Provenance/SemiringWithMonus.html#delta_idempotent>`_
   (:math:`\delta(\delta(a)) = \delta(a)`) are exposed at
   the top level.  A commutative variant
   `CommSemiringWithMonus
   <https://provsql.org/lean-docs/Provenance/SemiringWithMonus.html#CommSemiringWithMonus>`_
   adds ``mul_comm`` and is the class actually used by the
   aggregation proofs.  A contributor adding a new compiled semiring can use
   that module as a formal reference for what the ``zero`` /
   ``one`` / ``plus`` / ``times`` / ``monus`` / ``delta``
   methods are *required* to compute -- the Lean class is the
   ground truth, and the |cpp| overrides should be a faithful
   implementation of it.  The
   `Provenance.Semirings.* <https://provsql.org/lean-docs/Provenance.html>`_
   namespace already contains verified instances for
   ``Bool``, ``Nat`` (counting), ``BoolFunc`` (Boolean
   formulas), ``How`` (the how-provenance universal semiring),
   ``Why``, ``Which`` (lineage), ``MinMax`` (and ``MaxMin`` via
   ``OrderDual``), ``Tropical``, ``Lukasiewicz``, ``Viterbi``,
   and ``IntervalUnion`` (finite unions of intervals over a
   dense linear order, used for temporal databases), each with
   a proof of the m-semiring axioms, a concrete :math:`\delta`,
   and any extra properties (absorptivity, idempotence,
   left-distributivity of multiplication over monus) that
   matter for optimisation.

.. _semiring-optional-methods:

Optional Methods
^^^^^^^^^^^^^^^^

The following methods have default implementations that throw
:cfunc:`SemiringException`:

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Method
     - Gate type
   * - ``cmp(s1, op, s2)``
     - ``gate_cmp`` -- comparison of aggregate values.
   * - ``semimod(x, s)``
     - ``gate_semimod`` -- semimodule scalar multiplication (aggregation).
   * - ``agg(op, values)``
     - ``gate_agg`` -- aggregation operator.
   * - ``value(string)``
     - ``gate_value`` -- interpret a literal string as a semiring value.

Overriding these methods is only useful for *pseudo-semirings* that
produce a symbolic representation of the provenance (e.g. the
formula semiring renders a ``cmp`` gate as a literal string like
``"x > 5"``).  A proper semiring does not evaluate ``cmp`` gates
through these overrides: instead, before the main circuit traversal
starts, :cfile:`having_semantics.hpp` walks the circuit, finds every
``cmp`` gate that compares an aggregate against a constant, and
computes its semiring value using the ordinary ``plus`` / ``times``
/ ``monus`` operations of the semiring.  Each result is injected
into the provenance mapping keyed by the ``cmp`` gate itself, so
the main traversal reaches those gates with a pre-resolved value
and treats them like ordinary leaves.  The semiring's ``cmp`` /
``agg`` / ... overrides are therefore never reached on real
queries.

Absorptive Semirings
^^^^^^^^^^^^^^^^^^^^

Override ``absorptive()`` to return ``true`` if
:math:`\mathbb{1} \oplus a = \mathbb{1}` for all :math:`a` (e.g., the
Boolean semiring).  Absorptivity implies idempotency
(:math:`a \oplus a = a`); the evaluator exploits this flag to
deduplicate operands of ``plus`` gates and to short-circuit over the
multiplicative identity, which can significantly improve performance.
Idempotent-but-not-absorptive semirings (such as why-provenance and
which-provenance, where :math:`\mathbb{1} \oplus a \neq \mathbb{1}` in
general) should leave it at the default ``false``.

Boolean-Rewrite Compatibility
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The safe-query rewriter (see :doc:`query-rewriting` and
:doc:`../user/probabilities`) emits circuits whose root is wrapped
in a ``gate_assumed_boolean`` marker, signalling that the rewrite
preserves only Boolean semantics, not arbitrary semiring
semantics.  Each semiring declares its compatibility with this
rewrite by overriding a virtual predicate inherited from
:cfunc:`Semiring`:

.. code-block:: cpp

   virtual bool compatibleWithBooleanRewrite() const override { return true; }

The predicate is read by
:cfunc:`provenance_evaluate_compiled_internal`
(``src/provenance_evaluate_compiled.cpp``) immediately before the
``evaluate<S>`` template call.  When the root carries the
``gate_assumed_boolean`` marker and the chosen semiring's
predicate returns ``false``, the dispatcher raises a structured
error rather than silently producing an incorrect value.

Per-semiring decisions (see each override under
``src/semiring/*.h``) :

- ``true`` : ``Boolean``, ``BoolExpr``, ``Formula`` (symbolic
  serialisation, semantically opaque), ``IntervalUnion`` (covers
  ``sr_temporal``, ``sr_interval_num``, ``sr_interval_int``).
- ``false`` : every other compiled semiring (``Counting``,
  ``How``, ``Why``, ``Which``, ``Tropical``, ``Viterbi``,
  ``Lukasiewicz``, ``MinMax``) ; their algebras carry information
  (multiplicities, monomials, witness sets, lineages, costs,
  weights, graded truth values, extremal elements) that the
  Boolean rewrite would silently collapse.

ProvSQL Studio mirrors the C++ predicate via the
``boolean_rewrite_compatible`` flag of
``_COMPILED_SEMIRINGS`` in ``studio/provsql_studio/db.py``, and
filters its evaluation-strip dropdown to hide the incompatible
semirings whenever the root carries the marker.

The marker itself is a structural wrapper rather than a per-gate
flag, so it round-trips through circuit serialisation and is
visible to every downstream consumer (semiring evaluators,
PROV-XML export, ``view_circuit``, ProvSQL Studio's circuit
view).  The wrapping happens at the end of the safe-query
rewriter via :sqlfunc:`assume_boolean`, which creates a
single-input ``gate_assumed_boolean`` wrapping the original root.
The evaluator template treats the wrapper transparently for
compatible semirings ; only the dispatcher's pre-evaluation check
distinguishes them.

Boolean-Identity Folding
^^^^^^^^^^^^^^^^^^^^^^^^

In addition to the universal
``GenericCircuit::foldSemiringIdentities`` pass (which
collapses identities and absorbers shared by every semiring),
ProvSQL runs an opt-in
``GenericCircuit::foldBooleanIdentities`` pass at circuit-load
time when the provenance class is ``'boolean'``.  It applies three
Boolean-specific rewrite rules and wraps each rewritten gate in a
``gate_assumed_boolean`` marker (same mechanism as the safe-query
rewriter, so the resulting circuit refuses non-Boolean-compatible
semirings) :

- **B1, idempotence** : ``plus(x, x, ...) -> x`` after deduplication.
- **B2, plus-with-one absorber** : ``plus(one, ...) -> one``.
- **B3, absorption** : ``plus(u, times(u, v)) -> u`` and
  ``times(u, plus(u, v)) -> u``.

The pass is gated by the ``'boolean'`` provenance class because the
rewrites are sound only when evaluated under a Boolean-faithful
semiring ; the gate-level marker enforces this at evaluation time.

B3 only fires when the dominating literal ``u`` is a *direct* sibling
of the parent, and that literal is frequently exposed only **after** a
single-wire collapse (a diagonal ``times(x, x)`` deduped by B1 to a
single-wire ``times`` that ``foldSemiringIdentities`` then rewrites to
``x``).  ``foldBooleanIdentities`` therefore runs the Boolean rule
sweep and ``foldSemiringIdentities`` to a **joint fixpoint** --
alternating the two until neither changes the circuit -- rather than
iterating B1-B3 alone and collapsing only once at the end, which would
leave such absorptions undone.  Every rule strictly removes a wire or
gate (and none adds one), so the alternation terminates ; collapses
propagate one DAG level per iteration, giving an ``O(depth x circuit)``
bound, and the result is a circuit on which no rule applies.

The B3 absorption rule in particular has a large practical
effect on UNION-dominated-pair shapes (see
``test/bench/absorption_bench.sql``) where it collapses
exponential-sized DNFs to single-OR forms ; with the joint fixpoint
this also covers cross-product / high-degree self-join lineages
(e.g. ``SELECT DISTINCT 1 FROM e a, e b``), whose ``Theta(|I|)``-treewidth
sum-of-products collapses to a single OR of treewidth 1.


Example: The Boolean Semiring
-----------------------------

:cfunc:`Boolean` (in :cfile:`Boolean.h`) is the simplest semiring
and a good template.
It evaluates provenance to ``true``/``false``: a tuple is in the result
iff at least one derivation exists.

.. code-block:: cpp

   class Boolean : public semiring::Semiring<bool> {
   public:
     value_type zero()  const override { return false; }
     value_type one()   const override { return true; }

     value_type plus(const std::vector<value_type> &v) const override {
       return std::any_of(v.begin(), v.end(), [](bool x) { return x; });
     }
     value_type times(const std::vector<value_type> &v) const override {
       return std::all_of(v.begin(), v.end(), [](bool x) { return x; });
     }
     value_type monus(value_type x, value_type y) const override {
       return x & !y;
     }
     value_type delta(value_type x) const override { return x; }

     bool absorptive() const override { return true; }
   };

Key observations:

- ``plus`` = OR (any derivation suffices).
- ``times`` = AND (all inputs must hold).
- ``monus`` = ``x AND NOT y``.
- ``delta`` = identity (Boolean is already normalized).
- The semiring is absorptive: ``true OR true = true``.


Example: The Counting Semiring
------------------------------

:cfunc:`Counting` (in :cfile:`Counting.h`) counts the number of
distinct derivations
of each tuple.  Its carrier type is ``unsigned``.

.. code-block:: cpp

   class Counting : public semiring::Semiring<unsigned> {
   public:
     value_type zero()  const override { return 0; }
     value_type one()   const override { return 1; }

     value_type plus(const std::vector<value_type> &v) const override {
       return std::accumulate(v.begin(), v.end(), 0);
     }
     value_type times(const std::vector<value_type> &v) const override {
       return std::accumulate(v.begin(), v.end(), 1, std::multiplies<value_type>());
     }
     value_type monus(value_type x, value_type y) const override {
       return x <= y ? 0 : x - y;
     }
     value_type delta(value_type x) const override {
       return x != 0 ? 1 : 0;
     }
   };

This semiring is **not** absorptive (``1 + 1 = 2 ≠ 1``).


Step-by-Step: Adding a New Semiring
-----------------------------------

1. **Create the header file** in ``src/semiring/``.  Name it after the
   semiring (e.g., ``MySemiring.h``).  Implement the class inheriting from
   ``semiring::Semiring<V>``, inside the ``semiring`` namespace.

2. **Register with the compiled evaluator**.  In
   :cfile:`provenance_evaluate_compiled.cpp`, the function
   :cfunc:`provenance_evaluate_compiled_internal` dispatches on a
   semiring name string and a return-type OID.  Add a branch for
   your semiring:

   .. code-block:: cpp

      // In the appropriate type branch (VARCHAR, INT, BOOL, etc.):
      if (semiring == "mysemiring")
        return pec_...(constants, c, g, inputs, "mysemiring", drop_table);

   The ``pec_*`` helper functions instantiate the |cpp| semiring class,
   call ``GenericCircuit::evaluate<MySemiring>(...)``, and convert the
   result to a PostgreSQL ``Datum``.

3. **Create the SQL wrapper function** in ``sql/provsql.common.sql``.
   Follow the pattern of existing compiled semirings:

   .. code-block:: plpgsql

      CREATE OR REPLACE FUNCTION sr_mysemiring(token UUID, token2anot REGCLASS)
        RETURNS <return_type> AS
      $$
        SELECT provsql.provenance_evaluate_compiled(token, token2anot, 'mysemiring', NULL::<return_type>);
      $$ LANGUAGE SQL;

4. **Add a regression test** in ``test/sql/`` with expected output in
   ``test/expected/``.  Follow the pattern of ``test/sql/sr_boolean.sql``.

5. **Document** the new semiring in the user guide
   (``doc/source/user/semirings.rst``).


Evaluation Dispatch
-------------------

When a user calls :sqlfunc:`sr_boolean` (or any compiled semiring
function), the call chain is:

1. SQL function ``sr_boolean(token, mapping_table)`` calls
   ``provenance_evaluate_compiled(token, table, 'boolean', NULL::BOOLEAN)``.

2. The |cpp| function :cfunc:`provenance_evaluate_compiled` extracts the
   semiring name string and return-type OID, then calls
   :cfunc:`provenance_evaluate_compiled_internal`.

3. The internal function reads the circuit from mmap (via
   :cfunc:`getGenericCircuit`), creates a mapping from input gates to their
   annotations, and dispatches to the appropriate ``pec_*`` helper
   based on return type and semiring name.

4. The ``pec_*`` helper instantiates the semiring class and calls
   ``GenericCircuit::evaluate<S>(root, mapping)``, which performs a
   post-order DAG traversal applying semiring operations at each gate.


Symbolic Representation Semirings
---------------------------------

Some semirings -- in particular the *formula* semiring exposed by
:sqlfunc:`sr_formula` -- do not compute a numeric or boolean value
but rather render the provenance as a *symbolic expression*.  The
formula semiring's carrier is ``std::string`` and its operations
are concatenations: ``plus`` joins the children with ⊕, ``times``
joins them with ⊗, and so on.  The result is a textual rendering
of the provenance circuit suitable for inclusion in tests, slides,
or tutorials.

Two practical caveats follow from this:

**Operand ordering is not deterministic.**  When ``plus`` (or any
other commutative gate) walks its children, the order in which
they are visited depends on internal traversal order, which can
change between runs.  Tests that print symbolic representations
must therefore *normalise* the operand order before comparing --
typically with a small ``replace`` / ``regexp_replace`` that
rewrites any of the legal orderings to a canonical one.  See
``test/sql/union.sql`` and ``test/sql/distinct.sql`` for examples.

**Symbolic semirings are not "real" semirings.**  They render the
*structure* of the circuit but do not satisfy the semiring axioms
in the usual sense (e.g. concatenation of strings is associative
but not commutative, idempotent only in trivial cases, etc.).
They are meant for human inspection and testing, not for any
algorithmic use that depends on the algebraic properties of the
result.  In particular, optional operations like ``cmp`` and
``agg`` *can* be sensibly implemented on a symbolic semiring -- by
rendering ``cmp(s1, op, s2)`` as ``"s1 op s2"`` -- which is what
the formula semiring does, and is the main reason these optional
operations exist at all in the :cfunc:`Semiring` interface.  See
:ref:`semiring-optional-methods` above for the full story of why
real semirings do not need to implement them.
