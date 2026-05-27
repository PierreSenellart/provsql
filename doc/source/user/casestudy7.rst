Case Study: Peer-Review Assignment and Knowledge Compilation
============================================================

This case study looks at ProvSQL from the angle of **knowledge
compilation** (see :doc:`knowledge-compilation`): how the *shape* of a
SQL query, **together with the keys the schema declares**, determines the
shape of the Boolean provenance circuit it produces, and how that, in
turn, decides which probability method is cheap and which needs a full
compiler. It is driven through :doc:`ProvSQL Studio <studio>`, where the
circuit, its CNF, the compiled d-DNNF, and the method comparison all sit
side by side.

The thread running through the chapter is a single coverage question
asked three ways over the **same data**:

* asked so that it is **safe by shape** (hierarchical) -- read-once,
  probability is trivial;
* asked about **one paper**, where it is genuinely entangled yet still
  **read-once thanks to a key** (the primary key on ``expertise``);
* asked about the **whole program**, where it is genuinely
  :math:`\#P`-hard and a real compiler earns its keep.

The first queries involve no recursion; the difference is entirely in the
join pattern and in which keys the schema declares. A closing section
then turns to **recursive** reachability, where provenance becomes
network reliability.

The Scenario
------------

A conference-reviewing setup, with five uncertain (tuple-independent)
relations alongside deterministic dimension tables ``reviewers`` /
``papers`` / ``topics`` and one block-correlated table ``assignment``
(used in Step 7). Three relations drive the coverage queries, and two
more are graphs queried recursively in Step 8:

* ``bid(reviewer, paper)`` -- a reviewer offered to review a paper; the
  confidence is how firm the bid is (availability, willingness).
* ``expertise(reviewer, topic)`` -- the reviewer's area of competence.
* ``topic_of(paper, topic)`` -- the paper is about a topic.
* ``extends(citing, cited)`` -- a paper builds on an earlier paper
  (a directed **acyclic** citation graph; Step 8).
* ``coreview(a, b)`` -- two reviewers have served on a committee together
  (a **symmetric**, hence cyclic, collaboration graph; Step 8).

The instance has 14 reviewers, 4 topics and 7 papers. A key modelling
choice drives the whole study: ``expertise`` is declared with a
**primary key on** ``reviewer`` -- each reviewer has exactly one area.
That single functional dependency, ``reviewer`` :math:`\to` ``topic``, is
what makes the per-paper coverage query safe, as we will see. Several
reviewers deliberately **share** each area (five are database experts,
including Alice, Bob and Judy), so a paper's coverage lineage is
genuinely entangled: the same ``topic_of`` tuple is shared by all the
co-experts who bid on the paper.

Setup
-----

This case study assumes a working ProvSQL installation
(see :doc:`getting-provsql`) and a running ProvSQL Studio session
(see :doc:`studio`). Download
:download:`setup.sql <../../casestudy7/setup.sql>` and load it into a
fresh database:

.. code-block:: bash

    createdb peer_review_demo
    psql -d peer_review_demo -f setup.sql

Confidences are seeded by the script (a per-row ``conf`` column fed to
:sqlfunc:`set_prob`). Connect Studio to the fixture:

.. code-block:: bash

    provsql-studio --dsn postgresql:///peer_review_demo

and open `http://127.0.0.1:8000/ <http://127.0.0.1:8000/>`_. The schema
panel tags the five tracked relations (``bid``, ``expertise``,
``topic_of``, ``extends``, ``coreview``, all added with
:sqlfunc:`add_provenance`) with a :sc:`prov-tid` pill -- their tuples are
independent -- and ``assignment`` (set up with :sqlfunc:`repair_key`)
with a :sc:`prov-bid` pill, marking it block-correlated; the three
dimension tables (``reviewers``, ``papers``, ``topics``) are plain. Key
columns are underlined in the panel:
solid for a primary key, dotted for a ``repair_key`` grouping key (so
``assignment``'s ``reviewer`` shows the dotted underline). The
``*_label`` mappings let the eval strip's ``sr_formula`` / ``sr_why`` /
``sr_how`` name the leaves.

Most of the probability queries below are run twice, with
``provsql.boolean_provenance`` off and on. With the GUC **off** (the
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

    SELECT b.paper
    FROM bid b, expertise e
    WHERE b.reviewer = e.reviewer
    GROUP BY b.paper
    ORDER BY b.paper

This query is **hierarchical** -- among its existential variables, the
atoms mentioning ``topic`` (just ``expertise``) are a subset of those
mentioning ``reviewer`` (``bid`` and ``expertise``) -- so it is safe
:cite:`DBLP:journals/jacm/DalviS12`. Hierarchy is a property of the query
alone, independent of any key: ``topic`` is a dangling existential, and a
paper's lineage is ``OR`` over reviewers of ``bid ∧ (the reviewer has
some expertise)``, with nothing shared.

Click into ``p1``'s ``provsql`` cell. In Circuit mode the circuit is a
shallow ``⊕`` of ``⊗`` pairs, nothing shared. In the eval strip, pick
:guilabel:`Marginal probability` with the ``independent`` method: it returns
``≈ 0.666`` **exactly and instantly**, because ``independent`` is
applicable precisely to read-once circuits. Pick *Tree decomposition*
from the knowledge-compilation group: the treewidth is **1**, and the
compiled d-DNNF (the *Compiled d-D circuit* option) is a handful of
gates (a few dozen nodes, the exact count depending on the compiler).
This works the same with ``boolean_provenance`` off or on.

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

For the fixed paper ``p1``, this is the canonical non-hierarchical
pattern :math:`H_0` over ``reviewer`` and ``topic`` (one atom on each, and
``expertise`` on both). On our data the entanglement is concrete: Alice,
Bob and Judy are all database experts who bid on ``p1``, and ``p1`` is
about ``databases`` (topic ``t1``) -- so the single tuple
``topic_of(p1, t1)`` sits in *three* different disjuncts of ``p1``'s
coverage.

Run it with ``boolean_provenance`` **off** and ``independent``: it
**errors** -- *"Not an independent circuit"*. The literal circuit
(treewidth 2) reuses the ``topic_of(p1, t1)`` leaf, so it is not
read-once.

Now turn ``boolean_provenance`` **on** and run ``independent`` again: it
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
together**, not of the query alone. Relax ``expertise``'s key to
``(reviewer, topic)`` -- letting a reviewer span several areas -- and the
functional dependency is gone: the same question becomes a genuine
:math:`H_0`, and even ``boolean_provenance`` cannot make it read-once.

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
both mention it. This is the three-atom cycle :math:`H_0` over
``reviewer``, ``paper``, ``topic``, whose probability is
:math:`\#P`-hard in general :cite:`DBLP:journals/jacm/DalviS12`. The
primary key on ``expertise`` does **not** save it: under the FD the topic
is still shared across papers, so after the FD reduction the query
remains non-hierarchical (``reviewer`` and ``paper`` overlap on ``bid``
without nesting). Turn ``boolean_provenance`` on and the provenance
circuit is unchanged -- the safe-query rewriter finds no safe plan and
leaves the literal circuit in place -- so ``independent`` errors just as
it does with the GUC off. (Raising ``provsql.verbose_level`` would log
the rewriter falling through, but by default you simply observe that the
circuit, and the error, are the same.)

Click into the result's ``provsql`` cell. The circuit is visibly bushy.
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
DIMACS encoding ProvSQL streams to an external compiler, with one
``c input`` comment line per variable recording which provenance input
it stands for; Studio annotates each with the source tuple it resolves
to (``bid(r1,p1)``, ``topof(p1,t1)``, …). This is what lets you take a
satisfying assignment or a weighted count from an external tool and read
it back against the reviewing data. The same mapping is available as a
table through :sqlfunc:`tseytin_cnf_mapping`.

.. figure:: /_static/casestudy7/cnf-mapping.png
   :alt: The Tseytin CNF panel: one "c input" comment line per DIMACS
         variable, each annotated with the source tuple it resolves to,
         such as bid(p6, r14) and expertise(r14, t3).

   The Tseytin CNF panel for the hard query. Studio annotates each
   ``c input`` line with the source tuple the variable stands for
   (``bid(p6, r14)``, ``expertise(r14, t3)``, …), so a model or weighted
   count returned by an external tool reads back against the data.

Step 5: Compile, Measure, Compare
---------------------------------

Pick *Compiled d-D circuit* and a compiler (``d4``, ``c2d``, …): Studio
renders the d-DNNF on the canvas with a gates / edges / depth summary.
:sqlfunc:`ddnnf_stats` exposes the same numbers as ``jsonb`` (node and
edge counts, the AND/OR/NOT/input split, smoothness, depth, treewidth,
compile time), so the same circuit compiled by different tools can be
compared quantitatively rather than by eye.

For the whole picture at once, pick *Probability benchmark*: it times
every probability method on the hard circuit, one row each, and its
``d-DNNF (n/e)`` column shows the compiled size next to the run time.
The exact methods (``tree-decomposition``, each ``compilation`` tool,
each ``wmc`` counter) agree to full precision; ``monte-carlo`` lands in
its confidence band; ``independent`` shows its error. To export the
compiled circuit itself, pick *Compiled d-D (NNF text)*: the copy button
yields a c2d/d4 ``.nnf`` file whose variable numbering matches the CNF
from Step 4.

.. figure:: /_static/casestudy7/probability-benchmark.png
   :alt: The probability-benchmark table on the hard circuit, one row
         per method, with method, args, probability, time, and the
         compiled d-DNNF node/edge sizes; the compilation tools and WMC
         counters all return 0.8818, independent shows a "not an
         independent circuit" error, and two backends time out.

   The probability benchmark on the hard circuit. Every exact
   backend (each compiler, ``tree-decomposition``, the WMC counters)
   agrees on ``0.8818``; the ``d-DNNF (N/E)`` column shows how the
   compiled size varies by compiler, ``monte-carlo`` lands in its band,
   and ``independent`` reports the circuit is not independent.

Step 6: A Shortcut Before Compilation
-------------------------------------

Some queries never reach the compiler at all, because a
**probability-side pre-pass** resolves part of the circuit first. Ask for
papers with at least two bidding experts:

.. code-block:: postgresql

    SELECT b.paper
    FROM bid b, expertise e
    WHERE b.reviewer = e.reviewer
    GROUP BY b.paper
    HAVING count(*) >= 2
    ORDER BY b.paper

The ``HAVING count(*) >= 2`` clause becomes a comparison gate over the
group's contributing rows. When computing the probability of query
results, before any d-DNNF compiler runs, :sqlfunc:`probability_evaluate`
folds the provenance with a closed-form Poisson-binomial evaluation,
replacing the entire provenance with a Bernoulli gate. Compute the
probability for one of the papers (say, ``p1``): ProvSQL emits a NOTICE
reporting that the ``gate_cmp`` was shortcut by the pre-pass and how many
gates the circuit shrank by. All probability methods (even,
``independent``) compute the probability easily.

Step 7: Correlation via ``repair_key``
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

    SELECT paper
    FROM assignment
    GROUP BY paper
    ORDER BY paper

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

Step 8: Recursive Lineage -- Reachability and Reliability
---------------------------------------------------------

.. note::

   Recursive queries (``WITH RECURSIVE``) require **PostgreSQL ≥ 15**.

Every query so far has been a single conjunctive block. ProvSQL also
tracks provenance through ``WITH RECURSIVE`` (set semantics): the
recursive CTE is transparently evaluated to a fixpoint, and the result
carries provenance like any other query -- the provenance of a
reachability answer is the disjunction over the paths that reach it. The
two graphs introduced above exercise the two regimes: ``extends`` is
acyclic, ``coreview`` is cyclic.

**Acyclic data works for any semiring.** Ask what paper ``p6``
transitively builds on:

.. code-block:: postgresql

    WITH RECURSIVE anc(paper) AS (
        SELECT 'p6'
      UNION
        SELECT e.cited FROM extends e JOIN anc a ON e.citing = a.paper
    )
    SELECT paper FROM anc WHERE paper <> 'p6' ORDER BY paper

In the eval strip, ``sr_formula`` (with the ``extends_label`` mapping)
shows each ancestor's lineage as the conjunction along the path -- ``p1``
is reached as ``ext(p4,p1) ⊗ ext(p6,p4)`` -- and
:guilabel:`Marginal probability` returns its value (``p1``: ``0.72``,
``p5``: ``0.70``). Because the graph is acyclic the lineage is
read-once per ancestor, and *every* semiring evaluation is available, not
just probability.

**Cyclic data needs Boolean provenance.** ``coreview`` is symmetric, so
"who is reviewer ``r1`` connected to?" walks a cyclic graph:

.. code-block:: postgresql

    WITH RECURSIVE conn(node) AS (
        SELECT 'r1'
      UNION
        SELECT e.b FROM coreview e JOIN conn c ON e.a = c.node
    )
    SELECT node FROM conn WHERE node <> 'r1' ORDER BY node

Evaluate the probability with ``boolean_provenance`` **off** and the
fixpoint never stabilises -- ProvSQL stops with *"no fixpoint after N
rounds (cyclic data?)"*: under a general semiring a cycle keeps
contributing new derivations forever. Turn ``boolean_provenance`` **on**
and the value converges (Boolean provenance is *absorptive*: a longer,
cycle-revisiting derivation is absorbed by the shorter one it contains),
so the connection probability is well defined. It is exactly **network
reliability** -- the probability that ``r1`` stays connected to the
target when each collaboration edge is present independently -- which is
:math:`\#P`-hard in general. Here ``r1`` reaches ``r5`` with reliability
``0.5496``; the circuit is a genuine reliability circuit, so
``independent`` cannot evaluate it, while ``tree-decomposition`` and the
compilers can.

This is the same knowledge-compilation story as Step 3, reached through a
fixpoint instead of a fixed join pattern: reachability over a
probabilistic graph is where the provenance circuit, and a real compiler,
are indispensable. Note that cyclic recursion under
``boolean_provenance`` is sound for absorptive evaluation (probability,
Boolean) but not for multiplicity-counting semirings.

.. seealso::

   - :doc:`knowledge-compilation` for the full pipeline and every
     function used here.
   - :doc:`probabilities` for the probability methods.
   - :cite:`DBLP:series/synthesis/2011Suciu` for the theory of safe
     queries and the dichotomy.
