/**
 * @file
 * @brief ProvSQL upgrade script: 1.4.0 → 1.5.0
 *
 * 1.5.0 introduces continuous-distribution provenance and adds three
 * new gate types (`rv`, `arith`, `mixture`) to the `provenance_gate`
 * enum.  The mmap on-disk layout for existing 1.4.0 circuits is
 * unchanged — only the SQL enum grows.
 *
 * Continuous-distribution surface (gate types, sampler, range-check,
 * analytic CDF, hybrid evaluator, rv_support, rv_moment, rv_histogram,
 * and the `provsql.normal` / `uniform` / `exponential` / `erlang` /
 * `mixture` constructors) is created by the install script
 * `sql/provsql--1.5.0-dev.sql` — this upgrade script just bridges the
 * enum gap so `ALTER EXTENSION provsql UPDATE` from a 1.4.0 install
 * lands the user on a working 1.5.0 surface.
 *
 * @warning ABI: the new gate types are appended at the end of the C
 * `gate_type` enum (`src/provsql_utils.h`), preserving every existing
 * integer-to-name mapping.  Existing 1.4.0 mmap stores remain valid.
 */

SET search_path TO provsql;

-- New gate-type enum values, in the same order as the C enum.
ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'rv'      AFTER 'update';
ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'arith'   AFTER 'rv';
ALTER TYPE provenance_gate ADD VALUE IF NOT EXISTS 'mixture' AFTER 'arith';
