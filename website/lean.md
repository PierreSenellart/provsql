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

**Aggregation and the V_K interpretation.** Definition 7 of the ICDE 2026 paper – grouped aggregation over annotated relations via a K-semimodule of (value, annotation) pairs – is fully formalised: the free K-tensor construction ([`KTensor K M`](/lean-docs/Provenance/KSemiModule.html#KTensor)), the lifted value type V_K, the aggregation evaluator ([`Query.evaluateAggSum`](/lean-docs/Provenance/QueryAggregation.html#Query.evaluateAggSum)) for the `sum` aggregate, and the unified Definition-7 semantics for the full algebra including aggregation ([`Query.evaluateAnnotatedFull`](/lean-docs/Provenance/QueryEvaluateInVK.html#Query.evaluateAnnotatedFull)). The m-semiring class additionally carries a `δ` operator ([`SemiringWithMonus.delta`](/lean-docs/Provenance/SemiringWithMonus.html#SemiringWithMonus.delta)) with three axioms ([`delta_zero`](/lean-docs/Provenance/SemiringWithMonus.html#SemiringWithMonus.delta_zero), [`delta_natCast_pos`](/lean-docs/Provenance/SemiringWithMonus.html#SemiringWithMonus.delta_natCast_pos), [`delta_regrouping`](/lean-docs/Provenance/SemiringWithMonus.html#SemiringWithMonus.delta_regrouping)), and every concrete semiring instance ships a concrete `δ`.

**Homomorphism commutation.** Query evaluation commutes with m-semiring homomorphisms on the full algebra including aggregation ([`Query.evaluateAnnotatedFull_hom`](/lean-docs/Provenance/QueryEvaluateInVKHom.html#Query.evaluateAnnotatedFull_hom)), generalising Green-Karvounarakis-Tannen Proposition 3.5 (lifted to m-semirings à la Geerts-Poggi) and extending it to aggregation via the K-semimodule structure. This is the formal backing for ProvSQL's architecture: a single persistent provenance circuit is stored once, and each `sr_*` evaluator is the realisation of one m-semiring homomorphism out of it.

**Query rewriting.** A query rewriting evaluation strategy on annotated relations, implementing rules (R1)–(R5) of the ProvSQL ICDE 2026 paper. Correctness is fully formalised, sorry-free: the relational-algebra fragment (R1)–(R4) is proved by [`Query.rewriting_valid`](/lean-docs/Provenance/QueryRewriting.html#Query.rewriting_valid), and the unified statement covering all five rules (including the aggregation rule (R5) against the V_K-lifted evaluator) is [`Query.rewriting_valid_full`](/lean-docs/Provenance/QueryEvaluateInVK.html#Query.rewriting_valid_full).

**Provenance circuits and probability.** The provenance circuit representation, its evaluation in any m-semiring, the d-D (Decomposable + Deterministic) correctness theorem, and Tseitin CNF infrastructure are formalised in [`Provenance.Circuit`](/lean-docs/Provenance/Circuit.html), [`Provenance.Probability`](/lean-docs/Provenance/Probability.html), and [`Provenance.Tseitin`](/lean-docs/Provenance/Tseitin.html). These modules underpin ProvSQL's probability backend.

