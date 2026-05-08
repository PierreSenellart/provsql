# Compiled semirings: observations on what could come next

Notes on candidate (m-)semirings drawn from [https://provsql.org/lean-docs/]
that are **not** yet implemented as compiled semirings in `src/semiring/`.

The compiled-semiring path goes through
`provsql.provenance_evaluate_compiled` and gives the evaluator access to
the full circuit gate vocabulary (`cmp`, `agg`, `semimod`, `eq`,
`project`, `value`, ...) which the PL/pgSQL `provenance_evaluate` path
explicitly does not handle. That is the main reason to *compile* a
semiring rather than express it via SQL aggregates.

Already compiled (for reference): Boolean, BoolExpr, Counting, Formula
(debug pretty-printer), IntervalUnion (carriers `tstzmultirange`,
`nummultirange`, `int4multirange`), Lukasiewicz, Tropical, Viterbi,
Which, Why.

All proposals below are anchored on the Lean formalisation in
`../provenance-lean/Provenance/Semirings/`: where the description here
disagrees with Lean, Lean wins and the description is fixed.

## Plan

Four proposals, ranked by cost vs. coverage. Each is presented with its
showcase application use case.

### 1. `sr_lukasiewicz` : fuzzy logic semiring **[shipped]**

* Per the Lean reference (`Provenance/Semirings/Lukasiewicz.lean`,
  `instSemiringWithMonusLukasiewicz`):
  `a ⊕ b = max(a, b)`, `a ⊗ b = max(a + b - 1, 0)`,
  `a ⊖ b = 0` if `a ≤ b` else `a`. Zero is `0`, one is `1`. The
  formalisation proves this carrier is absorptive, idempotent, and
  satisfies left-distributivity of multiplication over monus.
* Tiny implementation: same shell as `Viterbi.h`, `double` carrier,
  `absorptive() = true`, no new `pec_*` dispatch (reuses `pec_float`).
  About 50 lines plus a test mirroring `sr_viterbi.sql`.
* Note: an earlier draft of this TODO listed `a ⊕ b = min(a+b, 1)`
  (the bounded-sum / probabilistic-t-conorm shape). The formalisation
  uses `max` for addition, so the t-norm pairing is with `max`, not
  bounded sum. Implementations must follow the Lean spec.
* **Use case : clinical decision support over uncertain symptom
  records.** A patient table has tuples annotated with
  degree-of-evidence in [0,1] for symptoms (e.g. "fever: 0.7",
  "cough: 0.4"). A query joining symptom evidence to a diagnostic
  rule "fever AND cough → flu" needs a t-norm under which long
  conjunctions do not collapse to near-zero (the Viterbi /
  probabilistic shape `a·b` does), and where `0.7 ⊗ 1.0 = 0.7`
  (Łukasiewicz preserves crisp truth). The bounded-loss multiplication
  is the standard fuzzy-medical choice for exactly this reason.
  Viterbi already covers the independent-probability case;
  Łukasiewicz fills the non-probabilistic graded-truth gap.

### 2. `pec_temporal` → `pec_multirange<...>` refactor, then `sr_interval_num` / `sr_interval_int` **[shipped]**

* Same algebra as `Temporal` but over numeric or integer multiranges
  instead of `tstzmultirange`. Lean reference is the same
  `IntervalUnion α` for any `DenselyOrdered` linear order with
  `BoundedOrder`: the temporal instance is a specialisation, not a
  separate algebra.
* Cost: trivial copy of `Temporal.h` per multirange type. **But**:
  each added carrier grows the dispatch table by one `pec_*` function
  plus an `OID_TYPE_*` entry, so adding two more multirange types
  means three near-identical `pec_*` functions.
* Refactor before duplicating: lift `pec_temporal` into a templated
  `pec_multirange<MultirangeOid, F_*_UNION, F_*_INTERSECT, F_*_MINUS>`
  so each new multirange type is a single dispatch line, not a
  copy-paste. The corresponding `semiring::Temporal` class generalises
  similarly (the multirange built-ins are the only PG-specific bits).
  The refactor pays for itself even if only one new carrier ships,
  because `Temporal` becomes the first call site of a clean
  abstraction rather than a one-off.
* **Use case (`sr_interval_num`) : sensor fusion with numeric
  validity ranges.** A sensor-fusion query joining two streams, each
  annotated with a *measurement-validity range* (e.g. "sensor reading
  valid for x ∈ [3.2, 7.8] ∪ [9.1, 12.0]"), needs to compute the
  validity range of the joined tuple. Union for ⊕ (alternate
  evidence), intersection for ⊗ (jointly required), monus for EXCEPT.
* **Use case (`sr_interval_int`) : page-range provenance in a
  digital library.** A scholarly query computing "which page intervals
  in the source documents support this conclusion" produces statements
  like "supported by pages [12,18] of doc A and pages [3,5] ∪ [40,42]
  of doc B". The `int4multirange` carrier encodes "which page
  intervals contributed" exactly.

### 3. `sr_minmax` / `sr_maxmin` : generic security / fuzzy-discrete semirings over user-defined PG enums **[shipped]**

* Min over addition, max over multiplication, over a bounded linear
  order. Lean reference: `MinMax α` for any
  `[LinearOrder α] [OrderBot α] [OrderTop α]`, with proofs of
  `MinMax.absorptive` and `MinMax.idempotent`. The dual
  `MaxMin α = MinMax (OrderDual α)` is the fuzzy / availability shape.
* Single compiled instance with a `Datum` carrier and an
  `OID_TYPE_ANYENUM` dispatch branch in the same shape as the
  `tstzmultirange` branch. ⊥ and ⊤ resolved per call from
  `pg_enum.enumsortorder`. A single compiled instantiation handles
  any user-defined PG enum: security lattices, fuzzy-discrete
  levels, three-valued logic à la Lean `TVL`.
* Caveat from the Lean reference: `MaxMin TVL` is **not**
  left-distributive over monus (theorem
  `TVL.not_mul_sub_left_distributive`), so the m-semiring laws do not
  hold uniformly across all enums. The docstring of `sr_minmax_enum`
  must call this out: monus distributes only when the carrier order
  satisfies `mul_sub_left_distributive`, which can only be checked
  per-enum, not at the C++ level.
* Worked example to ship alongside: `sr_security` over a
  `classification_level` enum, as a call site of `sr_minmax_enum`
  rather than a separate implementation.
* **Use case : multi-level security in a federated query.** A defense
  or healthcare deployment has tuples labelled with
  `classification_level ∈ {public, internal, confidential, secret,
  top_secret}`. A query joining four classified relations must return
  the *highest* classification touched (max-of-max under ⊗) and a
  UNION must yield the *most permissive* alternative (min-of-min
  under ⊕). This is the access-control semiring of Foster / Green /
  Tannen. Today the user has to write a per-deployment SQL custom
  semiring; `sr_minmax_enum` makes it a call site for any PG enum the
  user defines, including three-valued logic, fuzzy-discrete trust
  levels, and project-specific lattices.

### 4. `sr_how` : canonical ℕ[X] polynomial provenance

* The universal commutative-semiring provenance (Green, Karvounarakis,
  Tannen, *Provenance Semirings*, PODS'07). Lean reference:
  `Provenance/Semirings/How.lean`.
* Carrier is a multiset of multisets of input labels (i.e. canonical
  sum-of-products); ⊕ adds entries, ⊗ distributes. Output is
  exponential in the worst case, similar to `sr_why`.
* In ProvSQL the circuit DAG already *is* this object up to factoring,
  and `sr_formula` already pretty-prints the DAG as a symbolic
  expression. The piece a compiled `sr_how` would actually deliver
  beyond `sr_formula` is **canonical equality on polynomials**: two
  semantically-equal circuits collapse to identical text.
* **Use case : provenance-aware query equivalence in a
  data-integration platform.** When two ETL pipelines produce the
  same target relation by different SQL paths, an auditor wants to
  verify they produce the same *provenance*, not just the same
  tuples. Syntactic comparison of `sr_formula` output fails because
  the circuit factoring differs; `sr_how` canonicalises to the unique
  polynomial and supports `sr_how(t1, m) = sr_how(t2, m)` as a true
  equivalence test. This is the standard tool for regression-testing
  query rewrites, view-materialisation correctness, and "did this
  refactor change provenance?" audits.

## Priorities

1. **`sr_lukasiewicz`** : **shipped**. Lowest cost, smallest blast
   radius, no new dispatch path, aligns with an existing Lean
   instance.
2. **`pec_multirange<...>` refactor**, then `sr_interval_num` /
   `sr_interval_int`: **shipped**. The refactor lifted `Temporal`
   into a single `IntervalUnion(Oid)` class parameterised by the
   multirange type OID; the polymorphic `F_MULTIRANGE_*` built-ins
   apply uniformly across multirange carriers, so the dispatch is one
   branch per carrier and the C++ class is a single header.
3. **`sr_minmax` / `sr_maxmin`** : **shipped**. Two compiled
   instantiations of the same template, dispatched on
   `get_typtype(type) == TYPTYPE_ENUM`; bottom and top are looked up
   from `pg_enum.enumsortorder` via the `ENUMTYPOIDNAME` syscache and
   element comparison goes through `F_ENUM_CMP`. The SQL custom
   security semiring previously documented in `semirings.rst` and
   exercised by `test/sql/security.sql` was replaced by `sr_minmax`;
   `casestudy1` Step 4 now calls the compiled function. The new
   custom-semiring docs example is the bit(2) capability semiring
   (genuine commutative m-semiring on the four-element Boolean
   lattice, partial order so not subsumed by `sr_minmax` /
   `sr_maxmin`); see `test/sql/capability.sql`.
4. **`sr_how`** : ship only if circuit-equivalence checking becomes a
   stated need. The expanded polynomial output by itself is not the
   value-add over `sr_formula`; canonicalisation is.

## Implementation observations from the Tropical / Viterbi / Temporal work

These are useful to bear in mind for any future compiled semiring:

* **Carrier types currently dispatched** in
  `provenance_evaluate_compiled.cpp`: `bool`, `int4`, `float8`,
  `varchar`, `tstzmultirange`. Adding a new carrier type means a
  corresponding `pec_<type>` helper plus an `OID_TYPE_*` entry in
  `provsql_utils.{h,c}`.

* **Postgres-typed carriers (e.g. `Datum`)**: must call multirange /
  range built-ins via `OidFunctionCall*` (which sets up an `FmgrInfo`),
  not `DirectFunctionCall*` (leaves `flinfo` NULL, crashes inside the
  type-cache lookup). Built-in function OIDs are available as
  `F_*` macros in `utils/fmgroids.h`.

* **PG14+ guarded code** should use the `defined(DOXYGEN)` sentinel
  pattern so Doxygen documents the class even on the default
  `PG_VERSION_NUM` undefined view. See `Temporal.h` and `having_semantics.hpp`.

* **Absorptivity (`absorptive() = true`)** lets the circuit evaluator
  deduplicate `plus` operands and short-circuit over `one()`. Worth
  setting whenever the m-semiring axioms hold over the *carrier in C++*
  (not just over the mathematical carrier the Lean instance uses). For
  Tropical we conservatively returned `false` because `double` admits
  negative values, even though Tropical(ℕ ∪ {∞}) is absorptive.

* **HAVING wrappers** (`provsql_try_having_<name>` in
  `having_semantics.{hpp,cpp}`): adding one is essentially a one-line
  template instantiation. Cheap, and worth doing for completeness even
  when the new semiring does not override `cmp` (the default
  `SemiringException` keeps it a no-op for HAVING gates).

* **Test pattern**: the `personnel` table from `add_provenance.sql`
  plus `create_provenance_mapping(...)` with a per-id expression is a
  light-weight harness; see `test/sql/sr_*.sql` for templates. Use
  `round(...::numeric, N)` for floating-point outputs to avoid
  cross-version format drift (lesson from `sr_viterbi`).

* **SQL function naming**: `sr_<name>(token, mapping)` returning the
  carrier type, registered in `sql/provsql.common.sql` (or
  `provsql.14.sql` for PG14-only types). Add the corresponding entry
  to `_SQL_FUNC_MAP` in `doc/source/conf.py` with a Doxygen anchor
  obtained by running `doxygen Doxyfile-sql` and grepping
  `doc/doxygen-sql/html/group__compiled__semirings.html`.
