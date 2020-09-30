#include "dDNNF.h"

#include <unordered_map>
#include <stack>
#include <variant>
#include <cassert>

double dDNNF::dDNNFEvaluation(gate_t root) const
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
  std::stack<std::variant<RecursionParams,RecursionResult>> stack;
  stack.emplace(RecursionParams{root,0,0.});

  while(1) {
    double child_value{0.};

    if(stack.top().index()==1) { // RecursionResult
      child_value=std::get<1>(stack.top());
      stack.pop();
    }

    auto [g, children_processed, partial_value]=std::get<0>(stack.top());
    stack.pop();

    auto it = cache.find(g);
    double result;
    
    if(it!=cache.end()) {
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
        } else { // BooleanGate::OR
          partial_value += child_value;
        }
      }

      if(getGateType(g)!=BooleanGate::NOT && children_processed<getWires(g).size()) {
        stack.emplace(RecursionParams{g,children_processed+1,partial_value});
        stack.emplace(RecursionParams{getWires(g)[children_processed],0,0.});
      } else {
        result = partial_value;
        cache[g]=result;
        
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
