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
- SELECT-FROM-WHERE, inner and outer JOIN, `LATERAL`
- Subqueries in `FROM` (including deeply nested) and outside `FROM`
  (`EXISTS` / `NOT EXISTS`, `IN` / `NOT IN`, quantified comparisons such
  as `= ANY`, scalar subqueries), correlated or not
- GROUP BY, aggregation (including `FILTER` clauses), HAVING, SELECT DISTINCT
- UNION / UNION ALL / EXCEPT / EXCEPT ALL
- VALUES
- Common table expressions (`WITH`), including `WITH RECURSIVE` on PostgreSQL 15+
- UPDATE / INSERT / DELETE (when `provsql.update_provenance` is enabled)

See the [supported-features list](/docs/user/querying.html#supported-sql-features)
in the user documentation for the precise scope.

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
an external compiler) or approximately with `(ε, δ)` guarantees (Monte
Carlo, Karp-Luby, stopping-rule, sieve, certified-bounds d-trees, or
weighted model counting). In practice you do not pick a method: you ask
for the guarantee you want – exact, or additive / relative `(ε, δ)` –
and a cost-based chooser runs the cheapest method that meets it,
escalating automatically under a budget. Because exact probability
computation is #P-hard in general, an opt-in planner-side rewrite
recognises tractable query classes – hierarchical conjunctive queries, a
family of FD-aware extensions, and the broader inversion-free class –
and evaluates them in linear time. Any query answer can also be
[conditioned](/docs/user/conditioning.html) on an event with the `|`
operator, turning ProvSQL into a probability calculator. Inputs may
themselves be [continuous random
variables](/docs/user/continuous-distributions.html): eleven continuous
families (Normal, Uniform, Exponential, Erlang, Gamma / chi-squared,
log-normal, Weibull, Pareto, Beta, inverse-gamma, inverse-Gaussian), the
common discrete count
distributions (Poisson, binomial, geometric, hypergeometric, negative
binomial), and categorical, Gaussian-mixture, and empirical
distributions built from samples or a CDF table. Expectations,
variances, higher moments, quantiles, and information-theoretic
readouts (entropy, KL divergence, mutual information) are computed
analytically where a closed form exists and by Monte Carlo otherwise.
See the [probability documentation](/docs/user/probabilities.html).

## Aggregation, Updates, and Time {#aggregation}

Aggregation results carry provenance through **m-semimodules**:
`agg_token` values record symbolically how a `SUM`, `COUNT`,
`MIN`/`MAX`, or `AVG` depends on base tuples, support further
arithmetic, evaluate in any m-semiring, and give exact probabilities to
`HAVING` predicates. See the [aggregation
documentation](/docs/user/aggregation.html). Data modifications
(`INSERT` / `UPDATE` / `DELETE`) can themselves be
[provenance-tracked](/docs/user/data-modification.html), enabling audit
and undo; combined with the interval-union semiring, validity
timestamps turn a provenance-tracked database into a [temporal
database](/docs/user/temporal.html), time-travel queries included.

## ProvSQL Studio {#studio}

[ProvSQL Studio](/docs/user/studio.html) is a web UI for provenance
inspection that pairs with the extension. It runs as a separate Python
package (on PyPI as
[`provsql-studio`](https://pypi.org/project/provsql-studio/)), connects
to any ProvSQL-enabled PostgreSQL database, through five complementary
modes:

<ul class="studio-modes">
  <li><i class="fas fa-project-diagram"></i> <a href="/docs/user/studio.html#studio-circuit-mode"><strong>Circuit</strong></a> – render the provenance DAG behind a result token, with on-the-fly semiring evaluation on any pinned subnode.</li>
  <li><i class="fas fa-chart-bar"></i> <a href="/docs/user/studio.html#studio-contributions-mode"><strong>Contributions</strong></a> – rank the input tuples by their signed Shapley or Banzhaf contribution to a pinned answer.</li>
  <li><i class="fas fa-search-location"></i> <a href="/docs/user/studio.html#studio-where-mode"><strong>Where</strong></a> – highlight, on hover, the source cells that contributed to each output value.</li>
  <li><i class="fas fa-clock"></i> <a href="/docs/user/studio.html#studio-temporal-mode"><strong>Temporal</strong></a> – place the rows of a relation or a query on a validity timeline: as-of, during, and full-history views.</li>
  <li><i class="fas fa-book-open"></i> <a href="/docs/user/studio.html#studio-notebook-mode"><strong>Notebook</strong></a> – Jupyter-style notebooks with SQL, Markdown, circuit, and evaluation cells, saved and loaded as standard <code>.ipynb</code> files.</li>
</ul>

The tutorial and case studies of the documentation ship as runnable
example notebooks.

## ProvSQL Playground {#playground}

The [ProvSQL Playground](/playground/) is the whole system running in
your browser: PostgreSQL with ProvSQL compiled to WebAssembly (via
PGlite), with the unmodified Studio Python on top (via Pyodide) – no
install, no server, nothing leaves the page. It comes pre-loaded with
databases and runnable notebooks for the tutorial and the case studies.

## Lean Formalization {#lean}

Key parts of the algebraic framework underlying ProvSQL – m-semirings,
annotated databases, relational algebra semantics, and aggregation – have
been formally verified in Lean 4.
See the [Lean formalization](/lean/) page for details.

## License {#license}

[![MIT License](https://img.shields.io/github/license/PierreSenellart/provsql)](https://github.com/PierreSenellart/provsql/blob/master/LICENSE)

ProvSQL is free, open-source software, distributed under the permissive
[MIT License](https://github.com/PierreSenellart/provsql/blob/master/LICENSE).
You are free to use, modify, and redistribute it, including in commercial
and proprietary settings, as long as the copyright notice is preserved.
[Contributions](/contributors/) are welcome.

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
