#include "dDNNF.h"

#include <unordered_map>

double dDNNF::dDNNFEvaluation(unsigned g) const
{
  static std::unordered_map<unsigned, double> cache;

  auto it = cache.find(g);
  if(it!=cache.end())
    return it->second;

  double result;

  if(gates[g]==BooleanGate::IN)
    result = prob[g];
  else if(gates[g]==BooleanGate::NOT)
    result = 1-prob[*wires[g].begin()];
  else {
    result=(gates[g]==BooleanGate::AND?1:0);
    for(auto s: wires[g]) {
      double d = dDNNFEvaluation(s);
      if(gates[g]==BooleanGate::AND)
        result*=d;
      else
        result+=d;
    }
  }

  cache[g]=result;
  return result;
}

