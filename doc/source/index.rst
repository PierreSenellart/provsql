Welcome to ProvSQL's documentation!
===================================

ProvSQL is a PostgreSQL extension that adds semiring provenance and
uncertainty management to SQL queries.  It transparently rewrites
queries to track which input tuples contribute to each result, then
evaluates the provenance in any user-chosen semiring -- Boolean
reachability, counting, probability, Shapley values, and more.

This documentation is organized into three parts:

* The :ref:`user-guide` explains how to install, configure, and use
  ProvSQL from the SQL level.  Start here if you are new to
  ProvSQL.

* The :ref:`dev-guide` describes ProvSQL's internal architecture
  and is aimed at contributors.  It covers the PostgreSQL
  extension concepts ProvSQL relies on, the architecture and
  component map, the query rewriting pipeline, memory management,
  the where-provenance and data-modification subsystems,
  aggregation and semiring evaluation, probability computation,
  coding conventions, testing, debugging, and the build system.

* The :ref:`api-ref` provides auto-generated reference documentation
  for the SQL and C/C++ APIs (via Doxygen).

.. toctree::
   :hidden:

   ProvSQL’s Documentation <self>

.. _user-guide:

.. toctree::
   :maxdepth: 2
   :caption: User Guide

   user/introduction
   user/getting-provsql
   user/tutorial
   user/provenance-tables
   user/querying
   user/aggregation
   user/semirings
   user/probabilities
   user/shapley
   user/where-provenance
   user/data-modification
   user/temporal
   user/export
   user/configuration

.. _dev-guide:

.. toctree::
   :maxdepth: 2
   :caption: Developer Guide

   dev/introduction
   dev/postgresql-primer
   dev/architecture
   dev/query-rewriting
   dev/memory
   dev/where-provenance
   dev/data-modification
   dev/aggregation
   dev/semiring-evaluation
   dev/probability-evaluation
   dev/coding-conventions
   dev/testing
   dev/debugging
   dev/build-system

.. _api-ref:

.. toctree::
   :maxdepth: 2
   :caption: API Reference

   sql/provsql
   c/provsql

.. toctree::
   :maxdepth: 1
   :caption: References

   user/bibliography
