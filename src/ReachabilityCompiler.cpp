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
 *   *below state* -- the state over the node's domain induced by the
 *   edges introduced in its subtree -- to the gate computing "the
 *   subtree edges induce exactly this state";
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
 * states partition the worlds by their disjoint edge sets, so the
 * acceptance test over the combination of the two states is a
 * deterministic OR over decomposable AND pairs -- one linear-size
 * certified d-DNNF whose gates are shared across all the per-vertex
 * roots.
 *
 * The DP scaffold is generic over the *state algebra* (the @c Ops
 * template parameter below).  Two instantiations:
 *
 * - @c BoolOps -- plain reachability: the state is the transitively
 *   closed reachability relation over the domain (a bitset), composed
 *   by Warshall closure.  This is the historical behaviour.
 * - @c HopOps -- bounded-hop reachability: each relation entry is the
 *   *set of achievable walk lengths* up to the hop bound (a bitmask),
 *   composed in the capped min-plus-set semiring by the algebraic-path
 *   (Floyd-Warshall-Kleene) algorithm with diagonal star.  Worlds still
 *   map to exactly one state, so states partition worlds and every
 *   emitted OR remains deterministic; each edge variable is still
 *   introduced at one node, so ANDs remain decomposable.
 */
#include "ReachabilityCompiler.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <map>
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

/** @brief Maximum size of a DP domain: a bag (@c MAX_TREEWIDTH+1) plus the two terminals. */
constexpr int MAXD = TreeDecomposition::MAX_TREEWIDTH+3;

/** @brief One edge variable: a provenance token gating one or two arcs between two vertices. */
struct EdgeVariable {
  unsigned long u;      ///< First endpoint.
  unsigned long v;      ///< Second endpoint.
  bool arc_uv;          ///< Arc u -> v present when the variable is true.
  bool arc_vu;          ///< Arc v -> u present when the variable is true.
  bool certain = false; ///< Always-present arc(s): no gating variable (super-source arcs of untracked / constant sources).
  unsigned weight = 1;  ///< Walk-length contribution (0 for super-source arcs; only the hop mode reads it).
  std::string token;    ///< Provenance token (UUID; empty when certain).
  double prob;          ///< Tuple probability (unused when certain).
};

/** @brief One alternative of a BID block: an arc present iff its @c mulinput outcome is drawn. */
struct BlockAlternative {
  unsigned long u;      ///< First endpoint.
  unsigned long v;      ///< Second endpoint.
  bool arc_uv;          ///< Arc u -> v in this outcome.
  bool arc_vu;          ///< Arc v -> u in this outcome.
  std::string token;    ///< The @c mulinput token (the outcome's literal).
  double prob;          ///< Outcome probability.
  unsigned index;       ///< Outcome index within the block.
};

/** @brief A block of mutually exclusive arc alternatives (one @c repair_key block). */
struct EdgeBlock {
  std::string key;                          ///< The block's key variable (MULVAR UUID).
  std::vector<BlockAlternative> alts;       ///< The alternatives.
  std::vector<unsigned long> endpoints;     ///< Distinct non-self-loop endpoints (must share a bag).
};

// ---------------------------------------------------------------------
// State algebras.
// ---------------------------------------------------------------------

/**
 * @brief Plain-reachability state algebra.
 *
 * The state is a reachability relation over the first @c d domain
 * positions, as a bitset: bit @c i*MAXD+j set iff position @c j is
 * reachable from position @c i within the processed part of the graph.
 * Always reflexive and transitively closed.
 */
struct BoolOps {
  static constexpr bool tracks_lengths = false;
  using State = std::bitset<MAXD*MAXD>;
  using Entry = bool;
  /** @brief Hash functor (delegates to @c std::hash of the bitset). */
  struct Hash {
    std::size_t operator()(const State &s) const noexcept {
      return std::hash<State>()(s);
    }
  };

  /** @brief The identity (reflexive, empty) relation over @p d positions. */
  State identity(int d) const {
    State r;
    for (int i = 0; i < d; ++i)
      r.set(i*MAXD + i);
    return r;
  }
  /** @brief Warshall transitive closure over the first @p d positions, in place. */
  void close(State &r, int d) const {
    for (int k = 0; k < d; ++k)
      for (int i = 0; i < d; ++i)
        if (r[i*MAXD + k])
          for (int j = 0; j < d; ++j)
            if (r[k*MAXD + j])
              r.set(i*MAXD + j);
  }
  /** @brief Entrywise union of @p b into @p a (caller closes afterwards). */
  void unite(State &a, const State &b) const {
    a |= b;
  }
  /** @brief Record the arc @p pu -> @p pv (weight is a hop-mode concept). */
  void addArc(State &s, int pu, int pv, unsigned /*weight*/) const {
    s.set(pu*MAXD + pv);
  }
  /** @brief Read entry (@p i, @p j). */
  Entry get(const State &s, int i, int j) const {
    return s[i*MAXD + j];
  }
  /** @brief Whether an entry carries no information. */
  static bool emptyEntry(Entry e) {
    return !e;
  }
  /** @brief Merge an entry into (@p i, @p j) (used by domain re-expression). */
  void merge(State &s, int i, int j, Entry /*e*/) const {
    s.set(i*MAXD + j);
  }
};

/**
 * @brief Bounded-hop state algebra.
 *
 * Each relation entry is the set of achievable walk lengths between two
 * domain positions within the processed part of the graph, as a bitmask
 * over lengths @c 0..bound (so the diagonal carries bit 0).  Entries
 * compose by capped Minkowski sum -- @c mink(a,b) is the set of sums of
 * a length in @p a and one in @p b, lengths above the bound dropped --
 * and the closure is the algebraic-path (Kleene) algorithm with the
 * diagonal star, exact in this finite idempotent semiring.
 */
struct HopOps {
  static constexpr bool tracks_lengths = true;
  using Entry = std::uint64_t;
  /** @brief A matrix of length-set bitmasks (unused cells stay zero). */
  struct State {
    std::array<Entry, MAXD*MAXD> m;
    State() : m{} {
    }
    bool operator==(const State &o) const {
      return m == o.m;
    }
  };
  /** @brief FNV-1a over the matrix words. */
  struct Hash {
    std::size_t operator()(const State &s) const noexcept {
      std::uint64_t h = 1469598103934665603ull;
      for (Entry e : s.m) {
        h ^= e;
        h *= 1099511628211ull;
      }
      return static_cast<std::size_t>(h);
    }
  };

  Entry full;   ///< Mask of representable lengths: bits 0..bound.

  /** @brief Construct for walk lengths up to @p bound (at most 62). */
  explicit HopOps(unsigned bound)
    : full(bound >= 63 ? ~Entry(0) : ((Entry(1) << (bound+1)) - 1)) {
  }

  /** @brief Capped Minkowski sum of two length sets. */
  Entry mink(Entry a, Entry b) const {
    Entry r = 0;
    while (b) {
      const int s = __builtin_ctzll(b);
      b &= b - 1;
      r |= (a << s);
    }
    return r & full;
  }
  /** @brief Kleene star of a diagonal length set (always contains 0). */
  Entry star(Entry diag) const {
    Entry s = 1;
    for (;;) {
      const Entry ns = s | mink(s, diag);
      if (ns == s)
        return s;
      s = ns;
    }
  }

  /** @brief Identity: length 0 on the diagonal, nothing elsewhere. */
  State identity(int d) const {
    State r;
    for (int i = 0; i < d; ++i)
      r.m[i*MAXD + i] = 1;
    return r;
  }
  /**
   * @brief Algebraic-path closure over the first @p d positions, in place.
   *
   * Standard Floyd-Warshall-Kleene: for each pivot @c k,
   * @c r[i][j] |= r[i][k] * star(r[k][k]) * r[k][j], reading the pivot
   * row and column as snapshots from before the pass.
   */
  void close(State &r, int d) const {
    Entry col[MAXD], row[MAXD];
    for (int k = 0; k < d; ++k) {
      const Entry sk = star(r.m[k*MAXD + k]);
      for (int i = 0; i < d; ++i) {
        col[i] = r.m[i*MAXD + k];
        row[i] = mink(sk, r.m[k*MAXD + i]);
      }
      for (int i = 0; i < d; ++i) {
        if (!col[i])
          continue;
        for (int j = 0; j < d; ++j)
          if (row[j])
            r.m[i*MAXD + j] |= mink(col[i], row[j]);
      }
    }
  }
  /** @brief Entrywise union of @p b into @p a (caller closes afterwards). */
  void unite(State &a, const State &b) const {
    for (std::size_t i = 0; i < a.m.size(); ++i)
      a.m[i] |= b.m[i];
  }
  /** @brief Record an arc of walk length @p weight (a no-op above the bound). */
  void addArc(State &s, int pu, int pv, unsigned weight) const {
    const Entry b = (weight < 64) ? (Entry(1) << weight) & full : 0;
    s.m[pu*MAXD + pv] |= b;
  }
  /** @brief Read entry (@p i, @p j). */
  Entry get(const State &s, int i, int j) const {
    return s.m[i*MAXD + j];
  }
  /** @brief Whether an entry carries no information. */
  static bool emptyEntry(Entry e) {
    return e == 0;
  }
  /** @brief Merge an entry into (@p i, @p j) (used by domain re-expression). */
  void merge(State &s, int i, int j, Entry e) const {
    s.m[i*MAXD + j] |= e;
  }
};

// ---------------------------------------------------------------------
// The generic DP.
// ---------------------------------------------------------------------

/**
 * @brief Run the decomposition-aligned DP and report the per-vertex reads.
 *
 * Shared scaffold of @c compileAll() / @c compileAllHops(): groups the
 * rows into edge variables and BID blocks, decomposes the data graph,
 * makes the bottom-up and top-down sweeps, and at every vertex's
 * elimination bag calls @p onRead once per accepting (below, above)
 * state pair with the pair's combined relation entry (source row,
 * vertex column) and the pair's AND gate.  The pairs partition the
 * worlds, so any OR the caller builds over a fixed predicate of the
 * reported entries is deterministic.
 *
 * @param rows           Edge tuples.
 * @param source         Source vertex (ignored in multi-source mode).
 * @param directed       If @c false, every edge contributes both arcs.
 * @param max_states     Bound on the DP state count per node.
 * @param only_target    When set: ensure the vertex exists in the graph
 *                       (an isolated target is legal) and read it alone.
 * @param multi_sources  When set: virtual super-source mode.
 * @param ops            The state algebra.
 * @param dd             Output circuit (gates are emitted into it).
 * @param stats          Output statistics.
 * @param onRead         Read callback: @c (vertex, entry, pair_gate).
 * @return               The constant-true gate (the empty AND).
 */
template<class Ops, class ReadSink>
gate_t runReachabilityDP(const std::vector<ReachabilityCompiler::EdgeRow> &rows,
                         unsigned long source,
                         bool directed,
                         std::size_t max_states,
                         const unsigned long *only_target,
                         const std::vector<ReachabilityCompiler::SourceArc> *multi_sources,
                         const Ops &ops,
                         dDNNF &dd,
                         ReachabilityCompiler::Stats &stats,
                         ReadSink onRead)
{
  using State = typename Ops::State;
  using Table = std::unordered_map<State, gate_t, typename Ops::Hash>;
  using Accumulator = std::unordered_map<State, std::vector<gate_t>,
                                         typename Ops::Hash>;

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
  // Self-loops never affect plain reachability and are dropped there,
  // but they *do* pump walk lengths -- the recursive fixpoint derives
  // (v, h+1) from a self-loop at v -- so the hop mode keeps them as
  // ordinary weight-1 arcs.
  // ------------------------------------------------------------------
  std::vector<EdgeVariable> variables;
  std::vector<EdgeBlock> blocks;
  {
    // BID blocks first: rows carrying a block key are mutually exclusive
    // alternatives of one (k+1)-way variable; their tokens must not be
    // shared with anything else, and a pure self-loop alternative keeps
    // its probability mass but contributes no arc.
    std::unordered_map<std::string, std::size_t> key_to_block;
    std::unordered_set<std::string> block_tokens;
    for (const auto &row : rows) {
      if (row.block_key.empty())
        continue;
      auto [it, fresh] = key_to_block.try_emplace(row.block_key, blocks.size());
      if (fresh) {
        EdgeBlock blk;
        blk.key = row.block_key;
        blocks.push_back(std::move(blk));
      }
      EdgeBlock &blk = blocks[it->second];
      if (!block_tokens.insert(row.token).second)
        throw ReachabilityCompilerException(
                "provenance token " + row.token +
                " appears on several block alternatives");
      BlockAlternative alt;
      alt.u = row.src;
      alt.v = row.dst;
      if (row.src != row.dst) {
        alt.arc_uv = true;
        alt.arc_vu = !directed;
      } else {
        // A self-loop alternative is a pure probability-mass outcome for
        // plain reachability, but a length-pumping arc in hop mode.
        alt.arc_uv = Ops::tracks_lengths;
        alt.arc_vu = false;
      }
      alt.token = row.token;
      alt.prob = row.prob;
      alt.index = row.block_index;
      if (alt.arc_uv || alt.arc_vu) {
        for (unsigned long e : {row.src, row.dst})
          if (std::find(blk.endpoints.begin(), blk.endpoints.end(), e) ==
              blk.endpoints.end())
            blk.endpoints.push_back(e);
      }
      blk.alts.push_back(std::move(alt));
    }

    std::unordered_map<std::string, std::size_t> token_to_var;
    for (const auto &row : rows) {
      if (!row.block_key.empty())
        continue; // handled above
      if (row.src == row.dst && !Ops::tracks_lengths)
        continue; // self-loop, irrelevant to plain reachability
      if (block_tokens.find(row.token) != block_tokens.end())
        throw ReachabilityCompilerException(
                "provenance token " + row.token +
                " is shared between a block alternative and an edge");

      auto it = token_to_var.find(row.token);
      if (it == token_to_var.end()) {
        EdgeVariable var;
        var.u = row.src;
        var.v = row.dst;
        var.arc_uv = true;
        var.arc_vu = !directed && row.src != row.dst;
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
    // an edge and break decomposability: rejected.  Source arcs carry
    // walk length zero: the reported lengths count graph edges only.
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
        var.weight = 0;
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
        var.weight = 0;
        var.token = sa.token;
        var.prob = sa.prob;
        token_to_var[sa.token] = variables.size();
        variables.push_back(var);
      }
    }
  }
  for (const auto &var : variables)
    if (!var.certain)
      ++stats.nb_variables;
  stats.nb_variables += blocks.size();

  // ------------------------------------------------------------------
  // 2. Tree decomposition of the data graph (vertices: all endpoints
  //    plus the source, plus an explicitly requested target so an
  //    isolated target is legal), by min-fill elimination.
  // ------------------------------------------------------------------
  Graph graph;
  graph.add_node(source);
  if (only_target)
    graph.add_node(*only_target);
  for (const auto &var : variables) {
    if (var.u != var.v)
      graph.add_edge(var.u, var.v);
    else
      graph.add_node(var.u);   // hop-mode self-loop: no Gaifman edge
  }
  for (const auto &blk : blocks) {
    // All endpoints of a block must share a bag (the whole block is
    // introduced at once): force it with a clique.  This is the honest
    // treewidth condition for BID data -- a block spanning many distant
    // vertices genuinely raises the width.
    for (std::size_t i = 0; i < blk.endpoints.size(); ++i)
      for (std::size_t j = i+1; j < blk.endpoints.size(); ++j)
        graph.add_edge(blk.endpoints[i], blk.endpoints[j]);
    if (blk.endpoints.size() == 1)
      graph.add_node(blk.endpoints[0]);
  }

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
  stats.data_treewidth = td.getTreewidth();
  stats.nb_bags = td.getNbBags();
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
  // A block is introduced at the bag of its earliest-eliminated endpoint,
  // which (by the clique above and the elimination invariant) contains
  // every endpoint of the block.  A block with no real arc is irrelevant
  // to reachability and is skipped entirely.
  std::vector<std::vector<std::size_t> > blocks_at_bag(nb_bags);
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    if (blocks[i].endpoints.empty())
      continue;
    bag_t best = elimination_bag.at(blocks[i].endpoints[0]);
    for (unsigned long e : blocks[i].endpoints) {
      bag_t be = elimination_bag.at(e);
      if (bag_index(be) < bag_index(best))
        best = be;
    }
    blocks_at_bag[bag_index(best)].push_back(i);
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
  std::vector<gate_t> block_mulvar(blocks.size(), invalid_gate);
  std::vector<std::vector<gate_t> > block_mulin(blocks.size());
  std::vector<gate_t> block_none(blocks.size(), invalid_gate);
  auto mulinGate = [&](std::size_t bi, std::size_t ai) {
                     if (block_mulin[bi].empty())
                       block_mulin[bi].assign(blocks[bi].alts.size(),
                                              invalid_gate);
                     if (block_mulin[bi][ai] == invalid_gate) {
                       if (block_mulvar[bi] == invalid_gate)
                         block_mulvar[bi] =
                           dd.setGate(blocks[bi].key, BooleanGate::MULVAR);
                       const auto &alt = blocks[bi].alts[ai];
                       gate_t m = dd.setGate(alt.token, BooleanGate::MULIN,
                                             alt.prob);
                       dd.setInfo(m, alt.index);
                       dd.addWire(m, block_mulvar[bi]);
                       block_mulin[bi][ai] = m;
                     }
                     return block_mulin[bi][ai];
                   };
  auto noneGate = [&](std::size_t bi) {
                    if (block_none[bi] == invalid_gate) {
                      // "No alternative drawn": NOT over the (deterministic:
                      // the alternatives are mutually exclusive) OR of the
                      // block's mulinput literals; probability 1 - sum p_i.
                      gate_t o = dd.setGate(BooleanGate::OR);
                      dd.setInfo(o, DNNF_CERT_INFO);
                      for (std::size_t ai = 0; ai < blocks[bi].alts.size(); ++ai)
                        dd.addWire(o, mulinGate(bi, ai));
                      gate_t n = dd.setGate(BooleanGate::NOT);
                      dd.addWire(n, o);
                      block_none[bi] = n;
                    }
                    return block_none[bi];
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

  // A DP table maps each reachable state over the node's domain to the
  // gate computing "this part's valuation induces exactly this state";
  // an accumulator collects the (mutually exclusive) contributions to
  // each state before they are OR-ed.
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
                    stats.max_states = std::max(stats.max_states, t.size());
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
  auto trivialTable = [&](const std::vector<unsigned long> &domain) {
                        return Table{{ops.identity(static_cast<int>(domain.size())),
                                      true_gate}};
                      };
  auto isTrivial = [&](const Table &t, int d) {
                     // A table is a join identity only if it is the single
                     // always-true *identity-state* entry: a certain arc
                     // produces single-state TRUE tables whose state is
                     // not the identity, and dropping those in join() would
                     // lose the arc.
                     return t.size() == 1 && t.begin()->second == true_gate &&
                            t.begin()->first == ops.identity(d);
                   };

  // Re-express a table over another domain: forget the vertices that
  // leave (restriction of a closed state stays closed; any walk
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
                const State id = ops.identity(dt);
                Accumulator acc;
                for (const auto &entry : t) {
                  State r = id;
                  for (int i = 0; i < df; ++i) {
                    if (map[i] < 0)
                      continue;
                    for (int j = 0; j < df; ++j) {
                      if (map[j] < 0)
                        continue;
                      const auto e = ops.get(entry.first, i, j);
                      if (!Ops::emptyEntry(e))
                        ops.merge(r, map[i], map[j], e);
                    }
                  }
                  acc[r].push_back(entry.second);
                }
                return finalize(acc);
              };

  // Join two tables over the same domain, covering disjoint edge sets:
  // pairs of states are mutually exclusive (deterministic ORs) and the
  // gates variable-disjoint (decomposable ANDs); walks across the two
  // parts only alternate through domain vertices (the bag separates
  // them), hence the closure of the union.
  auto join = [&](const Table &t1, const Table &t2, int d) {
                if (isTrivial(t1, d))
                  return t2;
                if (isTrivial(t2, d))
                  return t1;
                Accumulator acc;
                for (const auto &left : t1)
                  for (const auto &right : t2) {
                    CHECK_FOR_INTERRUPTS();
                    State r = left.first;
                    ops.unite(r, right.first);
                    ops.close(r, d);
                    acc[std::move(r)].push_back(andGate(left.second,
                                                        right.second));
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
                          State present = entry.first;
                          if (var.arc_uv)
                            ops.addArc(present, pu, pv, var.weight);
                          if (var.arc_vu)
                            ops.addArc(present, pv, pu, var.weight);
                          ops.close(present, d);

                          if (var.certain) {
                            // Always-present arc: every world of this state
                            // moves to the augmented state, no branching
                            // (states may merge; their gates stay mutually
                            // exclusive).
                            acc[present].push_back(entry.second);
                            continue;
                          }
                          if (present == entry.first) {
                            // The edge cannot change the state in these
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
                      for (std::size_t bi : blocks_at_bag[b]) {
                        const EdgeBlock &blk = blocks[bi];
                        Accumulator acc;
                        for (const auto &entry : table) {
                          // (k+1)-way deterministic branching: one outcome
                          // per alternative (its arcs applied, gated by its
                          // mulinput literal) plus the none outcome.  If no
                          // outcome can change the state, the block is
                          // irrelevant for these worlds.
                          std::vector<State> outs(blk.alts.size());
                          bool all_same = true;
                          for (std::size_t ai = 0; ai < blk.alts.size(); ++ai) {
                            State present = entry.first;
                            const auto &alt = blk.alts[ai];
                            if (alt.arc_uv || alt.arc_vu) {
                              const int pu = positionIn(domain, alt.u);
                              const int pv = positionIn(domain, alt.v);
                              if (alt.arc_uv)
                                ops.addArc(present, pu, pv, 1);
                              if (alt.arc_vu)
                                ops.addArc(present, pv, pu, 1);
                              ops.close(present, d);
                            }
                            outs[ai] = present;
                            if (!(present == entry.first))
                              all_same = false;
                          }
                          if (all_same) {
                            acc[entry.first].push_back(entry.second);
                            continue;
                          }
                          acc[entry.first].push_back(
                            andGate(entry.second, noneGate(bi)));
                          for (std::size_t ai = 0; ai < blk.alts.size(); ++ai)
                            acc[outs[ai]].push_back(
                              andGate(entry.second, mulinGate(bi, ai)));
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
  //    is the full-graph state over b's domain.
  // ------------------------------------------------------------------
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
      // union; the pairs partition the worlds, so any OR the sink
      // builds over a fixed predicate of the entries is deterministic.
      if (!reads_at_bag[b].empty()) {
        const int ps = positionIn(domains[b], source);
        for (const auto &[R, g] : below[b])
          for (const auto &[A, h] : above[b]) {
            CHECK_FOR_INTERRUPTS();
            State closed = R;
            ops.unite(closed, A);
            ops.close(closed, d);
            gate_t pair_gate = invalid_gate;   // lazily created, shared
            for (unsigned long v : reads_at_bag[b]) {
              const auto e = ops.get(closed, ps, positionIn(domains[b], v));
              if (Ops::emptyEntry(e))
                continue;
              if (pair_gate == invalid_gate)
                pair_gate = andGate(g, h);
              onRead(v, e, pair_gate);
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

  stats.nb_gates = dd.getNbGates();
  return true_gate;
}

/**
 * @brief Build a deterministic OR over @p gates (or pass a single gate
 *        through), with the d-DNNF certificate.
 */
gate_t finalizeRoot(dDNNF &dd, const std::vector<gate_t> &gates)
{
  if (gates.size() == 1)
    return gates[0];
  gate_t o = dd.setGate(BooleanGate::OR);
  dd.setInfo(o, DNNF_CERT_INFO);
  for (gate_t g : gates)
    dd.addWire(o, g);
  return o;
}

/**
 * @brief Shared implementation of the two @c compileAllHops() overloads.
 */
ReachabilityCompiler::AllHopsResult compileAllHopsInternal(
  const std::vector<ReachabilityCompiler::EdgeRow> &rows,
  unsigned long source,
  bool directed,
  unsigned hop_bound,
  std::size_t max_states,
  const std::vector<ReachabilityCompiler::SourceArc> *multi_sources)
{
  if (hop_bound > ReachabilityCompiler::MAX_HOP_BOUND)
    throw ReachabilityCompilerException(
            "hop bound exceeds the supported maximum (" +
            std::to_string(ReachabilityCompiler::MAX_HOP_BOUND) + ")");

  ReachabilityCompiler::AllHopsResult result;
  dDNNF &dd = result.dd;
  const HopOps ops(hop_bound);

  // Per-(vertex, length) and per-vertex accumulators; the pairs reported
  // by the DP partition the worlds, so each OR built below is
  // deterministic.  Ordered maps make the output order deterministic.
  std::map<std::pair<unsigned long, unsigned>, std::vector<gate_t> > hop_acc;
  std::map<unsigned long, std::vector<gate_t> > within_acc;

  const gate_t true_gate = runReachabilityDP(
    rows, source, directed, max_states, nullptr, multi_sources, ops, dd,
    result.stats,
    [&](unsigned long v, HopOps::Entry mask, gate_t pair_gate) {
    within_acc[v].push_back(pair_gate);
    while (mask) {
      const unsigned h = static_cast<unsigned>(__builtin_ctzll(mask));
      mask &= mask - 1;
      hop_acc[{v, h}].push_back(pair_gate);
    }
  });

  result.roots.reserve(hop_acc.size());
  for (const auto &[key, gates] : hop_acc)
    result.roots.push_back(
      ReachabilityCompiler::VertexHopRoot{key.first, key.second,
                                          finalizeRoot(dd, gates)});
  result.within_roots.reserve(within_acc.size());
  for (const auto &[v, gates] : within_acc)
    result.within_roots.push_back(
      ReachabilityCompiler::VertexRoot{v, finalizeRoot(dd, gates)});

  dd.setRoot(true_gate);
  result.stats.nb_gates = dd.getNbGates();
  return result;
}

/**
 * @brief Shared implementation of @c compile() / @c compileAll().
 *
 * @param rows           Edge tuples.
 * @param source         Source vertex.
 * @param directed       If @c false, every edge contributes both arcs.
 * @param max_states     Bound on the DP state count per node.
 * @param only_target    When set: ensure the vertex exists in the graph
 *                       (an isolated target is legal) and emit a root for
 *                       it alone, skipping the other vertices' reads.
 * @param multi_sources  When set: virtual super-source mode.
 * @return               The shared d-DNNF, per-vertex roots, statistics.
 */
ReachabilityCompiler::AllResult compileAllBoolInternal(
  const std::vector<ReachabilityCompiler::EdgeRow> &rows,
  unsigned long source,
  bool directed,
  std::size_t max_states,
  const unsigned long *only_target,
  const std::vector<ReachabilityCompiler::SourceArc> *multi_sources)
{
  ReachabilityCompiler::AllResult result;
  dDNNF &dd = result.dd;
  const BoolOps ops;

  std::map<unsigned long, std::vector<gate_t> > accepting;
  const gate_t true_gate = runReachabilityDP(
    rows, source, directed, max_states, only_target, multi_sources, ops, dd,
    result.stats,
    [&](unsigned long v, BoolOps::Entry, gate_t pair_gate) {
    accepting[v].push_back(pair_gate);
  });

  result.roots.reserve(accepting.size());
  for (const auto &[v, gates] : accepting)
    result.roots.push_back(
      ReachabilityCompiler::VertexRoot{v, finalizeRoot(dd, gates)});

  dd.setRoot(true_gate);   // single-target callers re-point this
  result.stats.nb_gates = dd.getNbGates();
  return result;
}

} // namespace

ReachabilityCompiler::AllResult ReachabilityCompiler::compileAll(
  const std::vector<EdgeRow> &rows,
  unsigned long source,
  bool directed,
  std::size_t max_states)
{
  return compileAllBoolInternal(rows, source, directed, max_states, nullptr,
                                nullptr);
}

ReachabilityCompiler::AllResult ReachabilityCompiler::compileAll(
  const std::vector<EdgeRow> &rows,
  const std::vector<SourceArc> &sources,
  bool directed,
  std::size_t max_states)
{
  return compileAllBoolInternal(rows, 0, directed, max_states, nullptr,
                                &sources);
}

ReachabilityCompiler::AllHopsResult ReachabilityCompiler::compileAllHops(
  const std::vector<EdgeRow> &rows,
  unsigned long source,
  bool directed,
  unsigned hop_bound,
  std::size_t max_states)
{
  return compileAllHopsInternal(rows, source, directed, hop_bound, max_states,
                                nullptr);
}

ReachabilityCompiler::AllHopsResult ReachabilityCompiler::compileAllHops(
  const std::vector<EdgeRow> &rows,
  const std::vector<SourceArc> &sources,
  bool directed,
  unsigned hop_bound,
  std::size_t max_states)
{
  return compileAllHopsInternal(rows, 0, directed, hop_bound, max_states,
                                &sources);
}

ReachabilityCompiler::Result ReachabilityCompiler::compile(
  const std::vector<EdgeRow> &rows,
  unsigned long source,
  unsigned long target,
  bool directed,
  std::size_t max_states)
{
  AllResult all = compileAllBoolInternal(rows, source, directed, max_states,
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
