Case Study: Peer-Review Assignment and Knowledge Compilation
============================================================

This case study looks at ProvSQL from the angle of **knowledge
compilation** (see :doc:`knowledge-compilation`): how the *shape* of a
SQL query determines the shape of the Boolean provenance circuit it
produces, and how that, in turn, decides which probability method is
cheap and which needs a full compiler. It is driven through
:doc:`ProvSQL Studio <studio>`, where the circuit, its CNF, the compiled
d-DNNF, and the method comparison all sit side by side.

The headline is a single pair of queries over the **same data**: one is
*safe* (its lineage is read-once, probability is trivial), the other is
its *unsafe* sibling (its lineage is genuinely entangled, probability
computation is :math:`\#P`-hard in general). No recursion is involved anywhere; the
difference is entirely in the join pattern, and -- as we will see -- in
which keys the schema declares.

The Scenario
------------

A conference-reviewing setup, with three uncertain relations (alongside
deterministic dimension tables ``reviewers`` / ``papers`` / ``topics``,
and one block-correlated table ``assignment`` used at the end):

* ``bid(reviewer, paper)`` -- a reviewer offered to review a paper; the
  confidence is how firm the bid is (availability, willingness).
* ``expertise(reviewer, topic)`` -- the reviewer's area of competence.
* ``topic_of(paper, topic)`` -- the paper is about a topic.

The data is seeded so that paper ``p1`` is the interesting one: three
reviewers bid on it, two of them (Alice and Bob) share the same area of
expertise (``databases``), and ``p1`` itself spans two topics. That
shared topic is what entangles ``p1``'s coverage lineage.

A key modelling choice drives the whole study: ``expertise`` is declared
with a **primary key on** ``reviewer`` -- one area per reviewer. As we
will see, that single constraint is exactly what makes the safe query
safe :cite:`DBLP:journals/jacm/DalviS12`.

Setup
-----

This case study assumes a working ProvSQL installation
(see :doc:`getting-provsql`) and a running ProvSQL Studio session
(see :doc:`studio`). Download
:download:`setup.sql <../../casestudy7/setup.sql>` and load it into a
fresh database::

    createdb peer_review_demo
    psql -d peer_review_demo -f setup.sql

Confidences are seeded by the script (a per-row ``conf`` column
fed to :sqlfunc:`set_prob`). Connect Studio to the fixture::

    provsql-studio --dsn postgresql:///peer_review_demo

and open `http://127.0.0.1:8000/ <http://127.0.0.1:8000/>`_. The schema
panel tags ``bid``, ``expertise`` and ``topic_of`` (added with
:sqlfunc:`add_provenance`) with a :sc:`prov-tid` pill -- their tuples are
independent -- and ``assignment`` (set up with :sqlfunc:`repair_key`)
with a :sc:`prov-bid` pill, marking it block-correlated; the three
dimension tables are plain. The ``*_label`` mappings let the eval
strip's ``sr_formula`` / ``sr_why`` / ``sr_how`` name the leaves.

Step 1: A Safe Query
--------------------

"Which papers have a reviewer who bid on them and has some area of
expertise?"

.. code-block:: postgresql

    SELECT b.paper
    FROM bid b, expertise e
    WHERE b.reviewer = e.reviewer
    GROUP BY b.paper

This query is **hierarchical**, hence safe: among its existential
variables, the atoms mentioning the topic variable are a subset of those
mentioning the reviewer variable. Concretely, because ``expertise`` has a
primary key on ``reviewer``, each reviewer contributes exactly one
``expertise`` row, so no ``bid`` tuple is reused across the disjuncts of
a paper's lineage. The lineage is **read-once**.

Click into ``p1``'s ``provsql`` cell. In Circuit mode the circuit is a
shallow ``⊕`` of ``⊗`` pairs -- one ``bid ∧ expertise`` per reviewer,
nothing shared. In the eval strip, pick ``probability_evaluate`` with the
``independent`` method: it returns ``≈ 0.97`` **exactly and instantly**,
because ``independent`` is applicable precisely to read-once circuits.
Pick *Tree decomposition* from the knowledge-compilation group: the
treewidth is **1**, and the compiled d-DNNF (the *Compiled d-D circuit*
option) is a handful of gates.

Step 2: The Unsafe Sibling
--------------------------

Now ask the more natural question: "which papers are *competently
covered* -- reviewed by someone who bid on them **and** is an expert in
one of the paper's own topics?"

.. code-block:: postgresql

    SELECT b.paper
    FROM bid b, expertise e, topic_of t
    WHERE b.reviewer = e.reviewer
      AND e.topic    = t.topic
      AND t.paper    = b.paper
    GROUP BY b.paper

This is the canonical **non-hierarchical** query :math:`H_0` (a
three-atom cycle over ``reviewer``, ``paper``, ``topic``); its
probability computation is :math:`\#P`-hard in general
:cite:`DBLP:journals/jacm/DalviS12`. On our data the entanglement is
concrete: Alice and Bob both bid on ``p1`` and are both expert in
``databases``, and ``p1`` is about ``databases`` -- so the single tuple
``topic_of(p1, databases)`` appears in *two* different disjuncts of
``p1``'s coverage. The lineage is **not read-once**.

Click into ``p1``'s ``provsql`` cell: the circuit is visibly bushier,
with the ``topic_of(p1, databases)`` input feeding two ``⊗`` gates. Run
``probability_evaluate`` with ``independent`` and it now **errors** --
*"Not an independent circuit"* -- exactly the symptom of a non-read-once
lineage. Switch to ``tree-decomposition`` (or any external compiler) and
it succeeds: the treewidth is **2**, and :sqlfunc:`ddnnf_stats` reports a
d-DNNF roughly twice the size of the safe query's, on the same handful of
inputs.

This is the case study's core lesson, and it turns on the key: relax
``expertise``'s primary key to ``(reviewer, topic)`` -- letting a
reviewer span several areas -- and even the *safe* query of Step 1 stops
being read-once, because a reviewer's single ``bid`` tuple would then be
shared across their several expertise rows. Safety is a property of the
query **and** the integrity constraints, not of the query alone.

Step 3: From Circuit to CNF, and Back
-------------------------------------

Pin ``p1``'s unsafe provenance and pick *Tseytin CNF* from the
knowledge-compilation group of the eval strip. The panel shows the
DIMACS encoding ProvSQL streams to an external compiler, with one
``c input`` comment line per variable recording which provenance input
it stands for; Studio annotates each with the source tuple it resolves
to (``bid(r1,p1)``, ``topof(p1,t1)``, …). This is what lets you take a
satisfying assignment or a weighted count from an external tool and read
it back against the reviewing data. The same mapping is available as a
table through :sqlfunc:`tseytin_cnf_mapping`.

Step 4: Compile, Measure, Compare
---------------------------------

Pick *Compiled d-D circuit* and a compiler (``d4``, ``c2d``, …): Studio
renders the d-DNNF on the canvas with a gates / edges / depth summary.
:sqlfunc:`ddnnf_stats` exposes the same numbers as ``jsonb`` (node and
edge counts, the AND/OR/NOT/input split, smoothness, depth, treewidth,
compile time), so the same circuit compiled by different tools can be
compared quantitatively rather than by eye.

For the whole picture at once, pick *Probability benchmark*: it times
every probability method on ``p1``'s unsafe circuit, one row each, and
its ``d-DNNF (n/e)`` column shows the compiled size next to the run time.
The exact methods (``tree-decomposition``, each ``compilation`` tool,
each ``wmc`` counter) agree to full precision; ``monte-carlo`` lands in
its confidence band; ``independent`` shows its error. To export the
compiled circuit itself, pick *Compiled d-D (NNF text)*: the copy button
yields a c2d/d4 ``.nnf`` file whose variable numbering matches the CNF
from Step 3.

Step 5: A Shortcut Before Compilation
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

The ``HAVING count(*) >= 2`` becomes a comparison gate over the group's
contributing rows. Before any d-DNNF compiler runs,
:sqlfunc:`probability_evaluate` folds it with a closed-form
Poisson-binomial evaluation. Raise ``provsql.verbose_level`` to ``5`` or
more in the Config panel and run the probability: ProvSQL emits a NOTICE
reporting that the ``gate_cmp`` was shortcut by the pre-pass and how many
gates the circuit shrank by. Inspect :sqlfunc:`simplified_circuit_subgraph`
before and after: the formula the compiler would have seen is smaller
than the one you wrote -- worth knowing when reading a method's reported
timing.

Step 6: Correlation via ``repair_key``
--------------------------------------

Finally, the ``assignment`` table lists, per reviewer, the papers they
*could* be assigned to, made mutually exclusive by ``repair_key`` on
``reviewer`` (each reviewer ends up on exactly one paper). Ask which
papers get an assigned reviewer:

.. code-block:: postgresql

    SELECT paper
    FROM assignment
    GROUP BY paper

Click a result row's ``provsql`` cell: the circuit carries the
mutual-exclusion structure (a reviewer's candidate papers cannot both be
true). Here ``independent`` is not just imprecise but *wrong*: the
contributing tuples are correlated by construction. The
``tree-decomposition`` and ``compilation`` methods, which evaluate the
circuit as written, give the correct probability. Correlation, like
non-hierarchical joins, is a reason the provenance circuit -- and a real
knowledge compiler -- earns its keep.

.. seealso::

   - :doc:`knowledge-compilation` for the full pipeline and every
     function used here.
   - :doc:`probabilities` for the probability methods.
   - :cite:`DBLP:series/synthesis/2011Suciu` for the theory of safe
     queries and the dichotomy.
