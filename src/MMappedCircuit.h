#ifndef MMAPPED_CIRCUIT_H
#define MMAPPED_CIRCUIT_H

#include "MMappedUUIDHashTable.h"
#include "MMappedVector.hpp"

extern "C" {
#include "provsql_utils.h"
}

typedef struct GateInformation
{
  gate_type type;
  unsigned nb_children;
  unsigned children_idx;
  double prob;
  unsigned info1;
  unsigned info2;
} GateInformation;

class MMappedCircuit {
private:
MMappedUUIDHashTable mapping;
MMappedVector<GateInformation> gates;
MMappedVector<pg_uuid_t> wires;

static constexpr const char *GATES_FILENAME="provsql_gates.mmap";
static constexpr const char *WIRES_FILENAME="provsql_wires.mmap";
static constexpr const char *MAPPING_FILENAME="provsql_mapping.mmap";

public:
explicit MMappedCircuit() :
  mapping(MAPPING_FILENAME), gates(GATES_FILENAME), wires(WIRES_FILENAME) {
}
};

#endif /* MMAPPED_CIRCUIT_H */
