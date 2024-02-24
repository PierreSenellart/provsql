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

  GateInformation(gate_type t, unsigned n, unsigned i) :
    type(t), nb_children(n), children_idx(i), prob(-1.), info1(0), info2(0) {
  }
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

void createGate(pg_uuid_t token, gate_type type, const std::vector<pg_uuid_t> &children);
gate_type getGateType(pg_uuid_t token) const;
std::vector<pg_uuid_t> getChildren(pg_uuid_t token) const;
void setProb(pg_uuid_t token, double prob);
double getProb(pg_uuid_t token) const;
void setInfos(pg_uuid_t token, unsigned info1, unsigned info2);
std::pair<unsigned, unsigned> getInfos(pg_uuid_t token) const;
inline unsigned getNbGates() const {
  return gates.nbElements();
}
};

#endif /* MMAPPED_CIRCUIT_H */
