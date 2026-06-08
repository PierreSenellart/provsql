/**
 * @file ReachabilityCompiler.h
 * @brief Decomposition-aligned compilation of two-terminal reachability
 *        over bounded-treewidth data into a d-DNNF.
 *
 * Implements the data-side counterpart of ProvSQL's circuit-side
 * treewidth exploitation: instead of building a provenance circuit
 * along the relational-algebra plan (whose treewidth can grow with the
 * instance size) and decomposing it afterwards, the provenance of
 * *s-t reachability* is built **along a tree decomposition of the data
 * graph itself**, in the spirit of the provenance refinement of
 * Courcelle's theorem (Amarilli, Bourhis & Senellart, ICALP 2015 /
 * ICDT 2017).
 *
 * The construction is a bag-by-bag dynamic program whose state at a
 * decomposition node is the transitively-closed reachability relation
 * over the bag's vertices augmented with the two terminals @c s and
 * @c t (equivalently, the standard DP over the decomposition obtained
 * by adding @c s and @c t to every bag).  Each transition emits d-DNNF
 * gates directly:
 *
 * - states at a node are **mutually exclusive and exhaustive** over the
 *   valuations of the edge variables introduced in its subtree, so
 *   every OR gate is deterministic *by construction*;
 * - each edge variable is introduced at exactly one node, so the
 *   children of every AND gate mention disjoint variable sets and the
 *   circuit is decomposable *by construction*.
 *
 * No knowledge-compilation step is therefore needed: the result is fed
 * straight to @c dDNNF::probabilityEvaluation().  For data of treewidth
 * @f$k@f$ the d-DNNF has size linear in the number of edges (times a
 * function of @f$k@f$ only), which yields linear-time exact computation
 * of two-terminal network reliability -- a @c \#P-hard problem in
 * general -- on bounded-treewidth probabilistic graphs, including
 * **cyclic** graphs that the recursive-query fixpoint cannot handle.
 *
 * Directed reachability and undirected connectivity are both
 * supported: the DP state is a reachability *relation* (not a
 * partition), so the undirected case is simply the directed case with
 * each edge contributing both arcs.
 */
#ifndef REACHABILITY_COMPILER_H
#define REACHABILITY_COMPILER_H

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "dDNNF.h"
#include "TreeDecomposition.h"

/**
 * @brief Exception thrown when reachability compilation fails.
 *
 * Raised on unsupported input shapes (a provenance token shared by
 * edges with different endpoints) or when the per-node state space
 * exceeds the configured bound.  Data treewidth above
 * @c TreeDecomposition::MAX_TREEWIDTH surfaces as the usual
 * @c TreeDecompositionException instead.
 */
class ReachabilityCompilerException : public std::runtime_error {
public:
/** @brief Construct with a human-readable message. @param what  Message. */
explicit ReachabilityCompilerException(const std::string &what)
  : std::runtime_error(what) {
}
};

/**
 * @brief Compiles s-t reachability over a probabilistic edge relation
 *        into a d-DNNF, along a tree decomposition of the data graph.
 */
class ReachabilityCompiler {
public:
/**
 * @brief One row of the edge relation.
 *
 * Two rows may share a @c token only if they are mutual reverses
 * (@c (u,v) and @c (v,u)), the natural encoding of an undirected edge
 * in a directed edge relation; the pair is then treated as a single
 * bidirectional edge variable.
 */
struct EdgeRow {
  unsigned long src;   ///< Source vertex ID.
  unsigned long dst;   ///< Destination vertex ID.
  std::string token;   ///< Provenance token (UUID) of the edge tuple.
  double prob;         ///< Probability of the edge tuple.
  std::string block_key;     ///< Block-independent (BID) key variable (UUID) when the tuple is a @c mulinput alternative (e.g. from @c repair_key); empty for an independent tuple.
  unsigned block_index = 0;  ///< Outcome index within the block (the @c mulinput gate's info).
};

/** @brief Structural statistics of a compilation, for diagnostics and tests. */
struct Stats {
  unsigned data_treewidth = 0;   ///< Treewidth of the min-fill decomposition of the *data* graph.
  std::size_t nb_bags = 0;       ///< Number of bags of the decomposition.
  std::size_t max_states = 0;    ///< Maximum number of DP states at any node.
  std::size_t nb_gates = 0;      ///< Number of gates of the emitted d-DNNF.
  std::size_t nb_variables = 0;  ///< Number of edge variables (provenance tokens).
};

/** @brief A compiled reachability query: the d-DNNF and its statistics. */
struct Result {
  dDNNF dd;      ///< d-DNNF whose root computes "t is reachable from s"; input gates carry the edge tokens (UUID) and probabilities.
  Stats stats;   ///< Compilation statistics.
};

/** @brief One vertex's reachability circuit in an all-targets compilation. */
struct VertexRoot {
  unsigned long vertex;   ///< Vertex ID.
  gate_t root;            ///< Root of "vertex is reachable from the source" in the shared d-DNNF.
};

/** @brief An all-targets compilation: one shared d-DNNF, one root per reachable vertex. */
struct AllResult {
  dDNNF dd;                        ///< Shared circuit (gates are reused across vertices).
  std::vector<VertexRoot> roots;   ///< One entry per vertex reachable in the all-edges-present world (including the source itself, with a constant-true root).
  Stats stats;                     ///< Compilation statistics.
};

/** @brief One (vertex, walk length) circuit of a bounded-hop compilation. */
struct VertexHopRoot {
  unsigned long vertex;   ///< Vertex ID.
  unsigned hops;          ///< Exact walk length (number of edges).
  gate_t root;            ///< Root of "some walk of exactly this many edges connects the source to the vertex".
};

/**
 * @brief A bounded-hop all-targets compilation.
 *
 * One root per (vertex, walk length) pair achievable in the
 * all-edges-present world -- matching the rows the generic recursive
 * fixpoint derives for a hop-counting CTE, whose row @c (v,h)
 * provenance is "some *walk* of exactly @c h edges" (walks, not paths:
 * a cycle on the way pumps achievable lengths) -- plus, per vertex, the
 * root of "some walk of at most the bound", which a hop-counting query
 * deduplicating away the hop column computes as the OR of the
 * per-length roots.
 */
struct AllHopsResult {
  dDNNF dd;                              ///< Shared circuit.
  std::vector<VertexHopRoot> roots;      ///< Per (vertex, exact length) roots.
  std::vector<VertexRoot> within_roots;  ///< Per-vertex "within the bound" roots.
  Stats stats;                           ///< Compilation statistics.
};

/** @brief A multi-set any-reach compilation: one shared circuit, one root per target set. */
struct AnyReachAllResult {
  dDNNF dd;                    ///< Shared circuit (consed: identical subcircuits are the same gate).
  std::vector<gate_t> roots;   ///< One root per input set, in input order.
  Stats stats;                 ///< Compilation statistics (max_states maxed over the sweeps).
};

/**
 * @brief One source of a multi-source compilation.
 *
 * Modelled as an arc from a virtual super-source to @c vertex: gated by
 * the source tuple's provenance @c token when the source relation is
 * tracked (a *probabilistic source set*), always present when
 * @c certain (untracked sources, or the constant base arm of the
 * recursive shape).
 */
struct SourceArc {
  unsigned long vertex;   ///< Source vertex.
  std::string token;      ///< Provenance token of the source tuple (unused when certain).
  double prob;            ///< Source-tuple probability (unused when certain).
  bool certain;           ///< Always-present source (no gating variable).
};

/**
 * @brief Default bound on the number of DP states at a single
 *        decomposition node.
 *
 * The state space at a node is the set of reachable transitively-closed
 * relations over at most @c MAX_TREEWIDTH+3 elements; it is bounded for
 * fixed treewidth but can still be large near the treewidth cap, so a
 * guard keeps compilation from exhausting memory on adversarial data.
 */
static constexpr std::size_t DEFAULT_MAX_STATES = 100000;

/**
 * @brief Maximum supported hop bound for @c compileAllHops().
 *
 * Length sets are bitmasks over walk lengths @c 0..bound in a 64-bit
 * word; the driver falls back to the generic fixpoint above this.
 */
static constexpr unsigned MAX_HOP_BOUND = 62;

/**
 * @brief Compile s-t reachability over @p rows into a d-DNNF.
 *
 * @param rows        Edge tuples (vertex IDs, provenance token, probability).
 * @param source      Source vertex @c s.
 * @param target      Target vertex @c t.
 * @param directed    If @c false, every edge contributes both arcs
 *                    (undirected connectivity).
 * @param max_states  Bound on the DP state count per node.
 * @return            The compiled d-DNNF and statistics.
 * @throws TreeDecompositionException        if the data treewidth exceeds
 *                                           @c TreeDecomposition::MAX_TREEWIDTH.
 * @throws ReachabilityCompilerException     on unsupported input shapes or
 *                                           when @p max_states is exceeded.
 */
static Result compile(const std::vector<EdgeRow> &rows,
                      unsigned long source,
                      unsigned long target,
                      bool directed,
                      std::size_t max_states = DEFAULT_MAX_STATES);

/**
 * @brief Compile the reachability circuits of **every** vertex in one pass.
 *
 * Two sweeps over the tree decomposition -- bottom-up per-subtree state
 * tables, then a top-down "rest of the graph" pass -- yield, for each
 * vertex read at its elimination bag, the deterministic OR over
 * compatible (below, above) state pairs whose closure connects the
 * source to it.  Total circuit size stays linear in the number of edges
 * for fixed data treewidth: gates are shared across the per-vertex
 * roots.  This matches the semantics of the recursive-query relation
 * @c reach: one root per vertex reachable in the all-edges-present
 * world (vertices certainly unreachable are omitted).
 *
 * @param rows        Edge tuples (vertex IDs, provenance token, probability).
 * @param source      Source vertex @c s.
 * @param directed    If @c false, every edge contributes both arcs.
 * @param max_states  Bound on the DP state count per node.
 * @return            The shared d-DNNF, per-vertex roots, statistics.
 * @throws TreeDecompositionException / ReachabilityCompilerException as
 *         for @c compile().
 */
static AllResult compileAll(const std::vector<EdgeRow> &rows,
                            unsigned long source,
                            bool directed,
                            std::size_t max_states = DEFAULT_MAX_STATES);

/**
 * @brief Multi-source variant of @c compileAll().
 *
 * Reachability is from a virtual super-source whose arcs to the
 * @p sources are gated by the source tuples' tokens (or always present
 * for certain sources); a vertex's circuit therefore computes "some
 * present source reaches it".  Everything else is as in
 * @c compileAll(); the super-source itself is not reported in the
 * roots.
 *
 * @param rows        Edge tuples.
 * @param sources     Source arcs (at least one).
 * @param directed    If @c false, every edge contributes both arcs.
 * @param max_states  Bound on the DP state count per node.
 * @return            The shared d-DNNF, per-vertex roots, statistics.
 */
static AllResult compileAll(const std::vector<EdgeRow> &rows,
                            const std::vector<SourceArc> &sources,
                            bool directed,
                            std::size_t max_states = DEFAULT_MAX_STATES);

/**
 * @brief Bounded-hop variant of @c compileAll(): per-(vertex, exact
 *        walk length) circuits for every length up to @p hop_bound.
 *
 * Same DP, richer state: the relation entries are *sets of achievable
 * walk lengths* (bitmasks capped at @p hop_bound) instead of single
 * reachability bits, composed in the capped min-plus-set semiring
 * (Kleene closure with diagonal star).  States still partition the
 * worlds of the introduced edge variables, so determinism and
 * decomposability hold by the same argument; the price is the larger
 * state space, guarded by @p max_states as before.
 *
 * @param rows        Edge tuples.
 * @param source      Source vertex.
 * @param directed    If @c false, every edge contributes both arcs.
 * @param hop_bound   Maximum walk length (at most @c MAX_HOP_BOUND).
 * @param max_states  Bound on the DP state count per node.
 * @return            The shared d-DNNF, per-(vertex, length) and
 *                    per-vertex within-bound roots, statistics.
 */
static AllHopsResult compileAllHops(const std::vector<EdgeRow> &rows,
                                    unsigned long source,
                                    bool directed,
                                    unsigned hop_bound,
                                    std::size_t max_states = DEFAULT_MAX_STATES);

/**
 * @brief Multi-source bounded-hop compilation.
 *
 * The virtual super-source's arcs contribute walk length zero, so the
 * reported lengths count edges of the underlying graph only.
 *
 * @param rows        Edge tuples.
 * @param sources     Source arcs (at least one).
 * @param directed    If @c false, every edge contributes both arcs.
 * @param hop_bound   Maximum walk length (at most @c MAX_HOP_BOUND).
 * @param max_states  Bound on the DP state count per node.
 * @return            As the single-source overload.
 */
static AllHopsResult compileAllHops(const std::vector<EdgeRow> &rows,
                                    const std::vector<SourceArc> &sources,
                                    bool directed,
                                    unsigned hop_bound,
                                    std::size_t max_states = DEFAULT_MAX_STATES);

/**
 * @brief Compile "some vertex of @p set is reachable" into one
 *        certified circuit.
 *
 * Same DP, with the state extended by one bit per domain position --
 * "this position reaches some @p set vertex within the processed
 * part" -- folded under closure and projection, so the single
 * acceptance bit (at the source's position, over the root's table) is
 * read in one bottom-up sweep.  This is the deterministic circuit for
 * the OR of the per-vertex reachability roots over @p set -- which,
 * as an OR of *correlated* events (shared edges), could not otherwise
 * be marked: it serves cross-vertex aggregations ("is some vertex of
 * this region reachable") the per-vertex roots cannot.
 *
 * @param rows        Edge tuples.
 * @param sources     Source arcs (at least one).
 * @param set         The target vertex set (need not intersect the
 *                    graph; an absent vertex contributes nothing).
 * @param directed    If @c false, every edge contributes both arcs.
 * @param max_states  Bound on the DP state count per node.
 * @return            The circuit, its root computing the disjunction,
 *                    and statistics.
 */
static Result compileAnyReach(const std::vector<EdgeRow> &rows,
                              const std::vector<SourceArc> &sources,
                              const std::vector<unsigned long> &set,
                              bool directed,
                              std::size_t max_states = DEFAULT_MAX_STATES);

/**
 * @brief Multi-set variant of @c compileAnyReach(): one shared
 *        circuit, one root per target set.
 *
 * The shared prelude -- variable grouping, tree decomposition of the
 * data graph, bag assignments, literal gates -- is built once; one
 * bottom-up sweep then runs per set, with content-deduplicated gate
 * emission, so the parts of the circuit a set's seeds do not touch
 * are literally shared across sets (the per-set sweeps re-derive the
 * same gates).  This is the engine behind cross-vertex aggregation
 * planting, where one query yields many groups over the same graph.
 *
 * @param rows        Edge tuples.
 * @param sources     Source arcs (at least one).
 * @param sets        The target vertex sets (at least one; an absent
 *                    vertex contributes nothing).
 * @param directed    If @c false, every edge contributes both arcs.
 * @param max_states  Bound on the DP state count per node.
 * @return            The shared circuit, one root per set (in input
 *                    order), and statistics.
 */
static AnyReachAllResult compileAnyReachAll(
  const std::vector<EdgeRow> &rows,
  const std::vector<SourceArc> &sources,
  const std::vector<std::vector<unsigned long> > &sets,
  bool directed,
  std::size_t max_states = DEFAULT_MAX_STATES);

/**
 * @brief Compile "every vertex of @p set is reachable" (k-terminal /
 *        coverage reachability) into one certified circuit.
 *
 * Same DP, with the state extended by the *pending rescuer-set
 * antichain*: a forgotten target vertex not yet reached by the source
 * pends on the boundary positions that reach it (its rescuers), the
 * sets staying closed under the relation, shrinking losslessly at
 * forgets, discharged when the source reaches them and absorbed by
 * the empty (reject) set; acceptance, after a final collapse onto the
 * source domain, is the empty antichain.  Worlds map to one state
 * each, so the circuit is a certified d-DNNF like the rest of the
 * family: probability evaluation gives k-terminal reliability, and
 * absorptive-semiring evaluation is exact too -- nonnegative min-plus
 * gives the cost of the cheapest covering subgraph (directed Steiner
 * cost).
 *
 * @param rows        Edge tuples.
 * @param sources     Source arcs (at least one).
 * @param set         The target vertex set; a vertex absent from the
 *                    graph is unreachable, so the result is constant
 *                    false.
 * @param directed    If @c false, every edge contributes both arcs.
 * @param max_states  Bound on the DP state count per node.
 * @return            The circuit, its root computing the conjunction,
 *                    and statistics.
 */
static Result compileCoverReach(const std::vector<EdgeRow> &rows,
                                const std::vector<SourceArc> &sources,
                                const std::vector<unsigned long> &set,
                                bool directed,
                                std::size_t max_states = DEFAULT_MAX_STATES);

/**
 * @brief Multi-set variant of @c compileCoverReach(): one shared
 *        (content-deduplicated) circuit, one root per set, as
 *        @c compileAnyReachAll().
 */
static AnyReachAllResult compileCoverReachAll(
  const std::vector<EdgeRow> &rows,
  const std::vector<SourceArc> &sources,
  const std::vector<std::vector<unsigned long> > &sets,
  bool directed,
  std::size_t max_states = DEFAULT_MAX_STATES);
};

#endif /* REACHABILITY_COMPILER_H */
