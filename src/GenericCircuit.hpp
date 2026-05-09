/**
 * @file GenericCircuit.hpp
 * @brief Template implementation of @c GenericCircuit::evaluate().
 *
 * Provides the out-of-line definition of the @c evaluate() template method
 * declared in @c GenericCircuit.h.  This file must be included (directly
 * or transitively) by any translation unit that instantiates
 * @c GenericCircuit::evaluate<S>() for a specific semiring type @c S.
 *
 * The @c evaluate() method performs a post-order traversal of the sub-circuit
 * rooted at gate @p g, looking up input-gate values from @p provenance_mapping
 * and combining them using the semiring operations:
 *
 * | Gate type   | Semiring operation             |
 * |-------------|-------------------------------|
 * | gate_input  | lookup in @p provenance_mapping|
 * | gate_plus   | @c semiring.plus(children)     |
 * | gate_times  | @c semiring.times(children)    |
 * | gate_monus  | @c semiring.monus(left, right) |
 * | gate_delta  | @c semiring.delta(child)       |
 * | gate_cmp    | @c semiring.cmp(left, op, right)|
 * | gate_semimod| @c semiring.semimod(x, s)      |
 * | gate_agg    | @c semiring.agg(op, children)  |
 * | gate_value  | @c semiring.value(string)      |
 * | gate_one    | @c semiring.one()              |
 * | gate_zero   | @c semiring.zero()             |
 */
#include "GenericCircuit.h"

extern "C" {
#include "utils/lsyscache.h"
}

template<typename S, std::enable_if_t<std::is_base_of_v<semiring::Semiring<typename S::value_type>, S>, int> >
typename S::value_type GenericCircuit::evaluate(gate_t g, std::unordered_map<gate_t, typename S::value_type> &provenance_mapping, S semiring) const
{
  const auto it = provenance_mapping.find(g);
  if(it != provenance_mapping.end())
    return it->second;

  auto t = getGateType(g);

  switch(t) {
  case gate_one:
  case gate_input:
  case gate_update:
  case gate_mulinput:
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
        return evaluate<S>(u, provenance_mapping, semiring);
      });
    if(t==gate_plus) {
      childrenResult.erase(std::remove(std::begin(childrenResult), std::end(childrenResult), semiring.zero()),
                           childrenResult.end());
      return semiring.plus(childrenResult);
    } else if(t==gate_times) {
      for(const auto &c: childrenResult) {
        if(c==semiring.zero())
          return semiring.zero();
      }
      childrenResult.erase(std::remove(std::begin(childrenResult), std::end(childrenResult), semiring.one()),
                           childrenResult.end());
      return semiring.times(childrenResult);
    } else {
      if(childrenResult[0]==semiring.zero() || childrenResult[0]==childrenResult[1])
        return semiring.zero();
      else
        return semiring.monus(childrenResult[0], childrenResult[1]);
    }
  }

  case gate_delta:
    return semiring.delta(evaluate<S>(getWires(g)[0], provenance_mapping, semiring));

  case gate_project:
  case gate_eq:
    // Where-provenance gates, ignored
    return evaluate<S>(getWires(g)[0], provenance_mapping, semiring);

  case gate_cmp:
  {
    bool ok;
    ComparisonOperator op = cmpOpFromOid(getInfos(g).first, ok);
    if(!ok)
      throw CircuitException(
              "Comparison operator OID " +
              std::to_string(getInfos(g).first) +
              " not supported");

    return semiring.cmp(evaluate<S>(getWires(g)[0], provenance_mapping, semiring), op, evaluate<S>(getWires(g)[1], provenance_mapping, semiring));
  }

  case gate_semimod:
    return semiring.semimod(evaluate<S>(getWires(g)[0], provenance_mapping, semiring), evaluate<S>(getWires(g)[1], provenance_mapping, semiring));

  case gate_agg:
  {
    auto infos = getInfos(g);

    AggregationOperator op = getAggregationOperator(infos.first);

    std::vector<typename S::value_type> vec;
    for(auto it = getWires(g).begin(); it!=getWires(g).end(); ++it)
      vec.push_back(evaluate<S>(*it, provenance_mapping, semiring));
    return semiring.agg(op, vec);
    break;
  }

  case gate_value:
    return semiring.value(getExtra(g));

  default:
    throw CircuitException("Invalid gate type for semiring evaluation");
  }
}
