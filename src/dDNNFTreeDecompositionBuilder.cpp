#include <algorithm>

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
    if(td.getChildren(i).empty() && b.nb_gates == 1 && c.getGateType(b.gates[0]) == BooleanGate::IN)
      responsible_bag[b.gates[0]] = i;
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

  std::vector<dDNNFGate> result_gates = builddDNNF(td.root);

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

static bool isConnectible(const std::set<gate_t> &suspicious,
                          const TreeDecomposition::Bag &b)
{
  for(const auto &g: suspicious) {
    bool found=false;
    for(unsigned k=0; k<b.nb_gates; ++k)
      if(g==b.gates[k]) {
        found=true;
        break;
      }

    if(!found)
      return false;
  }

  return true;
}

std::vector<dDNNFTreeDecompositionBuilder::dDNNFGate> dDNNFTreeDecompositionBuilder::builddDNNFLeaf(
    bag_t root)
{
  // If the bag is empty, it behaves as if it was not there
  if(td.getBag(root).nb_gates==0) 
    return {};

  // Otherwise, since we have a friendly decomposition, we have a
  // single gate
  auto single_gate = td.getBag(root).gates[0];

  // We check if this bag is responsible for an input variable
  if(c.getGateType(single_gate)==BooleanGate::IN &&
      responsible_bag.find(single_gate)->second==root)
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
      std::set<gate_t> suspicious;

      if(isStrong(c.getGateType(single_gate), v))
        suspicious.insert(single_gate);

      result_gates.emplace_back(
          d.setGate(BooleanGate::AND),
          std::map<gate_t,bool>{std::make_pair(single_gate, v)},
          std::move(suspicious)
      );
    }

    return result_gates;
  }
}

bool dDNNFTreeDecompositionBuilder::isAlmostValuation(
    const std::map<gate_t,bool> &valuation) const
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

std::set<gate_t>
dDNNFTreeDecompositionBuilder::getSuspicious(
    const std::map<gate_t, bool> &valuation,
    bag_t root,
    const std::set<gate_t> &innocent) const
{
  std::set<gate_t> suspicious;

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

    for(unsigned k=0; k<td.getBag(root).nb_gates; ++k) {
      auto g=td.getBag(root).gates[k];
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

std::map<std::pair<std::map<gate_t,bool>,std::set<gate_t>>,std::vector<gate_t>>
dDNNFTreeDecompositionBuilder::collectGatesToOr(
    const std::vector<dDNNFGate> &gates1,
    const std::vector<dDNNFGate> &gates2,
    bag_t root)
{
  std::map<std::pair<std::map<gate_t,bool>,std::set<gate_t>>,std::vector<gate_t>>
    gates_to_or;

  for(auto g1: gates1) {
    std::map<gate_t,bool> partial_valuation;
    std::set<gate_t> partial_innocent;

    for(const auto &p: g1.valuation)
      for(unsigned k=0; k<td.getBag(root).nb_gates; ++k)
        if(p.first==td.getBag(root).gates[k]) {
          partial_valuation.insert(p);
          if(g1.suspicious.find(p.first)==g1.suspicious.end()) {
            partial_innocent.insert(p.first);
          }
        }
    
    // We check all suspicious gates are in the bag of the parent
    if(!isConnectible(g1.suspicious,td.getBag(root)))
      continue;

    for(auto g2: gates2) {
      auto valuation = partial_valuation;
      auto innocent = partial_innocent;
      
      // Check if these two almost-evaluations mutually agree and if so
      // build the valuation of the root
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

        for(unsigned k=0; k<td.getBag(root).nb_gates; ++k)
          if(p.first==td.getBag(root).gates[k]) {
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
      if(!isConnectible(g2.suspicious,td.getBag(root)))
        continue;

      // We check valuation is still an almost-valuation
      if(!isAlmostValuation(valuation))
        continue;

      auto suspicious = getSuspicious(valuation, root, innocent);

      gate_t and_gate;
      
      // We optimize a bit by avoiding creating an AND gate if there
      // is only one child, or if a second child is a TRUE gate
      std::vector<gate_t> gates_children;

      if(!(d.getGateType(g1.id)==BooleanGate::AND &&
            d.getWires(g1.id).empty())) 
        gates_children.push_back(g1.id);

      if(td.getChildren(root).size()==2)
        if(!(d.getGateType(g2.id)==BooleanGate::AND &&
            d.getWires(g2.id).empty())) 
          gates_children.push_back(g2.id);

      assert(gates_children.size()!=0);

      if(gates_children.size()==1) {
        and_gate = gates_children[0];
      } else {
        and_gate = d.setGate(BooleanGate::AND);
        for(auto x: gates_children)
          d.addWire(and_gate, x);
      }

      gates_to_or[make_pair(valuation,suspicious)].push_back(and_gate);
    }
  }

  return gates_to_or;
}

std::vector<dDNNFTreeDecompositionBuilder::dDNNFGate> dDNNFTreeDecompositionBuilder::builddDNNF(
    bag_t root)
{
  if(td.getChildren(root).empty())
    return builddDNNFLeaf(root);

  auto gates1 = builddDNNF(td.getChildren(root)[0]);
  auto gates2 = std::vector<dDNNFGate>{};

  if(td.getChildren(root).size()==2)
    gates2 = builddDNNF(td.getChildren(root)[1]);
  else
    gates2 = {{ gate_t{0}, {}, {} }};

  auto gates_to_or = collectGatesToOr(gates1, gates2, root);

  std::vector<dDNNFGate> result_gates;
  for(auto &p: gates_to_or) {
    gate_t result_gate;
    
    if(p.second.size()==1)
      result_gate = p.second[0];
    else {
      result_gate = d.setGate(BooleanGate::OR);
      for(auto &g: p.second)
        d.addWire(result_gate, g);
    }

    result_gates.emplace_back(result_gate, std::move(p.first.first), std::move(p.first.second));
  }

  return result_gates;
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
