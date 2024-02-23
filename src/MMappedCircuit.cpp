#include "MMappedCircuit.h"

extern "C" {
#include "provsql_mmap.h"
}

static MMappedCircuit *c;

MMappedCircuit::MMappedCircuit(bool read_only)
{
  if(read_only) {
    gates_file = new bip::managed_mapped_file(bip::open_read_only, GATES_FILENAME);
    wires_file = new bip::managed_mapped_file(bip::open_read_only, WIRES_FILENAME);
    mapping_file = new bip::managed_mapped_file(bip::open_read_only, MAPPING_FILENAME);
  } else {
    gates_file = new bip::managed_mapped_file(bip::open_or_create, GATES_FILENAME, INITIAL_FILE_SIZE);
    wires_file = new bip::managed_mapped_file(bip::open_or_create, WIRES_FILENAME, INITIAL_FILE_SIZE);
    mapping_file = new bip::managed_mapped_file(bip::open_or_create, MAPPING_FILENAME, INITIAL_FILE_SIZE);
  }
  mapping = mapping_file->find_or_construct<mmapped_unordered_map>("mapping")(mapping_file->get_segment_manager());
  // Get a raw pointer to access gates and wires
}

MMappedCircuit::~MMappedCircuit()
{
  delete mapping_file;
  delete gates_file;
  delete wires_file;
}

void initialize_provsql_mmap()
{
  c = new MMappedCircuit(false);
}

void destroy_provsql_mmap()
{
  delete c;
}
