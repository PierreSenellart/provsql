#include "GenericCircuit.h"

extern "C" {
#include "utils/lsyscache.h"
}

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
    return semiring.delta(evaluate<S>(getWires(g)[0], provenance_mapping));

  case gate_project:
  case gate_eq:
    // Where-provenance gates, ignored
    return evaluate<S>(getWires(g)[0], provenance_mapping);

  case gate_cmp:
  {
    auto infos = getInfos(g);
    char * opname = get_opname(infos.first);
    if(opname == nullptr)
      elog(ERROR, "Invalid OID for operator: %d", infos.first);

    std::string func_name {opname};

    ComparisonOperator op;

    if(func_name == "=") {
      op = ComparisonOperator::EQUAL;
    } else if(func_name == "<=") {
      op = ComparisonOperator::LE;
    } else if(func_name == "<") {
      op = ComparisonOperator::LT;
    } else if(func_name == ">") {
      op = ComparisonOperator::GT;
    } else if(func_name == ">=") {
      op = ComparisonOperator::GE;
    } else if(func_name == "<>") {
      op = ComparisonOperator::NE;
    } else {
      throw CircuitException("Comparison operator " + func_name + " not supported");
    }

    return semiring.cmp(evaluate<S>(getWires(g)[0], provenance_mapping), op, evaluate<S>(getWires(g)[1], provenance_mapping));
  }

  case gate_semimod:
    return semiring.semimod(evaluate<S>(getWires(g)[0], provenance_mapping), evaluate<S>(getWires(g)[1], provenance_mapping));

  case gate_agg:
  {
    auto infos = getInfos(g);
    AggregationOperator op;

    char *fname = get_func_name(infos.first);
    if(fname == nullptr)
      elog(ERROR, "Invalid OID for aggregation function: %d", infos.first);
    std::string func_name {fname};

    if(func_name == "sum") {
      op = AggregationOperator::SUM;
    } else if(func_name == "min") {
      op = AggregationOperator::MIN;
    } else if(func_name == "max") {
      op = AggregationOperator::MAX;
    } else if(func_name == "choose") {
      op = AggregationOperator::CHOOSE;
    } else {
      throw CircuitException("Aggregation operator " + func_name + " not supported");
    }

    std::vector<typename S::value_type> vec;
    for(auto it = getWires(g).begin(); it!=getWires(g).end(); ++it)
      vec.push_back(evaluate<S>(*it, provenance_mapping));
    return semiring.agg(op, vec);
    break;
  }

  case gate_value:
    return semiring.value(getExtra(g));

  default:
    throw CircuitException("Invalid gate type for semiring evaluation");
  }
}
