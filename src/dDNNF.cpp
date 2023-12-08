#include "dDNNF.h"

#include <unordered_map>
#include <stack>
#include <variant>
#include <cassert>
#include <algorithm>

std::unordered_set<gate_t> dDNNF::vars(gate_t root) const
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

void dDNNF::makeGatesBinary(BooleanGate type)
{
  for(gate_t g{0}; g<gates.size(); ++g) {
    if(getGateType(g)!=type || getWires(g).size()<=2)
      continue;

    if(getWires(g).size()==3) {
      const gate_t child = setGate(type);
      auto &w = getWires(g);
      for(size_t i=1; i<3; ++i) {
        addWire(child, w[i]);
      }
      w.resize(1);
      addWire(g, child);
    } else {
      const gate_t child1 = setGate(type);
      const gate_t child2 = setGate(type);

      auto &w = getWires(g);
      const auto k = w.size();

      for(unsigned i=0; i<k; ++i)
        if(i<k/2)
          addWire(child1, w[i]);
        else
          addWire(child2, w[i]);
      w.clear();
      addWire(g, child1);
      addWire(g, child2);
    }
  }
}

double dDNNF::probabilityEvaluation() const
{
  if (gates.size() == 0)
    return 0.;

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

  if(!isProbabilistic())
    return result;

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
          for(auto c: getWires(node))
            stack.push(std::make_pair(c, false));
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
          const auto n1=r1.size()-1;
          const auto n2=r2.size()-1;
          for(size_t k=0; k<=n1+n2; ++k) {
            double r = 0.;
            for(size_t k1=std::max(0,static_cast<int>(k-n2)); k1<=std::min(k,n1); ++k1) {
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

static long long comb(unsigned n, unsigned k)
{
  assert(k<=n);

  if(k == 0)
    return 1;
  else if(k > n/2)
    return comb(n,n-k);
  else return n * comb(n-1,k-1) / k;
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
      result[node] = {{0},{0,getProb(node)}};
      break;

    case BooleanGate::NOT:
      if(!b) {
        stack.push(std::make_pair(node, true));
        stack.push(std::make_pair(getWires(node)[0], false));
      } else {
        result[node] = result[getWires(node)[0]];
        auto k0=isProbabilistic()?0:result[node].size()-1;
        for(unsigned k=k0; k<result[node].size(); ++k)
          for(unsigned l=0; l<=k; ++l) {
            result[node][k][l] *= -1;
            result[node][k][l] += comb(k,l)*(isProbabilistic()?delta[node][k]:1);
          }
      }
      break;

    case BooleanGate::OR:
      if(!b) {
        if(getWires(node).size()==0) // Has to be an OR False gate
          result[node] = {{0.}};
        else {
          stack.push(std::make_pair(node, true));
          for(auto c: getWires(node))
            stack.push(std::make_pair(c, false));
        }
      } else {
        result[node] = result[getWires(node)[0]];
        for(size_t i=1; i<getWires(node).size(); ++i) {
          const auto &r = result[getWires(node)[i]];
          auto k0=isProbabilistic()?0:r.size()-1;
          for(unsigned k=k0; k<r.size(); ++k)
            for(unsigned l=0; l<r[k].size(); ++l)
              result[node][k][l]+=r[k][l];
        }
      }
      break;

    case BooleanGate::AND:
      if(!b) {
        if(getWires(node).size()==0) // Has to be an AND True gate
          result[node] = {{1.}};
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
          const auto n1=r1.size()-1;
          const auto n2=r2.size()-1;
          result[node].resize(n1+n2+1);
          auto k0=isProbabilistic()?0:n1+n2;
          for(size_t k=k0; k<=n1+n2; ++k) {
            result[node][k].resize(k+1);
            for(size_t l=0; l<=k; ++l) {
              for(size_t k1=std::max(0,static_cast<int>(k-n2)); k1<=std::min(k,n1); ++k1)
                for(size_t l1=std::max(0,static_cast<int>(l-k+k1)); l1<=std::min(k1,l); ++l1)
                  result[node][k][l] += r1[k1][l1] * r2[k-k1][l-l1];
            }
          }
        }
      }
      break;

    case BooleanGate::MULIN:
    case BooleanGate::MULVAR:
    case BooleanGate::UNDETERMINED:
      throw CircuitException("Incorrect gate type");
      break;
    }
  }

  return result[root];
}

double dDNNF::shapley(gate_t var) const {
  auto cond_pos = condition(var, true);
  auto cond_neg = condition(var, false);

  auto alpha_pos=cond_pos.shapley_alpha(root);
  auto alpha_neg=cond_neg.shapley_alpha(root);

  double result=0.;

  double k0=isProbabilistic()?0:alpha_pos.size()-1;
  for(size_t k=k0; k<alpha_pos.size(); ++k)
    for(size_t l=0; l<=k; ++l) {
      double pos = alpha_pos[k][l];
      double neg = alpha_neg[k][l];
      result += (pos-neg)/comb(k,l)/(k+1);
    }

  result *= getProb(var);

  // Avoid rounding errors that make expected Shapley value outside of [-1,1]
  if(result>1.)
    result=1.;
  else if(result<-1.)
    result=-1.;

  return result;
}

dDNNF dDNNF::condition(gate_t var, bool value) const {
  assert(getGateType(var)==BooleanGate::IN);

  dDNNF result=*this;

  result.setGateType(var, value ? BooleanGate::AND : BooleanGate::OR);
  result.probability_cache[var] = value?1.:0.;
  result.inputs.erase(var);
  auto it = id2uuid.find(var);
  if(it!=id2uuid.end()) {
    result.uuid2id.erase(it->second);
    result.id2uuid.erase(var);
  }

  return result;
}

std::vector<gate_t> dDNNF::topological_order(const std::vector<std::vector<gate_t> > &reversedWires) const
{
  std::vector<gate_t> result;

  std::stack<gate_t> nodesToProcess;
  std::vector<size_t> inDegree(wires.size());

  for(size_t g=0; g<wires.size(); ++g)
    if(!(inDegree[g] = wires[g].size()))
      nodesToProcess.push(gate_t{g});

  while(!nodesToProcess.empty()) {
    auto g = nodesToProcess.top();
    nodesToProcess.pop();
    result.push_back(g);
    for(auto p: reversedWires[static_cast<size_t>(g)])
      if(!(--inDegree[static_cast<size_t>(p)]))
        nodesToProcess.push(p);
  }

  return result;
}

void dDNNF::simplify() {
  std::vector<std::vector<gate_t> > reversedWires(gates.size());
  for(size_t i=0; i<wires.size(); ++i)
    for(auto g: wires[i])
      reversedWires[static_cast<size_t>(g)].push_back(gate_t{i});

  for(auto node: topological_order(reversedWires)) {
    auto &w = wires[static_cast<size_t>(node)];

    switch(getGateType(node)) {
    case BooleanGate::IN:
      break;

    case BooleanGate::AND:
    case BooleanGate::OR:
      if(w.size()==0)
        probability_cache[node]=(getGateType(node)==BooleanGate::AND?1.:0.);
      else if(w.size()==1) {
        if(node==getRoot()) {
          root=w[0];
        } else {
          for(auto p: reversedWires[static_cast<size_t>(node)])
            std::replace(wires[static_cast<size_t>(p)].begin(), wires[static_cast<size_t>(p)].end(), node, w[0]);
        }
        w.clear();
      } else {
        for(auto c=w.begin(); c!=w.end();) {
          if(getGateType(*c)==getGateType(node) && getWires(*c).size()==0)
            c = w.erase(c);
          else if(getGateType(*c)==(getGateType(node)==BooleanGate::AND?BooleanGate::OR:BooleanGate::AND) && getWires(*c).size()==0) {
            setGateType(node, getGateType(*c));
            probability_cache[node] = getGateType(*c)==BooleanGate::AND?1.:0.;
            w.clear();
            break;
          } else
            ++c;
        }
      }
      break;

    case BooleanGate::NOT:
      if(getGateType(w[0])==BooleanGate::AND && getWires(w[0]).size()==0) {
        setGateType(node, BooleanGate::OR);
        probability_cache[node]=0.;
        w.clear();
      } else if(getGateType(w[0])==BooleanGate::OR && getWires(w[0]).size()==0) {
        setGateType(node, BooleanGate::AND);
        probability_cache[node]=1.;
        w.clear();
      }
      break;

    case BooleanGate::MULIN:
    case BooleanGate::MULVAR:
    case BooleanGate::UNDETERMINED:
      throw CircuitException("Incorrect gate type");
      break;
    }
  }

  std::vector<bool> used(gates.size());
  std::stack<gate_t> to_process;
  to_process.push(root);

  while(!to_process.empty()) {
    auto g = to_process.top();
    to_process.pop();
    used[static_cast<size_t>(g)]=true;
    for(auto c: wires[static_cast<size_t>(g)])
      if(!used[static_cast<size_t>(c)])
        to_process.push(c);
  }

  size_t newi = 0;
  std::vector<gate_t> relabel(gates.size());
  for(size_t i=0; i<gates.size(); ++i)
  {
    if(!used[i]) {
      inputs.erase(gate_t{i});
      probability_cache.erase(gate_t{i});
      auto it = id2uuid.find(gate_t{i});
      if(it!=id2uuid.end()) {
        uuid2id.erase(it->second);
        id2uuid.erase(it);
      }
      continue;
    }

    relabel[i]=gate_t{newi};

    if(i!=newi) {
      gates[newi] = gates[i];
      wires[newi] = wires[i];
      prob[newi]=prob[i];

      auto it1 = probability_cache.find(gate_t{i});
      if(it1!=probability_cache.end()) {
        probability_cache[gate_t{newi}] = it1->second;
        probability_cache.erase(it1);
      }

      auto it2 = id2uuid.find(gate_t{i});
      if(it2!=id2uuid.end()) {
        id2uuid[gate_t{newi}] = it2->second;
        uuid2id[it2->second] = gate_t{newi};
        id2uuid.erase(it2);
      }

      if(root==gate_t{i})
        root=gate_t{newi};

      auto it3 = inputs.find(gate_t{i});
      if(it3!=inputs.end()) {
        inputs.insert(gate_t{newi});
        inputs.erase(it3);
      }
    }

    ++newi;
  }

  gates.resize(newi);
  wires.resize(newi);
  prob.resize(newi);

  for(auto &w: wires)
    for(size_t i=0; i<w.size(); ++i)
      w[i]=relabel[static_cast<size_t>(w[i])];
}
