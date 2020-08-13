#include "dDNNF.h"

#include <unordered_map>

double dDNNF::dDNNFEvaluation(gate_t g) const
{
  static std::unordered_map<gate_t, double> cache;

  auto it = cache.find(g);
  if(it!=cache.end())
    return it->second;

  double result;

  if(getGateType(g)==BooleanGate::IN)
    result = getProb(g);
  else if(getGateType(g)==BooleanGate::NOT)
    result = 1-getProb(*getWires(g).begin());
  else {
    result=(getGateType(g)==BooleanGate::AND?1:0);
    for(auto s: getWires(g)) {
      double d = dDNNFEvaluation(s);
      if(getGateType(g)==BooleanGate::AND)
        result*=d;
      else
        result+=d;
    }
  }

  cache[g]=result;
  return result;
}
