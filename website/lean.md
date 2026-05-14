---
layout: single
title: "Lean Formalization"
permalink: /lean/
---

Parts of the theory underlying ProvSQL have been formally verified in
[Lean 4](https://lean-lang.org/), providing a rigorous mathematical foundation
for the provenance semiring framework on which ProvSQL is built.

[Browse the Lean Documentation](/lean-docs/Provenance.html){: .btn .btn--primary}
<a href="https://github.com/PierreSenellart/provenance-lean" class="btn btn--inverse"><i class="fab fa-github"></i>&nbsp; Source</a>

## What Has Been Formalized

The formalization covers the core algebraic and database-theoretic foundations of provenance:

**Semirings with monus (m-semirings).** The central algebraic structure used by ProvSQL – commutative semirings extended with a monus operator for handling set difference – is formally defined and its properties proved. Concrete instances formalized include Boolean, counting (natural numbers), tropical, Łukasiewicz, min-max, Viterbi, multivariate polynomial semirings (*How*[*X*]), the lineage-based semirings *Which*[*X*] and *Why*[*X*], Boolean-function semirings (*Bool*[*X*]), and interval-union semirings over dense linear orders (used for temporal databases).

**Database foundations.** Formal definitions of tuples, relations, and databases, together with the standard relational algebra operations (selection, projection, join, union, difference).

**Annotated databases and query semantics.** The lifted semantics of relational algebra over annotated databases – showing how each operator propagates provenance annotations through the m-semiring operations – is formally developed and proved correct.

**Query rewriting.** A query rewriting evaluation strategy on annotated relations, implementing rules (R1)–(R5) of the ProvSQL ICDE 2026 paper. Correctness is partially formalized: the cases for projection (R1), cross product (R2), and duplicate elimination (R3) are machine-checked, with the multiset-difference case (R4) in progress.

