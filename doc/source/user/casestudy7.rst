.. nb:name: cs7
.. nb:database: cs7

Case Study: Peer-Review Assignment and Knowledge Compilation
============================================================

This case study looks at ProvSQL from the angle of **knowledge
compilation** (see :doc:`the knowledge-compilation chapter
<knowledge-compilation>`): how the *shape* of a
SQL query, **together with the keys the schema declares**, determines the
shape of the Boolean provenance circuit it produces, and how that, in
turn, decides which probability method is cheap and which needs a full
compiler. It is driven through :doc:`ProvSQL Studio <studio>`, where the
circuit, its CNF, the compiled d-DNNF, and the method comparison all sit
side by side.

.. nb:skip
.. tip::

   **Follow along in your browser, no install.** Open this case study `as a
   runnable notebook in the ProvSQL Playground
   <https://provsql.org/playground/?nb=cs7>`_, or open the bare `cs7 database
   <https://provsql.org/playground/?db=cs7>`_ and follow the Studio steps as
   you read. This chapter compares probability methods, including exact
   compilation through an *external* compiler (``d4``): those external-tool
   steps will not run in the Playground, which bundles none. The built-in
   tree-decomposition compiler (and everything else) does work, so the
   in-process side of the comparison is fully reproducible there. See the
   :ref:`Playground note <playground-note>`.

The thread running through the case study is a single coverage question
asked three ways over the **same data**:

* asked so that it is **safe by shape** (hierarchical) -- read-once,
  probability is trivial;
* asked about **one paper**, where it is genuinely entangled yet still
  **read-once thanks to a key** (the primary key on ``expertise``);
* asked about the **whole program**, where it is genuinely
  `#P-hard <https://en.wikipedia.org/wiki/%E2%99%AFP>`__ and a real
  compiler earns its keep.

These three are the first rungs of a ladder, none recursive. The case study
then climbs the rest, all over the same instance: a ``HAVING count(*)``
query that a probability-side pre-pass resolves before any compiler runs;
a self-join that is **inversion-free** -- hard to every circuit-level
method, yet linear-time from a query-derived variable order; and a
``repair_key`` table whose block correlation stays tractable too. A
closing section then turns to **recursive** reachability, where
provenance becomes network reliability.

The Scenario
------------

We consider a conference-reviewing setup. Eight relations carry
uncertainty -- seven tuple-independent, plus the block-correlated
``assignment`` -- alongside the deterministic dimension tables
``reviewers`` / ``papers`` / ``topics``. Three tuple-independent
relations make up the core instance and drive the coverage queries; the
rest are introduced where they are first used -- two graphs queried
recursively in Step 10, ``recommend`` / ``champion`` in Step 7, and
``assignment`` in Steps 8-9:

* ``bid(reviewer, paper)`` -- a reviewer offered to review a paper; the
  confidence is how firm the bid is (availability, willingness).
* ``expertise(reviewer, topic)`` -- the reviewer's area of competence.
* ``topic_of(paper, topic)`` -- the paper is about a topic.

The instance has 14 reviewers, 4 topics and 7 papers. A key modelling
choice drives the whole study: ``expertise`` is declared with a
**primary key on** ``reviewer`` -- each reviewer has exactly one area.
That single `functional dependency
<https://en.wikipedia.org/wiki/Functional_dependency>`__, ``reviewer``
:math:`\to` ``topic``, is
what makes the per-paper coverage query safe, as we will see. Several
reviewers deliberately **share** each area (five are database experts,
including Alice, Bob and Judy), so a paper's coverage lineage is
genuinely entangled: the same ``topic_of`` tuple is shared by all the
co-experts who bid on the paper.

Setup
-----

.. nb:omit-begin

This case study assumes a working ProvSQL installation
(see :doc:`getting-provsql`) and a running ProvSQL Studio session
(see :doc:`studio`). Download
:download:`setup.sql <../../casestudy7/setup.sql>` and load it into a
fresh database:

.. code-block:: bash

    createdb peer_review_demo
    psql -d peer_review_demo -f setup.sql


.. nb:omit-end

.. nb:setup: ../../casestudy7/setup.sql

Confidences are seeded by the script (a per-row ``conf`` column fed to
:sqlfunc:`set_prob`).

.. nb:omit-begin

Connect Studio to the fixture:

.. code-block:: bash

    provsql-studio --dsn postgresql:///peer_review_demo

and open `http://127.0.0.1:8000/ <http://127.0.0.1:8000/>`_.

.. nb:omit-end

The schema
panel tags every relation added with :sqlfunc:`add_provenance` with a
:sc:`prov-tid` pill -- their tuples are independent -- ``assignment``
(set up with :sqlfunc:`repair_key`) with a :sc:`prov-bid` pill marking it
block-correlated, and leaves the dimension tables (``reviewers``,
``papers``, ``topics``) plain. Key columns are underlined: solid for a
primary key, dotted for a ``repair_key`` grouping key (so
``assignment``'s ``reviewer`` shows the dotted underline). The
``*_label`` mappings let the eval strip's ``sr_formula`` / ``sr_why`` /
``sr_how`` name the leaves.

Most of the probability queries below are run twice, with
the ``'boolean'`` provenance class off and on. With it **off** (the
default) ProvSQL builds the provenance circuit literally from the join
plan. With it **on**, ProvSQL's safe-query rewriter recognises
hierarchical conjunctive queries -- including those made hierarchical by
a key -- and rewrites them into a read-once form, so the linear-time
``independent`` method applies. In Studio the GUC is controlled by the
:guilabel:`Boolean` position of the three-way
:guilabel:`Semiring` / :guilabel:`Boolean` / :guilabel:`Where`
provenance toggle (see :ref:`studio-query-toggles`); switching to
:guilabel:`Semiring` turns it back off.

Step 1: Safe by Shape
---------------------

"Which papers have a reviewer who bid on them and has some area of
expertise?"

.. code-block:: postgresql

    SELECT p.id, p.title
    FROM bid b, expertise e, papers p
    WHERE b.reviewer = e.reviewer AND b.paper = p.id
    GROUP BY p.id, p.title
    ORDER BY p.id

This query is **hierarchical** -- among its existential variables, the
atoms mentioning ``topic`` (just ``expertise``) are a subset of those
mentioning ``reviewer`` (``bid`` and ``expertise``) -- so it is safe
:cite:`DBLP:journals/jacm/DalviS12`. Hierarchy is a property of the query
alone, independent of any key: ``topic`` is a dangling existential, and a
paper's lineage is ``OR`` over reviewers of ``bid ∧ (the reviewer has
some expertise)``, with nothing shared.

Click into ``p1``'s ``provsql`` cell. In Circuit mode the circuit is a
shallow ``⊕`` of ``⊗`` pairs, nothing shared -- the shape ProvSQL's
default evaluation order happens to build for this query. That read-once
shape is not guaranteed in general: the literal circuit a plan
materialises can reuse leaves and stop being read-once -- as the very
next step shows -- which is what motivates the rewriter and the compilers
of the steps that follow. In the eval strip, pick
:guilabel:`Marginal probability` with the ``independent`` method: it returns
``≈ 0.666`` **exactly and instantly**, because ``independent`` is
applicable precisely to read-once circuits. The same structure also
compiles for free: pick *Compiled d-D circuit* with the *interpret as
d-D* route and ProvSQL reads the Boolean circuit straight into a d-D --
no `tree decomposition <https://en.wikipedia.org/wiki/Tree_decomposition>`__,
no external compiler -- a handful of gates,
because a read-once circuit can be read off as a d-D in a single
structural pass, with no search. *Tree decomposition* reaches the
same place from the other side (`treewidth
<https://en.wikipedia.org/wiki/Treewidth>`__ **1**, a similarly small
d-DNNF), as does any external compiler. This works the same with
the ``'boolean'`` provenance class off or on.

Step 2: Safe by a Key
---------------------

Now a sharper, genuinely entangled question -- "is paper ``p1``
**competently covered**: did some reviewer bid on it **and** is that
reviewer an expert in one of ``p1``'s own topics?"

.. code-block:: postgresql

    SELECT DISTINCT 1
    FROM bid b, expertise e, topic_of t
    WHERE b.reviewer = e.reviewer
      AND e.topic    = t.topic
      AND b.paper = 'p1' AND t.paper = 'p1'

For the fixed paper ``p1``, this is the opposite of Step 1's shape:
``bid`` mentions only ``reviewer``, ``topic_of`` only ``topic``, and
``expertise`` mentions both, so neither variable's atoms nest inside the
other's -- the query is **non-hierarchical**, the hard case. On our data
the entanglement is concrete: Alice, Bob and Judy are all database
experts who bid on ``p1``, and ``p1`` is
about ``databases`` (topic ``t1``), so the single tuple
``topic_of(p1, t1)`` is shared across *three* of ``p1``'s coverage
disjuncts. (A fourth disjunct, Carol via ``logic`` -- ``p1``'s other
topic ``t2`` -- reuses ``topic_of(p1, t2)`` the same way.)

The fixture ships a single ``label`` mapping -- the union of every
relation's per-tuple labels -- so a text-based `semiring
<https://en.wikipedia.org/wiki/Semiring>`__ can name the
leaves of a query spanning several relations through one mapping
argument. In the eval strip, ``sr_formula`` with the ``label`` mapping
prints ``p1``'s coverage as a disjunction of four
``bid ⊗ exp ⊗ topof`` products, with ``topof(p1,t1)`` written out in
three of them -- the shared leaf, in plain sight; ``sr_why`` and
``sr_how`` read the same mapping.

Run it with the ``'boolean'`` class **off** and ``independent``: it
**errors** -- *"Not an independent circuit"*. The literal circuit
(treewidth 2) reuses the ``topic_of(p1, t1)`` leaf, so it is not
read-once.

Now turn the ``'boolean'`` class **on** and run ``independent`` again: it
succeeds, returning ``≈ 0.4259``, matching *Tree decomposition* exactly.
The key is what makes the difference. Because ``expertise`` has a primary
key on ``reviewer`` (``reviewer`` :math:`\to` ``topic``), each reviewer
contributes a single topic, so the safe plan can **group the bidding
experts by topic and factor the shared** ``topic_of`` **leaf out**:

.. math::

   \bigvee_{t} \; \mathit{topic\_of}(p_1, t) \;\wedge\;
     \Bigl(\bigvee_{r:\,\mathit{exp}(r,t)} \mathit{bid}(r, p_1) \wedge
       \mathit{exp}(r, t)\Bigr).

Each leaf now appears once: the lineage *is* read-once, and the rewriter
emits exactly this factorisation. This is the case study's first lesson:
**safety is a property of the query and the integrity constraints
together**, not of the query alone.

To watch the key do the work, drop it and re-run the coverage query's
``independent`` evaluation under the ``'boolean'`` class **on**:

.. code-block:: postgresql

    ALTER TABLE expertise DROP CONSTRAINT expertise_pkey;

With the functional dependency ``reviewer`` :math:`\to` ``topic`` gone, a
reviewer may span several areas and the question becomes genuinely
non-hierarchical, so the safe-query rewriter can no longer factor the
shared ``topic_of`` leaf out: the lineage is **no longer read-once**.

The answer stays tractable nonetheless -- ``independent`` keeps returning
``≈ 0.4259``. What changes is the *circuit*. The query is now a
:math:`\#P`-hard union of conjunctive queries, and under the ``'boolean'``
class the **joint-width UCQ compiler** (on by default) recognises it and
compiles its provenance along a tree decomposition of the data into a
certified **d-D**, which ``independent`` then evaluates in linear time.
That decomposition is more complex than the read-once factorisation above:
the key, when present, is what buys the cheaper read-once form, and without
it the joint-width route takes over. Step 9 returns to this hard-and-
correlated regime in depth; see also the :doc:`tractable-cases table
<probabilities>` and :ref:`bounded-joint-width`.

Add the key back to recover the cheaper read-once form:

.. code-block:: postgresql

    ALTER TABLE expertise DROP CONSTRAINT IF EXISTS expertise_pkey;
    ALTER TABLE expertise ADD PRIMARY KEY (reviewer);

Step 3: Genuinely Hard
----------------------

The key rescued *one paper*. Ask the same thing about the **whole
program** and it does not:

.. code-block:: postgresql

    SELECT DISTINCT 1
    FROM bid b, expertise e, topic_of t
    WHERE b.reviewer = e.reviewer
      AND e.topic    = t.topic
      AND t.paper    = b.paper

"Is **any** paper competently covered?" Here ``paper`` is no longer
fixed: it is a third existential variable, and ``bid`` and ``topic_of``
both mention it. Now the three variables ``reviewer``, ``paper`` and
``topic`` form a cycle with no nesting anywhere, whose probability is
:math:`\#P`-hard in general :cite:`DBLP:journals/jacm/DalviS12`. The
primary key on ``expertise`` does **not** save it: under the FD the topic
is still shared across papers, so after the FD reduction the query
remains non-hierarchical (``reviewer`` and ``paper`` overlap on ``bid``
without nesting). Turn the ``'boolean'`` class on and the provenance
circuit is unchanged -- the safe-query rewriter finds no safe plan and
leaves the literal circuit in place -- so ``independent`` errors just as
it does with the GUC off. (Raising ``provsql.verbose_level`` would log
the rewriter falling through, but by default you simply observe that the
circuit, and the error, are the same.)

The result is a single row, so Studio displays its circuit automatically:
it is visibly bushy.
Run :guilabel:`Marginal probability` with ``tree-decomposition`` (or any
external compiler): it succeeds, returning ``≈ 0.8818``. The treewidth is
**4** (a property of the circuit, not the compiler), against **1** for
the safe query; compiling it (the *Compiled d-D circuit* option with,
say, ``d4``) yields a d-DNNF of order a thousand nodes -- against the
safe query's few dozen -- though the precise representation, and its
size, depend on the compiler. The hardness is in the shape, and a real
compiler is what turns it into a number.

Step 4: From Circuit to CNF, and Back
-------------------------------------

Pin the hard query's provenance and pick *Tseytin CNF* from the
knowledge-compilation group of the eval strip. The panel shows the
DIMACS `CNF <https://en.wikipedia.org/wiki/Conjunctive_normal_form>`__
ProvSQL streams to an external compiler, with one
``c input`` comment line per variable recording which provenance input
it stands for; Studio annotates each with the source tuple it resolves
to. This is what lets you take a
satisfying assignment or a `weighted count
<https://en.wikipedia.org/wiki/Sharp-SAT>`__ from an external tool and read
it back against the reviewing data. The same mapping is available as a
table through :sqlfunc:`tseytin_cnf_mapping`.

.. figure:: /_static/casestudy7/cnf-mapping.png
   :alt: The Tseytin CNF panel: one "c input" comment line per DIMACS
         variable, each annotated with the source tuple it resolves to,
         such as bid(p6, r14) and expertise(r14, t3).

   The Tseytin CNF panel for the hard query. Studio annotates each
   ``c input`` line with the source tuple the variable stands for
   (``bid(p6, r14)``, ``expertise(r14, t3)``…), so a model or weighted
   count returned by an external tool reads back against the data.

Step 5: Compile, Measure, Compare
---------------------------------

Pick *Compiled d-D circuit* and a compiler (``d4``, ``c2d``…): Studio
renders the d-DNNF on the canvas with a gates / edges / depth summary.
:sqlfunc:`ddnnf_stats` exposes the same numbers as ``jsonb`` (node and
edge counts, the AND/OR/NOT/input split, smoothness, depth, treewidth,
compile time), so the same circuit compiled by different tools can be
compared quantitatively rather than by eye.

For the whole picture at once, pick *Probability benchmark*: it times
every probability method on the hard circuit, one row each, and its
``d-DNNF (n/e)`` column shows the compiled size next to the run time.
The exact methods that finish (``tree-decomposition``, the
``compilation`` tools, the model counters) agree to full precision;
``monte-carlo`` lands in its confidence band; ``independent`` shows its
error; and backends that do not scale to this circuit -- such as
``possible-worlds`` enumeration -- hit the ``statement_timeout``. To export the
compiled circuit itself, pick *Compiled d-D (NNF text)*: the copy button
yields a c2d/d4 ``.nnf`` file whose variable numbering matches the CNF
from Step 4.

.. figure:: /_static/casestudy7/probability-benchmark.png
   :alt: The probability-benchmark table on the hard circuit, one row
         per method, with method, args, probability, time, and the
         compiled d-DNNF node/edge sizes; the compilers, tree-decomposition
         and the model counters that finish return 0.8818, monte-carlo
         returns 0.8822, independent shows a "not an independent circuit"
         error, and possible-worlds and the weightmc counter hit the
         statement timeout.

   The probability benchmark on the hard circuit. The compilers,
   ``tree-decomposition`` and the model counters that finish all agree on
   ``0.8818`` (the ``d-DNNF (N/E)`` column shows how the compiled size
   varies by compiler); ``monte-carlo`` lands in its band; ``independent``
   reports the circuit is not independent; and ``possible-worlds`` and
   ``weightmc`` hit the ``statement_timeout`` with the same cancellation
   message.

Which compilers and counters appear in those pickers depends on what is
installed. The **Tools** panel (the wrench icon in the top nav) lists the
external-tool registry -- every ``compile`` and ``wmc`` tool ProvSQL
knows, each flagged whether it is *available* on your backend (its binary
resolves on the server's ``PATH``) and *enabled* -- read live from
``provsql.tools`` (see :doc:`the external-tool registry </user/tool-registry>`).
The compiler dropdown
and the benchmark offer only the available, enabled ones, so the rows you
see reflect your machine.

Step 6: A Shortcut Before Compilation
-------------------------------------

Some queries never reach the compiler at all, because a
**probability-side pre-pass** resolves part of the circuit first. Ask for
papers with at least two bidding experts:

.. code-block:: postgresql

    SELECT p.id, p.title
    FROM bid b, expertise e, papers p
    WHERE b.reviewer = e.reviewer AND b.paper = p.id
    GROUP BY p.id, p.title
    HAVING count(*) >= 2
    ORDER BY p.id

The ``HAVING count(*) >= 2`` clause becomes a comparison gate over the
group's contributing rows. When computing the probability of query
results, before any d-DNNF compiler runs, :sqlfunc:`probability_evaluate`
folds the provenance with a closed-form `Poisson-binomial
<https://en.wikipedia.org/wiki/Poisson_binomial_distribution>`__ evaluation,
replacing the entire provenance with a Bernoulli gate. Compute the
probability for one of the papers (say, ``p1``): ProvSQL emits a NOTICE
reporting that the ``gate_cmp`` was shortcut by the pre-pass and how many
gates the circuit shrank by. All probability methods (even
``independent``) compute the probability easily.

Step 7: Hard-Looking, Secretly Linear
-------------------------------------

Steps 3-5 sent a :math:`\#P`-hard query to a compiler; Step 6 skipped the
compiler with a count-threshold shortcut. Here is a third escape: a query
whose lineage is **not** read-once -- so ``independent`` rejects it, and a
generic compiler treats it as hard -- yet whose *shape* admits a
linear-size `OBDD <https://en.wikipedia.org/wiki/Binary_decision_diagram>`__.
ProvSQL recognises this **inversion-free** class
:cite:`DBLP:conf/icdt/JhaS11` (the ``inversion-free`` method described
in :doc:`the chapter on probabilities <probabilities>`) at planning
time and
evaluates it with a structured d-DNNF built over a query-derived variable
order, instead of reaching for a compiler.

For this step the fixture adds one prolific bidder, **Olga** (``r15``),
who skimmed a 24-paper submission batch (``q01`` to ``q24``), and two
post-review signals: ``recommend(reviewer, paper)`` -- she recommended the
paper for acceptance -- and ``champion(reviewer, paper)`` -- she would
champion it at the PC meeting. Ask for a reviewer whose bids overlap
**both** a recommendation **and** a championing:

.. code-block:: postgresql

    SELECT r.id, r.name
    FROM bid b1, recommend a, bid b2, champion c, reviewers r
    WHERE b1.reviewer = a.reviewer AND b1.paper = a.paper
      AND b1.reviewer = b2.reviewer
      AND b2.reviewer = c.reviewer AND b2.paper = c.paper
      AND b1.reviewer = r.id
    GROUP BY r.id, r.name

Grouping on the reviewer ``OR``\ s the two evidence sides over all of
Olga's papers, and both sides **share** the ``bid(r15, *)`` leaves -- so
the lineage is not read-once and ``independent`` rejects it. But it is a
*consistent-unification self-join* on ``bid`` with a single root variable
(``reviewer``), which is exactly the inversion-free condition. In Circuit
mode the per-row root carries a teal :sc:`IF` badge (the inversion-free
certificate); pin it to read the certificate header and the
variable-block order.

Now compare methods on that one row. ``tree-decomposition`` gives up
(*"Treewidth greater than 10"*); ``possible-worlds`` refuses (the witness
has more than 64 inputs); and a real compiler does not finish -- with a
``statement_timeout`` set, ``d4`` is cut off on the 24-paper instance,
exactly as the hard query of Step 3 was. Yet :guilabel:`Marginal
probability` with the ``inversion-free`` method -- and the **default**
method, which takes the inversion-free rung automatically once the
certificate is present -- returns ``0.975314`` in milliseconds. The query
*looks* as hard as Step 3 to every circuit-level method, but its
tractability is visible in the query, not in the materialised circuit:
only the planner's query-derived order recovers it. To see that order
made concrete, pick *Compiled d-D circuit* with the *inversion-free*
route: the d-DNNF it builds is strikingly **deep** -- a long chain of
decisions following the query-derived variable order, the shape of an
OBDD rather than the bushy d-DNNF a treewidth-based compiler produces.

Step 8: Correlation via ``repair_key``
--------------------------------------

.. figure:: /_static/casestudy7/schema-keys.png
   :alt: Schema-panel detail: the assignment table (PROV-BID) with a
         dotted underline on its reviewer grouping key, above the bid
         table (PROV-TID) whose reviewer and paper primary-key columns
         are solid-underlined.

   The schema panel distinguishes key kinds: ``assignment`` is BID
   (``repair_key`` on ``reviewer``, dotted underline), while the TID
   tables carry solid-underlined primary keys.

Finally, the ``assignment`` table lists, per reviewer, the papers they
*could* be assigned to, made mutually exclusive by ``repair_key`` on
``reviewer`` (each reviewer ends up on exactly one paper; the schema
panel marks ``reviewer`` with the dotted grouping-key underline). Ask
which papers get an assigned reviewer:

.. code-block:: postgresql

    SELECT p.id, p.title
    FROM assignment a JOIN papers p ON a.paper = p.id
    GROUP BY p.id, p.title
    ORDER BY p.id

Click a result row's ``provsql`` cell: the circuit carries the
mutual-exclusion structure (a reviewer's candidate papers cannot both be
true), encoded as ``mulinput`` gates that share a block key. ProvSQL
captures this correlation in the circuit, so the probability accounts
for it -- ``p1`` (covered if Alice, Bob **or** Carol is assigned to it)
gets ``0.875``, ``p2`` ``0.75`` -- and **every** method agrees,
including ``independent``: its evaluator gives mutually exclusive
``mulinput`` siblings special treatment (summing their probabilities
within a block rather than multiplying), so this kind of block
correlation, unlike the non-hierarchical cycle of Step 3, stays
tractable without a compiler.

Step 9: Hard *and* Correlated -- Bounded Joint Width
----------------------------------------------------

Step 3's whole-program coverage was :math:`\#P`-hard in its *shape* and needed
a compiler; Step 8's ``repair_key`` correlation stayed tractable because the
query *shape* was safe. What about a query that is **both** -- the hard cyclic
shape **and** correlated inputs? Lifted inference presumes
tuple-independence, so the Dalvi-Suciu safe-query rewrite (Steps 1-2) and the
inversion-free certificate (Step 7) do not apply, and ``independent`` rejects
the circuit outright.

ProvSQL has a fourth planner-time escape for exactly this: the **joint-width
UCQ compiler**. It is the non-recursive sibling of the reachability compiler
of Step 10: when a :math:`\#P`-hard union of conjunctive queries has bounded
**joint treewidth** -- the treewidth of the data *together with* its
correlation structure -- ProvSQL recognises the shape at planning time and
compiles its provenance along a tree decomposition of the data into a certified
**d-D**, exactly, in time linear in the data :cite:`Amarilli2016thesis`. Unlike
every other tractable route, it stays exact over correlated inputs.

Like the safe-query rewrite of Steps 1-2, this is part of the Boolean
machinery, so it needs the ``'boolean'`` provenance class. Within that class it
is on automatically (the ``provsql.joint_width`` GUC, on by default, is only a
debug switch to turn it off). Ask the Step-3 cyclic coverage question **per
paper** -- "for each paper, how competently is it covered?":

.. code-block:: postgresql

    SET provsql.provenance = 'boolean';

    SELECT t.paper, probability_evaluate(provenance())
    FROM bid b, expertise e, topic_of t
    WHERE b.reviewer = e.reviewer AND e.topic = t.topic AND t.paper = b.paper
    GROUP BY t.paper ORDER BY t.paper

The planner substitutes the head-pinned joint-width provenance per group, but
computes them all in **one pass**: the facts are gathered once, the joint
graph decomposed once, and a single bottom-up sweep emits one d-D per paper
(``ucq_joint_provenance_answer`` caches them and hands each group its token).
The per-paper values match a circuit compiler to full precision -- ``p1``
``0.425869``, ``p4`` ``0.300776`` -- so the marginal you read back is the
exact one, produced from the *data* treewidth without an external tool.

Now the same cyclic shape over the **correlated** ``assignment`` table of
Step 8 -- "is any paper covered by its *assigned* expert reviewer?":

.. code-block:: postgresql

    SELECT DISTINCT 1
    FROM assignment a, expertise e, topic_of t
    WHERE a.reviewer = e.reviewer AND e.topic = t.topic AND t.paper = a.paper

``independent`` rejects this (*"Not an independent circuit"*: a reviewer's
candidate papers are mutually exclusive, so the lineage is neither
tuple-independent nor read-once), and the safe and inversion-free routes do
not apply to the cyclic shape. Yet ProvSQL returns the exact ``0.735868`` from
the joint-width route, because the joint treewidth -- the data graph
*together with* the ``repair_key`` exclusion blocks -- is bounded. This is the
one cell of the method comparison that nothing else fills: a query that is at
once :math:`\#P`-hard *and* over correlated inputs, evaluated exactly and in
linear data time. (Setting ``provsql.joint_width = off`` reverts ``provenance()``
to the literal circuit of Steps 3-5, leaving an external compiler to handle the
correlated circuit and ``independent`` to reject it, so you can see the two
routes side by side; see :doc:`the tractable-cases table <probabilities>`.)

Step 10: Recursive Lineage -- Reachability and Reliability
----------------------------------------------------------

.. note::

   Recursive queries (``WITH RECURSIVE``) require **PostgreSQL ≥ 15**.

Every query so far has been a single conjunctive block. ProvSQL also
tracks provenance through ``WITH RECURSIVE`` (set semantics): the
recursive CTE is transparently evaluated to a fixpoint, and the result
carries provenance like any other query -- the provenance of a
reachability answer is the disjunction over the paths that reach it. Two
more tuple-independent relations exercise the two recursive regimes:
``extends(citing, cited)`` -- a paper builds on an earlier one -- is a
directed **acyclic** citation graph, while ``coreview(a, b)`` -- two
reviewers served on a committee together -- is **symmetric**, hence
cyclic.

**Acyclic data works for any semiring.** Ask what paper ``p6``
transitively builds on:

.. code-block:: postgresql

    WITH RECURSIVE anc(paper) AS (
        SELECT 'p6'
      UNION
        SELECT e.cited FROM extends e JOIN anc a ON e.citing = a.paper
    )
    SELECT p.id, p.title
    FROM anc JOIN papers p ON anc.paper = p.id
    WHERE anc.paper <> 'p6' ORDER BY p.id

In the eval strip, ``sr_formula`` (with the ``extends_label`` mapping)
shows each ancestor's lineage as the conjunction along the path -- ``p1``
is reached as ``ext(p4,p1) ⊗ ext(p6,p4)`` -- and
:guilabel:`Marginal probability` returns its value (``p1``: ``0.72``,
``p5``: ``0.70``). Because the graph is acyclic the lineage is
read-once per ancestor, and *every* semiring evaluation is available, not
just probability.

**Cyclic data needs an absorptive provenance class.** ``coreview`` is
symmetric, so "who is reviewer ``r1`` connected to?" walks a cyclic graph:

.. nb:scheme: absorptive

.. code-block:: postgresql

    WITH RECURSIVE conn(node) AS (
        SELECT 'r1'
      UNION
        SELECT e.b FROM coreview e JOIN conn c ON e.a = c.node
    )
    SELECT r.id, r.name
    FROM conn JOIN reviewers r ON conn.node = r.id
    WHERE conn.node <> 'r1' ORDER BY r.id

Evaluate the probability under the default ``'semiring'`` class and the
fixpoint never stabilises -- ProvSQL stops with *"no fixpoint after 1000
rounds (cyclic data?)"*: under a general semiring a cycle keeps contributing
new derivations forever. Switch the provenance class to ``'absorptive'`` (in
Studio the provenance toggle, in SQL ``SET provsql.provenance =
'absorptive'``) and the value converges: an absorptive semiring satisfies
:math:`1 \oplus a = 1`, so a longer, cycle-revisiting derivation is absorbed
by the shorter one it contains, and the fixpoint is the set of minimal,
repetition-free paths. (``'boolean'`` works too -- it *implies*
``'absorptive'`` -- but the recursion needs only absorptivity, so
``'absorptive'`` is the class to name here.)

The answer is exactly **two-terminal network reliability** -- the probability
that ``r1`` stays connected when each collaboration edge is present
independently -- which is :math:`\#P`-hard in general. Yet ProvSQL does not
reach for a general compiler here. It recognises the recursive-reachability
shape and, following the provenance refinement of Courcelle's theorem
:cite:`DBLP:conf/icalp/AmarilliBS15`, **compiles along a tree decomposition
of the collaboration graph itself** into a certified **d-D** (a
deterministic, decomposable circuit -- not in negation normal form, so not a
d-DNNF) -- one per reachable vertex, of total size linear in the number of
edges when the graph has bounded treewidth (see :ref:`network-reliability-btw`). The materialised
tokens carry the ``'absorptive'`` assumption marker.

Because that compiled circuit is already decomposable, :guilabel:`Marginal
probability` reads the reliability straight off it -- ``r1`` reaches ``r5``
with reliability ``0.5496`` -- and even the linear ``independent`` method
evaluates it, where on the bushy :math:`\#P`-hard circuit of Step 3 it had to
refuse. That contrast is the lesson: Step 3's hardness lived in the *query
shape* and needed a real compiler to dissolve, whereas here it is tamed by a
structural property of the *data* -- the bounded treewidth of the graph --
with no external tool involved at all.

The flip side of absorptivity is that these tokens are sound for absorptive
evaluation only (probability, Boolean, nonnegative min-plus); the
``'absorptive'`` marker makes multiplicity-counting or why-provenance
evaluation -- genuinely infinite on cyclic data -- refuse them rather than
return an unjustified value.

.. seealso::

   - :doc:`The knowledge-compilation chapter <knowledge-compilation>`
     for the full pipeline and every function used here.
   - :doc:`The chapter on probabilities <probabilities>` for the
     probability methods.
   - The *Probabilistic Databases* synthesis lecture
     :cite:`DBLP:series/synthesis/2011Suciu` for the theory of safe
     queries and the dichotomy.
