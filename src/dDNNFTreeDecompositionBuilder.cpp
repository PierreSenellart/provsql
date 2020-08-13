#include <algorithm>
#include <stack>
#include <variant>

#include "dDNNFTreeDecompositionBuilder.h"

/* Turn a bounded-treewidth circuit c for which a tree decomposition td
 * is provided into a dNNF rooted at root, following the construction in
 * Section 5.1 of https://arxiv.org/pdf/1811.02944 */
dDNNF&& dDNNFTreeDecompositionBuilder::build() && {
  // Theoretically, the BooleanCircuit could be modified by getGate, but
  // since we know root is a gate of the circuit (assert in constructor),
  // it is impossible.
  auto root_id = const_cast<BooleanCircuit &>(c).getGate(root);

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

  std::vector<dDNNFGate> result_gates = builddDNNF();

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

std::vector<dDNNFTreeDecompositionBuilder::dDNNFGate> dDNNFTreeDecompositionBuilder::builddDNNFLeaf(
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
      {}
    };
    dDNNFGate neg = { negated_input_gate.find(single_gate)->second,
      {std::make_pair(single_gate,false)}, 
      {} 
    };
    return { std::move(pos), std::move(neg) };
  } else {
    std::vector<dDNNFGate> result_gates;

    // We create two TRUE gates (AND gates with no inputs)
    for(auto v: {true, false}) {
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
dDNNFTreeDecompositionBuilder::getSuspicious(
    const valuation_t &valuation,
    bag_t bag,
    const suspicious_t &innocent) const
{
  suspicious_t suspicious;

  for(const auto &p: valuation) {
    // We first check if this gate was innocent because it was
    // innocent in a child
    if(innocent.find(p.first)!=innocent.end())
      continue;
    
    // Otherwise, we check if it is strong
    bool strong=isStrong(c.getGateType(p.first),p.second);

    if(!strong)
      continue;

    // We have a strong gate not innocented by the children bags,
    // it is suspicious unless we also have in the bag an input to
    // that gate which is strong for that gate
    bool susp=true;

    for(auto g: td.getBag(bag)) {
      if(g==p.first)
        continue;

      if(circuitHasWire(p.first,g)) {
        bool value = valuation.find(g)->second;
        if(isStrong(c.getGateType(p.first),value)) {
          susp=false;
          break;
        }
      }
    }
      
    if(susp)
      suspicious.insert(p.first);
  }

  return suspicious;
}

std::map<std::pair<dDNNFTreeDecompositionBuilder::valuation_t, dDNNFTreeDecompositionBuilder::suspicious_t>, std::vector<gate_t>>
dDNNFTreeDecompositionBuilder::collectGatesToOr(
    const std::vector<dDNNFGate> &gates1,
    const std::vector<dDNNFGate> &gates2,
    bag_t bag)
{
  std::map<std::pair<valuation_t,suspicious_t>, std::vector<gate_t>>
    gates_to_or;

  for(auto g1: gates1) {
    valuation_t partial_valuation;
    suspicious_t partial_innocent;

    for(const auto &p: g1.valuation)
      for(auto g: td.getBag(bag))
        if(p.first==g) {
          partial_valuation.insert(p);
          if(g1.suspicious.find(p.first)==g1.suspicious.end()) {
            partial_innocent.insert(p.first);
          }
        }
    
    // We check all suspicious gates are in the bag of the parent
    if(!isConnectible(g1.suspicious,td.getBag(bag)))
      continue;

    for(auto g2: gates2) {
      auto valuation = partial_valuation;
      auto innocent = partial_innocent;
      
      // Check if these two almost-evaluations mutually agree and if so
      // build the valuation of the bag
      bool agree=true;

      for(const auto &p: g2.valuation) {
        bool found=false;
        auto it=g1.valuation.find(p.first);
        if(it!=g1.valuation.end()) {
          found=true;
          agree=(it->second==p.second);
        }
        
        if(!agree)
          break;

        for(auto g: td.getBag(bag))
          if(p.first==g) {
            if(!found)
              valuation.insert(p);
            if(g2.suspicious.find(p.first)==g2.suspicious.end()) {
              innocent.insert(p.first);
            }
        }
      }

      if(!agree)
        continue;
    
      // We check all suspicious gates are in the bag of the parent
      if(!isConnectible(g2.suspicious,td.getBag(bag)))
        continue;

      // We check valuation is still an almost-valuation
      if(!isAlmostValuation(valuation))
        continue;

      auto suspicious = getSuspicious(valuation, bag, innocent);

      gate_t and_gate;
      
      // We optimize a bit by avoiding creating an AND gate if there
      // is only one child, or if a second child is a TRUE gate
      gate_t gates_children[2];
      unsigned nb = 0;

      if(!(d.getGateType(g1.id)==BooleanGate::AND &&
            d.getWires(g1.id).empty()))
        gates_children[nb++]=g1.id;

      if(td.getChildren(bag).size()==2)
        if(!(d.getGateType(g2.id)==BooleanGate::AND &&
            d.getWires(g2.id).empty())) 
          gates_children[nb++]=g2.id;

      assert(nb!=0);

      if(nb==1) {
        and_gate = gates_children[0];
      } else {
        and_gate = d.setGate(BooleanGate::AND);
        for(auto x: gates_children)
          d.addWire(and_gate, x);
      }

      gates_to_or[std::make_pair(valuation,suspicious)].push_back(and_gate);
    }
  }

  return gates_to_or;
}

std::vector<dDNNFTreeDecompositionBuilder::dDNNFGate> dDNNFTreeDecompositionBuilder::builddDNNF()
{
  // Unfortunately, tree decompositions can be quite deep so we need to
  // simulate recursion with a heap-based stack, to avoid exhausting the
  // actual memory stack

  enum class Recursion {
    START,
    AFTER_FIRST_RECURSIVE_CALL,
    AFTER_SECOND_RECURSIVE_CALL
  };

  struct RecursionParams
  {
    Recursion location;
    bag_t bag;
    std::vector<dDNNFGate> gates1;

    RecursionParams(Recursion l, bag_t b, std::vector<dDNNFGate> g = std::vector<dDNNFGate>()) :
      location(l), bag(b), gates1(std::move(g)) {}
  };

  using RecursionResult = std::vector<dDNNFGate>;

  std::stack<std::variant<RecursionParams,RecursionResult>> stack;
  stack.emplace(RecursionParams{Recursion::START, td.root});

  while(!stack.empty()) {
    RecursionResult result;

    if(stack.top().index()==1) { // RecursionResult
      result = std::move(std::get<1>(stack.top()));
      stack.pop();
      if(stack.empty())
        return result;
    }

    auto [location, bag, gates1] = std::move(std::get<0>(stack.top()));
    stack.pop();

    switch(location) {
      case Recursion::START:
        if(td.getChildren(bag).empty())
          stack.emplace(builddDNNFLeaf(bag));
        else {
          stack.emplace(RecursionParams{Recursion::AFTER_FIRST_RECURSIVE_CALL, bag});
          stack.emplace(RecursionParams{Recursion::START, td.getChildren(bag)[0]});
        }
        break;

      case Recursion::AFTER_FIRST_RECURSIVE_CALL:
        stack.emplace(RecursionParams{Recursion::AFTER_SECOND_RECURSIVE_CALL, bag, result});

        if(td.getChildren(bag).size()==2)
          stack.emplace(RecursionParams{Recursion::START, td.getChildren(bag)[1]});
        else
          stack.emplace(RecursionResult{{ gate_t{0}, {}, {} }});
        break;

      case Recursion::AFTER_SECOND_RECURSIVE_CALL:
        const auto &gates2 = result;

        auto gates_to_or = collectGatesToOr(gates1, gates2, bag);

        std::vector<dDNNFGate> result_gates;
        for(auto &p: gates_to_or) {
          gate_t result_gate;
          
          if(p.second.size()==1)
            result_gate = *p.second.begin();
          else {
            result_gate = d.setGate(BooleanGate::OR);
            for(auto &g: p.second)
              d.addWire(result_gate, g);
          }

          result_gates.emplace_back(result_gate, std::move(p.first.first), std::move(p.first.second));
        }

        stack.emplace(std::move(result_gates));
        break;
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
