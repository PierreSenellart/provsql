/**
 * @file FixSystemGraph.h
 * @brief Strongly connected components of the dependency graph of an
 *        equation system (a recursive query, see @c gate_fixsystem).
 */
#ifndef FIX_SYSTEM_GRAPH_H
#define FIX_SYSTEM_GRAPH_H

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

/**
 * @brief Strongly connected components of a directed graph, in the order
 *        in which equations along its edges can be solved.
 *
 * Iterative Tarjan.  With an edge @c j → @c i when the equation of @c i
 * reads unknown @c j, every component comes after the components it
 * reads, so solving them in the returned order only ever reads solved
 * unknowns, or those of the component itself.
 *
 * @param out  @c out[j] lists the vertices with an edge from @c j.
 * @return     The components, each a list of vertices.
 */
inline std::vector<std::vector<std::size_t> >
fixSystemComponents(const std::vector<std::vector<std::size_t> > &out)
{
  const std::size_t n = out.size();
  const std::size_t UNSEEN = static_cast<std::size_t>(-1);
  std::vector<std::size_t> index(n, UNSEEN), low(n, 0);
  std::vector<char> on_stack(n, 0);
  std::vector<std::size_t> tstack;
  std::vector<std::vector<std::size_t> > comps;
  std::size_t counter = 0;

  for(std::size_t root = 0; root < n; ++root) {
    if(index[root] != UNSEEN)
      continue;
    std::vector<std::pair<std::size_t, std::size_t> > call{{root, 0}};
    index[root] = low[root] = counter++;
    tstack.push_back(root);
    on_stack[root] = 1;
    while(!call.empty()) {
      const std::size_t v = call.back().first;
      std::size_t &k = call.back().second;
      if(k < out[v].size()) {
        const std::size_t u = out[v][k++];
        if(index[u] == UNSEEN) {
          index[u] = low[u] = counter++;
          tstack.push_back(u);
          on_stack[u] = 1;
          call.emplace_back(u, 0);
        } else if(on_stack[u])
          low[v] = std::min(low[v], index[u]);
        continue;
      }
      if(low[v] == index[v]) {
        std::vector<std::size_t> comp;
        std::size_t u;
        do {
          u = tstack.back();
          tstack.pop_back();
          on_stack[u] = 0;
          comp.push_back(u);
        } while(u != v);
        comps.push_back(std::move(comp));
      }
      call.pop_back();
      if(!call.empty())
        low[call.back().first] = std::min(low[call.back().first], low[v]);
    }
  }
  /* Tarjan emits a component after every component reachable from it,
   * i.e. after those that read it: reverse into solving order. */
  std::reverse(comps.begin(), comps.end());
  return comps;
}

#endif /* FIX_SYSTEM_GRAPH_H */
