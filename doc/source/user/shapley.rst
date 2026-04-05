Shapley and Banzhaf Values
===========================

ProvSQL computes *Shapley values* and *Banzhaf values* – game-theoretic
measures from cooperative game theory that quantify the individual
contribution of each input tuple to a query result.

Background
----------

Given a Boolean query result whose truth depends on a set of input tuples,
the **Shapley value** of an input tuple ``t`` is the average marginal
contribution of ``t`` over all orderings of the input tuples.

The **Banzhaf value** is a simpler variant: the average marginal contribution
over all subsets of the other inputs (not weighted by ordering).

Both measures are always computed under the probabilistic model
:cite:`DBLP:journals/pacmmod/KarmakarMSB24`, giving *expected*
Shapley/Banzhaf values: each input token contributes according
to its probability (set with :sqlfunc:`set_prob`, see :doc:`probabilities`).
When no probabilities are set they default to 1, which recovers the
standard deterministic Shapley/Banzhaf values.

Computing Shapley Values
-------------------------

:sqlfunc:`shapley` takes the provenance token of the query result and the
token of the input tuple whose contribution to measure:

.. code-block:: postgresql

    SELECT person,
           shapley(provenance(), m.token) AS sv
    FROM suspects, witness_mapping m;

To compute Shapley values for all input variables at once (more efficient
than calling :sqlfunc:`shapley` once per variable), use
:sqlfunc:`shapley_all_vars`:

.. code-block:: postgresql

    SELECT person, value AS witness, sv
    FROM suspects,
         shapley_all_vars(provenance()) AS (variable uuid, sv double precision),
         witness_mapping m
    WHERE m.token = variable;

Computing Banzhaf Values
-------------------------

:sqlfunc:`banzhaf` and :sqlfunc:`banzhaf_all_vars` have the same calling
conventions as their Shapley counterparts:

.. code-block:: postgresql

    SELECT person,
           banzhaf(provenance(), m.token) AS bv
    FROM suspects, witness_mapping m;

Computation Notes
------------------

Shapley-value computation is generally #P-hard. ProvSQL compiles the
provenance circuit to a d-DNNF and evaluates it efficiently. The optional
third argument selects the compilation method (``'tree-decomposition'``,
``'d4'``, ``'c2d'``, etc.), with the same semantics as for
:sqlfunc:`probability_evaluate`.

Choosing Between Shapley and Banzhaf
--------------------------------------

* **Shapley values** satisfy a set of axioms (efficiency, symmetry,
  dummy, additivity) that uniquely characterise them as a fair measure of
  individual contribution.
* **Banzhaf values** are faster to compute and satisfy a slightly different
  set of axioms; they are appropriate when the efficiency guarantee is not
  required.
