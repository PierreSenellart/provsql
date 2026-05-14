\set ECHO none
\pset format unaligned
SET search_path TO provsql_test,provsql;

-- All MC bins below are deterministic via a fixed seed and sample count.
SET provsql.monte_carlo_seed = 42;
SET provsql.rv_mc_samples = 10000;

-- gate_value (Dirac): single bin at the constant.
WITH h AS (
  SELECT provsql.rv_histogram(
           (provsql.as_random(7.5))::uuid,
           10) AS j
)
SELECT jsonb_array_length(j) AS nbins,
       (j->0->>'bin_lo')::float8 = 7.5 AS lo_ok,
       (j->0->>'bin_hi')::float8 = 7.5 AS hi_ok,
       (j->0->>'count')::int = 10000 AS count_ok
  FROM h;

-- gate_rv: total count = sample budget, every bin in support.
WITH h AS (
  SELECT provsql.rv_histogram(
           (provsql.uniform(0, 1))::uuid,
           20) AS j
)
SELECT jsonb_array_length(j) AS nbins,
       (SELECT sum((b->>'count')::int) FROM jsonb_array_elements(j) b)
         = 10000 AS total_ok,
       (SELECT min((b->>'bin_lo')::float8) FROM jsonb_array_elements(j) b)
         >= 0 AS lo_in_support,
       (SELECT max((b->>'bin_hi')::float8) FROM jsonb_array_elements(j) b)
         <= 1 AS hi_in_support
  FROM h;

-- gate_arith: hand-build X + Y where X, Y ~ N(0, 1).  Sample distribution
-- is N(0, 2); the histogram total must match the sample budget and the
-- bin count must respect the user-supplied bins argument.
DO $$
DECLARE
  x uuid := (provsql.normal(0, 1))::uuid;
  y uuid := (provsql.normal(0, 1))::uuid;
  sum_tok uuid := public.uuid_generate_v4();
  hist jsonb;
  total int;
BEGIN
  PERFORM provsql.create_gate(sum_tok, 'arith', ARRAY[x, y]);
  PERFORM provsql.set_infos(sum_tok, 0);  -- PROVSQL_ARITH_PLUS = 0
  hist := provsql.rv_histogram(sum_tok, 30);
  SELECT sum((b->>'count')::int) INTO total
    FROM jsonb_array_elements(hist) b;
  IF total <> 10000 THEN
    RAISE EXCEPTION 'rv_histogram arith total=%, expected 10000', total;
  END IF;
  IF jsonb_array_length(hist) > 30 THEN
    RAISE EXCEPTION 'rv_histogram arith returned % bins, expected <= 30',
                    jsonb_array_length(hist);
  END IF;
END
$$;

-- Determinism: same seed + same sample count -> identical histogram.
SELECT provsql.rv_histogram(
         (provsql.normal(0, 1))::uuid, 15)
     = provsql.rv_histogram(
         (provsql.normal(0, 1))::uuid, 15)
       AS deterministic;

-- bins parameter is honoured: bins=5 returns at most 5 bins.
SELECT jsonb_array_length(
         provsql.rv_histogram(
           (provsql.uniform(0, 1))::uuid, 5)) <= 5
       AS bins_param_respected;

-- Default bins (no argument) is 30.
SELECT jsonb_array_length(
         provsql.rv_histogram(
           (provsql.uniform(0, 1))::uuid)) <= 30
       AS default_bins_ok;

-- Non-scalar root errors.  Use a gate_input UUID via add_provenance.
CREATE TABLE rv_hist_t(id text);
INSERT INTO rv_hist_t VALUES ('a');
SELECT add_provenance('rv_hist_t');
DO $$
DECLARE
  input_tok uuid := (SELECT provsql FROM rv_hist_t WHERE id = 'a');
BEGIN
  BEGIN
    PERFORM provsql.rv_histogram(input_tok, 10);
    RAISE EXCEPTION 'expected rv_histogram to reject non-scalar root';
  EXCEPTION WHEN OTHERS THEN
    IF SQLERRM NOT LIKE '%not a scalar%' THEN
      RAISE EXCEPTION 'unexpected error: %', SQLERRM;
    END IF;
  END;
END
$$;
SELECT remove_provenance('rv_hist_t');
DROP TABLE rv_hist_t;

-- bins <= 0 errors.
DO $$
BEGIN
  BEGIN
    PERFORM provsql.rv_histogram(
      (provsql.normal(0, 1))::uuid, 0);
    RAISE EXCEPTION 'expected rv_histogram to reject bins = 0';
  EXCEPTION WHEN OTHERS THEN
    IF SQLERRM NOT LIKE '%bins must be positive%' THEN
      RAISE EXCEPTION 'unexpected error: %', SQLERRM;
    END IF;
  END;
END
$$;

-- rv_mc_samples = 0 errors for gate_rv (no MC fallback available).
SET provsql.rv_mc_samples = 0;
DO $$
BEGIN
  BEGIN
    PERFORM provsql.rv_histogram(
      (provsql.normal(0, 1))::uuid, 10);
    RAISE EXCEPTION 'expected rv_histogram to reject rv_mc_samples = 0';
  EXCEPTION WHEN OTHERS THEN
    IF SQLERRM NOT LIKE '%rv_mc_samples%' THEN
      RAISE EXCEPTION 'unexpected error: %', SQLERRM;
    END IF;
  END;
END
$$;
RESET provsql.rv_mc_samples;

SELECT 'ok' AS continuous_rv_histogram_done;
