-- Pristine, untracked source tables; bench.py copies them into the tracked
-- r, s, d before every cold run so that all input tokens are fresh.
\set N 100000
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";
CREATE EXTENSION IF NOT EXISTS provsql;
SELECT setseed(0.42);
CREATE TABLE r0 AS
  SELECT i AS id, i % (:N / 10) AS a, (i * 7919) % 100 AS b,
         (random() * 1000)::int AS v,
         CASE WHEN random() < 0.1 THEN NULL ELSE 'x' || (i % 50) END AS t
  FROM generate_series(0, :N - 1) i;
CREATE TABLE s0 AS
  SELECT i AS id, (i * 31) % (:N / 10) AS a, (random() * 1000)::int AS w
  FROM generate_series(0, :N - 1) i;
CREATE TABLE d0 AS
  SELECT i AS a, 'label' || (i % 1000) AS label
  FROM generate_series(0, :N / 10 - 1) i;
CREATE TABLE d_plain AS SELECT * FROM d0;
CREATE INDEX ON d_plain(a);
-- layered DAG: 40 layers of 50 vertices, 3 out-edges per vertex
CREATE TABLE edge0 AS
  SELECT DISTINCT src, dst FROM (
    SELECT l * 50 + k AS src, (l + 1) * 50 + (k * 7 + j * 13) % 50 AS dst
    FROM generate_series(0, 38) l, generate_series(0, 49) k, generate_series(0, 2) j
  ) x;
INSERT INTO edge0 SELECT 0, k FROM generate_series(1, 49) k;
ANALYZE;
