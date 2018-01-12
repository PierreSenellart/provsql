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

unsigned WhereCircuit::setGate(const uuid &u, WhereGate type)
{
  unsigned id = Circuit::setGate(u, type);
  if(type == WhereGate::IN)
    inputs.insert(id);
  return id;
}

unsigned WhereCircuit::setGateProjection(const uuid &u, vector<int> &&infos)
{
  unsigned id = setGate(u, WhereGate::PROJECT);
  projection_info[id]=infos;
  return id;
}
  
unsigned WhereCircuit::setGateEquality(const uuid &u, int pos1, int pos2)
{
  unsigned id = setGate(u, WhereGate::EQ);
  equality_info[id]=make_pair(pos1,pos2);
  return id;
}

std::string WhereCircuit::toString(unsigned g) const
{
  return "";
}
