.. _knowledge-compilation:

Knowledge Compilation
=====================

ProvSQL rewrites SQL queries into **Boolean provenance circuits**
transparently, through its planner hook. For a knowledge-compilation
audience this is a convenient front-end: realistic, structured SQL
workloads become Boolean formulas you would otherwise have to
synthesise by hand. This chapter follows the full pipeline behind
:doc:`probability evaluation <probabilities>` and shows how to inspect
every intermediate artifact:

.. code-block:: text

    SQL query  ─►  provenance circuit  ─►  CNF  ─►  d-DNNF  ─►  probability

Each arrow is exposed by a SQL function, and each artifact can be
rendered as GraphViz DOT or printed as DIMACS text. The same surfaces
drive the knowledge-compilation panels of :doc:`ProvSQL Studio
<studio>`.

The circuit
-----------

Every probabilistic query result carries a provenance token (a UUID)
naming the root of its Boolean circuit (see :doc:`provenance-tables`
and :doc:`querying`). The circuit is a DAG over ``input`` leaves and
``plus`` / ``times`` / ``monus`` gates. :sqlfunc:`view_circuit` renders
it as DOT; Studio's :ref:`circuit mode <studio-circuit-mode>` renders
it interactively. The remaining sections take such a token and walk it
down to a probability.

For the examples below, assign probabilities to the inputs first (see
:doc:`probabilities`):

.. code-block:: postgresql

    SELECT set_prob(provenance(), reliability) FROM sightings;

From circuit to CNF
-------------------

Before invoking an external compiler, ProvSQL encodes the Boolean
circuit into a CNF formula by the **Tseytin transformation**: one fresh
variable per gate, plus clauses asserting that each gate variable is
equivalent to the Boolean combination of its inputs. This is the exact
encoding the extension streams to ``d4`` / ``c2d`` / ``minic2d`` /
``dsharp`` on a temporary file; :sqlfunc:`tseytin_cnf` returns it as
text instead:

.. code-block:: postgresql

    SELECT tseytin_cnf(provenance()) FROM suspects WHERE id = 1;

The output is DIMACS, opening with a ``p cnf <variables> <clauses>``
header. With the default ``weighted => true``, the literal-weight lines
required by weighted model counters are appended, one per input
literal::

    p cnf 6 7
    -4 0
    5 0
    -6 1 0
    ...
    w 1 0.5
    w -1 0.5
    w 2 0.6
    w -2 0.4

Pass ``weighted => false`` to obtain the bare CNF (no ``w`` lines), for
plain (unweighted) model counting or for feeding a compiler that reads
weights separately:

.. code-block:: postgresql

    SELECT tseytin_cnf(provenance(), weighted => false)
    FROM suspects WHERE id = 1;

From CNF to d-DNNF
------------------

An external **knowledge compiler** turns the CNF into a
deterministic, decomposable negation normal form (d-DNNF), on which
weighted model counting (hence probability) is linear-time.
:sqlfunc:`compile_to_ddnnf_dot` runs the requested compiler and returns
the resulting d-DNNF as a GraphViz digraph over ``AND`` / ``OR`` /
``NOT`` / input gates:

.. code-block:: postgresql

    SELECT compile_to_ddnnf_dot(provenance(), 'd4')
    FROM suspects WHERE id = 1;

The second argument names the compiler. ProvSQL ships bindings for:

``'d4'``
    Decision-DNNF compiler with backtracking
    :cite:`DBLP:conf/ijcai/LagniezM17`; the default for the ``compilation``
    probability method.
``'d4v2'``
    The rewritten d4 :cite:`LagniezM21d4v2`.
``'c2d'``
    The original dtree-guided compiler :cite:`DBLP:conf/ecai/Darwiche04`.
``'minic2d'``
    Top-down CNF-to-SDD compiler :cite:`DBLP:conf/ijcai/OztokD15`.
``'dsharp'``
    DPLL-style compiler built on sharpSAT :cite:`DBLP:conf/ai/MuiseMBH12`.
``'panini-obdd'``, ``'panini-obdd-and'``, ``'panini-decdnnf'``
    Three target languages of KCBox's Panini compiler
    :cite:`KCBoxPanini`:

    * ``OBDD`` is the canonical ordered Boolean decision diagram, a strict
      subset of d-DNNF whose decisions are restricted to a global variable
      order.
    * ``OBDD[AND]`` augments OBDD with internal AND nodes
      :cite:`DBLP:journals/jair/LaiLY17`, recovering a more compact
      representation while keeping polynomial Apply.
    * ``Decision-DNNF`` drops the variable order, retaining only the
      decomposability + determinism of d-DNNF; it is the canonical
      target of ``d4``.

    Panini also ships ``R2-D2`` and ``CCDD`` target languages
    :cite:`DBLP:journals/corr/abs-2202-10025`. ProvSQL does **not**
    expose them: both emit ``K`` (kernelize) nodes encoding
    literal-equivalence constraints over a shared kernel variable,
    which break the decomposability invariant of a d-DNNF. A direct
    AND-translation gives silently-wrong probabilities; a correct
    translation requires case-splitting on the kernel variables and
    is not yet implemented.

``'d4'`` versus ``'d4v2'``
~~~~~~~~~~~~~~~~~~~~~~~~~~

``d4`` is the original 2017 Decision-DNNF compiler with backtracking
:cite:`DBLP:conf/ijcai/LagniezM17`. ``d4v2`` is a 2021 rewrite
:cite:`LagniezM21d4v2` with the same target language, often (but not
always) faster on heavier formulas. Both share the same DIMACS CNF
input and the same NNF output format, so :sqlfunc:`compile_to_ddnnf_dot`
and :sqlfunc:`probability_evaluate` accept them interchangeably; the
two binaries are typically installed side by side.

Each compiler must be installed and reachable on the PostgreSQL
server's ``PATH``, or in a directory listed in the
``provsql.tool_search_path`` GUC (see :doc:`configuration`). The same
query, compiled by different tools, yields different d-DNNFs for the
same Boolean function: comparing their size and sharing is instructive,
and is one of the things the Studio knowledge-compilation panel makes
visual.

The in-process route: tree decomposition
-----------------------------------------

ProvSQL also has a **built-in** compiler that needs no external tool:
it builds a tree decomposition of the Boolean circuit and compiles it
to a d-DNNF in time linear in the circuit and singly-exponential in the
treewidth :cite:`DBLP:journals/mst/AmarilliCMS20`. This is the
``tree-decomposition`` probability method.

:sqlfunc:`tree_decomposition_dot` exposes the underlying min-fill
decomposition as DOT, so the variable-elimination order that yields the
in-process d-DNNF is itself inspectable. Its first line is a comment
carrying the treewidth::

    // treewidth=3

.. code-block:: postgresql

    SELECT tree_decomposition_dot(provenance())
    FROM suspects WHERE id = 1;

Weighted model counters: the ``wmc`` umbrella
---------------------------------------------

The ``wmc`` method dispatches to a family of exact weighted model
counters, each consuming the same DIMACS CNF emitted by
:sqlfunc:`tseytin_cnf` (with the literal-weight ``c p weight`` lines
required by the MCC 2024 input format):

``'ganak'``
    A projected weighted model counter from the meelgroup
    :cite:`DBLP:conf/ijcai/SharmaRSM19`. Compact and self-contained;
    requires a single ``ganak`` binary on ``PATH``.

``'sharpsat-td'``
    Tree-decomposition-guided exact counter
    :cite:`DBLP:conf/cp/KorhonenJ21`. Needs the ``flow_cutter_pace17``
    helper alongside ``sharpsat-td`` so the counter can shell out to
    its tree-decomposer.

``'dpmc'``
    Two-stage planner + executor pipeline :cite:`DBLP:conf/cp/DudekPV20`:
    ``htb`` builds a project-join tree, ``dmc`` consumes it. Both
    binaries must be installed.

``'weightmc'``
    The approximate ApproxMC variant (kept under the umbrella for
    discoverability; see :doc:`probabilities` for its
    ``epsilon;delta`` knob).

Invocation goes through :sqlfunc:`probability_evaluate`:

.. code-block:: postgresql

    SELECT probability_evaluate(provenance(), 'wmc', 'ganak')
    FROM suspects WHERE id = 1;

Switching between counters is a single-string change, which is what the
benchmark below exploits to compare them on the same circuit.

Inspecting the in-memory circuit
--------------------------------

Before any compilation runs, the ``provsql.simplify_on_load`` GUC may
have already rewritten parts of the circuit: identity / absorber
collapses, ``gate_cmp`` resolutions for circuits over random variables,
hybrid-evaluator simplifications (see :doc:`probabilities`).
:sqlfunc:`simplified_circuit_subgraph` returns the post-simplification
DAG, rooted at the given token, as a JSON adjacency list. This is the
exact shape the probability evaluators traverse:

.. code-block:: postgresql

    SELECT jsonb_pretty(simplified_circuit_subgraph(provenance()))
    FROM suspects WHERE id = 1;

Each node carries its gate type, an inline ``extra`` field for typed
leaves (random variables, ``cmp`` thresholds), and the longest-path
depth from the root. The longest path is the canonical
circuit-depth notion: it tracks the deepest chain of operators between
the node and the output, which is what governs evaluation cost and
matches the depth a renderer would draw.

Comparing probability methods
-----------------------------

:sqlfunc:`probability_benchmark` times every probability-evaluation
method on a single circuit token and returns one row per method with
its wall-clock duration and result. It runs ``independent``,
``possible-worlds``, ``tree-decomposition``, ``monte-carlo``, each
external compiler through the ``compilation`` method, and each weighted
model counter under the ``wmc`` umbrella. Methods that cannot apply
(an uninstalled compiler, a non-independent circuit, a treewidth
blow-up, …) are captured per row in an ``error`` column rather than
aborting the whole comparison.

``probability_benchmark`` returns a table, a shape the planner hook
does not rewrite, so call it with the rewriter standing back, either
on a token materialised in a plain (non-provenance) table, or with
``provsql.active`` temporarily off:

.. code-block:: postgresql

    SET provsql.active = off;
    SET provsql.monte_carlo_seed = 42;   -- reproducible Monte-Carlo row

    SELECT method, args,
           ROUND(probability::numeric, 6) AS prob,
           ROUND(milliseconds::numeric, 1) AS ms,
           error
    FROM probability_benchmark('00000000-0000-0000-0000-000000000000')
    ORDER BY method, args NULLS FIRST;

    SET provsql.active = on;

The exact methods agree to numerical precision; the approximate ones
(``monte-carlo``, ``weightmc``) land within their confidence band. The
second and third arguments tune the Monte-Carlo sample count (default
``10000``) and the ``epsilon;delta`` forwarded to ``weightmc`` (default
``'0.8;0.2'``).

Checking tool availability
--------------------------

Because the compilers and model counters are optional external
binaries, :sqlfunc:`tool_available` reports whether a given tool
resolves to an executable on the backend's effective ``PATH``,
including the ``provsql.tool_search_path`` prefix, exactly as a
subsequent ``compilation`` or ``view_circuit`` call would see it:

.. code-block:: postgresql

    SELECT tool_available('d4'), tool_available('c2d');

A bare name is resolved through the shell; a name containing a slash is
tested as a path. Studio uses this to grey out compilers that are not
installed rather than letting them fail at run time.

In ProvSQL Studio
-----------------

The knowledge-compilation pipeline is available without leaving the
browser. In Studio's :ref:`evaluation strip <studio-circuit-eval-strip>`,
the same artifacts surface as inline panels: the DIMACS CNF, the
compiled d-DNNF rendered beside the original circuit, the tree
decomposition with its treewidth, and a one-click benchmark across all
probability methods. Compilers that :sqlfunc:`tool_available` reports
as missing are filtered out of the pickers.

.. seealso::

   - :doc:`probabilities` for the probability methods these artifacts feed.
   - :doc:`semirings` for the broader semiring-evaluation surface.
   - :doc:`export` and :sqlfunc:`view_circuit` for circuit visualisation.
   - :doc:`configuration` for ``provsql.tool_search_path`` and
     related GUCs.
