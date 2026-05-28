.. _knowledge-compilation:

Knowledge Compilation
=====================

ProvSQL builds **provenance circuits** from SQL queries transparently,
through its planner hook. When Boolean provenance is required, whether
for probability evaluation, for Shapley value computation, or simply at
the user's request, a Boolean circuit of a particularly convenient form
is obtained. This chapter follows the full pipeline behind
:doc:`probability evaluation <probabilities>` and shows how to inspect
every intermediate artifact:

.. code-block:: text

    SQL query  →  provenance circuit  →  CNF  →  d-DNNF  →  probability

Each arrow is exposed by a SQL function, and each artifact can be
rendered as `GraphViz DOT <https://graphviz.org/doc/info/lang/>`_ or
printed as `DIMACS
<https://jix.github.io/varisat/manual/0.2.0/formats/dimacs.html>`_ text.
The same surfaces drive the knowledge-compilation panels of
:doc:`ProvSQL Studio <studio>`.

.. note::

   Because this chapter is entirely about knowledge compilation over
   Boolean provenance, you will usually want ProvSQL's Boolean-only
   optimisations switched on for the whole session. They are gated
   behind the :ref:`provsql.boolean_provenance
   <provsql-boolean-provenance>` GUC (off by default): issue ``SET
   provsql.boolean_provenance = on;``, or pick :guilabel:`Boolean` on
   ProvSQL Studio's :guilabel:`Provenance scheme` switch next to the
   query box.

The circuit
-----------

Every probabilistic query result carries a provenance token (a UUID)
naming the root of its provenance circuit (see :doc:`provenance-tables`
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
circuit into a `CNF
<https://en.wikipedia.org/wiki/Conjunctive_normal_form>`_ formula by the
`Tseytin transformation
<https://en.wikipedia.org/wiki/Tseytin_transformation>`_: one fresh
variable per gate, plus clauses asserting that each gate variable is
equivalent to the Boolean combination of its inputs. This is the exact
encoding the extension streams to ``d4`` / ``c2d`` / ``minic2d`` /
``dsharp`` on a temporary file; :sqlfunc:`tseytin_cnf` returns it as
text instead:

.. code-block:: postgresql

    SELECT tseytin_cnf(provenance()) FROM suspects WHERE id = 1;

The output is DIMACS. With the default ``mapping => true`` it opens with
one ``c input`` comment line per input variable, the self-documenting
variable mapping described :ref:`below <tseytin-cnf-mapping>`; the
``p cnf <variables> <clauses>`` header and the clauses follow. With the
default ``weighted => true``, the literal-weight lines required by
`weighted model counters <https://en.wikipedia.org/wiki/Model_counting>`_
are appended, one per input literal:

.. code-block:: text

    c input 1 7f3a2b1c-… 0.5
    c input 2 9b2c4d5e-… 0.6
    c input 3 a1b2c3d4-… 0.7
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

.. _tseytin-cnf-mapping:

Reading a model back: the variable mapping
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A DIMACS variable is just an integer, so a satisfying assignment or
weighted count returned by an external tool is meaningless until you
know which provenance input each variable stands for. This is what the
``c input`` lines at the top of the output above record, one per input
variable: its DIMACS number, the originating provenance UUID, and its
probability. They make the CNF self-documenting at no cost to the
solver: model counters and compilers ignore ``c`` comment lines, so the
file stays valid DIMACS. Pass ``mapping => false`` to omit them. Only
input variables appear (the auxiliary Tseytin variables, one per gate,
are not provenance inputs).

The same information is available as a table through
:sqlfunc:`tseytin_cnf_mapping`, which is what you want when reading a
tool's output back programmatically:

.. code-block:: postgresql

    SELECT * FROM tseytin_cnf_mapping(
      '00000000-0000-0000-0000-000000000000');

Feed it a concrete token, either a literal UUID or one materialised in a
plain table. You cannot pull the token inline from a provenance-tracked
relation in the same statement (``FROM suspects s,
tseytin_cnf_mapping(s.provsql) m``): while ``provsql.active`` is on the
planner hook refuses to rewrite a multi-column function applied to a
tracked relation (*FROM function with multiple output attributes not
supported*). Set ``provsql.active`` off for that pattern.

The ``variable`` column matches the DIMACS numbering, ``gate`` is the
original-circuit input UUID, and ``probability`` its weight. In ProvSQL
Studio, the Tseytin CNF panel goes one step further and annotates each
``c input`` line with the *source tuple* the variable came from
(resolved from the tracked relations), so you can see at a glance that,
say, variable 3 is ``suspects(5, …)``.

From CNF to d-DNNF
------------------

An external **knowledge compiler** turns the CNF into a
deterministic, decomposable `negation normal form
<https://en.wikipedia.org/wiki/Negation_normal_form>`_ (d-DNNF), on which
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

    * ``OBDD`` is the canonical `ordered Boolean decision diagram
      <https://en.wikipedia.org/wiki/Binary_decision_diagram>`_, a strict
      subset of d-DNNF whose decisions are restricted to a global variable
      order.
    * ``OBDD[AND]`` augments OBDD with internal AND nodes
      :cite:`DBLP:journals/jair/LaiLY17`, recovering a more compact
      representation while keeping polynomial Apply.
    * ``Decision-DNNF`` drops the variable order, retaining only the
      decomposability + determinism of d-DNNF; it is the canonical
      target of ``d4``.

    Panini also ships ``R2-D2`` and ``CCDD`` target languages.
    ProvSQL does **not** expose them: both emit ``K`` (kernelize) nodes encoding
    literal-equivalence constraints over a shared kernel variable,
    which break the decomposability invariant of a d-DNNF. A direct
    AND-translation gives silently-wrong probabilities; a correct
    translation requires case-splitting on the kernel variables and
    is not yet implemented.

Each compiler must be installed and reachable on the PostgreSQL
server's ``PATH``, or in a directory listed in the
``provsql.tool_search_path`` GUC (see :doc:`configuration`). The same
query, compiled by different tools, yields different d-DNNFs for the
same Boolean function: comparing their size and sharing is instructive,
and is one of the things the Studio knowledge-compilation panel makes
visual.

Exporting the d-DNNF as text
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

:sqlfunc:`compile_to_ddnnf_dot` is for the eye. To get the compiled
circuit as a machine-readable artifact -- to feed it to an external
d-DNNF reasoner or verifier, or to archive it -- :sqlfunc:`compile_to_ddnnf`
returns the same circuit in the standard c2d / d4 ``.nnf`` text format:

.. code-block:: postgresql

    SELECT compile_to_ddnnf(provenance(), 'd4')
    FROM suspects WHERE id = 1;

The output opens with an ``nnf <#nodes> <#edges> <#vars>`` header, then
one line per node: ``L <lit>`` for a literal leaf, ``A <k> …`` for an
AND over ``k`` earlier nodes, ``O <j> <k> …`` for an OR (``j`` is the
decision variable, ``0`` when ProvSQL records none). It accepts the same
compiler / meta-route names as :sqlfunc:`compile_to_ddnnf_dot`.

The literal variables use the **same numbering as**
:sqlfunc:`tseytin_cnf`, even when an external compiler renumbered them
internally, so a ``.cnf`` and a ``.nnf`` of the same circuit
cross-reference and :sqlfunc:`tseytin_cnf_mapping` interprets both. In
Studio the *Compiled d-D (NNF text)* eval-strip option shows the NNF and
the copy button yields it verbatim.

Measuring the compiled d-DNNF
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

To make that comparison quantitative rather than visual,
:sqlfunc:`ddnnf_stats` returns the structural statistics of the d-DNNF a
given compiler produces, as a jsonb object:

.. code-block:: postgresql

    SELECT jsonb_pretty(ddnnf_stats(provenance(), 'd4'))
    FROM suspects WHERE id = 1;

The object reports ``nodes`` and ``edges``, the ``and`` / ``or`` /
``not`` / ``inputs`` gate-type split, whether the result is ``smooth``
(every OR gate's children share their variable set), the longest-path
``depth``, the circuit ``treewidth`` (``null`` when it exceeds the
supported limit), and the ``compile_ms`` wall-clock compile time. It
accepts the same compiler names as :sqlfunc:`compile_to_ddnnf_dot`,
including the in-process ``interpret-as-dd`` and ``tree-decomposition``
routes, so a single string change re-measures a different compiler on
the same circuit:

.. code-block:: json

    {
        "and": 3, "or": 4, "not": 0, "inputs": 5,
        "nodes": 12, "edges": 14, "depth": 6,
        "smooth": true, "treewidth": 3,
        "compiler": "d4", "compile_ms": 1.83
    }

In Studio, the compiled-d-DNNF canvas shows a gates / edges / depth
summary in its subtitle, and the probability benchmark (below) adds a
``d-DNNF (n/e)`` column so the size of every compiler's output sits
beside its run time in one table.

The in-process routes
---------------------

ProvSQL also ships two **built-in** compilers that need no external
tool. Both are accepted wherever a compiler name is, by
:sqlfunc:`compile_to_ddnnf_dot`, :sqlfunc:`compile_to_ddnnf`,
:sqlfunc:`ddnnf_stats`, and the matching :sqlfunc:`probability_evaluate`
methods.

``interpret-as-dd`` reinterprets the provenance circuit *directly* as a
d-DNNF, with no compilation step, reading each ``times`` as an AND of
independent children and each ``plus`` as an *independent* OR, the
latter rewritten by De Morgan into a ``NOT`` over an AND of ``NOT``\ s
(``¬(¬a ∧ ¬b ∧ …)``) so the result stays a genuine d-DNNF. It is
therefore exact only on circuits whose gates are genuinely independent,
the shape an independent or read-once query produces; it does not try to
certify that ``plus`` gates are deterministic (mutually exclusive),
which would be expensive to assert. Gate types it cannot read this way
raise an unsupported-gate error. It is the cheapest route, and the one
the default method tries first.

``tree-decomposition`` is the structural fallback: it builds a `tree
decomposition <https://en.wikipedia.org/wiki/Tree_decomposition>`_ of
the Boolean circuit and compiles it to a d-DNNF in time linear in the
circuit and singly-exponential in the `treewidth
<https://en.wikipedia.org/wiki/Treewidth>`_
:cite:`DBLP:journals/mst/AmarilliCMS20`. It fails, raising a *Treewidth
greater than N* error, when the treewidth exceeds the supported limit;
the default method then falls through to an external compiler (the
``provsql.fallback_compiler`` GUC, default ``d4``).

:sqlfunc:`tree_decomposition_dot` exposes the underlying min-fill
decomposition as DOT, so the variable-elimination order that yields the
in-process d-DNNF is itself inspectable. Its first line is a comment
carrying the treewidth:

.. code-block:: text

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

``probability_benchmark`` takes a concrete token, so feed it a literal
UUID or one materialised in a plain table, exactly like
:sqlfunc:`tseytin_cnf_mapping` above (and with the same restriction: do
not call it inline over a provenance-tracked relation while
``provsql.active`` is on):

.. code-block:: postgresql

    SET provsql.monte_carlo_seed = 42;   -- reproducible Monte-Carlo row

    SELECT method, args,
           ROUND(probability::numeric, 6) AS prob,
           ROUND(milliseconds::numeric, 1) AS ms,
           error
    FROM probability_benchmark('00000000-0000-0000-0000-000000000000')
    ORDER BY method, args NULLS FIRST;

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
as missing are filtered out of the pickers, and the Studio benchmark
skips those methods altogether: unlike the SQL
:sqlfunc:`probability_benchmark`, which still emits a row per method and
records the failure in its ``error`` column, their rows simply do not
appear.

.. seealso::

   - :doc:`probabilities` for the probability methods these artifacts feed.
   - :doc:`semirings` for the broader semiring-evaluation surface.
   - :doc:`export` and :sqlfunc:`view_circuit` for circuit visualisation.
   - :doc:`configuration` for ``provsql.tool_search_path`` and
     related GUCs.
