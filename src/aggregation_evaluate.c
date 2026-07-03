/**
 * @file aggregation_evaluate.c
 * @brief Compatibility stub for the removed @c provsql.aggregation_evaluate().
 *
 * The aggregation-over-GROUP-BY custom-semiring dispatcher is gone from the
 * SQL surface (the agg_token aggregates and the compiled sr_* readouts cover
 * it), but the C symbol must stay: the frozen 1.0.0 fixture declares the
 * function and PostgreSQL validates the library symbol when CREATE EXTENSION
 * provsql VERSION '1.0.0' runs (upgrade-path tests), even though later
 * upgrade scripts drop the function.  The historical implementation
 * dispatched over SPI to a same-named plpgsql driver that the current
 * surface does not define, so a call could not succeed anyway; the stub
 * returns NULL unconditionally.
 */
#include "postgres.h"
#include "fmgr.h"

PG_FUNCTION_INFO_V1(aggregation_evaluate);

/** @brief Always-NULL stub kept for old-version installs. */
Datum aggregation_evaluate(PG_FUNCTION_ARGS)
{
  (void) fcinfo;
  PG_RETURN_NULL();
}
