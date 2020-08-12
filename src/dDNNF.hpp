#include "dDNNF.h"

#include <unordered_map>
#include <algorithm>
#include <cassert>

/* Turn a bounded-treewidth circuit c for which a tree decomposition td is
 * provided into a dNNF rooted at root, following the construction in
 * Section 5.1 of https://arxiv.org/pdf/1811.02944 */
template<unsigned W>
dDNNF::dDNNF(const BooleanCircuit &c, const uuid &root, TreeDecomposition<W> &td)
{
  // Theoretically, the BooleanCircuit could be modified by getGate, but
  // only if root is not a gate of the circuit, so we check that it is a
  // gate.
  assert(c.hasGate(root));
  unsigned root_id = const_cast<BooleanCircuit &>(c).getGate(root);

  // We make the tree decomposition friendly
  td.makeFriendly(root_id);

  // We look for bags responsible for each variable
  std::unordered_map<unsigned, unsigned long> responsible_bag;
  for(unsigned i=0; i<td.bags.size(); ++i) {
    const auto &b = td.bags[i];
    if(td.children[i].empty() && b.nb_gates == 1 && c.gates[b.gates[0]] == BooleanGate::IN)
      responsible_bag[b.gates[0]] = i;
  }

  // A friendly tree decomposition has leaf bags for every variable
  // nodes. Let's just check that to be safe.
  assert(responsible_bag.size()==c.inputs.size());

  // Create the input and negated input gates
  std::unordered_map<unsigned,unsigned long> input_gate, negated_input_gate;
  for(auto g: c.inputs) {
    unsigned gate = setGate(BooleanGate::IN, c.prob[g]);
    unsigned not_gate = setGate(BooleanGate::NOT);
    addWire(not_gate, gate);
    input_gate[g]=gate;
    negated_input_gate[g]=not_gate;
  }

  std::vector<dDNNFGate> result_gates = 
    builddDNNF(c, td.root, td, responsible_bag, input_gate, negated_input_gate);

  unsigned long result_id = setGate("root", BooleanGate::OR);

  for(const auto &p: result_gates) {
    if(p.suspicious.empty() && p.valuation.find(root_id)->second) {
      addWire(result_id, p.id);
      return;
    }
  }
}

bool isStrong(BooleanGate type, bool value)
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

template<unsigned W>
static bool isConnectible(const std::set<unsigned> &suspicious,
                          const typename TreeDecomposition<W>::Bag &b)
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

template<unsigned W>
std::vector<dDNNF::dDNNFGate> dDNNF::builddDNNF(
    const BooleanCircuit &c, 
    unsigned root,
    const TreeDecomposition<W> &td,
    const std::unordered_map<unsigned, unsigned long> &responsible_bag,
    const std::unordered_map<unsigned, unsigned long> &input_gate,
    const std::unordered_map<unsigned, unsigned long> &negated_input_gate)
{
  std::vector<dDNNFGate> result_gates;

  if(td.children[root].empty()) {
    // If the bag is empty, it behaves as if it was not there
    if(td.bags[root].nb_gates!=0) {
      // Otherwise, since we have a friendly decomposition, we have a
      // single gate
      unsigned single_gate = td.bags[root].gates[0];

      // We check if this bag is responsible for an input variable
      if(c.gates[single_gate]==BooleanGate::IN &&
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
        result_gates = { pos, neg };
      } else {
        // We create two TRUE gates (AND gates with no inputs)
        for(auto v: {true, false}) {
          std::set<unsigned> suspicious;

          if(isStrong(c.gates[single_gate], v))
            suspicious.insert(single_gate);

          result_gates.push_back({
              setGate(BooleanGate::AND),
              {std::make_pair(single_gate, v)},
              suspicious
              });
        }
      }
    }
  } else {
    std::vector<dDNNFGate> gates1 = builddDNNF(c, td.children[root][0], td,
                                      responsible_bag, input_gate, negated_input_gate);
    std::vector<dDNNFGate> gates2;

    if(td.children[root].size()==2)
      gates2 = builddDNNF(c, td.children[root][1], td,
                             responsible_bag, input_gate, negated_input_gate);
    else
      gates2 = {{ 0, {}, {} }};
  
    std::map<std::pair<std::map<unsigned,bool>,std::set<unsigned>>,std::vector<unsigned>>
      gates_to_or;

    for(auto g1: gates1) {
      std::map<unsigned,bool> partial_valuation;
      std::set<unsigned> partial_innocent;

      for(const auto &p: g1.valuation)
        for(unsigned k=0; k<td.bags[root].nb_gates; ++k)
          if(p.first==td.bags[root].gates[k]) {
            partial_valuation.insert(p);
            if(g1.suspicious.find(p.first)==g1.suspicious.end()) {
              partial_innocent.insert(p.first);
            }
          }
      
      // We check all suspicious gates are in the bag of the parent
      if(!isConnectible<W>(g1.suspicious,td.bags[root]))
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

          for(unsigned k=0; k<td.bags[root].nb_gates; ++k)
            if(p.first==td.bags[root].gates[k]) {
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
        if(!isConnectible<W>(g2.suspicious,td.bags[root]))
          continue;

        // We check valuation is still an almost-valuation
        bool almostvaluation = true;
        for(const auto &p1: valuation) {
          for(const auto &p2: valuation) {
            if(p1.first==p2.first)
              continue;
            if(!isStrong(c.gates[p1.first],p2.second))
              continue;

            if(std::find(c.wires[p1.first].begin(),c.wires[p1.first].end(),p2.first)!=
               c.wires[p1.first].end()) {
              switch(c.gates[p1.first]) {
                case BooleanGate::AND:
                case BooleanGate::OR:
                  almostvaluation = (p1.second==p2.second);
                  break;
                case BooleanGate::NOT:
                  almostvaluation = (p1.second!=p2.second);
                  break;
                default:
                  ;
              }

              if(!almostvaluation)
                break;
            }
          }
          if(!almostvaluation)
            break;
        }

        if(!almostvaluation)
          continue;

        std::set<unsigned> suspicious;
        for(const auto &p: valuation) {
          // We first check if this gate was innocent because it was
          // innocent in a child
          if(innocent.find(p.first)!=innocent.end())
            continue;
          
          // Otherwise, we check if it is strong
          bool strong=isStrong(c.gates[p.first],p.second);

          if(!strong)
            continue;

          // We have a strong gate not innocented by the children bags,
          // it is suspicious unless we also have in the bag an input to
          // that gate which is strong for that gate
          bool susp=true;

          for(unsigned k=0; k<td.bags[root].nb_gates; ++k) {
            auto g=td.bags[root].gates[k];
            if(g==p.first)
              continue;

            if(std::find(c.wires[p.first].begin(),c.wires[p.first].end(),g)!=
               c.wires[p.first].end()) {
              bool value = valuation[g];
              if(isStrong(c.gates[p.first],value)) {
                susp=false;
                break;
              }
            }
          }
           
          if(susp)
            suspicious.insert(p.first);
        }

        unsigned long and_gate;
       
        // We optimize a bit by avoiding creating an AND gate if there
        // is only one child, or if a second child is a TRUE gate
        std::vector<unsigned long> gates_children;

        if(!(gates[g1.id]==BooleanGate::AND &&
             wires[g1.id].empty())) 
          gates_children.push_back(g1.id);

        if(td.children[root].size()==2)
          if(!(gates[g2.id]==BooleanGate::AND &&
              wires[g2.id].empty())) 
            gates_children.push_back(g2.id);

        if(gates_children.size()==1) {
          and_gate = gates_children[0];
        } else {
          and_gate = setGate(BooleanGate::AND);
          for(auto x: gates_children)
            addWire(and_gate, x);
        }

        gates_to_or[make_pair(valuation,suspicious)].push_back(and_gate);
      }
    }

    for(const auto &p: gates_to_or) {
      unsigned long result_gate;
     
      if(p.second.size()==1)
        result_gate = p.second[0];
      else {
        result_gate = setGate(BooleanGate::OR);
        for(auto &g: p.second)
          addWire(result_gate, g);
      }

      result_gates.push_back({result_gate, p.first.first, p.first.second});
    }
  }
    
  return result_gates;
}

double dDNNF::dDNNFEvaluation(unsigned g) const
{
  static std::unordered_map<unsigned, double> cache;

  auto it = cache.find(g);
  if(it!=cache.end())
    return it->second;

  double result;

  if(gates[g]==BooleanGate::IN)
    result = prob[g];
  else if(gates[g]==BooleanGate::IN)
    result = 1-dDNNFEvaluation(wires[g][0]);
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

std::ostream &operator<<(std::ostream &o, const dDNNF::dDNNFGate &g)
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
