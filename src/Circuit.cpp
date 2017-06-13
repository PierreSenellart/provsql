#include "Circuit.h"

#include <cassert>
#include <string>

using namespace std;

bool Circuit::hasGate(const uuid &u) const
{
  return uuid2id.find(u)!=uuid2id.end();
}

unsigned Circuit::getGate(const uuid &u)
{
  auto it=uuid2id.find(u);
  if(it==uuid2id.end()) {
    unsigned id=addGate(UNDETERMINED);
    uuid2id[u]=id;
    return id;
  } else 
    return it->second;
}

unsigned Circuit::addGate(gateType type)
{
  unsigned id=gates.size();
  gates.push_back(type);
  prob.push_back(-1);
  wires.resize(id+1);
  return id;
}

void Circuit::setGate(const uuid &u, gateType type, double p)
{
  unsigned id = getGate(u);
  gates[id] = type;
  prob[id] = p;
}

void Circuit::addWire(unsigned f, unsigned t)
{
  wires[f].insert(t);
}

std::string Circuit::toString(unsigned g) const
{
  std::string op;
  string result;

  switch(gates[g]) {
    case IN:
      return to_string(g)+"["+to_string(prob[g])+"]";
    case NOT:
      op="¬";
      break;
    case UNDETERMINED:
      op="?";
      break;
    case AND:
      op="∧";
      break;
    case OR:
      op="∨";
      break;
  }

  for(auto s: wires[g]) {
    if(gates[g]==NOT)
      result = op;
    else if(!result.empty())
      result+=" "+op+" ";
    result+=toString(s);
  }

  return "("+result+")";
}
