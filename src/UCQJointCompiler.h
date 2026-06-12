/**
 * @file UCQJointCompiler.h
 * @brief Phase C of the joint-width UCQ compiler: a UCQ-specialised
 *        homomorphism-type DP that runs directly over a tree
 *        decomposition of the joint encoding, emitting a certified d-D
 *        (deterministic, decomposable circuit) by construction.
 *
 * This is the data-side counterpart of ProvSQL's circuit-side treewidth
 * exploitation, and the UCQ counterpart of @c ReachabilityCompiler: a
 * bag-by-bag dynamic program whose state carries, for each disjunct, the
 * set of **partial homomorphisms** realised by the facts of the
 * processed subtree (the correlated regime additionally carries in-bag
 * **gate valuations**).  It is the operational form of the thesis's
 * @f$\exists T\, q''(T) \wedge q_{wf}(T)@f$ rewriting (Amarilli, PhD
 * thesis tel-01345836, Prop. 4.2.9): rather than guessing the set @c T
 * of true gates with a second-order variable, the DP enumerates
 * per-bag valuations, with mutual exclusivity giving determinism for
 * free.
 *
 * Each transition emits d-D gates directly:
 *
 * - states at a node are **mutually exclusive and exhaustive** over the
 *   valuations of the world variables introduced in its subtree (every
 *   world induces exactly one (hom-set, sat) state), so every OR gate is
 *   deterministic *by construction*;
 * - each world variable is introduced at exactly one node, so the
 *   children of every AND gate mention disjoint variable sets and the
 *   circuit is decomposable *by construction*.
 *
 * No knowledge-compilation step is therefore needed: the result feeds
 * straight to @c dDNNF::probabilityEvaluation() (the marginal of an
 * irrelevant world variable folds to a unit factor, exactly as in the
 * reachability compiler, so the strictly-non-smooth circuit still
 * evaluates correctly).  For a joint encoding of width @f$k@f$ and a
 * UCQ whose disjuncts have at most @f$e@f$ enumerating variables, the
 * DP state is bounded by a function of @f$k@f$ and @f$e@f$ alone, giving
 * exact probability of a @c \#P-hard UCQ in time linear in the data.
 *
 * The exponential parameter is the number of **enumerating** variables
 * (existential join variables outside the key/FD determination closure),
 * not @c n_vars; the design target is queries with at most 4-5 of them.
 */
#ifndef UCQ_JOINT_COMPILER_H
#define UCQ_JOINT_COMPILER_H

#include <cstddef>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "JointEncoding.h"
#include "TreeDecomposition.h"
#include "dDNNF.h"

/**
 * @brief One atom of a conjunctive query: a relation symbol applied to
 *        query variables.
 *
 * Constants are pre-filtered by the SQL layer (selection pushed down),
 * so an atom mentions only variables; @c vars index into the disjunct's
 * variable list.  A repeated variable encodes an equality (a self-join
 * on equal columns); two atoms may share a @c relation_id (a self-join).
 */
struct Atom {
  unsigned relation_id = 0;       ///< Dense id of the relation symbol.
  std::vector<unsigned> vars;     ///< Query-variable indices, one per column.
};

/**
 * @brief One conjunctive query (a disjunct of the UCQ).
 */
struct CQ {
  unsigned n_vars = 0;            ///< Number of query variables.
  std::vector<Atom> atoms;        ///< The conjuncts.
};

/**
 * @brief A union of conjunctive queries.
 */
struct UCQ {
  std::vector<CQ> disjuncts;      ///< The disjuncts (at least one).
};

/**
 * @brief Compiles a Boolean UCQ over a joint encoding into a certified
 *        d-D, along a tree decomposition of the joint graph.
 */
class UCQJointCompiler {
public:
/** @brief Structural statistics of a compilation, for diagnostics and tests. */
struct Stats {
  unsigned joint_treewidth = 0;       ///< Treewidth of the min-fill decomposition of the joint graph.
  unsigned data_treewidth_lb = 0;     ///< Degeneracy lower bound of the data-only graph.
  unsigned circuit_treewidth_lb = 0;  ///< Degeneracy lower bound of the slice-only graph.
  std::size_t nb_bags = 0;            ///< Number of bags of the decomposition.
  std::size_t max_states = 0;         ///< Maximum number of DP states at any node.
  std::size_t dd_size = 0;            ///< Number of gates of the emitted d-D.
  std::size_t nb_variables = 0;       ///< Number of world variables (events).
  std::vector<unsigned> n_enumerating;///< Per-disjunct static count of enumerating variables.
};

/** @brief A compiled UCQ: the d-D and its statistics. */
struct Result {
  dDNNF dd;      ///< d-D whose root computes "the UCQ holds"; input gates carry the event tokens and probabilities.
  Stats stats;   ///< Compilation statistics.
};

/**
 * @brief Default bound on the number of DP states at a single node.
 *
 * The cap (not the static enumerating-variable count) is the true
 * safety net: the realised-state count is governed by data sparsity and
 * the absorbing @c sat collapse, typically far below the a-priori bound.
 */
static constexpr std::size_t DEFAULT_MAX_STATES = 1u << 16;

/**
 * @brief Compile a Boolean UCQ over @p enc into a certified d-D.
 *
 * @param enc            The joint encoding (facts, events, joint graph).
 * @param ucq            The Boolean UCQ.
 * @param max_treewidth  Reject (fall through to the ladder) when the
 *                       joint width exceeds this.
 * @param max_states     Bound on the DP state count per node.
 * @return               The compiled d-D and statistics.
 * @throws TreeDecompositionException  if the joint width exceeds
 *                                     @p max_treewidth (the degeneracy
 *                                     screen or the min-fill build).
 * @throws JointCompilerException      on unsupported input shapes or
 *                                     when @p max_states is exceeded.
 */
static Result compile(const JointEncoding &enc,
                      const UCQ &ucq,
                      unsigned max_treewidth = TreeDecomposition::MAX_TREEWIDTH,
                      std::size_t max_states = DEFAULT_MAX_STATES);

/**
 * @brief Per-answer evaluation by a single top-down DP (data-graph regime).
 *
 * The full multi-output construction (Amarilli, tel-01345836, §4.2.9): ONE
 * bottom-up sweep emits one circuit root per answer, rather than @c k
 * head-pinned @c compile() sweeps.  The head variables become a
 * **state-level key** -- they are never existentially projected (a forgotten
 * head element is recorded as a fixed value), completed answers are tracked
 * per head-tuple in the state, and an answer is emitted as its own d-DNNF
 * root when its head elements leave the tree decomposition.  All answer roots
 * share one circuit, so a single probability pass (with the gate cache) values
 * them all.  The candidate answers are **discovered** by the sweep -- no
 * candidate list is needed.  Equivalent answers and probabilities to
 * @c k head-pinned @c compile() calls, in one pass instead of @c k.
 *
 * The compiler's job ends at the **circuit**: it returns the shared d-D and
 * one root gate per answer (the materialisation / probability / Shapley is the
 * caller's, on the returned roots), keeping a single evaluation pipeline.
 *
 * @param enc        The joint encoding (data-graph / TID-BID or correlated).
 * @param ucq        The UCQ; the head variables must occur in every disjunct.
 * @param head_vars  Query-variable indices of the head.
 */
struct AnswerRoot {
  std::vector<unsigned long> head;  ///< Bound element value per head variable.
  gate_t root;                      ///< This answer's d-D root in @c dd.
};
struct AnswerCircuit {
  dDNNF dd;                         ///< The shared certified d-D.
  std::vector<AnswerRoot> answers;  ///< One root per discovered answer.
  std::size_t max_states = 0;       ///< Peak DP state count.
};
static AnswerCircuit compileAnswersOneDP(
  const JointEncoding &enc,
  const UCQ &ucq,
  const std::vector<unsigned> &head_vars,
  unsigned max_treewidth = TreeDecomposition::MAX_TREEWIDTH,
  std::size_t max_states = DEFAULT_MAX_STATES);
};

#endif /* UCQ_JOINT_COMPILER_H */
