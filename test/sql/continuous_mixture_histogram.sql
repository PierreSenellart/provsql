\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- Bimodal Gaussian mixture: two well-separated narrow normals.
-- rv_histogram samples rv_mc_samples draws from each branch with
-- weights π and (1-π), so for π = 0.5 the bimodal density should be
-- visible as two roughly equal mass regions at the extremes of the
-- support and an empty middle band.
SET provsql.monte_carlo_seed = 7;
SET provsql.rv_mc_samples    = 20000;

CREATE TEMP TABLE p(t uuid);
INSERT INTO p VALUES (public.uuid_generate_v4());
SELECT set_prob((SELECT t FROM p), 0.5);

CREATE TEMP TABLE bimix AS
  SELECT random_variable_uuid(
           provsql.mixture(
             (SELECT t FROM p),
             provsql.normal(-5, 0.5),
             provsql.normal( 5, 0.5))) AS u;

CREATE TEMP TABLE bimix_hist AS
  SELECT provsql.rv_histogram((SELECT u FROM bimix), 40) AS h;

-- Bin count is 40 as requested.
SELECT jsonb_array_length((SELECT h FROM bimix_hist)) AS nb_bins;

-- The mixture should not have collapsed into a single normal: the
-- empirical support spans at least 8 units (each peak's centre is
-- 5 units away from 0, and a normal's 99% mass is ±2.5σ).  Take
-- bin_hi[last] - bin_lo[first] as the empirical range.
SELECT (
  SELECT (h->-1->>'bin_hi')::float8 - (h->0->>'bin_lo')::float8
    FROM bimix_hist
) > 8.0 AS range_spans_both_peaks;

-- The two modes are around ±5, and the band [-2, 2] should be near
-- empty.  Sum the counts of bins whose midpoint falls in [-2, 2]
-- and check it is small relative to the total mass.
SELECT (
  WITH bins AS (
    SELECT jsonb_array_elements(h) AS b FROM bimix_hist
  ), m AS (
    SELECT
      ( (b->>'bin_lo')::float8 + (b->>'bin_hi')::float8 ) / 2.0 AS mid,
      (b->>'count')::int AS cnt
    FROM bins
  )
  SELECT COALESCE(SUM(cnt) FILTER (WHERE mid BETWEEN -2 AND 2), 0)::float8
       / NULLIF(SUM(cnt), 0)::float8 FROM m
) < 0.02 AS middle_band_near_empty;

-- Left half (mid < 0) and right half (mid >= 0) carry roughly equal
-- mass (within 5%) under π = 0.5.
SELECT (
  WITH bins AS (
    SELECT jsonb_array_elements(h) AS b FROM bimix_hist
  ), m AS (
    SELECT
      ( (b->>'bin_lo')::float8 + (b->>'bin_hi')::float8 ) / 2.0 AS mid,
      (b->>'count')::int AS cnt
    FROM bins
  )
  SELECT abs(
      ( COALESCE(SUM(cnt) FILTER (WHERE mid < 0), 0)::float8
      - COALESCE(SUM(cnt) FILTER (WHERE mid >= 0), 0)::float8 )
    / NULLIF(SUM(cnt), 0)::float8 )
   FROM m
) < 0.05 AS halves_roughly_equal;

-- Structural invariants: bins are contiguous (bin_hi[i] = bin_lo[i+1])
-- and the per-bin counts sum to rv_mc_samples.
SELECT (
  WITH ordered AS (
    SELECT ordinality, b FROM bimix_hist,
      jsonb_array_elements(h) WITH ORDINALITY t(b, ordinality)
  ), adj AS (
    SELECT a.ordinality                AS i,
           (a.b->>'bin_hi')::float8     AS hi,
           (n.b->>'bin_lo')::float8     AS next_lo
      FROM ordered a
      JOIN ordered n ON n.ordinality = a.ordinality + 1
  )
  SELECT NOT EXISTS (SELECT 1 FROM adj WHERE abs(hi - next_lo) > 1e-9)
) AS bins_contiguous;

SELECT (
  WITH bins AS (
    SELECT (b->>'count')::int AS cnt
      FROM bimix_hist, jsonb_array_elements(h) AS b
  )
  SELECT SUM(cnt) FROM bins
) = 20000 AS total_counts_match;

RESET provsql.monte_carlo_seed;
RESET provsql.rv_mc_samples;
