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
explicit MMappedCircuit(bool read_only = false) :
  mapping(MAPPING_FILENAME, read_only),
  gates(GATES_FILENAME, read_only),
  wires(WIRES_FILENAME, read_only) {
}

/* Non-const public methods, to be used in MMap Worker */
void createGate(pg_uuid_t token, gate_type type, const std::vector<pg_uuid_t> &children);
void setInfos(pg_uuid_t token, unsigned info1, unsigned info2);
void setProb(pg_uuid_t token, double prob);

/* Const public methods, can be directly used in regular backends */
gate_type getGateType(pg_uuid_t token) const;
std::vector<pg_uuid_t> getChildren(pg_uuid_t token) const;
double getProb(pg_uuid_t token) const;
std::pair<unsigned, unsigned> getInfos(pg_uuid_t token) const;
inline unsigned getNbGates() const {
  return gates.nbElements();
}
};

#endif /* MMAPPED_CIRCUIT_H */
