Testing
=======

ProvSQL's test suite uses PostgreSQL's ``pg_regress`` framework.
All tests are **integration tests** that run SQL against a live
PostgreSQL instance with the ProvSQL extension loaded.


Test Layout
-----------

::

   test/
   ├── schedule.common   # Test schedule for all PostgreSQL versions
   ├── schedule.14       # Additional tests for PostgreSQL >= 14
   ├── schedule          # Generated: concatenation of the above (gitignored)
   ├── sql/              # Input SQL scripts (one per test)
   │   ├── setup.sql
   │   ├── add_provenance.sql
   │   ├── sr_boolean.sql
   │   └── ...
   └── expected/         # Expected output (one per test)
       ├── setup.out
       ├── add_provenance.out
       ├── sr_boolean.out
       └── ...

- ``test/sql/<name>.sql`` -- the SQL statements to execute.
- ``test/expected/<name>.out`` -- the expected ``psql``-style output.
  This includes both SQL commands echoed back and their results.
- ``test/schedule.common`` / ``test/schedule.14`` -- the source schedules.
  ``test/schedule`` is generated from them by the Makefile and is
  gitignored; edit the source files instead.


The Schedule File
-----------------

Each line in ``test/schedule.common`` (or ``test/schedule.14`` for tests
that require PostgreSQL 14+) is either a comment (``#``) or a
``test:`` directive listing one or more test names:

.. code-block:: text

   # Basic checks
   test: provenance_in_from identify_token subquery create_provenance_mapping

   # Introducing a few semirings
   test: sr_formula sr_counting sr_boolean sr_why sr_which sr_tropical sr_viterbi sr_lukasiewicz

Tests on the **same line** run in parallel.  Tests on **different lines**
run sequentially.  The first test (``setup``) creates the test schema
and tables; all subsequent tests depend on it.


Running Tests
-------------

.. code-block:: bash

   # Full suite (requires PostgreSQL superuser and extension installed)
   make test
   # or equivalently:
   make installcheck

   # With a specific PostgreSQL port
   make installcheck EXTRA_REGRESS_OPTS="--port=5434"

The test runner creates a temporary database ``contrib_regression``,
runs all tests from the schedule, compares actual output to expected,
and reports differences.

Upgrade-Chain Parity
--------------------

.. code-block:: bash

   make upgrade-parity-test
   # with psql options:
   make upgrade-parity-test PSQL_ARGS=--port=5434

``test/upgrade_parity.sh`` builds one database through
``CREATE EXTENSION provsql VERSION '1.0.0'`` followed by
``ALTER EXTENSION provsql UPDATE`` (exercising the whole chain of
``sql/upgrades/`` scripts) and one through a direct
``CREATE EXTENSION``, then diffs their catalogs: every function
(signature, return type, body hash, volatility, security definer),
aggregate, operator (with commutators), cast (with its context --
implicit vs assignment), type, enum value, relation, and every
extension member with its schema. Any difference means an upgrade
script failed to replicate the installed surface and the check fails
with the diff (``<`` = upgraded-only / stale, ``>`` = missing from
the chain).

This is the strong form of the ``extension_upgrade`` pg_regress
canary, which smoke-tests a handful of features; run it before every
release. It has caught missing objects, function-body drift, casts
left at the wrong context, shell operators left unfilled by
COMMUTATOR / NEGATOR forward references, and functions created in the
wrong schema by a script lacking ``SET search_path``.

Coverage
--------

.. code-block:: bash

   make coverage
   make coverage PROVSQL_COVERAGE_PORT=55000 GCOVR=~/.local/bin/gcovr

``make coverage`` rebuilds the C/|cpp| extension instrumented with
gcov, runs the full suite against a throwaway PostgreSQL cluster
owned by the invoking user (nothing is installed into the system
PostgreSQL, no sudo; requires PostgreSQL >= 18 and ``gcovr``), and
produces a line+branch report under ``coverage/`` plus
``coverage/zero_call.txt``, the list of provsql functions the suite
never calls -- filtered to genuine gaps (catalog-detected I/O / cast
/ aggregate support functions and planner-substituted placeholders
are excluded).  See ``test/coverage/README.md``.  Afterwards the
working tree holds instrumented objects: run ``make clean && make``
before a normal ``make install``, or the relink fails at server
start with an undefined ``__gcov`` symbol.


Writing a New Test
------------------

1. **Create the SQL file** ``test/sql/<name>.sql``.  Write the SQL
   statements that exercise the feature you are testing.  The
   ``search_path`` is set once at the database level by
   ``setup.sql`` (``ALTER DATABASE ... SET search_path``); do not
   ``SET`` it per test unless you need a non-standard path.

2. **Generate expected output**.  Run the test once and capture the
   output:

   .. code-block:: bash

      # Run the specific test (after make install and server restart)
      make installcheck EXTRA_REGRESS_OPTS="--schedule=test/schedule"

   Then copy the actual output to the expected file:

   .. code-block:: bash

      cp /tmp/tmp.provsqlXXXX/results/<name>.out test/expected/<name>.out

   Review the expected output to make sure it is correct.

3. **Add to the schedule**.  Insert a ``test: <name>`` line in
   ``test/schedule.common`` (or ``test/schedule.14`` for tests that
   require PostgreSQL 14+) at an appropriate position.  Place it
   after any tests it depends on (e.g., after ``setup`` and
   ``probability_setup`` if your test uses probabilities).  Do not
   edit ``test/schedule`` -- it is generated by the Makefile.

4. **Run the full suite** to verify nothing is broken:

   .. code-block:: bash

      make test


Optional-Tool Skip Pattern
--------------------------

Some tests depend on external tools that may not be installed (e.g.,
``c2d``, ``d4``, ``dsharp``, ``minic2d``, ``weightmc``).
To make these tests pass regardless of whether the tool is present,
use psql's ``\if`` with a backtick-evaluated shell command:

.. code-block:: psql

   \if `which d4 > /dev/null 2>&1 && echo true || echo false`
   -- test body that requires d4
   SELECT probability_evaluate(provenance(), 'compilation', 'd4') ...;
   \else
   \echo 'SKIPPING: d4 not available'
   \endif

Then provide two expected-output files:

- ``test/expected/d4.out`` -- the output when ``d4`` **is** available
  (the normal test results).
- ``test/expected/d4_1.out`` -- the output when ``d4`` **is not**
  available (just the skip message).

``pg_regress`` tries all ``_N.out`` alternatives and passes if the
actual output matches any of them.


Reading Test Failures
---------------------

When a test fails, ``pg_regress`` writes a diff to a temporary file:

::

   /tmp/tmp.provsqlXXXX/regression.diffs

This file shows a unified diff between expected and actual output for
each failing test.  ``make test`` will display the path to this file
and open it in a pager.

Common causes of failure:

- **UUID values**: provenance UUIDs are random and must never appear
  in expected output.  Project away the ``provsql`` column before
  comparing, and make tests depend on provenance *content* by
  evaluating it in a semiring (e.g., :sqlfunc:`sr_boolean`,
  :sqlfunc:`sr_counting`), displaying the symbolic representation
  (via :sqlfunc:`sr_formula`), or
  computing a probability.
- **Symbolic representation ordering**: when the symbolic
  representation is computed over a ⊕ (or other associative /
  commutative) gate, the order in which children are visited is not
  deterministic.  Tests that print symbolic representations must
  normalize the ordering before comparison.  The idiomatic pattern
  is a ``REPLACE`` or ``REGEXP_REPLACE`` that rewrites any variant
  orderings to a canonical one.  See ``test/sql/union.sql`` and
  ``test/sql/distinct.sql`` for examples.
- **Floating-point precision**: probability results may differ slightly
  across platforms.  Use ``ROUND()`` in tests.
- **Platform-dependent error CONTEXT**: an error raised inside a
  plpgsql wrapper carries a ``CONTEXT`` block whose text can vary
  across platforms (e.g. whitespace from a blanked ``INTO``).  Set
  ``\set VERBOSITY terse`` around the statement so only the stable
  message line is compared (see ``test/sql/ucq_joint.sql``).

Asserting that a planner route fired
------------------------------------

Value-equality assertions cannot tell a specialized route from its
fallback when both produce the same number (which is the point of a
transparent route).  The robust idiom is a plan-based check: a small
plpgsql helper runs ``EXPLAIN (VERBOSE, COSTS OFF)`` on the query and
asserts that the plan text contains the substituted function name
(e.g. ``ucq_joint_provenance_answer``).  See ``jw_fires()`` in
``test/sql/ucq_joint_answers.sql``; token *inequality* against the
standard provenance is a weaker alternative (it fails to detect a
silent fallback whose value happens to agree, which is how a
joint-width recogniser bug stayed hidden until the firing check was
added).
