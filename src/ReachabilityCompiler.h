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

#include <bitset>
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

private:
/**
 * @brief Shared implementation of @c compile() / @c compileAll().
 *
 * @param rows         Edge tuples.
 * @param source       Source vertex.
 * @param directed     If @c false, every edge contributes both arcs.
 * @param max_states   Bound on the DP state count per node.
 * @param only_target  When set: ensure the vertex exists in the graph
 *                     (an isolated target is legal) and emit a root for
 *                     it alone, skipping the other vertices' reads.
 * @return             The shared d-DNNF, per-vertex roots, statistics.
 */
static AllResult compileAllInternal(const std::vector<EdgeRow> &rows,
                                    unsigned long source,
                                    bool directed,
                                    std::size_t max_states,
                                    const unsigned long *only_target,
                                    const std::vector<SourceArc> *multi_sources);
/** @brief Maximum size of a DP domain: a bag (@c MAX_TREEWIDTH+1) plus the two terminals. */
static constexpr int MAXD = TreeDecomposition::MAX_TREEWIDTH+3;

/**
 * @brief A reachability relation over a DP domain, as a bitset.
 *
 * Bit @c i*MAXD+j is set iff the domain's @c j-th vertex is reachable
 * from its @c i-th vertex within the processed part of the graph.
 * Always reflexive and transitively closed.
 */
using Rel = std::bitset<MAXD*MAXD>;

/**
 * @brief Transitive closure of @p r over the first @p d domain positions.
 * @param r  Relation to close.
 * @param d  Domain size.
 * @return   The closed relation.
 */
static Rel transitiveClosure(Rel r, int d);
};

#endif /* REACHABILITY_COMPILER_H */
