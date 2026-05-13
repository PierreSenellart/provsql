Case Study: The City Air-Quality Sensor Network
=================================================

This case study demonstrates ProvSQL's continuous-distribution
surface (see :doc:`continuous-distributions`) end-to-end through
ProvSQL Studio (see :doc:`studio`). It is the first case study
driven primarily by Studio rather than ``psql``: random variables
benefit far more from interactive visualisation – PDFs, CDFs,
mixture DAG layouts, conditional histograms, simplifier
before-vs-after – than from text-mode output, and the workflow
below makes the rewriter, the simplifier, the analytic and
Monte-Carlo paths, and conditional inference all visible in the
canvas.

The Scenario
------------

A municipal observatory operates a small air-quality sensor
network. Sensors of three different vendors report a
:math:`PM_{2.5}` concentration (*fine particulate matter*, i.e.
airborne particles with aerodynamic diameter at most 2.5 μm,
expressed in micrograms per cubic metre) on a fixed schedule. The sensors differ in calibration and noise
characteristics:

* high-end units report ``Normal(μ, σ)`` with small σ;
* low-cost units report ``Uniform[μ−δ, μ+δ]`` over a small window;
* a drift-prone unit reports ``Exponential(λ)`` while its
  internal hardware self-tests cycle;
* a multi-pass aggregating unit reports ``Erlang(k, λ)`` over the
  pass count.

A reference station with a calibrated lab-grade instrument
contributes deterministic readings.

Regulatory categories partition the value axis: *Good* below 12,
*Moderate* between 12.1 and 35, *Unhealthy* above 35.1 (loosely
following the US EPA AQI breakpoints for PM2.5 in their pre-2024
form, simplified to three tiers). Each station has a Bernoulli
probability of being in calibration on a given day. A separate batch table of *historical* readings carries
the same shape so cross-batch queries via ``UNION ALL`` are
meaningful.

Your tasks:

* inspect the per-row distributions and the rewriter's effect on
  threshold queries;
* compute the probability that each station's reading exceeds an
  *Unhealthy* threshold, exercising the planner-hook rewrite for
  ``WHERE reading > 35``;
* model calibration uncertainty as a Bernoulli mixture and inspect
  the resulting ``gate_mixture`` shape;
* aggregate per-district readings and watch the simplifier fold
  the mixture cascade;
* run conditional inference (``E[reading | reading > 35]``) and
  see the closed-form truncated-distribution mean against the
  unconditional one;
* mix in ``HAVING`` on grouped random variables, ``UNION ALL`` over
  historical data, and a side-by-side comparison of probability
  methods (``'independent'`` vs ``'monte-carlo'`` vs
  ``'tree-decomposition'``).

Setup
-----

This case study assumes a working ProvSQL installation
(see :doc:`getting-provsql`) and a running ProvSQL Studio
session pointed at it (see :doc:`studio`). The fixture lives at
``studio/scripts/demo_continuous.sh`` in the ProvSQL source tree;
it creates a fresh database, runs the schema below, and seeds the
random-variable readings via the constructors documented in
:doc:`continuous-distributions`:

.. code-block:: bash

    bash studio/scripts/demo_continuous.sh

The schema is five tables:

* ``stations(id, name, district)`` – four monitoring stations
  across two districts, provenance-tracked.
* ``readings(station_id, ts, pm25 random_variable)`` – one
  ``pm25`` reading per station per timestamp; the
  ``random_variable`` carries the per-station noise model
  (normal, uniform, exponential, erlang, or a deterministic
  lifted from the reference station).
* ``calibration_status(station_id, p)`` – Bernoulli probability
  that each station is in calibration on the day of interest.
* ``categories(name, lo, hi)`` – three regulatory categories
  (*Good* / *Moderate* / *Unhealthy*) keyed by their interval
  bounds.
* ``historical_readings(...)`` – same shape as ``readings``,
  populated from yesterday's batch.

Connect Studio to the fixture::

    provsql-studio --dsn postgresql:///air_quality_demo

and open `http://127.0.0.1:8000/ <http://127.0.0.1:8000/>`_ in a
browser. The schema panel surfaces ``readings.pm25`` with type
``random_variable``: a visual confirmation that ProvSQL has
recognised the new column shape.

.. figure:: /_static/casestudy6/schema-panel.png
   :alt: Studio schema panel listing readings, historical_readings,
         calibration_status, stations (all PROV-tagged), categories
         (no provenance), and the station_mapping table, with the
         pm25 column visible on readings.

   The schema panel opened from the top nav. The four
   provenance-tracked tables carry the purple :sc:`prov` pill;
   ``readings`` and ``historical_readings`` expose ``pm25`` as a
   ``random_variable`` column.

Step 1: Inspect a Noisy Reading
--------------------------------

In the Studio query box::

    SELECT id, ts, pm25, provsql
    FROM readings
    WHERE station_id = 's1'
    ORDER BY ts;

The result table renders ``pm25`` as a clickable ``random_variable``
cell carrying the underlying gate UUID. Click into a row's
``pm25``: Studio switches to Circuit mode and renders the
``gate_rv`` leaf with the distribution-kind glyph (a small *N* for
a normal, *U* for uniform, *E* for exponential, *Γ* for erlang).
Pick *Distribution profile* under the *Distribution* optgroup of
the eval strip and click :guilabel:`Run`: the panel returns
:math:`\mu` and :math:`\sigma^2` headline stats and an inline
histogram with a PDF/CDF toggle.

The histogram is backed server-side by :sqlfunc:`rv_histogram`;
pinning ``provsql.monte_carlo_seed`` in the Config panel (under
*Provenance*) makes the shape reproducible across re-runs.

.. figure:: /_static/casestudy6/gate-rv-distribution-profile.png
   :alt: Studio Circuit mode showing the gate_rv N(28,2) leaf at
         the top of the canvas and the Distribution profile
         eval-strip panel below, with mu=28, sigma=2, support
         (-infinity, +infinity) and an inline histogram.

   The ``gate_rv`` leaf for ``pm25`` on row 1 is a ``N(28, 2)``
   circle; the eval-strip *Distribution profile* panel shows the
   :math:`\mu`, :math:`\sigma`, support, and an inline histogram.

Step 2: A First Probabilistic Threshold
----------------------------------------

The :math:`PM_{2.5}` *Unhealthy* category begins at 35.1. Find
the rows whose reading might cross it::

    SELECT id, station_id, ts, provsql AS prov
    FROM readings
    WHERE pm25 > 35;

Two transformations happen invisibly:

* The :math:`> 35` operator on the ``random_variable`` column
  resolves to ``random_variable_gt``, which the planner hook
  intercepts at planning time and rewrites into a ``gate_cmp``
  conjoined into each row's ``provsql`` column. The procedure
  body itself is never executed.
* The implicit ``integer → random_variable`` cast lifts the
  literal ``35`` into a ``gate_value``.

Click into a result row's ``prov`` cell. Circuit mode shows the
Boolean wrapper (a ``gate_times`` over the row's input token and
the ``gate_cmp``); the cmp's child link reaches into the
``gate_rv`` from Step 1.

The eval strip's :sqlfunc:`probability_evaluate` entry exposes the
five compiled methods (see :doc:`probabilities`). Pick
``monte-carlo`` and set ``n = 10000``; the panel returns the
probability with a Hoeffding confidence band. Pin
``provsql.monte_carlo_seed = 42`` in the Config panel and re-run:
the result is now identical across runs. Toggle the seed back to
``-1`` and re-run to see the band shift between runs.

.. figure:: /_static/casestudy6/cmp-circuit-mc-eval.png
   :alt: Studio Circuit mode showing the gate_times wrapper above
         the iota input gate (left) and the > gate_cmp (right);
         the cmp's children are the N(28,2) gate_rv and the
         constant 35.

   The provenance of one row from ``WHERE pm25 > 35``: a
   ``gate_times`` (``⊗``) wraps the row's input token ``ι`` and a
   ``gate_cmp`` ``>`` whose children are the ``N(28, 2)`` leaf and
   the constant ``35``. The eval strip below switches to
   ``probability_evaluate`` and exposes the method picker.

Step 3: Calibration via Mixtures
---------------------------------

Each station has a probability of being mis-calibrated; a
mis-calibrated unit reports ``pm25 * 1.2`` instead of ``pm25``.
Express this as a Bernoulli mixture::

    SELECT r.id, r.station_id,
           provsql.mixture(cs.p, r.pm25, r.pm25 * 1.2) AS pm25_calibrated,
           r.provsql
    FROM readings r JOIN calibration_status cs USING (station_id)
    WHERE r.station_id = 's1';

Click into a result row's ``pm25_calibrated`` cell. Circuit mode
renders the ``gate_mixture`` with three labelled outgoing edges
(``p`` / ``x`` / ``y``) matching the SQL constructor's argument
order. The parent circle renders the Bernoulli probability
inline.

The same node-inspector panel exposes ``Distribution profile``
on the mixture root: the histogram becomes bimodal, with the two
modes corresponding to the in-spec and out-of-spec readings,
weighted by the calibration probability.

.. figure:: /_static/casestudy6/mixture-node.png
   :alt: Mix node with three labelled outgoing edges (p, x, y);
         the p child renders inline as 95% on the parent circle,
         and the x and y children are the N(28,2) and the
         scale-shifted reading respectively.

   The ``gate_mixture`` for the calibrated reading. The parent's
   inline ``95%`` is the Bernoulli probability that station ``s1``
   is in spec; the three outgoing edges label ``p`` / ``x`` / ``y``
   to match the SQL constructor argument order.

Step 4: Aggregation Over Random Variables
------------------------------------------

Compute average :math:`PM_{2.5}` per district::

    SELECT s.district,
           avg(r.pm25)  AS avg_pm25,
           sum(r.pm25)  AS total_pm25,
           provsql
    FROM readings r JOIN stations s ON s.id = r.station_id
    GROUP BY s.district;

Click into a row's ``avg_pm25`` cell. Circuit mode shows the
:sqlfunc:`avg` lowering: a ``gate_arith(DIV, num, denom)``
over two ``gate_arith(PLUS, …)`` subtrees, each child a per-row
``gate_mixture`` produced by ``rv_aggregate_semimod``.
The right child of the outer division is the count of *included*
rows under their per-row provenance: rows whose provenance is
false contribute the additive identity to both numerator and
denominator. Run *Distribution profile* on the root: the panel
shows the per-district average as a tight distribution centred at
the inclusion-weighted mean.

.. figure:: /_static/casestudy6/sum-of-mixtures.png
   :alt: DAG with a single root division node, two PLUS subtrees
         under it, eight Mix nodes at the next level, and the
         gate_rv leaves (N(28,2), N(40,4), U(12,24), U(10,22))
         plus iota input gates at the bottom.

   The ``avg(pm25)`` cell for the *centre* district lowers to
   ``gate_arith(DIV, gate_arith(PLUS, mixtures), gate_arith(PLUS,
   one-mixtures))``. The eight mixtures correspond to the four
   stations × two timestamps that fall in the district; the
   ``gate_rv`` leaves at the bottom are the per-reading
   distributions; the ``ι`` leaves anchor each row's provenance.

Step 5: The Simplifier in Action
---------------------------------

Toggle ``provsql.simplify_on_load`` off in the Config panel and
re-load the same ``avg_pm25`` cell. The canvas now shows the
*raw* gate-creation graph: comparators that are always satisfied
(e.g. a mixture branch whose calibration probability is ``1`` or
``0``), single-child arith roots, and ``gate_one`` /
``gate_zero`` semiring identities are all present unfolded.
Toggle the GUC back on; the canvas re-renders with the universal
peephole collapsed (Boolean comparators decidable from the
propagated support become ``gate_input`` leaves with probability
``0`` or ``1``; identity gates are dropped). The two views are
semantically identical; the simplified view is what the semiring
evaluators and Monte-Carlo sampler actually consume.

.. figure:: /_static/casestudy6/simplify-before-after.png
   :alt: Side-by-side composite: the raw circuit with
         provsql.simplify_on_load off on the left, and the
         simplified circuit on the right;
         the structure is broadly similar because aggregation queries
         leave the per-row mixture spine intact.

   The same ``avg(pm25)`` circuit with ``provsql.simplify_on_load``
   toggled off (left) vs on (right). For the per-district average,
   the simplifier's peephole pass leaves the per-row mixture spine
   intact because no mixture probability collapses to ``0`` or
   ``1``; the contrast is more dramatic on queries with degenerate
   mixtures or single-child arith roots (try
   ``provsql.mixture(1.0, X, Y)`` to see the collapse).

Step 6: Conditional Inference
------------------------------

Re-open the filtered query from Step 2::

    SELECT id, station_id, ts, pm25, provsql
    FROM readings
    WHERE pm25 > 35
      AND station_id = 's1';

Click a result row's ``pm25`` cell. The eval strip's
:guilabel:`Condition on` text input auto-presets to the row's
provenance UUID, and the :guilabel:`Conditioned by:` badge
underneath the input is active. Pick *Distribution profile* and
run: the histogram now shows the *truncated* shape, restricted to
the tail above ``35``. Pick *Moment* with ``k = 1`` and ``raw``:
the panel returns the closed-form Mills-ratio mean of the
truncated normal, exactly :math:`\mu + \sigma \cdot
\frac{\phi(\alpha)}{1 - \Phi(\alpha)}` with
:math:`\alpha = (35 - \mu)/\sigma`. Click the active badge to
clear the conditioning; the panel reverts to the unconditional
mean :math:`\mu`. Click the muted badge to restore the row
provenance.

The closed-form truncation table covers Normal (Mills ratio),
Uniform (intersected support), and Exponential (memorylessness on
a lower bound or finite-interval truncation). For other shapes,
the joint circuit between ``pm25`` and the row's provenance is
loaded with shared ``gate_rv`` leaves correctly coupled, and the
conditional moment is estimated by rejection sampling at budget
``provsql.rv_mc_samples``.

.. figure:: /_static/casestudy6/condition-on-active.png
   :alt: The Distribution profile panel showing supp [35, +infinity],
         mu approximately 40.82, sigma approximately 3.35; the
         Condition on input is populated with the row's provenance
         UUID and the Conditioned by badge is active.

   The conditional distribution profile for row 5 (``pm25 ∼
   N(40, 4)``) under the event ``pm25 > 35``. Studio auto-presets
   the *Condition on* input with the row's provenance UUID and
   activates the *Conditioned by* badge; the panel's header
   reflects the truncated support ``[35, +∞]`` and the
   Mills-ratio mean ``μ ≈ 40.82``, ``σ ≈ 3.35`` (closed form on
   the truncated normal).

Step 7: Diagnostic Sampling
----------------------------

For raw inspection or downstream analytics, draw samples from the
conditional distribution. With the same row pinned and the
*Conditioned by* badge active, pick *Sample* under the
*Distribution* optgroup; set ``n = 200`` and run. The result
renders as a ``<details>`` panel with a six-value inline preview
and a "show full list" expander; clicking it dumps all 200
samples.

If the conditioning event is so unlikely that fewer than 200
samples land within the ``provsql.rv_mc_samples`` budget, the
panel surfaces a hint pointing at the GUC, e.g. *Only 47 samples
accepted within budget 10000; widen* ``provsql.rv_mc_samples`` *or
loosen the conditioning*. Re-running with a larger budget (set
``rv_mc_samples = 50000`` in the Config panel) recovers the full
batch.

.. figure:: /_static/casestudy6/sample-panel-expanded.png
   :alt: The Sample evaluator panel expanded to a details element
         showing 1 sample out of 100 accepted, and a hint reading
         "MC accepted 1/100. Raise provsql.rv_mc_samples in the
         Config panel to widen the rejection-sampling budget."

   The *Sample* evaluator on row 1 (``pm25 ∼ N(28, 2)``) under
   ``pm25 > 35``: the conditioning event lies 3.5 σ above the
   mean, so only 1 sample is accepted out of the 100-iteration
   budget. The actionable hint points at the
   ``provsql.rv_mc_samples`` GUC.

Step 8: Combining Batches via UNION ALL
----------------------------------------

Combine today's readings with the historical batch::

    (SELECT pm25, provsql FROM readings)
    UNION ALL
    (SELECT pm25, provsql FROM historical_readings);

Pick a result row's ``provsql`` cell. Circuit mode shows the
expected ``gate_plus`` over the two branches' provenance: each
contributing row's provenance is preserved, and the alternative
between the two batches is encoded as the semiring addition.
``probability_evaluate(provenance())`` on the result gives the
probability that *either* batch produced an in-range reading for
that station.

Step 9: HAVING on Grouped Random Variables
-------------------------------------------

Filter the per-district aggregates from Step 4 by an expected
condition::

    SELECT s.district,
           avg(r.pm25)              AS avg_pm25,
           provsql
    FROM readings r JOIN stations s ON s.id = r.station_id
    GROUP BY s.district
    HAVING expected(avg(r.pm25)) > 20;

The :sqlfunc:`expected` dispatcher recognises the inner ``avg``
as a ``random_variable`` aggregate and runs the analytical
``rv_moment`` path; the ``HAVING`` lifts into a ``gate_cmp``
above a ``gate_agg``. Click a surviving row's ``provsql`` cell;
Circuit mode shows the cmp / agg layering. Because the
``gate_delta`` operator is transparent to the random-variable
event walker, the structural view matches the semantic one.
Under Monte Carlo, the ``gate_agg`` arm of
``monteCarloRV::evalScalar`` evaluates the per-group running
aggregate on each iteration, so ``HAVING`` and continuous
random variables compose without an extra workaround.

Step 10: Independent vs Monte Carlo
------------------------------------

For threshold queries whose contributing rows have structurally
independent provenance, the ``'independent'`` probability method
(see :doc:`probabilities`) is *exact* and far cheaper than Monte
Carlo. Compare the three available exact methods against
``monte-carlo`` on the Step 2 query:

.. code-block:: postgresql

    SELECT id,
      probability_evaluate(provenance(), 'independent')         AS p_ind,
      probability_evaluate(provenance(), 'monte-carlo', '10000') AS p_mc,
      probability_evaluate(provenance(), 'tree-decomposition')  AS p_td
    FROM readings WHERE pm25 > 35;

Studio's eval strip exposes these methods directly; running each
method against the same pinned subnode shows the analytic
``independent`` and ``tree-decomposition`` returning the same
value to full precision, while ``monte-carlo`` returns a
Hoeffding-bounded estimate that tightens as ``n`` grows.

Step 11: psql Fallback for Gate Introspection
----------------------------------------------

A handful of inspection functions do not have a dedicated Studio
control. Copy a ``gate_rv`` UUID from Studio's pinned-node toolbar
and inspect it in ``psql``:

.. code-block:: postgresql

    SELECT get_gate_type(:'uuid'::uuid)   AS gate_type,
           get_infos(:'uuid'::uuid)        AS infos,
           get_extra(:'uuid'::uuid)        AS extra,
           get_prob(:'uuid'::uuid)         AS prob;

For a ``gate_rv``, ``extra`` is the distribution-blob text
encoding (``"normal:2.5,0.5"``, ``"uniform:1,3"``,
``"exponential:0.4"``, ``"erlang:3,0.2"``); for a ``gate_arith``,
``infos.info1`` is the ``provsql_arith_op`` tag and the children
are the operand UUIDs. The bidirectional flow Studio → psql →
Studio (round-tripping a UUID back into the Studio query box)
makes the gate-level inspection available without ever leaving
the original session.

Discussion
----------

The case study made four things visible that no ``psql``-only
session could:

1. **The rewriter's effect.** A ``WHERE pm25 > 35`` in the query
   box turned into a ``gate_cmp`` in the canvas. The procedure
   bodies for the ``random_variable`` comparison operators are
   never executed; the planner hook is the entire story.
2. **The simplifier's effect.** Toggling
   ``provsql.simplify_on_load`` flipped between the raw and
   folded circuit views without re-running the query, so the
   difference between gate-creation structure and
   evaluation-time structure is observable directly.
3. **Conditional vs unconditional shape.** Toggling the
   *Conditioned by* badge swapped the inline histogram between
   the unconditional :math:`PM_{2.5}` distribution and the
   threshold-truncated one in a single click; the closed-form
   Mills-ratio mean from :sqlfunc:`moment` matches the
   visualisation.
4. **Mixture-of-mixtures shape.** The ``gate_mixture`` rendering
   for the calibration uncertainty, layered on top of the
   ``gate_arith`` for the calibration-scaled reading, made the
   per-row provenance tower legible without manually expanding it
   in the query.

Where ``psql`` remains the right tool: bulk inserts and the
gate-introspection family (``get_gate_type``, ``get_infos``,
``get_extra``, ``get_prob``); Studio's pinned-node toolbar
copies the UUID out so the round-trip is one paste.

See :doc:`continuous-distributions` for the full surface and
:doc:`studio` for the Studio reference.
