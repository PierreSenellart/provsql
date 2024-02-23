#ifndef MMAPPED_CIRCUIT_H
#define MMAPPED_CIRCUIT_H

#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/unordered_map.hpp>

extern "C" {
#include "provsql_utils.h"
}

namespace bip=boost::interprocess;

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
bip::managed_mapped_file *gates_file;
bip::managed_mapped_file *wires_file;
bip::managed_mapped_file *mapping_file;

using mmapped_unordered_map =
  boost::unordered_map<
    pg_uuid_t,
    std::pair<unsigned, unsigned>,
    boost::hash<pg_uuid_t>,
    std::equal_to<pg_uuid_t>,
    bip::allocator<pg_uuid_t, bip::managed_mapped_file::segment_manager> >;

mmapped_unordered_map *mapping;
GateInformation *gates;
pg_uuid_t *wires;

static constexpr const char *GATES_FILENAME="gates.mmap";
static constexpr const char *WIRES_FILENAME="wires.mmap";
static constexpr const char *MAPPING_FILENAME="mapping.mmap";
static constexpr unsigned INITIAL_FILE_SIZE = 1<<16;

public:
explicit MMappedCircuit(bool read_only);
~MMappedCircuit();
};

#endif /* MMAPPED_CIRCUIT_H */
