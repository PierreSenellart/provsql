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
 * certified d-D whose gates are shared across all the per-vertex
 * roots.
 *
 * The DP scaffold is generic over the *state algebra* (the @c Ops
 * template parameter below).  Four instantiations:
 *
 * - @c BoolOps -- plain reachability: the state is the transitively
 *   closed reachability relation over the domain (a bitset), composed
 *   by Warshall closure.  This is the default behaviour.
 * - @c HopOps -- bounded-hop reachability: each relation entry is the
 *   *set of achievable walk lengths* up to the hop bound (a bitmask),
 *   composed in the capped min-plus-set semiring by the algebraic-path
 *   (Floyd-Warshall-Kleene) algorithm with diagonal star.
 * - @c SetReachOps -- any-of-S reachability: the relation plus one bit
 *   per position, "reaches an S-vertex within the processed part".
 * - @c CoverOps -- all-of-S (k-terminal) reachability: the relation
 *   plus the pending rescuer-set antichain (see its doc comment).
 *
 * In every case worlds map to exactly one state, so states partition
 * worlds and every emitted OR remains deterministic; each edge
 * variable is still introduced at one node, so ANDs remain
 * decomposable.
 */
#include "ReachabilityCompiler.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <functional>
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
 * @brief Context handed to @c Ops::liftExtra() by the domain
 *        re-expression: the outgoing domain's vertices and the source's
 *        position in it, for algebras whose extra state depends on
 *        *which* vertices are forgotten (the coverage algebra records a
 *        rescuer set when a forgotten vertex is a target-set member).
 */
struct LiftContext {
  const std::vector<unsigned long> &from_domain;   ///< Vertices of the outgoing domain, position-indexed.
  int from_ps;                                     ///< Source position in the outgoing domain.
};

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
  static constexpr bool has_target_set = false;
  static constexpr bool final_collapse = false;
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
  /** @brief Domain-dependent state seeding (a set-reachability concept). */
  void seed(State &, const std::vector<unsigned long> &) const {
  }
  /** @brief Per-position extra state transfer under re-expression (no-op). */
  void liftExtra(const State &, State &, const std::vector<int> &,
                 const LiftContext &) const {
  }
  /** @brief Post-closure canonicalisation hook (a coverage concept). */
  void normalize(State &, int, int) const {
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
  static constexpr bool has_target_set = false;
  static constexpr bool final_collapse = false;
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
  /** @brief Domain-dependent state seeding (a set-reachability concept). */
  void seed(State &, const std::vector<unsigned long> &) const {
  }
  /** @brief Per-position extra state transfer under re-expression (no-op). */
  void liftExtra(const State &, State &, const std::vector<int> &,
                 const LiftContext &) const {
  }
  /** @brief Post-closure canonicalisation hook (a coverage concept). */
  void normalize(State &, int, int) const {
  }
};

/**
 * @brief Set-reachability state algebra: plain reachability plus, per
 *        domain position, one bit "this position reaches some vertex
 *        of the target set within the processed part".
 *
 * The bit is the Courcelle congruence for @f$\exists y\, S(y) \wedge
 * \mathrm{reach}(x, y)@f$: set membership of a domain vertex seeds its
 * own bit; closure propagates bits backwards along the (transitively
 * closed) relation in one pass; forgetting a vertex needs no special
 * handling, since closure already folded its bit into every position
 * that reaches it.  Worlds still map to exactly one state, so the
 * emitted ORs stay deterministic.
 */
struct SetReachOps {
  static constexpr bool tracks_lengths = false;
  static constexpr bool has_target_set = true;
  static constexpr bool final_collapse = false;
  using Entry = bool;
  /** @brief Closed relation plus the per-position set-reachability bits. */
  struct State {
    std::bitset<MAXD*MAXD> rel;   ///< Reachability relation (as @c BoolOps).
    std::bitset<MAXD> dvec;       ///< Position reaches a set vertex within the part.
    bool operator==(const State &o) const {
      return rel == o.rel && dvec == o.dvec;
    }
  };
  /** @brief Hash over both components. */
  struct Hash {
    std::size_t operator()(const State &s) const noexcept {
      return std::hash<std::bitset<MAXD*MAXD> >()(s.rel) * 1099511628211ull
             ^ std::hash<std::bitset<MAXD> >()(s.dvec);
    }
  };

  const std::unordered_set<unsigned long> *target_set;   ///< The vertex set S.

  /** @brief Identity relation, no bits (seeding is domain-dependent). */
  State identity(int d) const {
    State s;
    for (int i = 0; i < d; ++i)
      s.rel.set(i*MAXD + i);
    return s;
  }
  /** @brief Warshall closure, then one backward propagation of the bits. */
  void close(State &s, int d) const {
    for (int k = 0; k < d; ++k)
      for (int i = 0; i < d; ++i)
        if (s.rel[i*MAXD + k])
          for (int j = 0; j < d; ++j)
            if (s.rel[k*MAXD + j])
              s.rel.set(i*MAXD + j);
    // One pass suffices: rel is transitively closed, and dvec(y)
    // already accounts for everything y reaches within the part.
    for (int i = 0; i < d; ++i) {
      if (s.dvec[i])
        continue;
      for (int j = 0; j < d; ++j)
        if (s.rel[i*MAXD + j] && s.dvec[j]) {
          s.dvec.set(i);
          break;
        }
    }
  }
  /** @brief Entrywise union (caller closes afterwards). */
  void unite(State &a, const State &b) const {
    a.rel |= b.rel;
    a.dvec |= b.dvec;
  }
  /** @brief Record the arc @p pu -> @p pv. */
  void addArc(State &s, int pu, int pv, unsigned /*weight*/) const {
    s.rel.set(pu*MAXD + pv);
  }
  /** @brief Read relation entry (@p i, @p j). */
  Entry get(const State &s, int i, int j) const {
    return s.rel[i*MAXD + j];
  }
  /** @brief Whether an entry carries no information. */
  static bool emptyEntry(Entry e) {
    return !e;
  }
  /** @brief Merge a relation entry (domain re-expression). */
  void merge(State &s, int i, int j, Entry /*e*/) const {
    s.rel.set(i*MAXD + j);
  }
  /** @brief Seed the bits of the domain's set vertices. */
  void seed(State &s, const std::vector<unsigned long> &domain) const {
    for (std::size_t i = 0; i < domain.size(); ++i)
      if (target_set->count(domain[i]))
        s.dvec.set(static_cast<int>(i));
  }
  /** @brief Carry surviving positions' bits through a re-expression. */
  void liftExtra(const State &from, State &to,
                 const std::vector<int> &map, const LiftContext &) const {
    for (std::size_t i = 0; i < map.size(); ++i)
      if (map[i] >= 0 && from.dvec[static_cast<int>(i)])
        to.dvec.set(map[i]);
  }
  /** @brief Post-closure canonicalisation hook (a coverage concept). */
  void normalize(State &, int, int) const {
  }
};

/**
 * @brief Coverage (k-terminal) state algebra: plain reachability plus
 *        the *pending rescuer-set antichain* -- the Courcelle
 *        congruence for @f$\forall y\, S(y) \rightarrow
 *        \mathrm{reach}(x, y)@f$.
 *
 * When a target-set vertex @c v is forgotten, either the source
 * already reaches it (nothing recorded) or its fate now depends only
 * on the boundary: @c v ends up reachable iff the source eventually
 * reaches one of its *rescuers*, the domain positions that reach
 * @c v within the processed part.  The state therefore carries, next
 * to the closed relation, the family of pending rescuer sets:
 *
 * - kept *closed* under the relation (anything reaching a rescuer is
 *   one; the relation being transitively closed, one backward pass
 *   per closure suffices), which makes the forget step a plain
 *   intersection with the surviving positions -- lossless by
 *   transitivity;
 * - *discharged* (dropped) as soon as a set acquires the source's
 *   position: the vertex is reached, in every world of this state;
 * - the *empty* set is the absorbing reject -- a vertex none of whose
 *   rescuers survived can never be reached -- and absorbs the whole
 *   antichain (it is a subset of every set);
 * - reduced to the *antichain of minimal sets*: hitting a subset
 *   implies hitting its supersets, so only minimal sets constrain.
 *
 * Acceptance, after a final re-expression onto the singleton
 * @c {source} domain (which forgets -- and thereby resolves -- every
 * remaining target vertex): no pending set.  Worlds still map to
 * exactly one state, so the emitted ORs stay deterministic and the
 * d-D certificate carries over; the antichain is bounded by the
 * domain size alone (at most @f$\binom{d}{\lfloor d/2\rfloor}@f$
 * sets), so the state space stays a function of the treewidth, with
 * the usual @c max_states guard.
 */
struct CoverOps {
  static constexpr bool tracks_lengths = false;
  static constexpr bool has_target_set = true;
  static constexpr bool final_collapse = true;
  using Entry = bool;
  using Mask = std::uint32_t;
  static_assert(MAXD <= 32, "rescuer sets are 32-bit masks");
  /** @brief Closed relation plus the sorted antichain of pending rescuer sets. */
  struct State {
    std::bitset<MAXD*MAXD> rel;   ///< Reachability relation (as @c BoolOps).
    std::vector<Mask> pending;    ///< Minimal rescuer sets, sorted (canonical).
    bool operator==(const State &o) const {
      return rel == o.rel && pending == o.pending;
    }
  };
  /** @brief Hash over both components. */
  struct Hash {
    std::size_t operator()(const State &s) const noexcept {
      std::size_t h = std::hash<std::bitset<MAXD*MAXD> >()(s.rel);
      for (Mask m : s.pending)
        h = h * 1099511628211ull ^ m;
      return h;
    }
  };

  const std::unordered_set<unsigned long> *target_set;   ///< The vertex set S.

  /** @brief Identity relation, nothing pending. */
  State identity(int d) const {
    State s;
    for (int i = 0; i < d; ++i)
      s.rel.set(i*MAXD + i);
    return s;
  }
  /** @brief Warshall closure (the pending sets are updated by @c normalize()). */
  void close(State &s, int d) const {
    for (int k = 0; k < d; ++k)
      for (int i = 0; i < d; ++i)
        if (s.rel[i*MAXD + k])
          for (int j = 0; j < d; ++j)
            if (s.rel[k*MAXD + j])
              s.rel.set(i*MAXD + j);
  }
  /** @brief Entrywise union; pending sets concatenate (normalised next). */
  void unite(State &a, const State &b) const {
    a.rel |= b.rel;
    a.pending.insert(a.pending.end(), b.pending.begin(), b.pending.end());
  }
  /** @brief Record the arc @p pu -> @p pv. */
  void addArc(State &s, int pu, int pv, unsigned /*weight*/) const {
    s.rel.set(pu*MAXD + pv);
  }
  /** @brief Read relation entry (@p i, @p j). */
  Entry get(const State &s, int i, int j) const {
    return s.rel[i*MAXD + j];
  }
  /** @brief Whether an entry carries no information. */
  static bool emptyEntry(Entry e) {
    return !e;
  }
  /** @brief Merge a relation entry (domain re-expression). */
  void merge(State &s, int i, int j, Entry /*e*/) const {
    s.rel.set(i*MAXD + j);
  }
  /** @brief No seeding: target vertices are resolved when forgotten. */
  void seed(State &, const std::vector<unsigned long> &) const {
  }
  /** @brief Re-express the pending sets; resolve forgotten target vertices. */
  void liftExtra(const State &from, State &to,
                 const std::vector<int> &map, const LiftContext &ctx) const {
    // Surviving rescuers keep their sets alive (an emptied set is the
    // absorbing reject; normalize() collapses around it).
    for (Mask m : from.pending) {
      Mask r = 0;
      for (std::size_t i = 0; i < map.size(); ++i)
        if (map[i] >= 0 && (m & (Mask{1} << i)))
          r |= Mask{1} << map[i];
      to.pending.push_back(r);
    }
    // A forgotten target vertex is either already reached by the
    // source, or pends on the surviving positions that reach it.
    for (std::size_t i = 0; i < map.size(); ++i) {
      if (map[i] >= 0 || !target_set->count(ctx.from_domain[i]))
        continue;
      if (from.rel[static_cast<std::size_t>(ctx.from_ps)*MAXD + i])
        continue;   // discharged
      Mask r = 0;
      for (std::size_t u = 0; u < map.size(); ++u)
        if (map[u] >= 0 && from.rel[u*MAXD + i])
          r |= Mask{1} << map[u];
      to.pending.push_back(r);
    }
  }
  /**
   * @brief Canonicalise after a closure: re-close the pending sets
   *        under the relation, discharge the source-reached ones, and
   *        reduce to the sorted minimal antichain.
   */
  void normalize(State &s, int d, int ps) const {
    if (s.pending.empty())
      return;
    for (Mask &m : s.pending) {
      // One backward pass (rel is transitively closed).
      Mask grown = m;
      for (int u = 0; u < d; ++u) {
        if (grown & (Mask{1} << u))
          continue;
        for (int w = 0; w < d; ++w)
          if ((m & (Mask{1} << w)) && s.rel[static_cast<std::size_t>(u)*MAXD + w]) {
            grown |= Mask{1} << u;
            break;
          }
      }
      m = grown;
    }
    // Discharge, then keep the sorted minimal antichain (the empty set,
    // the absorbing reject, is minimal and absorbs everything).
    const Mask ps_bit = Mask{1} << ps;
    std::vector<Mask> keep;
    keep.reserve(s.pending.size());
    for (Mask m : s.pending)
      if (!(m & ps_bit))
        keep.push_back(m);
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    s.pending.clear();
    for (Mask m : keep) {
      bool dominated = false;
      for (Mask k : s.pending)
        if ((k & m) == k) {   // an already-kept subset constrains more
          dominated = true;
          break;
        }
      if (!dominated)
        s.pending.push_back(m);
    }
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
                         ReadSink onRead,
                         /* When set, acceptance is a predicate of the root's
                          * below-table alone: the sink is called once per
                          * root state with (state, gate, source position) and
                          * the whole top-down sweep is skipped. */
                         const std::function<void(const typename Ops::State &,
                                                  gate_t, int)> *root_sink
                           = nullptr,
                         /* Multi-set mode (Ops with a target_set member,
                          * i.e. SetReachOps): the prelude -- variable
                          * grouping, tree decomposition, bag assignments,
                          * literal gates -- is built once, then one
                          * bottom-up sweep runs per target set, with
                          * content-deduplicated (hash-consed) gate
                          * emission so the parts of the circuit a set's
                          * seeds do not touch come out as the *same*
                          * gates across sets.  The sink receives
                          * (set index, root state, gate, source position);
                          * the top-down sweep is skipped. */
                         const std::vector<std::unordered_set<unsigned long> >
                           *multi_sets = nullptr,
                         const std::function<void(std::size_t,
                                                  const typename Ops::State &,
                                                  gate_t, int)> *multi_sink
                           = nullptr)
{
  using State = typename Ops::State;
  using Table = std::unordered_map<State, gate_t, typename Ops::Hash>;
  using Accumulator = std::unordered_map<State, std::vector<gate_t>,
                                         typename Ops::Hash>;

  /* The state algebra the sweeps read through: re-pointed per set in
   * multi-set mode (each sweep seeds a different target set), constant
   * otherwise. */
  const Ops *opsp = &ops;

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
   * every *accepted* compilation by a linear pass. */
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
  // *by construction*; mark them with the d-D certificate so the
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
  /* Content dedup (hash-consing) of the emitted AND / OR gates,
   * enabled when several target sets share the circuit: per-set sweeps
   * re-derive identical subcircuits everywhere their seeds make no
   * difference, and consing makes those the *same* gate, so downstream
   * consumers (notably the store materialisation, which walks every
   * gate instance) pay for the shared structure once.  Sound
   * unconditionally: structural identity implies functional identity,
   * and determinism / decomposability are properties of a gate's
   * children.  The (dominant) binary ANDs are keyed on the packed
   * child-id pair; the ORs on the gate type plus the sorted child ids
   * as raw bytes. */
  const bool consing = multi_sets != nullptr && multi_sets->size() > 1;
  struct PairHash {
    std::size_t operator()(const std::pair<std::uint64_t,
                                           std::uint64_t> &p) const noexcept {
      // Mix the halves (splitmix-style) so consecutive ids spread.
      std::uint64_t x = p.first * 0x9e3779b97f4a7c15ull ^ p.second;
      x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
      x ^= x >> 27; x *= 0x94d049bb133111ebull;
      return static_cast<std::size_t>(x ^ (x >> 31));
    }
  };
  std::unordered_map<std::pair<std::uint64_t, std::uint64_t>, gate_t,
                     PairHash> cons_and;
  std::unordered_map<std::string, gate_t> cons_or;
  auto consKey = [](std::vector<gate_t> children) {
                   std::sort(children.begin(), children.end());
                   std::string key;
                   key.reserve(children.size() * sizeof(gate_t));
                   for (gate_t c : children)
                     key.append(reinterpret_cast<const char *>(&c),
                                sizeof(gate_t));
                   return key;
                 };

  auto andGate = [&](gate_t a, gate_t b) {
                   if (a == true_gate)
                     return b;
                   if (b == true_gate)
                     return a;
                   std::pair<std::uint64_t, std::uint64_t> key;
                   if (consing) {
                     const auto ai =
                       static_cast<std::uint64_t>(
                         static_cast<std::underlying_type<gate_t>::type>(a));
                     const auto bi =
                       static_cast<std::uint64_t>(
                         static_cast<std::underlying_type<gate_t>::type>(b));
                     key = std::minmax(ai, bi);
                     auto it = cons_and.find(key);
                     if (it != cons_and.end())
                       return it->second;
                   }
                   gate_t g = dd.setGate(BooleanGate::AND);
                   dd.setInfo(g, DNNF_CERT_INFO);
                   dd.addWire(g, a);
                   dd.addWire(g, b);
                   if (consing)
                     cons_and.emplace(key, g);
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
                        std::string key;
                        if (consing) {
                          key = consKey(entry.second);
                          auto it = cons_or.find(key);
                          if (it != cons_or.end()) {
                            t.emplace(entry.first, it->second);
                            continue;
                          }
                        }
                        gate_t g = dd.setGate(BooleanGate::OR);
                        dd.setInfo(g, DNNF_CERT_INFO);
                        for (gate_t c : entry.second)
                          dd.addWire(g, c);
                        if (consing)
                          cons_or.emplace(std::move(key), g);
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
  auto trivialState = [&](const std::vector<unsigned long> &domain) {
                        auto s = opsp->identity(static_cast<int>(domain.size()));
                        opsp->seed(s, domain);
                        return s;
                      };
  auto trivialTable = [&](const std::vector<unsigned long> &domain) {
                        return Table{{trivialState(domain), true_gate}};
                      };
  auto isTrivial = [&](const Table &t,
                       const std::vector<unsigned long> &domain) {
                     // A table is a join identity only if it is the single
                     // always-true *trivial-state* entry (the identity
                     // relation plus the domain's seeds): a certain arc
                     // produces single-state TRUE tables whose state is
                     // not trivial, and dropping those in join() would
                     // lose the arc.
                     return t.size() == 1 && t.begin()->second == true_gate &&
                            t.begin()->first == trivialState(domain);
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
                State id = opsp->identity(dt);
                opsp->seed(id, to);
                const LiftContext ctx{from, positionIn(from, source)};
                const int to_ps = positionIn(to, source);
                Accumulator acc;
                for (const auto &entry : t) {
                  State r = id;
                  for (int i = 0; i < df; ++i) {
                    if (map[i] < 0)
                      continue;
                    for (int j = 0; j < df; ++j) {
                      if (map[j] < 0)
                        continue;
                      const auto e = opsp->get(entry.first, i, j);
                      if (!Ops::emptyEntry(e))
                        opsp->merge(r, map[i], map[j], e);
                    }
                  }
                  opsp->liftExtra(entry.first, r, map, ctx);
                  opsp->normalize(r, dt, to_ps);
                  acc[r].push_back(entry.second);
                }
                return finalize(acc);
              };

  // Join two tables over the same domain, covering disjoint edge sets:
  // pairs of states are mutually exclusive (deterministic ORs) and the
  // gates variable-disjoint (decomposable ANDs); walks across the two
  // parts only alternate through domain vertices (the bag separates
  // them), hence the closure of the union.
  auto join = [&](const Table &t1, const Table &t2,
                  const std::vector<unsigned long> &domain) {
                const int d = static_cast<int>(domain.size());
                if (isTrivial(t1, domain))
                  return t2;
                if (isTrivial(t2, domain))
                  return t1;
                const int ps = positionIn(domain, source);
                Accumulator acc;
                for (const auto &left : t1)
                  for (const auto &right : t2) {
                    CHECK_FOR_INTERRUPTS();
                    State r = left.first;
                    opsp->unite(r, right.first);
                    opsp->close(r, d);
                    opsp->normalize(r, d, ps);
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
                      const int ps = positionIn(domain, source);
                      for (std::size_t vi : variables_at_bag[b]) {
                        const EdgeVariable &var = variables[vi];
                        const int pu = positionIn(domain, var.u);
                        const int pv = positionIn(domain, var.v);

                        Accumulator acc;
                        for (const auto &entry : table) {
                          State present = entry.first;
                          if (var.arc_uv)
                            opsp->addArc(present, pu, pv, var.weight);
                          if (var.arc_vu)
                            opsp->addArc(present, pv, pu, var.weight);
                          opsp->close(present, d);
                          opsp->normalize(present, d, ps);

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
                                opsp->addArc(present, pu, pv, 1);
                              if (alt.arc_vu)
                                opsp->addArc(present, pv, pu, 1);
                              opsp->close(present, d);
                              opsp->normalize(present, d, ps);
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
  auto runBottomUp = [&]() {
                       std::vector<Table> below(nb_bags);
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
                         Table table = frame.has_table
                                       ? std::move(frame.table)
                                       : trivialTable(domains[b]);
                         below[b] = applyEdges(std::move(table), b);

                         if (stack.size() == 1) {
                           stack.pop_back();
                           break;
                         }

                         // Merge into the parent's partial join.
                         Frame &parent = stack[stack.size()-2];
                         const std::size_t pb = bag_index(parent.bag);
                         Table lifted = lift(below[b], domains[b],
                                             domains[pb]);
                         if (!parent.has_table) {
                           parent.table = std::move(lifted);
                           parent.has_table = true;
                         } else {
                           parent.table = join(parent.table, lifted,
                                               domains[pb]);
                         }
                         stack.pop_back();
                       }
                       return below;
                     };

  // ------------------------------------------------------------------
  // 5m. Multi-set mode: one bottom-up sweep per target set over the
  //     shared prelude, the per-set acceptance read off the root table
  //     (as in 5b); the consed emission makes the seed-independent
  //     parts of the per-set circuits literally shared.
  // ------------------------------------------------------------------
  if (multi_sets) {
    if constexpr (Ops::has_target_set) {
      const std::size_t rb = bag_index(td.getRoot());
      const int ps = positionIn(domains[rb], source);
      const std::vector<unsigned long> source_domain{source};
      Ops per_set_ops = ops;
      opsp = &per_set_ops;
      for (std::size_t si = 0; si < multi_sets->size(); ++si) {
        CHECK_FOR_INTERRUPTS();
        per_set_ops.target_set = &(*multi_sets)[si];
        const std::vector<Table> below = runBottomUp();
        if constexpr (Ops::final_collapse) {
          // Re-express the root table onto the singleton source domain:
          // forgetting every remaining vertex resolves the in-domain
          // target vertices, so acceptance is a predicate of the
          // collapsed state alone.
          const Table collapsed =
            lift(below[rb], domains[rb], source_domain);
          for (const auto &[R, g] : collapsed)
            (*multi_sink)(si, R, g, 0);
        } else {
          for (const auto &[R, g] : below[rb])
            (*multi_sink)(si, R, g, ps);
        }
      }
      stats.nb_gates = dd.getNbGates();
      return true_gate;
    } else {
      throw ReachabilityCompilerException(
              "multi-set mode requires a set-seeded state algebra");
    }
  }

  const std::vector<Table> below = runBottomUp();

  // ------------------------------------------------------------------
  // 5b. Root-only acceptance: when the caller's predicate is a function
  //     of the root's below-table alone (set reachability reads one bit
  //     at the source's position), report each root state and skip the
  //     whole top-down sweep.
  // ------------------------------------------------------------------
  if (root_sink) {
    const std::size_t rb = bag_index(td.getRoot());
    const int ps = positionIn(domains[rb], source);
    for (const auto &[R, g] : below[rb])
      (*root_sink)(R, g, ps);
    stats.nb_gates = dd.getNbGates();
    return true_gate;
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
            opsp->unite(closed, A);
            opsp->close(closed, d);
            gate_t pair_gate = invalid_gate;   // lazily created, shared
            for (unsigned long v : reads_at_bag[b]) {
              const auto e = opsp->get(closed, ps, positionIn(domains[b], v));
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
          prefix[i+1] = join(prefix[i], lifted[i], domains[b]);
        suffix[m] = trivialTable(domains[b]);
        for (std::size_t i = m; i-- > 0; )
          suffix[i] = join(lifted[i], suffix[i+1], domains[b]);

        for (std::size_t i = 0; i < m; ++i) {
          Table siblings = join(prefix[i], suffix[i+1], domains[b]);
          Table a = applyEdges(join(above[b], siblings, domains[b]), b);
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
 *        through), with the d-D certificate.
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
 * @return               The shared d-D, per-vertex roots, statistics.
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

/**
 * @brief Implementation of @c compileAnyReachAll() (and, through a
 *        one-set wrapper, @c compileAnyReach()): the shared prelude --
 *        variable grouping, tree decomposition, bag assignments,
 *        literal gates -- built once, then one bottom-up sweep per
 *        target set with the set-reachability algebra and
 *        content-deduplicated gate emission, each set's acceptance
 *        read off the root table.
 */
ReachabilityCompiler::AnyReachAllResult compileAnyReachAllInternal(
  const std::vector<ReachabilityCompiler::EdgeRow> &rows,
  const std::vector<ReachabilityCompiler::SourceArc> &sources,
  const std::vector<std::vector<unsigned long> > &sets,
  bool directed,
  std::size_t max_states)
{
  ReachabilityCompiler::AnyReachAllResult result;
  dDNNF &dd = result.dd;

  if (sets.empty())
    throw ReachabilityCompilerException("no target sets given");

  /* Restrict each target set to vertices that actually exist (edge
   * endpoints and source vertices): an absent vertex is unreachable
   * and contributes nothing -- and, crucially, the virtual
   * super-source is allocated the first id *above* this universe, so
   * filtering also prevents a caller-supplied id from accidentally
   * colliding with it and seeding the acceptance bit everywhere. */
  std::unordered_set<unsigned long> universe;
  for (const auto &row : rows) {
    universe.insert(row.src);
    universe.insert(row.dst);
  }
  for (const auto &sa : sources)
    universe.insert(sa.vertex);
  std::vector<std::unordered_set<unsigned long> > target_sets(sets.size());
  for (std::size_t si = 0; si < sets.size(); ++si)
    for (unsigned long v : sets[si])
      if (universe.count(v))
        target_sets[si].insert(v);
  SetReachOps ops;
  ops.target_set = &target_sets[0];

  // Per set, the accepting root states are those whose source position
  // carries the set-reachability bit; they partition the worlds, so
  // their OR is deterministic.
  std::vector<std::vector<gate_t> > accepting(sets.size());
  const std::function<void(std::size_t, const SetReachOps::State &,
                           gate_t, int)> sink =
    [&](std::size_t si, const SetReachOps::State &state, gate_t g, int ps) {
      if (state.dvec[ps])
        accepting[si].push_back(g);
    };

  runReachabilityDP(
    rows, 0, directed, max_states, nullptr, &sources, ops, dd,
    result.stats,
    [](unsigned long, SetReachOps::Entry, gate_t) {
  },
    nullptr, &target_sets, &sink);

  result.roots.reserve(sets.size());
  for (std::size_t si = 0; si < sets.size(); ++si) {
    gate_t root;
    if (accepting[si].empty()) {
      // No world reaches the set: constant false.
      root = dd.setGate(BooleanGate::OR);
      dd.setInfo(root, DNNF_CERT_INFO);
    } else
      root = finalizeRoot(dd, accepting[si]);
    result.roots.push_back(root);
  }
  dd.setRoot(result.roots[0]);   // single-set callers read this
  result.stats.nb_gates = dd.getNbGates();
  return result;
}

/**
 * @brief Implementation of @c compileCoverReachAll() (and, through a
 *        one-set wrapper, @c compileCoverReach()): the coverage
 *        algebra over the shared prelude, one bottom-up sweep per
 *        target set, acceptance -- no pending rescuer set after the
 *        final collapse onto the source domain -- read per set.
 */
ReachabilityCompiler::AnyReachAllResult compileCoverReachAllInternal(
  const std::vector<ReachabilityCompiler::EdgeRow> &rows,
  const std::vector<ReachabilityCompiler::SourceArc> &sources,
  const std::vector<std::vector<unsigned long> > &sets,
  bool directed,
  std::size_t max_states)
{
  ReachabilityCompiler::AnyReachAllResult result;
  dDNNF &dd = result.dd;

  if (sets.empty())
    throw ReachabilityCompilerException("no target sets given");

  /* The universe filter serves the super-source collision as in the
   * any-reach case -- but here an absent vertex is *unreachable*, so a
   * set containing one compiles to constant false rather than being
   * silently shrunk.  The universe is the *decomposed graph's* vertex
   * set: a vertex appearing only in self-loops never enters the DP
   * (self-loops are irrelevant to plain reachability), is unreachable
   * from anywhere else, and must count as absent. */
  std::unordered_set<unsigned long> universe;
  for (const auto &row : rows)
    if (row.src != row.dst) {
      universe.insert(row.src);
      universe.insert(row.dst);
    }
  for (const auto &sa : sources)
    universe.insert(sa.vertex);
  std::vector<std::unordered_set<unsigned long> > target_sets(sets.size());
  std::vector<bool> absent(sets.size(), false);
  for (std::size_t si = 0; si < sets.size(); ++si)
    for (unsigned long v : sets[si]) {
      if (universe.count(v))
        target_sets[si].insert(v);
      else
        absent[si] = true;
    }
  CoverOps ops;
  ops.target_set = &target_sets[0];

  // Per set, the accepting collapsed states are those with no pending
  // rescuer set; they partition the worlds, so their OR is
  // deterministic.
  std::vector<std::vector<gate_t> > accepting(sets.size());
  const std::function<void(std::size_t, const CoverOps::State &,
                           gate_t, int)> sink =
    [&](std::size_t si, const CoverOps::State &state, gate_t g, int) {
      if (state.pending.empty())
        accepting[si].push_back(g);
    };

  runReachabilityDP(
    rows, 0, directed, max_states, nullptr, &sources, ops, dd,
    result.stats,
    [](unsigned long, CoverOps::Entry, gate_t) {
  },
    nullptr, &target_sets, &sink);

  result.roots.reserve(sets.size());
  for (std::size_t si = 0; si < sets.size(); ++si) {
    gate_t root;
    if (absent[si] || accepting[si].empty()) {
      // An absent target vertex, or no covering world: constant false.
      root = dd.setGate(BooleanGate::OR);
      dd.setInfo(root, DNNF_CERT_INFO);
    } else
      root = finalizeRoot(dd, accepting[si]);
    result.roots.push_back(root);
  }
  dd.setRoot(result.roots[0]);   // single-set callers read this
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

ReachabilityCompiler::Result ReachabilityCompiler::compileAnyReach(
  const std::vector<EdgeRow> &rows,
  const std::vector<SourceArc> &sources,
  const std::vector<unsigned long> &set,
  bool directed,
  std::size_t max_states)
{
  AnyReachAllResult all =
    compileAnyReachAllInternal(rows, sources, {set}, directed, max_states);
  Result result;
  result.dd = std::move(all.dd);
  result.stats = all.stats;
  return result;
}

ReachabilityCompiler::AnyReachAllResult ReachabilityCompiler::compileAnyReachAll(
  const std::vector<EdgeRow> &rows,
  const std::vector<SourceArc> &sources,
  const std::vector<std::vector<unsigned long> > &sets,
  bool directed,
  std::size_t max_states)
{
  return compileAnyReachAllInternal(rows, sources, sets, directed, max_states);
}

ReachabilityCompiler::Result ReachabilityCompiler::compileCoverReach(
  const std::vector<EdgeRow> &rows,
  const std::vector<SourceArc> &sources,
  const std::vector<unsigned long> &set,
  bool directed,
  std::size_t max_states)
{
  AnyReachAllResult all =
    compileCoverReachAllInternal(rows, sources, {set}, directed, max_states);
  Result result;
  result.dd = std::move(all.dd);
  result.stats = all.stats;
  return result;
}

ReachabilityCompiler::AnyReachAllResult ReachabilityCompiler::compileCoverReachAll(
  const std::vector<EdgeRow> &rows,
  const std::vector<SourceArc> &sources,
  const std::vector<std::vector<unsigned long> > &sets,
  bool directed,
  std::size_t max_states)
{
  return compileCoverReachAllInternal(rows, sources, sets, directed,
                                      max_states);
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
