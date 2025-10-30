#ifndef PROVENANCE_EVALUATE_COMPILED_HPP
#define PROVENANCE_EVALUATE_COMPILED_HPP

extern "C" {
#include "executor/spi.h"
}

#include "provsql_utils_cpp.h"
#include "CircuitFromMMap.h"
#include "Circuit.hpp"
#include "GenericCircuit.hpp"

extern const char *drop_temp_table;

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
