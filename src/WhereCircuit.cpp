#include "WhereCircuit.h"

extern "C" {
#include "provsql_utils.h"
#include <unistd.h>
}

#include <cassert>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>

using namespace std;

// Has to be redefined because of name hiding
unsigned WhereCircuit::setGate(const uuid &u, WhereGate type)
{
  unsigned id = Circuit::setGate(u, type);
  if(type == WhereGate::IN)
    inputs.insert(id);
  return id;
}

unsigned WhereCircuit::setGate(const uuid &u, WhereGate type, int info1, int info2)
{
  unsigned id = setGate(u, type);
  return id;
}

unsigned WhereCircuit::addGate()
{
  unsigned id=Circuit::addGate();
  return id;
}
