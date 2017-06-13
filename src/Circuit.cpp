#include "Circuit.h"

extern "C" {
#include "provsql_utils.h"
}

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
  rwires.resize(id+1);
  if(type==IN)
    inputs.push_back(id);
  return id;
}

void Circuit::setGate(const uuid &u, gateType type, double p)
{
  unsigned id = getGate(u);
  gates[id] = type;
  prob[id] = p;
  if(type==IN)
    inputs.push_back(id);
}

void Circuit::addWire(unsigned f, unsigned t)
{
  wires[f].insert(t);
  rwires[t].insert(f);
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

bool Circuit::evaluate(unsigned g, const unordered_set<unsigned> &sampled) const
{
  bool disjunction=false;

  switch(gates[g]) {
    case IN:
      return sampled.find(g)!=sampled.end();
    case NOT:
      return !evaluate(*(wires[g].begin()), sampled);
    case AND:
      disjunction = false;
      break;
    case OR:
      disjunction = true;
      break;
    case UNDETERMINED:
      return false;
  }

  for(auto s: wires[g]) {
    bool e = evaluate(s, sampled);
    if(disjunction && e)
      return true;
    if(!disjunction && !e)
      return false;
  }

  if(disjunction)
    return false;
  else
    return true;
}

double Circuit::monteCarlo(unsigned g, unsigned samples) const
{
  unsigned success=0;

  for(unsigned i=0; i<samples; ++i) {
    unordered_set<unsigned> sampled;
    for(unsigned in : inputs) {
      if(rand() *1. / RAND_MAX < prob[in]) {
        sampled.insert(in);
      }
    }

    if(evaluate(g, sampled))
      ++success;
    
    if(provsql_interrupted)
      throw CircuitException("Interrupted");
  }

  return success*1./samples;
}

double Circuit::possibleWorlds(unsigned g) const
{ 
  if(inputs.size()>=8*sizeof(unsigned long long))
    throw CircuitException("Too many possible worlds to iterate over");

  unsigned long long nb=(1<<inputs.size());
  double totalp=0.;

  for(unsigned long long i=0; i < nb; ++i) {
    unordered_set<unsigned> s;
    double p = 1;

    for(unsigned j=0; j<inputs.size(); ++j) {
      if(i & (1 << j)) {
        s.insert(inputs[j]);
        p*=prob[inputs[j]];
      } else {
        p*=1-prob[inputs[j]];
      }
    }

    if(evaluate(g, s))
      totalp+=p;

    if(provsql_interrupted)
      throw CircuitException("Interrupted");
  }

  return totalp;
}
