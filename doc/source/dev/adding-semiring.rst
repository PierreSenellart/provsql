Adding a New Semiring
=====================

ProvSQL evaluates provenance circuits over arbitrary (m-)semirings.
This page explains the semiring interface, walks through two existing
implementations, and gives a step-by-step guide for adding a new one.


The ``Semiring<V>`` Interface
-----------------------------

All semirings inherit from the abstract template class
``semiring::Semiring<V>`` defined in :cfile:`Semiring.h`.
The template parameter ``V`` is the *carrier type* (e.g., ``bool``,
``unsigned``, ``std::string``).

Required methods (pure virtual)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Every semiring **must** implement:

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

Optional methods
^^^^^^^^^^^^^^^^

The following methods have default implementations that throw
``SemiringException``.  Override them only if the semiring supports
the corresponding gate types:

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

Absorptive semirings
^^^^^^^^^^^^^^^^^^^^

Override ``absorptive()`` to return ``true`` if :math:`a \oplus a = a`
for all :math:`a` (e.g., Boolean, Why-provenance).  This lets the
circuit evaluator deduplicate children of ``plus`` gates, which can
significantly improve performance.


Example: The Boolean Semiring
-----------------------------

:cfile:`Boolean.h` is the simplest semiring and a good template.
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

:cfile:`Counting.h` counts the number of distinct derivations
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
   semiring (e.g., ``MyRing.h``).  Implement the class inheriting from
   ``semiring::Semiring<V>``, inside the ``semiring`` namespace.

2. **Register with the compiled evaluator**.  In
   ``src/provenance_evaluate_compiled.cpp``, the function
   ``provenance_evaluate_compiled_internal`` dispatches on a semiring
   name string and a return-type OID.  Add a branch for your semiring:

   .. code-block:: cpp

      // In the appropriate type branch (VARCHAR, INT, BOOL, etc.):
      if (semiring == "myring")
        return pec_...(constants, c, g, inputs, "myring", drop_table);

   The ``pec_*`` helper functions instantiate the C++ semiring class,
   call ``GenericCircuit::evaluate<MySemiring>(...)``, and convert the
   result to a PostgreSQL ``Datum``.

3. **Create the SQL wrapper function** in ``sql/provsql.common.sql``.
   Follow the pattern of existing compiled semirings:

   .. code-block:: plpgsql

      CREATE OR REPLACE FUNCTION sr_myring(token UUID, token2anot REGCLASS)
        RETURNS <return_type> AS
      $$
        SELECT provsql.provenance_evaluate_compiled(token, token2anot, 'myring', NULL::<return_type>);
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

2. The C++ function :cfunc:`provenance_evaluate_compiled` extracts the
   semiring name string and return-type OID, then calls
   ``provenance_evaluate_compiled_internal``.

3. The internal function reads the circuit from mmap (via
   ``getGenericCircuit``), creates a mapping from input gates to their
   annotations, and dispatches to the appropriate ``pec_*`` helper
   based on return type and semiring name.

4. The ``pec_*`` helper instantiates the semiring class and calls
   ``GenericCircuit::evaluate<S>(root, mapping)``, which performs a
   post-order DAG traversal applying semiring operations at each gate.
