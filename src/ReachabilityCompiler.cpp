/**
 * @file ReachabilityCompiler.cpp
 * @brief Implementation of the decomposition-aligned reachability compiler.
 *
 * See @c ReachabilityCompiler.h for the construction's design and the
 * structural argument (determinism and decomposability by construction).
 *
 * The shared implementation makes two sweeps over a min-fill tree
 * decomposition of the data graph, both with explicit stacks
 * (decompositions of path-like data are themselves path-like, so
 * recursion depth would be linear in the data):
 *
 * - **bottom-up**: for every node, the table mapping each reachable
 *   *below state* -- the transitively-closed reachability relation over
 *   the node's domain induced by the edges introduced in its subtree --
 *   to the gate computing "the subtree edges induce exactly this state";
 * - **top-down**: symmetrically, the *above state* tables over the edges
 *   introduced outside the subtree, derived from the parent's above
 *   table joined with the sibling subtrees' below tables (prefix/suffix
 *   joins keep this linear in the node arity) and the parent's local
 *   edges.
 *
 * The domain of every node is its bag plus the source vertex
 * (equivalently, the DP runs on the decomposition with the source added
 * to every bag, still a valid decomposition of width at most tw+1).
 * Each vertex is then read at its elimination bag: below and above
 * states partition the worlds by their disjoint edge sets, so "the
 * closure of (below ∪ above) connects the source to the vertex" is a
 * deterministic OR over decomposable AND pairs -- one linear-size
 * certified d-DNNF whose gates are shared across all the per-vertex
 * roots.
 */
#include "ReachabilityCompiler.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
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
  bool certain = false; ///< Always-present arc(s): no gating variable (super-source arcs of untracked / constant sources).
  std::string token;    ///< Provenance token (UUID; empty when certain).
  double prob;          ///< Tuple probability (unused when certain).
};

} // namespace

ReachabilityCompiler::Rel ReachabilityCompiler::transitiveClosure(Rel r, int d)
{
  // Warshall over the first d positions; d <= MAXD, so this is a small
  // constant amount of work per call.
  for (int k = 0; k < d; ++k)
    for (int i = 0; i < d; ++i)
      if (r[i*MAXD + k])
        for (int j = 0; j < d; ++j)
          if (r[k*MAXD + j])
            r.set(i*MAXD + j);
  return r;
}

ReachabilityCompiler::AllResult ReachabilityCompiler::compileAllInternal(
  const std::vector<EdgeRow> &rows,
  unsigned long source,
  bool directed,
  std::size_t max_states,
  const unsigned long *only_target,
  const std::vector<SourceArc> *multi_sources)
{
  AllResult result;
  dDNNF &dd = result.dd;

  // Multi-source mode: reachability is from a virtual super-source whose
  // arcs to the given sources are ordinary (or certain) directed edge
  // variables; everything downstream is the single-source DP.  The
  // super-source gets an ID above every real vertex.
  unsigned long super_source = 0;
  if (multi_sources) {
    if (multi_sources->empty())
      throw ReachabilityCompilerException("no sources given");
    for (const auto &row : rows)
      super_source = std::max({super_source, row.src, row.dst});
    for (const auto &sa : *multi_sources)
      super_source = std::max(super_source, sa.vertex);
    ++super_source;
    source = super_source;
  }

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
  if (multi_sources) {
    // Source arcs: super-source -> vertex, gated by the source tuple's
    // token (one variable per token; a duplicate token must target the
    // same vertex) or always present for certain sources (dedup'd).  A
    // token sharing with an *edge* variable would couple the source to
    // an edge and break decomposability: rejected.
    std::unordered_set<std::string> edge_tokens;
    for (const auto &var : variables)
      if (!var.certain)
        edge_tokens.insert(var.token);
    std::unordered_map<std::string, std::size_t> token_to_var;
    std::unordered_set<unsigned long> certain_done;
    for (const auto &sa : *multi_sources) {
      if (sa.certain) {
        if (!certain_done.insert(sa.vertex).second)
          continue;
        EdgeVariable var;
        var.u = super_source;
        var.v = sa.vertex;
        var.arc_uv = true;
        var.arc_vu = false;
        var.certain = true;
        variables.push_back(var);
      } else {
        if (edge_tokens.find(sa.token) != edge_tokens.end())
          throw ReachabilityCompilerException(
                  "provenance token " + sa.token +
                  " is shared between a source and an edge");
        auto it = token_to_var.find(sa.token);
        if (it != token_to_var.end()) {
          if (variables[it->second].v != sa.vertex)
            throw ReachabilityCompilerException(
                    "provenance token " + sa.token +
                    " is shared by sources with different vertices");
          continue;
        }
        EdgeVariable var;
        var.u = super_source;
        var.v = sa.vertex;
        var.arc_uv = true;
        var.arc_vu = false;
        var.token = sa.token;
        var.prob = sa.prob;
        token_to_var[sa.token] = variables.size();
        variables.push_back(var);
      }
    }
  }
  for (const auto &var : variables)
    if (!var.certain)
      ++result.stats.nb_variables;

  // ------------------------------------------------------------------
  // 2. Tree decomposition of the data graph (vertices: all endpoints
  //    plus the source, plus an explicitly requested target so an
  //    isolated target is legal), by min-fill elimination.
  // ------------------------------------------------------------------
  Graph graph;
  graph.add_node(source);
  if (only_target)
    graph.add_node(*only_target);
  for (const auto &var : variables)
    graph.add_edge(var.u, var.v);

  /* No degeneracy pre-probe here, deliberately: it was implemented and
   * measured (TreeDecomposition::degeneracyLowerBound now accepts a
   * Graph for that purpose), but min-fill's own abort -- the first
   * elimination whose neighbourhood exceeds the cap -- rejects every
   * adversarial family tried (cliques, supercritical random graphs) at
   * least as fast as the O(V+E) peel, while an always-on probe would tax
   * every *accepted* compilation by a linear pass.  See the
   * bounded-treewidth TODO for the numbers. */
  std::unordered_map<unsigned long, bag_t> elimination_bag;
  const TreeDecomposition td(std::move(graph), &elimination_bag);
  result.stats.data_treewidth = td.getTreewidth();
  result.stats.nb_bags = td.getNbBags();
  const std::size_t nb_bags = td.getNbBags();

  // Each variable is introduced at exactly one node: the bag created
  // when the earlier-eliminated endpoint was eliminated contains both
  // endpoints (elimination invariant), and a unique introduction point
  // is what makes the emitted AND gates decomposable.
  std::vector<std::vector<std::size_t> > variables_at_bag(nb_bags);
  for (std::size_t i = 0; i < variables.size(); ++i) {
    bag_t bu = elimination_bag.at(variables[i].u);
    bag_t bv = elimination_bag.at(variables[i].v);
    bag_t b = bag_index(bu) < bag_index(bv) ? bu : bv;
    variables_at_bag[bag_index(b)].push_back(i);
  }

  // Read points: every vertex is read at its elimination bag (which
  // contains it); a single-target compilation reads only that vertex.
  std::vector<std::vector<unsigned long> > reads_at_bag(nb_bags);
  for (const auto &[v, b] : elimination_bag) {
    if (multi_sources && v == super_source)
      continue;   // the virtual super-source is not a user vertex
    if (!only_target || v == *only_target)
      reads_at_bag[bag_index(b)].push_back(v);
  }

  // ------------------------------------------------------------------
  // 3. Gate-emission helpers.
  //
  // Every emitted OR is deterministic and every emitted AND decomposable
  // *by construction*; mark them with the d-DNNF certificate so the
  // certificate-aware consumers (independentEvaluation, interpretAsDD)
  // can evaluate the circuit linearly.
  // ------------------------------------------------------------------
  const gate_t invalid_gate{static_cast<std::underlying_type<gate_t>::type>(-1)};
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
  // node's domain) to the gate computing "this part's valuation induces
  // exactly this state"; an accumulator collects the (mutually
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
                    result.stats.max_states =
                      std::max(result.stats.max_states, t.size());
                    if (t.size() > max_states)
                      throw ReachabilityCompilerException(
                              "state space exceeds the per-node bound (" +
                              std::to_string(max_states) +
                              "); the data treewidth is too large for "
                              "reachability compilation");
                    return t;
                  };

  // ------------------------------------------------------------------
  // 4. Domains.  Every node's domain is its bag plus the source (the DP
  //    runs on the decomposition with the source added to every bag,
  //    still a valid tree decomposition).
  // ------------------------------------------------------------------
  std::vector<std::vector<unsigned long> > domains(nb_bags);
  for (std::size_t b = 0; b < nb_bags; ++b) {
    auto &d = domains[b];
    d.reserve(td.getBag(bag_t{b}).size()+1);
    for (gate_t g : td.getBag(bag_t{b}))
      d.push_back(static_cast<std::underlying_type<gate_t>::type>(g));
    d.push_back(source);
    std::sort(d.begin(), d.end());
    d.erase(std::unique(d.begin(), d.end()), d.end());
  }

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
  auto trivialTable = [&](const std::vector<unsigned long> &domain) {
                        return Table{{identityOn(static_cast<int>(domain.size())),
                                      true_gate}};
                      };
  auto isTrivial = [&](const Table &t, int d) {
                     // A table is a join identity only if it is the single
                     // always-true *identity-relation* state: a certain arc
                     // produces single-state TRUE tables whose relation is
                     // not the identity, and dropping those in join() would
                     // lose the arc.
                     return t.size() == 1 && t.begin()->second == true_gate &&
                            t.begin()->first == identityOn(d);
                   };

  // Re-express a table over another domain: forget the vertices that
  // leave (restriction of a closed relation stays closed; any path
  // through a forgotten vertex between surviving vertices was already
  // recorded by closure) and introduce the fresh ones with identity
  // only.  States may collapse, hence the accumulator.
  auto lift = [&](const Table &t, const std::vector<unsigned long> &from,
                  const std::vector<unsigned long> &to) {
                if (from == to)
                  return t;
                const int df = static_cast<int>(from.size());
                const int dt = static_cast<int>(to.size());
                std::vector<int> map(from.size());
                for (int i = 0; i < df; ++i) {
                  auto it = std::lower_bound(to.begin(), to.end(), from[i]);
                  map[i] = (it != to.end() && *it == from[i])
                           ? static_cast<int>(it - to.begin()) : -1;
                }
                const Rel id = identityOn(dt);
                Accumulator acc;
                for (const auto &entry : t) {
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

  // Join two tables over the same domain, covering disjoint edge sets:
  // pairs of states are mutually exclusive (deterministic ORs) and the
  // gates variable-disjoint (decomposable ANDs); reachability across
  // the two parts only alternates through domain vertices (the bag
  // separates them), hence the closure of the union.
  auto join = [&](const Table &t1, const Table &t2, int d) {
                if (isTrivial(t1, d))
                  return t2;
                if (isTrivial(t2, d))
                  return t1;
                Accumulator acc;
                for (const auto &left : t1)
                  for (const auto &right : t2) {
                    CHECK_FOR_INTERRUPTS();
                    Rel r = transitiveClosure(left.first | right.first, d);
                    acc[r].push_back(andGate(left.second, right.second));
                  }
                return finalize(acc);
              };

  // Introduce the edge variables assigned to bag b into a table over
  // that bag's domain.
  auto applyEdges = [&](Table table, std::size_t b) {
                      const auto &domain = domains[b];
                      const int d = static_cast<int>(domain.size());
                      for (std::size_t vi : variables_at_bag[b]) {
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

                          if (var.certain) {
                            // Always-present arc: every world of this state
                            // moves to the augmented relation, no branching
                            // (states may merge; their gates stay mutually
                            // exclusive).
                            acc[present].push_back(entry.second);
                            continue;
                          }
                          if (present == entry.first) {
                            // The edge cannot change reachability in these
                            // worlds: its value is irrelevant, keep the gate
                            // as is (the OR of the two cofactors would
                            // simplify to it anyway).
                            acc[entry.first].push_back(entry.second);
                          } else {
                            acc[present].push_back(
                              andGate(entry.second, inGate(vi)));
                            acc[entry.first].push_back(
                              andGate(entry.second, notGate(vi)));
                          }
                        }
                        table = finalize(acc);
                      }
                      return table;
                    };

  // ------------------------------------------------------------------
  // 5. Bottom-up sweep: below[b] = state table of bag b's subtree
  //    (children joined, local edges applied), retained for the
  //    top-down sweep and the reads.
  // ------------------------------------------------------------------
  std::vector<Table> below(nb_bags);
  {
    struct Frame {
      bag_t bag;
      std::size_t next_child = 0;
      Table table;
      bool has_table = false;
      explicit Frame(bag_t b) : bag(b) {
      }
    };

    std::vector<Frame> stack;
    stack.push_back(Frame(td.getRoot()));

    while (!stack.empty()) {
      Frame &frame = stack.back();
      const auto &children = td.getChildren(frame.bag);

      if (frame.next_child < children.size()) {
        bag_t c = children[frame.next_child++];
        stack.push_back(Frame(c));
        continue;
      }

      CHECK_FOR_INTERRUPTS();

      const std::size_t b = bag_index(frame.bag);
      Table table = frame.has_table ? std::move(frame.table)
                    : trivialTable(domains[b]);
      below[b] = applyEdges(std::move(table), b);

      if (stack.size() == 1) {
        stack.pop_back();
        break;
      }

      // Merge into the parent's partial join.
      Frame &parent = stack[stack.size()-2];
      const std::size_t pb = bag_index(parent.bag);
      Table lifted = lift(below[b], domains[b], domains[pb]);
      if (!parent.has_table) {
        parent.table = std::move(lifted);
        parent.has_table = true;
      } else {
        parent.table = join(parent.table, lifted,
                            static_cast<int>(domains[pb].size()));
      }
      stack.pop_back();
    }
  }

  // ------------------------------------------------------------------
  // 6. Top-down sweep and reads.  above[b] covers exactly the edges
  //    introduced outside bag b's subtree; for a child c of b,
  //    above(c) = lift( applyEdges_b( above(b) ⊗ siblings' below ) ).
  //    Reads at b pair below[b] with above[b]: the closure of the union
  //    is the full-graph reachability relation over b's domain.
  // ------------------------------------------------------------------
  std::unordered_map<unsigned long, gate_t> vertex_root;
  {
    std::vector<Table> above(nb_bags);
    const std::size_t rb = bag_index(td.getRoot());
    above[rb] = trivialTable(domains[rb]);

    std::vector<bag_t> stack{td.getRoot()};
    while (!stack.empty()) {
      const bag_t nu = stack.back();
      stack.pop_back();
      CHECK_FOR_INTERRUPTS();
      const std::size_t b = bag_index(nu);
      const int d = static_cast<int>(domains[b].size());

      // Reads: for every (below, above) state pair, the closure of the
      // union; the pairs partition the worlds, so each vertex's OR over
      // its accepting pairs is deterministic.
      if (!reads_at_bag[b].empty()) {
        const int ps = positionIn(domains[b], source);
        std::unordered_map<unsigned long, std::vector<gate_t> > accepting;
        for (const auto &[R, g] : below[b])
          for (const auto &[A, h] : above[b]) {
            CHECK_FOR_INTERRUPTS();
            const Rel closed = transitiveClosure(R | A, d);
            gate_t pair_gate = invalid_gate;   // lazily created, shared
            for (unsigned long v : reads_at_bag[b]) {
              if (!closed[ps*MAXD + positionIn(domains[b], v)])
                continue;
              if (pair_gate == invalid_gate)
                pair_gate = andGate(g, h);
              accepting[v].push_back(pair_gate);
            }
          }
        for (auto &[v, gates] : accepting) {
          if (gates.size() == 1)
            vertex_root[v] = gates[0];
          else {
            gate_t o = dd.setGate(BooleanGate::OR);
            dd.setInfo(o, DNNF_CERT_INFO);
            for (gate_t g : gates)
              dd.addWire(o, g);
            vertex_root[v] = o;
          }
        }
      }

      // Children: prefix/suffix joins of the lifted sibling tables keep
      // this linear in the arity.
      const auto &children = td.getChildren(nu);
      const std::size_t m = children.size();
      if (m > 0) {
        std::vector<Table> lifted(m);
        for (std::size_t i = 0; i < m; ++i)
          lifted[i] = lift(below[bag_index(children[i])],
                           domains[bag_index(children[i])], domains[b]);

        std::vector<Table> prefix(m+1), suffix(m+1);
        prefix[0] = trivialTable(domains[b]);
        for (std::size_t i = 0; i < m; ++i)
          prefix[i+1] = join(prefix[i], lifted[i], d);
        suffix[m] = trivialTable(domains[b]);
        for (std::size_t i = m; i-- > 0; )
          suffix[i] = join(lifted[i], suffix[i+1], d);

        for (std::size_t i = 0; i < m; ++i) {
          Table siblings = join(prefix[i], suffix[i+1], d);
          Table a = applyEdges(join(above[b], siblings, d), b);
          const std::size_t cb = bag_index(children[i]);
          above[cb] = lift(a, domains[b], domains[cb]);
          stack.push_back(children[i]);
        }
      }

      // above[b] is no longer needed (children got theirs, reads done).
      above[b] = Table();
    }
  }

  result.roots.reserve(vertex_root.size());
  for (const auto &[v, g] : vertex_root)
    result.roots.push_back(VertexRoot{v, g});
  std::sort(result.roots.begin(), result.roots.end(),
            [](const VertexRoot &a, const VertexRoot &b) {
    return a.vertex < b.vertex;
  });

  dd.setRoot(true_gate);   // single-target callers re-point this
  result.stats.nb_gates = dd.getNbGates();
  return result;
}

ReachabilityCompiler::AllResult ReachabilityCompiler::compileAll(
  const std::vector<EdgeRow> &rows,
  unsigned long source,
  bool directed,
  std::size_t max_states)
{
  return compileAllInternal(rows, source, directed, max_states, nullptr,
                            nullptr);
}

ReachabilityCompiler::AllResult ReachabilityCompiler::compileAll(
  const std::vector<EdgeRow> &rows,
  const std::vector<SourceArc> &sources,
  bool directed,
  std::size_t max_states)
{
  return compileAllInternal(rows, 0, directed, max_states, nullptr, &sources);
}

ReachabilityCompiler::Result ReachabilityCompiler::compile(
  const std::vector<EdgeRow> &rows,
  unsigned long source,
  unsigned long target,
  bool directed,
  std::size_t max_states)
{
  AllResult all = compileAllInternal(rows, source, directed, max_states,
                                     &target, nullptr);
  Result result;
  result.stats = all.stats;

  gate_t root{0};
  bool found = false;
  for (const auto &vr : all.roots)
    if (vr.vertex == target) {
      root = vr.root;
      found = true;
      break;
    }
  if (!found) {
    // The target is certainly unreachable: constant false.
    root = all.dd.setGate(BooleanGate::OR);
    all.dd.setInfo(root, DNNF_CERT_INFO);
    result.stats.nb_gates = all.dd.getNbGates();
  }
  all.dd.setRoot(root);
  result.dd = std::move(all.dd);
  return result;
}
