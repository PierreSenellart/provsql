#include "GenericCircuit.h"

template<typename S, std::enable_if_t<std::is_base_of_v<semiring::Semiring<typename S::value_type>, S>, int> >
typename S::value_type GenericCircuit::evaluate(gate_t g, std::unordered_map<gate_t, typename S::value_type> &provenance_mapping) const
{
  S semiring;

  const auto it = provenance_mapping.find(g);
  if(it != provenance_mapping.end())
    return it->second;

  auto t = getGateType(g);

  switch(t) {
  case gate_one:
  case gate_input:
  case gate_update:
    // If not in provenance mapping, return no provenance (one of the semiring)
    return semiring.one();

  case gate_zero:
    return semiring.zero();

  case gate_plus:
  case gate_times:
  case gate_monus: {
    auto children = getWires(g);
    std::vector<typename S::value_type> childrenResult;
    std::transform(children.begin(), children.end(), std::back_inserter(childrenResult), [&](auto u) {
        return evaluate<S>(u, provenance_mapping);
      });
    if(t==gate_plus)
      return semiring.plus(childrenResult);
    else if(t==gate_times)
      return semiring.times(childrenResult);
    else /* gate_monus */
      return semiring.monus(childrenResult[0], childrenResult[1]);
  }

  case gate_delta:
    return semiring.delta(evaluate<S>(getWires(g)[0], provenance_mapping));

  case gate_project:
  case gate_eq:
    // Where-provenance gates, ignored
    return evaluate<S>(getWires(g)[0], provenance_mapping);

  default:
    throw CircuitException("Invalid gate type for semiring evaluation");
  }
}
