/**
 * @file provenance_evaluate_compiled.hpp
 * @brief Template helper for populating provenance mappings from SPI results.
 *
 * When evaluating a provenance circuit over a user-defined semiring, the
 * first step is to build a @c provenance_mapping that maps each input
 * gate to its semiring value.  The values come from a database table or
 * temporary view queried via SPI; this template handles the SPI-result
 * iteration that is common to all semiring types.
 *
 * After reading the SPI result set, the function optionally drops the
 * temporary helper table (if @p drop_table is @c true) and closes the
 * SPI connection.
 */
#ifndef PROVENANCE_EVALUATE_COMPILED_HPP
#define PROVENANCE_EVALUATE_COMPILED_HPP

extern "C" {
#include "executor/spi.h"
}

#include "provsql_utils_cpp.h"
#include "CircuitFromMMap.h"
#include "Circuit.hpp"

/** @brief DROP TABLE statement for the per-query temporary provenance mapping table. */
extern const char *drop_temp_table;

/**
 * @brief Populate a provenance mapping from the current SPI result set.
 *
 * Iterates over the rows returned by the most recent SPI query.  Each
 * row is expected to have two columns:
 * 1. A value column (column 1) containing the semiring value as text.
 * 2. A UUID column (column 2) identifying the circuit input gate.
 *
 * The @p charp_to_value converter is called on the text of column 1 to
 * produce the corresponding @c T value.
 *
 * @tparam T                 Semiring value type.
 * @param constants          Cached OID constants (used to verify column types).
 * @param c                  The @c GenericCircuit used to resolve gate IDs.
 * @param provenance_mapping Output map from gate IDs to semiring values;
 *                           entries are added by this function.
 * @param charp_to_value     Converter from the column-1 text to a @c T value.
 * @param drop_table         If @c true, execute the @c drop_temp_table SQL
 *                           statement after processing all rows.
 */
template<typename T>
void initialize_provenance_mapping(
  const constants_t &constants,
  GenericCircuit &c,
  std::unordered_map<gate_t, T> &provenance_mapping,
  const std::function<T(const char *)> &charp_to_value,
  bool drop_table
  )
{
  for (uint64 i = 0; i < SPI_processed; i++) {
    HeapTuple tuple = SPI_tuptable->vals[i];
    TupleDesc tupdesc = SPI_tuptable->tupdesc;

    if (SPI_gettypeid(tupdesc, 2) != constants.OID_TYPE_UUID) {
      SPI_finish();
      throw CircuitException("Invalid type for provenance mapping attribute");
      continue;
    }

    char *value = SPI_getvalue(tuple, tupdesc, 1);
    char *uuid = SPI_getvalue(tuple, tupdesc, 2);

    provenance_mapping[c.getGate(uuid)]=charp_to_value(value);

    pfree(value);
    pfree(uuid);
  }
  if(drop_table)
    SPI_exec(drop_temp_table, 0);
  SPI_finish();
}

#endif /* PROVENANCE_EVALUATE_COMPILED_HPP */
