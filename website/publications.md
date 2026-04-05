---
layout: single
title: "Publications"
permalink: /publications/
toc: true
toc_label: "Contents"
toc_sticky: true
---

## Papers about ProvSQL

These papers describe ProvSQL itself or the theoretical foundations on which it is built,
including its data model, query rewriting architecture, knowledge compilation approach,
and the semiring provenance theory that motivates its design.

{% bibliography --query @*[keyword ~= provsql-paper] %}

## Foundational Works

These papers establish the theoretical framework on which ProvSQL is built:
why- and where-provenance, provenance semirings, aggregate provenance, m-semirings
for non-monotone queries, the connection between provenance and probabilistic
databases, circuit-based provenance representations, and algebraic provenance
for update queries.

{% bibliography --query @*[keyword ~= provsql-foundation] %}

---

[Download full bibliography (.bib)](/_bibliography/references.bib){: .btn .btn--inverse}

