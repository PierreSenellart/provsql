#include <algorithm>
#include <stack>
#include <variant>

#include "dDNNFTreeDecompositionBuilder.h"

/* Turn a bounded-treewidth circuit c for which a tree decomposition td
 * is provided into a dNNF rooted at root, following the construction in
 * Section 5.1 of https://arxiv.org/pdf/1811.02944 */
dDNNF&& dDNNFTreeDecompositionBuilder::build() && {
  // We make the tree decomposition friendly
  td.makeFriendly(root_id);

  // We look for bags responsible for each variable
  for(bag_t i{0}; i<td.bags.size(); ++i) {
    const auto &b = td.getBag(i);
    if(td.getChildren(i).empty() && b.size()==1 && c.getGateType(*b.begin()) == BooleanGate::IN)
      responsible_bag[*b.begin()] = i;
  }

  // A friendly tree decomposition has leaf bags for every variable
  // nodes. Let's just check that to be safe.
  assert(responsible_bag.size()==c.inputs.size());

  // Create the input and negated input gates
  for(auto g: c.inputs) {
    auto gate = d.setGate(BooleanGate::IN, c.getProb(g));
    auto not_gate = d.setGate(BooleanGate::NOT);
    d.addWire(not_gate, gate);
    input_gate[g]=gate;
    negated_input_gate[g]=not_gate;
  }

  gate_vector_t<dDNNFGate> result_gates = builddDNNF();

  auto result_id = d.setGate("root", BooleanGate::OR);

  for(const auto &p: result_gates) {
    if(p.suspicious.empty() && p.valuation.find(root_id)->second) {
      d.addWire(result_id, p.id);
      break;
    }
  }

  return std::move(d);
}

constexpr bool isStrong(BooleanGate type, bool value)
{
  switch(type) {
    case BooleanGate::OR:
      return value;
    case BooleanGate::AND:
      return !value;
    case BooleanGate::IN:
      return false;
    default:
      return true;
  }
}

static bool isConnectible(const dDNNFTreeDecompositionBuilder::suspicious_t &suspicious,
                          const TreeDecomposition::Bag &b)
{
  for(const auto &g: suspicious) {
    if(b.find(g)==b.end())
      return false;
  }

  return true;
}

dDNNFTreeDecompositionBuilder::gate_vector_t<dDNNFTreeDecompositionBuilder::dDNNFGate> dDNNFTreeDecompositionBuilder::builddDNNFLeaf(
    bag_t bag)
{
  // If the bag is empty, it behaves as if it was not there
  if(td.getBag(bag).size()==0) 
    return {};

  // Otherwise, since we have a friendly decomposition, we have a
  // single gate
  auto single_gate = *td.getBag(bag).begin();

  // We check if this bag is responsible for an input variable
  if(c.getGateType(single_gate)==BooleanGate::IN &&
      responsible_bag.find(single_gate)->second==bag)
  {
    // No need to create an extra gate, just point to the variable and
    // negated variable gate; no suspicious gate.
    dDNNFGate pos = { input_gate.find(single_gate)->second,
      {std::make_pair(single_gate,true)},
      suspicious_t{}
    };
    dDNNFGate neg = { negated_input_gate.find(single_gate)->second,
      {std::make_pair(single_gate,false)}, 
      suspicious_t{} 
    };
    return { std::move(pos), std::move(neg) };
  } else {
    gate_vector_t<dDNNFGate> result_gates;

    // We create two TRUE gates (AND gates with no inputs)
    for(auto v: {true, false}) {
      // Optimization: we know the root is set to True, so no need to
      // construct valuations incompatible with this
      if(single_gate==root_id && !v)
        continue;

      suspicious_t suspicious;
      if(isStrong(c.getGateType(single_gate), v))
        suspicious.insert(single_gate);

      result_gates.emplace_back(
          d.setGate(BooleanGate::AND),
          valuation_t{std::make_pair(single_gate, v)},
          std::move(suspicious)
      );
    }

    return result_gates;
  }
}

bool dDNNFTreeDecompositionBuilder::isAlmostValuation(
    const valuation_t &valuation) const
{
  for(const auto &p1: valuation) {
    for(const auto &p2: valuation) {
      if(p1.first==p2.first)
        continue;
      if(!isStrong(c.getGateType(p1.first),p2.second))
        continue;

      if(circuitHasWire(p1.first,p2.first)) {
        switch(c.getGateType(p1.first)) {
          case BooleanGate::AND:
          case BooleanGate::OR:
            if(p1.second!=p2.second)
              return false;
            break;
          case BooleanGate::NOT:
            if(p1.second==p2.second)
              return false;
          default:
            ;
        }
      }
    }
  }

  return true;
}

dDNNFTreeDecompositionBuilder::suspicious_t
dDNNFTreeDecompositionBuilder::getInnocent(
    const valuation_t &valuation,
    const suspicious_t &innocent) const
{
  suspicious_t result = innocent;

  for(const auto &[g1,val]: valuation) {
    if(innocent.find(g1)!=innocent.end())
      continue;

    // We check if it is strong, if not it is innocent
    if(!isStrong(c.getGateType(g1), valuation.find(g1)->second)) {
      result.insert(g1);
      continue;
    }

    // We have a strong gate not innocented by the children bags,
    // it is only innocent if we also have in the bag an input to
    // that gate which is strong for that gate
    for(const auto &[g2, value]: valuation) {
      if(g2==g1)
        continue;

      if(circuitHasWire(g1,g2)) {
        if(isStrong(c.getGateType(g1), value)) {
          result.insert(g1);
          break;
        }
      }
    }
  }

  return result;
}

std::ostream &operator<<(std::ostream &o, const dDNNFTreeDecompositionBuilder::gates_to_or_t &gates_to_or)
{
  for(auto &[valuation, m]: gates_to_or) {
    o << "{";
    bool first=true;
    for(auto &[var, val]: valuation) {
      if(!first)
        o << ",";
      o << "(" << var << "," << val << ")";
      first=false;
    }
    o << "}: ";

    for(auto &[innocent, gates]: m) {
      o << "{";
      bool first=true;
      for(auto &x: innocent) {
        if(!first)
          o << ",";
        o << x;
        first=false;
      }
      o << "} ";
      o << "[";
      first=true;
      for(auto &x: gates) {
        if(!first)
          o << ",";
        o << x;
        first=false;
      }
      o << "] ";
    }
    
    o << "\n";
  }

  return o;
}

dDNNFTreeDecompositionBuilder::gates_to_or_t dDNNFTreeDecompositionBuilder::collectGatesToOr(
    bag_t bag,
    const gate_vector_t<dDNNFGate> &children_gates,
    const gates_to_or_t &partial)
{
  gates_to_or_t gates_to_or;

  for(auto g: children_gates) {
    // We check all suspicious gates are in the bag of the parent
    if(!isConnectible(g.suspicious,td.getBag(bag)))
      continue;

    // Find all valuations in partial that are compatible with this partial
    // valuation, if it exists
    auto compatibleValuation = [&g](const auto &p) {
                                 for(const auto &[var, val]: p.first) {
                                   auto it = g.valuation.find(var);
                                   if(it != g.valuation.end() && it->second != val)
                                     return false;
                                 }
                                 return true;
                               };

    for (auto it = std::find_if(partial.begin(), partial.end(), compatibleValuation);
         it != partial.end();
         it = std::find_if(std::next(it), partial.end(), compatibleValuation)) {
      auto &[matching_valuation, m] = *it;

      valuation_t valuation = matching_valuation;
      suspicious_t extra_innocent{};
      for(auto &[var, val]: g.valuation) {
         if(td.getBag(bag).find(var)!=td.getBag(bag).end()) {
           if(matching_valuation.find(var)==matching_valuation.end())
             valuation[var]=val;

           if(g.suspicious.find(var)==g.suspicious.end()) {
             extra_innocent.insert(var);
          }
        }
      }
          
      // We check valuation is still an almost-valuation
      if(!isAlmostValuation(valuation))
        continue;

      for(auto &[innocent, gates]: m) {
        suspicious_t new_innocent = extra_innocent;

        for(auto s: innocent)
          new_innocent.insert(s);
            
        new_innocent = getInnocent(valuation, new_innocent);

        if(gates.empty())
          gates_to_or[valuation][new_innocent].push_back(g.id);
        else {
          for(auto g2: gates) {
            gate_t and_gate;

            // We optimize a bit by avoiding creating an AND gate if there
            // is only one child, or if a second child is a TRUE gate
            gate_t gates_children[2];
            unsigned nb = 0;

            if(!(d.getGateType(g.id)==BooleanGate::AND &&
                  d.getWires(g.id).empty()))
              gates_children[nb++]=g.id;

            if(!(d.getGateType(g2)==BooleanGate::AND &&
                  d.getWires(g2).empty())) 
              gates_children[nb++]=g2;

            if(nb==0) {
              // We have one (or two) TRUE gates; we just reuse it -- even
              // though we reuse a child gate, connections will still make
              // sense as the valuation and suspicious set been correctly
              // computed
              and_gate = g.id;
            } else if(nb==1) {
              // Only one non-TRUE gate; we reuse it. Similarly as in the
              // previous case, even though we reuse this gate, the connections
              // made from it will still take into account the valuation and
              // suspicious set
              and_gate = gates_children[0];
            } else {
              and_gate = d.setGate(BooleanGate::AND);
              for(auto x: gates_children) {
                d.addWire(and_gate, x);
              }
            }

            gates_to_or[valuation][new_innocent].push_back(and_gate);
          }
        }
      }
    }
  }

  return gates_to_or;
}

dDNNFTreeDecompositionBuilder::gate_vector_t<dDNNFTreeDecompositionBuilder::dDNNFGate> dDNNFTreeDecompositionBuilder::builddDNNF()
{
  // Unfortunately, tree decompositions can be quite deep so we need to
  // simulate recursion with a heap-based stack, to avoid exhausting the
  // actual memory stack
  struct RecursionParams
  {
    bag_t bag;
    size_t children_processed;
    gates_to_or_t gates_to_or;

    RecursionParams(bag_t b, size_t c, gates_to_or_t g) :
      bag(b), children_processed(c), gates_to_or(std::move(g)) {}

    RecursionParams(bag_t b) :
      bag(b), children_processed(0) {
        gates_to_or_t::mapped_type m;
        m[suspicious_t{}] = {};
        gates_to_or[valuation_t{}] = std::move(m);
      }
  };

  using RecursionResult = gate_vector_t<dDNNFGate>;

  std::stack<std::variant<RecursionParams,RecursionResult>> stack;
  stack.emplace(RecursionParams{td.root});

  while(!stack.empty()) {
    RecursionResult result;

    if(stack.top().index()==1) { // RecursionResult
      result = std::move(std::get<1>(stack.top()));
      stack.pop();
      if(stack.empty())
        return result;
    }

    auto [bag, children_processed, gates_to_or] = std::move(std::get<0>(stack.top()));
    stack.pop();

    if(td.getChildren(bag).empty()) {
      auto x = builddDNNFLeaf(bag);
      stack.emplace(x);
    } else {
      if(children_processed>0) {
        gates_to_or = collectGatesToOr(bag, result, gates_to_or);
      }

      if(children_processed==td.getChildren(bag).size()) {
        gate_vector_t<dDNNFGate> result_gates;

        for(auto &[valuation, m]: gates_to_or) {
          for(auto &[innocent, gates]: m) {
            gate_t result_gate;
            
            assert(gates.size()!=0);

            suspicious_t suspicious;
            for(auto &[var, val]: valuation)
              if(innocent.find(var)==innocent.end())
                suspicious.insert(var);

            if(gates.size()==1)
              result_gate = *gates.begin();
            else {
              result_gate = d.setGate(BooleanGate::OR);
              for(auto &g: gates) {
                d.addWire(result_gate, g);
              }
            }

            result_gates.emplace_back(result_gate, std::move(valuation), std::move(suspicious));
          }
        }

        stack.emplace(std::move(result_gates));
      } else {
        stack.emplace(RecursionParams{bag, children_processed+1, std::move(gates_to_or)});
        stack.emplace(RecursionParams{td.getChildren(bag)[children_processed]});
      }
    }
  }

  // We return from within the while loop, when we hit the last return
  // value
  assert(false);
}

std::ostream &operator<<(std::ostream &o, const dDNNFTreeDecompositionBuilder::dDNNFGate &g)
{
  o << g.id << "; {";
  bool first=true;
  for(const auto &p: g.valuation) {
    if(!first)
      o << ",";
    first=false;
    o << "(" << p.first << "," << p.second << ")";
  }
  o << "}; {";
  first=true;
  for(auto x: g.suspicious) {
    if(!first)
      o << ",";
    first=false;
    o << x;
  }
  o << "}";

  return o;
}

bool dDNNFTreeDecompositionBuilder::circuitHasWire(gate_t f, gate_t t) const
{
  return wiresSet.find(std::make_pair(f,t))!=wiresSet.end();
}
