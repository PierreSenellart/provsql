#include "MMappedCircuit.h"

extern "C" {
#include "provsql_mmap.h"
}

static MMappedCircuit *c;

void initialize_provsql_mmap()
{
  c = new MMappedCircuit();
}

void destroy_provsql_mmap()
{
  delete c;
}
