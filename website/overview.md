---
layout: single
title: "Overview"
permalink: /overview/
toc: true
toc_label: "Contents"
toc_sticky: true
---

ProvSQL is a [PostgreSQL](https://www.postgresql.org/) extension that adds
**semiring provenance** and **uncertainty management** to SQL queries.
It is implemented as a PostgreSQL planner hook that transparently rewrites
queries – no changes to the application or schema are required.

For a full introduction to the concepts and capabilities, see the
[Introduction](/docs/user/introduction.html) in the user documentation.

A pre-built container is also published on Docker Hub as
[`inriavalda/provsql`](https://hub.docker.com/r/inriavalda/provsql), for
a zero-install trial.  See the [Docker
instructions](/docs/user/getting-provsql.html#docker-container) in the
installation guide.

## Query Rewriting {#query-rewriting}

When a table is registered for provenance tracking via
[`add_provenance()`](/doxygen-sql/html/group__table__management.html#ga00f0d0b04b2b693c974e72aaf095cb3b),
each tuple gains a `provsql` UUID column. ProvSQL's planner hook intercepts
every query involving such tables and rewrites it to compute a provenance
circuit over those UUIDs, appending the result UUID to the SELECT list.

The rewriter handles:
- SELECT-FROM-WHERE, JOIN, nested subqueries
- GROUP BY, SELECT DISTINCT
- UNION / UNION ALL / EXCEPT
- VALUES
- Common table expressions (`WITH`), including `WITH RECURSIVE` on PostgreSQL 15+
- UPDATE / INSERT / DELETE (when `provsql.update_provenance` is enabled)

## Semiring Evaluation {#semirings}

Once a query carries provenance, its circuit can be evaluated in any
commutative **(m-)semiring** through a single compiled-evaluation path:
Boolean provenance, tuple counting, why-, how-, and which-provenance,
symbolic formulas, and the tropical, Viterbi, Łukasiewicz, and
min-max / max-min semirings, among others. The same circuit also drives
**where-provenance** – which source cells contributed to each output
value – and **Shapley / Banzhaf values** that quantify each input
tuple's contribution to an answer, both computed in a single traversal.
See the [semiring documentation](/docs/user/semirings.html).

## Probability Evaluation {#probabilities}

ProvSQL is also a **probabilistic database**: attach a probability to
each input tuple with `set_prob()`, and the provenance circuit becomes
the lineage formula whose probability is the marginal probability of the
query answer. ProvSQL computes it exactly (by independent-circuit
evaluation, tree decomposition, or d-DNNF knowledge compilation through
an external compiler) or approximately (by Monte Carlo sampling or
weighted model counting). Because exact probability computation is
#P-hard in general, an opt-in planner-side rewrite recognises tractable
query classes – hierarchical conjunctive queries, a family of FD-aware
extensions, and the broader inversion-free class – and evaluates them in
linear time. Inputs may also be [continuous random
variables](/docs/user/continuous-distributions.html) (Normal, Uniform,
Exponential, Erlang, and mixtures), with expectations and moments
computed analytically or by Monte Carlo. See the
[probability documentation](/docs/user/probabilities.html).

## ProvSQL Studio {#studio}

[ProvSQL Studio](/docs/user/studio.html) is a web UI for provenance
inspection that pairs with the extension. It runs as a separate Python
package (on PyPI as
[`provsql-studio`](https://pypi.org/project/provsql-studio/)), connects
to any ProvSQL-enabled PostgreSQL database, and offers two complementary
modes: a **Circuit** view that renders the provenance DAG behind a
result token with on-the-fly semiring evaluation on any pinned subnode,
and a **Where** view that highlights, on hover, the source cells that
contributed to each output value.

## Lean Formalization {#lean}

Key parts of the algebraic framework underlying ProvSQL – m-semirings,
annotated databases, relational algebra semantics, and aggregation – have
been formally verified in Lean 4.
See the [Lean formalization](/lean/) page for details.

## Archival and Citation {#archival}

[![DOI](https://zenodo.org/badge/DOI/10.5281/zenodo.19512786.svg)](https://doi.org/10.5281/zenodo.19512786)
[![Archived in Software Heritage](https://archive.softwareheritage.org/badge/origin/https://github.com/PierreSenellart/provsql/)](https://archive.softwareheritage.org/browse/origin/?origin_url=https://github.com/PierreSenellart/provsql)

ProvSQL is continuously archived by
[Software Heritage](https://www.softwareheritage.org/), the universal
software preservation infrastructure. You can browse the archived
source tree at
[archive.softwareheritage.org](https://archive.softwareheritage.org/browse/origin/?origin_url=https://github.com/PierreSenellart/provsql).

Every tagged release receives a persistent DOI from
[Zenodo](https://zenodo.org/). The concept DOI above resolves to the
latest version; a versioned DOI is available for each release from
the Zenodo record page.

To cite ProvSQL in academic work, click the
[**Cite this repository**](https://github.com/PierreSenellart/provsql)
button on the GitHub repository page, or read the
[`CITATION.cff`](https://github.com/PierreSenellart/provsql/blob/master/CITATION.cff)
file directly. The canonical reference is:

> Aryak Sen, Silviu Maniu, Pierre Senellart.
> **ProvSQL: A General System for Keeping Track of the Provenance and
> Probability of Data.**
> *Proc. 42nd IEEE International Conference on Data Engineering (ICDE),
> Montréal, Canada, May 2026.*
> [arXiv:2504.12058](https://arxiv.org/abs/2504.12058)

<a href="/assets/provsql.bib" class="btn btn--inverse">Download BibTeX</a>

See the [Publications](/publications/) page for a full list of research
papers related to ProvSQL.

## Architecture {#architecture}

The diagram below shows the end-to-end flow of a query through ProvSQL
(see the [architecture chapter](/docs/dev/architecture.html) in the
developer guide for details):

![ProvSQL dataflow](/assets/images/dataflow.svg)

- [SQL API reference](/doxygen-sql/html/) – user-facing SQL functions
- [C/C++ API reference](/doxygen-c/html/) – internal implementation
- [Source code](https://github.com/PierreSenellart/provsql) on GitHub
- [Video demonstrations](/demos/) of ProvSQL in action
- [Contributors](/contributors/) and funding

[Get Started](/docs/user/getting-provsql.html){: .btn .btn--primary}
