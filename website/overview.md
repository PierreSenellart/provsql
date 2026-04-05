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
- UPDATE / INSERT / DELETE (when `provsql.update_provenance` is enabled)

Semiring evaluations, probability computation, and Shapley/Banzhaf values
are described in the [user documentation](/docs/).

## Lean Formalization {#lean}

Key parts of the algebraic framework underlying ProvSQL – m-semirings,
annotated databases, and relational algebra semantics – have been formally
verified in Lean 4.
See the [Lean formalization](/lean/) page for details.

## Architecture and More {#architecture}

- [SQL API reference](/doxygen-sql/html/) – user-facing SQL functions
- [C/C++ API reference](/doxygen-c/html/) – internal implementation
- [Source code](https://github.com/PierreSenellart/provsql) on GitHub
- [Video demonstrations](/demos/) of ProvSQL in action
- [Contributors](/contributors/) and funding

[Get Started](/docs/user/getting-provsql.html){: .btn .btn--primary}
