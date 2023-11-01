#include "dDNNF.h"

#include <unordered_map>
#include <stack>
#include <variant>
#include <cassert>
#include <algorithm>

long long comb(unsigned n, unsigned k)
{
  assert(k<=n);

  if(k == 0)
    return 1;
  else if(k > n/2)
    return comb(n,n-k);
  else return n * comb(n-1,k-1) / k;
}

std::unordered_set<gate_t> dDNNF::vars(gate_t root)
{
  // No recursion, so using a stack to handle all children; order of
  // calls is here irrelevant
  std::stack<gate_t> to_process;
  std::unordered_set<gate_t> processed;
  to_process.push(root);

  std::unordered_set<gate_t> result;
  while(!to_process.empty())
  {
    auto g = to_process.top();
    to_process.pop();

    if(processed.find(g)==processed.end()) {
      if(getGateType(g)==BooleanGate::IN)
        result.insert(g);
      else {
        for(auto c: getWires(g))
          to_process.push(c);
      }
      processed.insert(g);
    }
  }

  return result;
}

void dDNNF::makeSmooth()
{
  gate_t original_gates_size{gates.size()};
  // gates.size() might change in the loop, but newly added gates do not
  // need to be iterated upon

  for(gate_t g = gate_t{0}; g<original_gates_size; ++g) {
    if(getGateType(g)!=BooleanGate::OR || getWires(g).size()<=1)
      continue;

    std::unordered_set<gate_t> all;
    std::vector<std::unordered_set<gate_t> > varss;

    std::for_each(getWires(g).begin(), getWires(g).end(),
                  [&](gate_t g) {
      varss.push_back(vars(g));
    });
    std::for_each(varss.begin(), varss.end(),
                  [&](const auto &s) {
      all.insert(s.begin(),s.end());
    });

    for(auto v: all) {
      bool modified = false;
      for(size_t i=0; i<varss.size(); ++i) {
        if(varss[i].find(v)==varss[i].end()) {
          if(!modified) {
            if(getGateType(getWires(g)[i])!=BooleanGate::AND) {
              gate_t and_gate = setGate(BooleanGate::AND);
              addWire(and_gate, getWires(g)[i]);
              getWires(g)[i]=and_gate;
            }
            modified = true;
          }

          gate_t dummy_or_gate = setGate(BooleanGate::OR);
          gate_t dummy_not_gate = setGate(BooleanGate::NOT);
          addWire(dummy_or_gate, v);
          addWire(dummy_not_gate, v);
          addWire(dummy_or_gate, dummy_not_gate);
          addWire(getWires(g)[i],dummy_or_gate);
        }
      }
    }
  }
}

void dDNNF::makeAndGatesBinary()
{
  for(gate_t g{0}; g<gates.size(); ++g) {
    if(getGateType(g)!=BooleanGate::AND || getWires(g).size()<=2)
      continue;

    auto &w = getWires(g);
    const auto k = w.size();

    const gate_t and1 = setGate(BooleanGate::AND);
    const gate_t and2 = setGate(BooleanGate::AND);
    for(unsigned i=0; i<k; ++i)
      if(k<k/2)
        addWire(and1, w[i]);
      else
        addWire(and2, w[i]);
    w.clear();
    addWire(g, and1);
    addWire(g, and2);
  }
}

double dDNNF::dDNNFProbabilityEvaluation(gate_t root) const
{
  // Unfortunately, dDNNFs can be quite deep so we need to simulate
  // recursion with a heap-based stack, to avoid exhausting the actual
  // memory stack
  using RecursionParams = struct {
    gate_t g;
    size_t children_processed;
    double partial_value;
  };
  using RecursionResult = double;
  std::stack<std::variant<RecursionParams,RecursionResult> > stack;
  stack.emplace(RecursionParams{root,0,0.});

  while(1) {
    double child_value{0.};

    if(stack.top().index()==1) {   // RecursionResult
      child_value=std::get<1>(stack.top());
      stack.pop();
    }

    auto [g, children_processed, partial_value]=std::get<0>(stack.top());
    stack.pop();

    auto it = probability_cache.find(g);

    if(it!=probability_cache.end()) {
      if(stack.empty())
        return it->second;
      else
        stack.emplace(it->second);
    } else {
      if(children_processed==0) {
        switch(getGateType(g)) {
        case BooleanGate::IN:
          partial_value = getProb(g);
          break;
        case BooleanGate::NOT:
          partial_value = 1-getProb(*getWires(g).begin());
          break;
        case BooleanGate::AND:
          partial_value = 1;
          break;
        case BooleanGate::OR:
          partial_value = 0;
          break;
        default:
          assert(false);
        }
      } else {
        if(getGateType(g) == BooleanGate::AND) {
          partial_value *= child_value;
        } else {   // BooleanGate::OR
          partial_value += child_value;
        }
      }

      if(getGateType(g)!=BooleanGate::NOT && children_processed<getWires(g).size()) {
        stack.emplace(RecursionParams{g,children_processed+1,partial_value});
        stack.emplace(RecursionParams{getWires(g)[children_processed],0,0.});
      } else {
        double result = partial_value;
        probability_cache[g]=result;

        if(stack.empty())
          return result;
        else
          stack.emplace(result);
      }
    }
  }

  // We return from within the while loop, when the stack is empty
  assert(false);
}

std::unordered_map<gate_t, std::vector<double> > dDNNF::shapley_delta(gate_t root) const {
  std::unordered_map<gate_t, std::vector<double> > result;

  // Stack to simulate recursion: contains a pair (node, b) where b
  // indicates whether this is the beginning (false) or ending (true) of
  // the processing of a node
  std::stack<std::pair<gate_t, bool> > stack;
  stack.emplace(std::make_pair(root, false));

  while(!stack.empty())
  {
    auto [node, b] = stack.top();
    stack.pop();

    if(result.find(node)!=result.end()) {
      // Already processed, skip
      continue;
    }

    switch(getGateType(node)) {
    case BooleanGate::IN:
      result[node] = {1-getProb(node), getProb(node)};
      break;

    case BooleanGate::NOT:
    case BooleanGate::OR:
      if(!b) {
        if(getWires(node).size()==0) // Has to be an OR False gate
          result[node] = {1};
        else {
          stack.push(std::make_pair(node, true));
          stack.push(std::make_pair(getWires(node)[0], false));
        }
      } else {
        result[node] = result[getWires(node)[0]];
      }
      break;

    case BooleanGate::AND:
    {
      if(!b) {
        if(getWires(node).size()==0) // Has to be an AND True gate
          result[node] = {1};
        else {
          stack.push(std::make_pair(node, true));
          for(auto c: getWires(node))
            stack.push(std::make_pair(c, false));
        }
      } else {
        if(getWires(node).size()==1)
          result[node] = result[getWires(node)[0]];
        else {
          assert(getWires(node).size()==2); // AND has been made binary
          const auto &r1 = result[getWires(node)[0]];
          const auto &r2 = result[getWires(node)[1]];
          const auto n1=r1.size();
          const auto n2=r2.size();
          for(size_t k=0; k<n1+n2; ++k) {
            double r = 0.;
            for(size_t k1=std::max(size_t{0},k-n2); k1<=std::min(k,n1); ++k1) {
              r+=r1[k1]*r2[k-k1];
            }
            result[node].push_back(r);
          }
        }
      }

      break;
    }

    case BooleanGate::MULIN:
    case BooleanGate::MULVAR:
    case BooleanGate::UNDETERMINED:
      throw CircuitException("Incorrect gate type");
      break;
    }
  }

  return result;
}

std::vector<std::vector<double> > dDNNF::shapley_alpha(gate_t root) const {
  std::unordered_map<gate_t, std::vector<double> > delta {shapley_delta(root)};
  std::unordered_map<gate_t, std::vector<std::vector<double> > > result;

  // Stack to simulate recursion: contains a pair (node, b) where b
  // indicates whether this is the beginning (false) or ending (true) of
  // the processing of a node
  std::stack<std::pair<gate_t, bool> > stack;
  stack.emplace(std::make_pair(root, false));

  while(!stack.empty())
  {
    auto [node, b] = stack.top();
    stack.pop();

    if(result.find(node)!=result.end()) {
      // Already processed, skip
      continue;
    }

    switch(getGateType(node)) {
    case BooleanGate::IN:
      result[node] = {{0},{0,1}};
      break;

    case BooleanGate::NOT:
      if(!b) {
        stack.push(std::make_pair(node, true));
        stack.push(std::make_pair(getWires(node)[0], false));
      } else {
        result[node] = result[getWires(node)[0]];
        for(unsigned k=0; k<result[node].size(); ++k)
          for(unsigned l=0; l<result[node].size(); ++l) {
            result[node][k][l] *= -1;
            result[node][k][l] += comb(k,l)*delta[node][k];
          }
      }
      break;

    case BooleanGate::OR:
      if(!b) {
        if(getWires(node).size()==0) // Has to be an OR False gate
          result[node] = {{0}};
        else {
          stack.push(std::make_pair(node, true));
          for(auto c: getWires(node))
            stack.push(std::make_pair(c, false));
        }
      } else {
        result[node] = result[getWires(node)[0]];
        for(size_t i=0; i<getWires(node).size(); ++i) {
          const auto &r = result[getWires(node)[i]];
          for(unsigned k=0; k<r.size(); ++k)
            for(unsigned l=0; l<r[k].size(); ++l)
              result[node][k][l]+=r[k][l];
        }
      }
      break;

    case BooleanGate::AND:
    // TODO

    case BooleanGate::MULIN:
    case BooleanGate::MULVAR:
    case BooleanGate::UNDETERMINED:
      throw CircuitException("Incorrect gate type");
      break;
    }
  }

  return result[root];
}

double dDNNF::shapley(gate_t g, gate_t var) const {
  // TODO
  return 0.;
}

dDNNF dDNNF::condition(gate_t var, bool value) const {
  assert(getGateType(var)==BooleanGate::IN);

  std::vector<std::vector<gate_t> > reversedWires(gates.size());
  for(size_t i=0; i<wires.size(); ++i)
    for(auto g: wires[i])
      reversedWires[static_cast<size_t>(g)].push_back(gate_t{i});

  dDNNF result = *this;

  std::stack<std::pair<gate_t,bool> > to_process;
  to_process.emplace(std::make_pair(var,value));

  while(!to_process.empty()) {
    auto [node, val]=to_process.top();
    to_process.pop();

    bool propagate = false;

    auto &w = result.wires[static_cast<size_t>(node)];

    switch(result.getGateType(node)) {
    case BooleanGate::IN:
      result.setGateType(node, val?BooleanGate::AND:BooleanGate::OR);
      result.probability_cache[node] = val?1.:0.;
      propagate = true;
      break;

    case BooleanGate::AND:
    {
      bool remove_all = !val;
      if(remove_all) {
        w.clear();
        result.setGateType(node, BooleanGate::OR);
        result.probability_cache[node] = 0.;
        propagate=true;
      } else {
        for(auto c=w.begin(); c!=w.end();) {
          auto it=result.probability_cache.find(*c);
          if(it!=result.probability_cache.end() && it->second==1.)
            c = w.erase(c);
          else
            ++c;
        }
        if(w.size()==0)
          propagate=true;
      }
      break;
    }

    case BooleanGate::OR:
    {
      bool remove_all = val;
      if(remove_all) {
        w.clear();
        result.setGateType(node, BooleanGate::AND);
        result.probability_cache[node] = 1.;
        propagate=true;
      } else {
        for(auto c=w.begin(); c!=w.end();) {
          auto it=result.probability_cache.find(*c);
          if(it!=result.probability_cache.end() && it->second==0.)
            c = w.erase(c);
          else
            ++c;
        }
        if(w.size()==0)
          propagate=true;
      }
      break;
    }

    case BooleanGate::NOT:
      result.setGateType(node, val?BooleanGate::OR:BooleanGate::AND);
      w.clear();
      result.probability_cache[node] = val?0.:1.;
      propagate=true;
      break;

    case BooleanGate::MULIN:
    case BooleanGate::MULVAR:
    case BooleanGate::UNDETERMINED:
      throw CircuitException("Incorrect gate type");
      break;
    }

    if(propagate)
      for(auto g: reversedWires[static_cast<size_t>(node)])
        to_process.emplace(std::make_pair(g, val));
  }

  return result;
}
