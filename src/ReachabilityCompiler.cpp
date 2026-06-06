/**
 * @file ReachabilityCompiler.cpp
 * @brief Implementation of the decomposition-aligned reachability compiler.
 *
 * See @c ReachabilityCompiler.h for the construction's design and the
 * structural argument (determinism and decomposability by construction).
 * The implementation is a single bottom-up pass over a min-fill tree
 * decomposition of the data graph, with an explicit stack (decompositions
 * of path-like data are themselves path-like, so recursion depth would be
 * linear in the data).
 */
#include "ReachabilityCompiler.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

/* The DP loops are the runtime hot spots on large instances; keep the
 * backend cancellable, mirroring TreeDecomposition.cpp's guard pattern
 * (no-op outside the PostgreSQL extension build). */
#ifdef TDKC
#include "tdkc_interrupt.h"
#define CHECK_FOR_INTERRUPTS() provsql_tdkc_poll()
#else
extern "C" {
#include "postgres.h"
#include "miscadmin.h"
}
#endif

namespace {

/** @brief Underlying integer of a @c bag_t. */
inline std::size_t bag_index(bag_t b)
{
  return static_cast<std::underlying_type<bag_t>::type>(b);
}

/** @brief One edge variable: a provenance token gating one or two arcs between two vertices. */
struct EdgeVariable {
  unsigned long u;      ///< First endpoint.
  unsigned long v;      ///< Second endpoint.
  bool arc_uv;          ///< Arc u -> v present when the variable is true.
  bool arc_vu;          ///< Arc v -> u present when the variable is true.
  std::string token;    ///< Provenance token (UUID).
  double prob;          ///< Tuple probability.
};

} // namespace

ReachabilityCompiler::Rel ReachabilityCompiler::transitiveClosure(Rel r, int d)
{
  // Warshall over the first d positions; d <= MAXD = 13, so this is a
  // small constant amount of work per call.
  for (int k = 0; k < d; ++k)
    for (int i = 0; i < d; ++i)
      if (r[i*MAXD + k])
        for (int j = 0; j < d; ++j)
          if (r[k*MAXD + j])
            r.set(i*MAXD + j);
  return r;
}

ReachabilityCompiler::Result ReachabilityCompiler::compile(
  const std::vector<EdgeRow> &rows,
  unsigned long source,
  unsigned long target,
  bool directed,
  std::size_t max_states)
{
  Result result;
  dDNNF &dd = result.dd;

  // ------------------------------------------------------------------
  // 1. Group rows into edge variables (one per provenance token).
  //
  // A token shared by two mutual-reverse rows is the natural encoding
  // of an undirected edge in a directed edge relation and becomes one
  // bidirectional variable; any other sharing would break the
  // independence (and decomposability) assumptions, so it is rejected.
  // Self-loops never affect reachability and are dropped.
  // ------------------------------------------------------------------
  std::vector<EdgeVariable> variables;
  {
    std::unordered_map<std::string, std::size_t> token_to_var;
    for (const auto &row : rows) {
      if (row.src == row.dst)
        continue; // self-loop, irrelevant to reachability

      auto it = token_to_var.find(row.token);
      if (it == token_to_var.end()) {
        EdgeVariable var;
        var.u = row.src;
        var.v = row.dst;
        var.arc_uv = true;
        var.arc_vu = !directed;
        var.token = row.token;
        var.prob = row.prob;
        token_to_var[row.token] = variables.size();
        variables.push_back(var);
      } else {
        EdgeVariable &var = variables[it->second];
        if (row.src == var.u && row.dst == var.v) {
          // duplicate arc, idempotent
        } else if (row.src == var.v && row.dst == var.u) {
          var.arc_uv = var.arc_vu = true; // mutual-reverse pair
        } else {
          throw ReachabilityCompilerException(
                  "provenance token " + row.token +
                  " is shared by edges with different endpoints");
        }
      }
    }
  }
  result.stats.nb_variables = variables.size();

  // ------------------------------------------------------------------
  // 2. Tree decomposition of the data graph (vertices: all endpoints
  //    plus the two terminals), by min-fill elimination.
  // ------------------------------------------------------------------
  Graph graph;
  graph.add_node(source);
  graph.add_node(target);
  for (const auto &var : variables)
    graph.add_edge(var.u, var.v);

  std::unordered_map<unsigned long, bag_t> elimination_bag;
  const TreeDecomposition td(std::move(graph), &elimination_bag);
  result.stats.data_treewidth = td.getTreewidth();
  result.stats.nb_bags = td.getNbBags();

  // Each variable is introduced at exactly one node: the bag created
  // when the earlier-eliminated endpoint was eliminated contains both
  // endpoints (elimination invariant), and a unique introduction point
  // is what makes the emitted AND gates decomposable.
  std::vector<std::vector<std::size_t> > variables_at_bag(td.getNbBags());
  for (std::size_t i = 0; i < variables.size(); ++i) {
    bag_t bu = elimination_bag.at(variables[i].u);
    bag_t bv = elimination_bag.at(variables[i].v);
    bag_t b = bag_index(bu) < bag_index(bv) ? bu : bv;
    variables_at_bag[bag_index(b)].push_back(i);
  }

  // ------------------------------------------------------------------
  // 3. Gate-emission helpers.
  // ------------------------------------------------------------------
  const gate_t invalid_gate{static_cast<std::underlying_type<gate_t>::type>(-1)};
  // Every emitted OR is deterministic and every emitted AND decomposable
  // *by construction*; mark them with the d-DNNF certificate so the
  // certificate-aware consumers (independentEvaluation, interpretAsDD)
  // can evaluate the circuit linearly.
  const gate_t true_gate = dd.setGate(BooleanGate::AND); // empty AND = true
  dd.setInfo(true_gate, DNNF_CERT_INFO);

  std::vector<gate_t> var_in(variables.size(), invalid_gate);
  std::vector<gate_t> var_not(variables.size(), invalid_gate);
  auto inGate = [&](std::size_t i) {
                  if (var_in[i] == invalid_gate)
                    var_in[i] = dd.setGate(variables[i].token, BooleanGate::IN,
                                           variables[i].prob);
                  return var_in[i];
                };
  auto notGate = [&](std::size_t i) {
                   if (var_not[i] == invalid_gate) {
                     var_not[i] = dd.setGate(BooleanGate::NOT);
                     dd.addWire(var_not[i], inGate(i));
                   }
                   return var_not[i];
                 };
  auto andGate = [&](gate_t a, gate_t b) {
                   if (a == true_gate)
                     return b;
                   if (b == true_gate)
                     return a;
                   gate_t g = dd.setGate(BooleanGate::AND);
                   dd.setInfo(g, DNNF_CERT_INFO);
                   dd.addWire(g, a);
                   dd.addWire(g, b);
                   return g;
                 };

  // A DP table maps each reachable state (a closed relation over the
  // node's domain) to the gate computing "the processed valuation
  // induces exactly this state"; an accumulator collects the (mutually
  // exclusive) contributions to each state before they are OR-ed.
  using Table = std::unordered_map<Rel, gate_t>;
  using Accumulator = std::unordered_map<Rel, std::vector<gate_t> >;

  auto finalize = [&](Accumulator &acc) {
                    Table t;
                    t.reserve(acc.size());
                    for (auto &entry : acc) {
                      if (entry.second.size() == 1)
                        t.emplace(entry.first, entry.second[0]);
                      else {
                        // Deterministic OR: the contributions partition the
                        // worlds inducing this state.
                        gate_t g = dd.setGate(BooleanGate::OR);
                        dd.setInfo(g, DNNF_CERT_INFO);
                        for (gate_t c : entry.second)
                          dd.addWire(g, c);
                        t.emplace(entry.first, g);
                      }
                    }
                    acc.clear();
                    return t;
                  };

  // ------------------------------------------------------------------
  // 4. Domains.  The DP works over the decomposition obtained by adding
  //    the two terminals to every bag (still a valid tree decomposition,
  //    of width at most tw+2), so each node's domain is its bag plus
  //    {s, t} and the (s -> t) bit of a state is meaningful at the root.
  // ------------------------------------------------------------------
  auto domainOf = [&](bag_t b) {
                    std::vector<unsigned long> d;
                    d.reserve(td.getBag(b).size()+2);
                    for (gate_t g : td.getBag(b))
                      d.push_back(static_cast<std::underlying_type<gate_t>::type>(g));
                    d.push_back(source);
                    d.push_back(target);
                    std::sort(d.begin(), d.end());
                    d.erase(std::unique(d.begin(), d.end()), d.end());
                    return d;
                  };
  auto positionIn = [](const std::vector<unsigned long> &domain, unsigned long v) {
                      return static_cast<int>(
                        std::lower_bound(domain.begin(), domain.end(), v) -
                        domain.begin());
                    };
  auto identityOn = [](int d) {
                      Rel r;
                      for (int i = 0; i < d; ++i)
                        r.set(i*MAXD + i);
                      return r;
                    };

  // Lift a child table to the parent's domain: forget the vertices that
  // leave the bag (restriction of a closed relation stays closed; any
  // path through a forgotten vertex between surviving vertices was
  // already recorded by closure) and introduce the fresh ones with
  // identity only.  States may collapse, hence the accumulator.
  auto lift = [&](const Table &child, const std::vector<unsigned long> &from,
                  const std::vector<unsigned long> &to) {
                int df = static_cast<int>(from.size());
                int dt = static_cast<int>(to.size());
                std::vector<int> map(from.size());
                for (int i = 0; i < df; ++i) {
                  auto it = std::lower_bound(to.begin(), to.end(), from[i]);
                  map[i] = (it != to.end() && *it == from[i])
                           ? static_cast<int>(it - to.begin()) : -1;
                }
                const Rel id = identityOn(dt);
                Accumulator acc;
                for (const auto &entry : child) {
                  Rel r = id;
                  for (int i = 0; i < df; ++i) {
                    if (map[i] < 0)
                      continue;
                    for (int j = 0; j < df; ++j)
                      if (map[j] >= 0 && entry.first[i*MAXD + j])
                        r.set(map[i]*MAXD + map[j]);
                  }
                  acc[r].push_back(entry.second);
                }
                return finalize(acc);
              };

  auto checkStateBound = [&](std::size_t n) {
                           result.stats.max_states = std::max(result.stats.max_states, n);
                           if (n > max_states)
                             throw ReachabilityCompilerException(
                                     "state space exceeds the per-node bound (" +
                                     std::to_string(max_states) +
                                     "); the data treewidth is too large for "
                                     "reachability compilation");
                         };

  // ------------------------------------------------------------------
  // 5. Bottom-up DP over the decomposition, explicit stack.
  // ------------------------------------------------------------------
  struct Frame {
    bag_t bag;
    std::size_t next_child = 0;
    Table table;            // join of the already-merged children, lifted here
    bool has_table = false;
    explicit Frame(bag_t b) : bag(b) {
    }
  };

  std::vector<Frame> stack;
  stack.push_back(Frame{td.getRoot()});

  Table root_table;
  std::vector<unsigned long> root_domain;

  while (!stack.empty()) {
    Frame &frame = stack.back();
    const auto &children = td.getChildren(frame.bag);

    if (frame.next_child < children.size()) {
      bag_t c = children[frame.next_child++];
      stack.push_back(Frame{c});
      continue;
    }

    CHECK_FOR_INTERRUPTS();

    // All children merged; finish this node.
    std::vector<unsigned long> domain = domainOf(frame.bag);
    const int d = static_cast<int>(domain.size());

    Table table = frame.has_table
                  ? std::move(frame.table)
                  : Table{{identityOn(d), true_gate}};

    // Introduce the edge variables assigned to this bag.
    for (std::size_t vi : variables_at_bag[bag_index(frame.bag)]) {
      const EdgeVariable &var = variables[vi];
      const int pu = positionIn(domain, var.u);
      const int pv = positionIn(domain, var.v);

      Accumulator acc;
      for (const auto &entry : table) {
        Rel present = entry.first;
        if (var.arc_uv)
          present.set(pu*MAXD + pv);
        if (var.arc_vu)
          present.set(pv*MAXD + pu);
        present = transitiveClosure(present, d);

        if (present == entry.first) {
          // The edge cannot change reachability in these worlds: its
          // value is irrelevant, keep the gate as is (the OR of the
          // two cofactors would simplify to it anyway).
          acc[entry.first].push_back(entry.second);
        } else {
          acc[present].push_back(andGate(entry.second, inGate(vi)));
          acc[entry.first].push_back(andGate(entry.second, notGate(vi)));
        }
      }
      table = finalize(acc);
      checkStateBound(table.size());
    }

    // Hand the finished table to the parent (or keep it if root).
    if (stack.size() == 1) {
      root_table = std::move(table);
      root_domain = std::move(domain);
      stack.pop_back();
      break;
    }

    Frame &parent = stack[stack.size()-2];
    std::vector<unsigned long> parent_domain = domainOf(parent.bag);
    Table lifted = lift(table, domain, parent_domain);
    checkStateBound(lifted.size());

    if (!parent.has_table) {
      parent.table = std::move(lifted);
      parent.has_table = true;
    } else {
      // Join: the two subtrees' variable sets are disjoint (each
      // variable has a unique introduction bag), so the AND gates are
      // decomposable; pairs of child states are mutually exclusive, so
      // the resulting ORs stay deterministic.  Reachability across the
      // two processed parts only alternates through domain vertices
      // (the bag separates the parts), hence the closure of the union.
      const int dp = static_cast<int>(parent_domain.size());
      Accumulator acc;
      for (const auto &left : parent.table)
        for (const auto &right : lifted) {
          CHECK_FOR_INTERRUPTS();
          Rel r = transitiveClosure(left.first | right.first, dp);
          acc[r].push_back(andGate(left.second, right.second));
        }
      parent.table = finalize(acc);
      checkStateBound(parent.table.size());
    }

    stack.pop_back();
  }

  // ------------------------------------------------------------------
  // 6. Output: t reachable from s iff the root state has the (s -> t)
  //    bit; the accepting states are mutually exclusive, so the final
  //    OR is deterministic.
  // ------------------------------------------------------------------
  const int ps = positionIn(root_domain, source);
  const int pt = positionIn(root_domain, target);

  std::vector<gate_t> accepting;
  for (const auto &entry : root_table)
    if (entry.first[ps*MAXD + pt])
      accepting.push_back(entry.second);

  gate_t output;
  if (accepting.empty()) {
    output = dd.setGate(BooleanGate::OR); // empty OR = false
    dd.setInfo(output, DNNF_CERT_INFO);
  } else if (accepting.size() == 1)
    output = accepting[0];
  else {
    output = dd.setGate(BooleanGate::OR);
    dd.setInfo(output, DNNF_CERT_INFO);
    for (gate_t g : accepting)
      dd.addWire(output, g);
  }
  dd.setRoot(output);

  result.stats.nb_gates = dd.getNbGates();
  return result;
}
