/**
 * @file JointEncoding.h
 * @brief Phase A of the joint-width UCQ compiler: assemble the *joint
 *        graph* of the data and its correlation structure, and the
 *        per-fact gating information the homomorphism DP consumes.
 *
 * The joint-width compiler (see @c UCQJointCompiler.h) evaluates an
 * arbitrary UCQ -- including the queries that are @c \#P-hard under the
 * Dalvi-Suciu dichotomy -- exactly, in time tractable whenever the
 * **joint treewidth** of the data and its correlation structure is
 * bounded (Amarilli, PhD thesis tel-01345836, §4.2; Amarilli, Bourhis
 * & Senellart, arXiv:1511.08723 App. D).  The screen must run on the
 * *joint* graph and nothing weaker: thesis Prop. 4.2.11 exhibits
 * instances with @c tw(data)=1 and @c tw(circuit)=0 that are still
 * @c \#P-hard, all the hardness living in the fact-to-gate mapping.
 *
 * @c JointEncoding builds that graph.  In the data-graph regime
 * (every fact gated by an independent @c gate_input, pairwise-distinct
 * tokens -- the §3.5 fast path) the joint graph is exactly the Gaifman
 * graph of the facts and the screen is the data treewidth, which is the
 * sound screen there (no correlation edges exist for Prop. 4.2.11 to
 * exploit).  Correlated inputs -- facts whose provenance tokens are
 * internal gates over shared events -- additionally contribute the
 * circuit slice's vertices and wires (handled by the gate
 * machinery); their shared event leaves become real shared
 * vertices, so the co-occurrence cliques of Prop. 4.3.2 arise
 * automatically.
 */
#ifndef JOINT_ENCODING_H
#define JOINT_ENCODING_H

#include <stdexcept>
#include <string>
#include <vector>

#include "Graph.h"

/**
 * @brief Exception thrown when joint-width compilation cannot proceed.
 *
 * Raised on unsupported input shapes (a @c gate_input token shared by
 * facts over different element tuples, an unsupported gate type in a
 * circuit slice) so the SQL layer can fall back to the standard ladder.
 * Joint width above the configured bound surfaces as the usual
 * @c TreeDecompositionException instead.
 */
class JointCompilerException : public std::runtime_error {
public:
/** @brief Construct with a human-readable message. @param what Message. */
explicit JointCompilerException(const std::string &what)
  : std::runtime_error(what) {
}
};

/**
 * @brief One row of an atom's relation, as handed in by the SQL layer.
 *
 * Mirrors the columnar convention of @c reachability_evaluate: the SQL
 * wrapper collects the post-selection rows of each relation, maps
 * arbitrary SQL values to dense element ids **with a dictionary shared
 * across relations** (so join-compatible values get the same id), and
 * passes parallel arrays.
 */
struct FactRow {
  unsigned relation_id = 0;             ///< Dense id of the relation symbol.
  std::vector<unsigned long> elements;  ///< Dense ids of the row's domain elements.
  std::string token;                    ///< Provenance gate (UUID); empty marks a certain (untracked) fact.
  double prob = 1.0;                    ///< Tuple probability (for an independent @c gate_input token).
};

/**
 * @brief How a fact's presence is gated in a possible world.
 *
 * The data-graph fast path uses only @c CERTAIN and @c INDEP; the
 * correlated regime adds @c GATE (presence is the value of an internal
 * gate of the shared circuit slice).
 */
enum class FactGateKind {
  CERTAIN,   ///< Always present: an untracked relation, constant-true token.
  INDEP,     ///< Present iff its independent Bernoulli event is drawn (data-graph regime).
  GATE       ///< Present iff its slice gate evaluates true (correlated regime).
};

/**
 * @brief Gate kind of a circuit-slice node (correlated regime).
 *
 * The slice is normalised to arity ≤ 2 (a fan-in-@e k AND/OR becomes a
 * balanced binary tree of fresh gates), so AND/OR have exactly two
 * children and NOT exactly one; INPUT leaves carry a probability.
 */
enum class SliceGateType { INPUT, AND, OR, NOT };

/**
 * @brief One node of the extracted circuit slice (correlated regime).
 */
struct SliceGate {
  SliceGateType type = SliceGateType::INPUT; ///< Node kind.
  std::vector<unsigned> children;            ///< Child slice indices (≤ 2; empty for INPUT).
  double prob = 1.0;                         ///< Marginal (INPUT only).
  std::string token;                         ///< Provenance token of the leaf (INPUT only; for the d-D IN gate UUID).
};

/**
 * @brief A deduplicated fact participating in the DP.
 *
 * Several @c FactRow occurrences of the same provenance token (a
 * self-join scanning one relation, or duplicated provenance) collapse
 * to a single @c Fact serving every matching atom -- treating the
 * occurrences as independent would be silently wrong, so the encoding
 * dedups by token.
 */
struct Fact {
  unsigned relation_id = 0;             ///< Dense id of the relation symbol.
  std::vector<unsigned long> elements;  ///< Dense element ids (the fact's tuple).
  FactGateKind kind = FactGateKind::INDEP; ///< How the fact's presence is gated.
  std::size_t event = 0;                ///< Index into @c events (when @c INDEP).
  std::size_t gate = 0;                 ///< Index into @c slice (when @c GATE).
};

/**
 * @brief One independent Bernoulli world variable.
 *
 * In the data-graph regime each fact is its own event; the correlated
 * regime makes events the leaves of the shared circuit slice.
 */
struct Event {
  std::string token;   ///< Provenance token (UUID) of the leaf.
  double prob = 1.0;   ///< Marginal probability.
};

/**
 * @brief The joint encoding of an instance: facts, world events, and
 *        the joint graph the screen and the DP run over.
 */
class JointEncoding {
public:
std::vector<Fact> facts;     ///< Deduplicated facts.
std::vector<Event> events;   ///< Independent world variables (data-graph regime).
std::vector<SliceGate> slice;///< Circuit slice (correlated regime); gate vertex = @c n_elements + index.
bool correlated = false;     ///< @c true when the slice is present (correlated regime).
unsigned long n_elements = 0;///< One past the largest domain element id (vertex ids @c [0,n_elements)).

unsigned data_treewidth_lb = 0;    ///< Degeneracy lower bound of the data-only graph (diagnostics).
unsigned circuit_treewidth_lb = 0; ///< Degeneracy lower bound of the slice-only graph (0 in the data-graph regime).

/**
 * @brief Build the data-graph (§3.5 fast path) encoding from raw rows.
 *
 * Dedups facts by token (a token over identical element tuples serves
 * many atoms; a @c gate_input token shared across *different* element
 * tuples is rejected with @c JointCompilerException, routing the caller
 * to the general path).  Each independent fact becomes one Bernoulli
 * event; certain facts (empty token) are always present.  The joint
 * graph is the Gaifman graph of the facts (one clique per fact over its
 * elements).
 *
 * @param rows  Per-atom relation rows (already dense-encoded).
 * @return      The assembled encoding.
 */
static JointEncoding fromFacts(const std::vector<FactRow> &rows);

/**
 * @brief Construct the correlated-regime encoding from facts and an
 *        already-extracted circuit slice.
 *
 * The caller (the SQL glue) walks the mmap circuit from the distinct
 * fact tokens down to @c gate_input leaves and normalises the slice to
 * arity ≤ 2, filling @p slice and, per fact, the index of its slice
 * gate (or marking it certain for an untracked / constant-true token).
 * This factory assembles the joint graph (element vertices, gate
 * vertices, fact-to-gate and gate-wire cliques) and the diagnostics.
 *
 * @param facts        Deduplicated facts (kind @c GATE carry a slice
 *                     index in @c Fact::gate; kind @c CERTAIN are always
 *                     present).
 * @param slice        The circuit slice (arity ≤ 2).
 * @param n_elements   One past the largest element id used.
 * @return             The assembled correlated encoding.
 */
static JointEncoding fromCorrelated(std::vector<Fact> facts,
                                    std::vector<SliceGate> slice,
                                    unsigned long n_elements);

/**
 * @brief Construct the joint graph for the screen and the DP.
 *
 * Data-graph regime: the Gaifman graph of the facts -- per-fact cliques
 * over domain elements, isolated nodes for unary facts.  Correlated
 * regime: additionally the gate vertices @c [n_elements, n_elements +
 * |slice|), with a clique over @c {elements} ∪ @c {fact gate} per fact
 * (thesis Def. 4.2.5, rule 1) and a clique over @c {gate} ∪
 * @c {children} per internal slice gate (the stronger ternary
 * co-occurrence of arXiv:1511.08723, rule 2).
 *
 * @return  The joint graph (a fresh copy; the decomposition consumes it).
 */
Graph buildGraph() const;
};

#endif /* JOINT_ENCODING_H */
