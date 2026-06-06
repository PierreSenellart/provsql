/**
 * @file BooleanCircuit.h
 * @brief Boolean provenance circuit with support for knowledge compilation.
 *
 * @c BooleanCircuit represents the provenance formula of a query result
 * as a Boolean circuit (AND/OR/NOT/IN gates).  It supports multiple
 * methods for computing the probability of the formula being true under
 * tuple-independent probabilistic databases:
 *
 * | Method                 | Description                                              |
 * |------------------------|----------------------------------------------------------|
 * | @c possibleWorlds()    | Exact enumeration over all 2^n possible worlds           |
 * | @c compilation()       | Knowledge compilation to d-DNNF via an external tool     |
 * | @c monteCarlo()        | Monte Carlo sampling approximation                       |
 * | @c WeightMC()          | Weighted model counting via weightmc                     |
 * | @c independentEvaluation() | Exact evaluation for disconnected circuits           |
 * | @c interpretAsDD()     | Direct tree-decomposition-based compilation              |
 * | @c makeDD()            | Generic d-DNNF construction dispatcher                   |
 *
 * ### Multivalued inputs (@c MULIN / @c MULVAR)
 * Multivalued input gates model tuples drawn from a discrete probability
 * distribution.  @c rewriteMultivaluedGates() rewrites them into standard
 * AND/OR/NOT circuits before knowledge compilation.
 *
 * The circuit is Boost-serialisable for transmission to external processes.
 */
#ifndef BOOLEAN_CIRCUIT_H
#define BOOLEAN_CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <map>
#include <vector>
#include <iosfwd>

#include <boost/archive/binary_oarchive.hpp>
#include <boost/serialization/unordered_map.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/set.hpp>
#include <boost/serialization/vector.hpp>

#include "Circuit.hpp"

/**
 * @brief Gate types for a Boolean provenance circuit.
 *
 * - @c UNDETERMINED  Placeholder for a gate whose type has not been set yet.
 * - @c AND           Logical conjunction of child gates.
 * - @c OR            Logical disjunction of child gates.
 * - @c NOT           Logical negation of a single child gate.
 * - @c IN            An input (variable) gate representing a base tuple.
 * - @c MULIN         A multivalued-input gate (one of several options).
 * - @c MULVAR        Auxiliary gate grouping all @c MULIN siblings.
 */
enum class BooleanGate {
  UNDETERMINED, ///< Placeholder gate whose type has not been set yet
  AND,          ///< Logical conjunction of child gates
  OR,           ///< Logical disjunction of child gates
  NOT,          ///< Logical negation of a single child gate
  IN,           ///< Input (variable) gate representing a base tuple
  MULIN,        ///< Multivalued-input gate (one of several options)
  MULVAR        ///< Auxiliary gate grouping all MULIN siblings
};
class dDNNF;

/**
 * @brief d-DNNF certificate value for the (gate-type-specific) per-gate
 *        info field.
 *
 * On an OR (@c gate_plus) gate, info = @c DNNF_CERT_INFO asserts
 * **determinism**: the children are mutually exclusive.  On an AND
 * (@c gate_times) gate it asserts **decomposability**: the children
 * mention disjoint sets of variables.  Both are semantic properties of
 * the gate's (content-addressed) children, so a truthfully-set mark
 * remains true however the gate is later re-derived.  Set by
 * constructions that establish the property structurally (the
 * decomposition-aligned reachability compilation); trusted by
 * @c independentEvaluation() and @c interpretAsDD(), the same trust
 * model as the planner-asserted inversion-free certificate.
 */
constexpr unsigned DNNF_CERT_INFO = 1;

/**
 * @brief Boolean circuit for provenance formula evaluation.
 *
 * Inherits the gate/wire infrastructure from @c Circuit<BooleanGate> and
 * adds probability annotation, info integers (for multivalued inputs), and
 * a rich set of evaluation algorithms.
 */
class BooleanCircuit : public Circuit<BooleanGate> {
private:
/**
 * @brief Evaluate the sub-circuit at @p g on one sampled world.
 *
 * Each gate in @p sampled is treated as @c true; all other @c IN gates
 * are @c false.
 *
 * @param g        Root gate to evaluate.
 * @param sampled  Set of input gates that are @c true in this world.
 * @return         Boolean value of the circuit at gate @p g.
 */
bool evaluate(gate_t g, const std::unordered_set<gate_t> &sampled) const;

/**
 * @brief Recursive helper for @c interpretAsDD().
 *
 * @param g       Current gate to process.
 * @param seen    Set of variable gates already consumed (read-once check
 *                in the uncertified region; per-island registration in
 *                certified regions).
 * @param dd      The d-DNNF being constructed.
 * @param island  Island-local memoisation (source gate to @p dd gate) for
 *                the certified region being walked, or @c nullptr in the
 *                uncertified region.  Within an island, shared sub-DAGs
 *                map to shared @p dd gates and each variable registers in
 *                @p seen once; sharing across islands re-walks and throws.
 * @return        Gate ID in @p dd corresponding to @p g.
 */
gate_t interpretAsDDInternal(gate_t g, std::set<gate_t> &seen, dDNNF &dd,
                             std::unordered_map<gate_t, gate_t> *island) const;
/**
 * @brief Recursive helper for @c independentEvaluation().
 * @param g     Current gate to evaluate.
 * @param seen  Set of variable gates (IN / MULVAR) already consumed; a second
 *              occurrence means the circuit is not read-once.
 * @param memo  Memoised probability of variable-free (constant-only) gates, so a
 *              shared constant subgraph is evaluated once -- this is what keeps
 *              the whole evaluation @c O(circuit) rather than re-traversing
 *              shared subgraphs.  Variable-bearing gates are never memoised (a
 *              re-visit must reach @p seen and throw).
 * @param island  Island-local memoisation for the certified (d-DNNF-marked)
 *                region being walked, or @c nullptr in the uncertified
 *                region.  A certified gate reached from the uncertified
 *                region starts a maximal island: inside it every gate is
 *                memoised (sharing is licensed by the certificate) and each
 *                variable still registers once in @p seen, so entanglement
 *                with the outside -- or with another island -- throws like a
 *                read-once violation.
 * @return      Probability at gate @p g.
 */
double independentEvaluationInternal(
  gate_t g, std::set<gate_t> &seen,
  std::unordered_map<gate_t, double> &memo,
  std::unordered_map<gate_t, double> *island) const;

public:
/**
 * @brief Is gate @p g certified by the d-DNNF per-gate marking?
 *
 * @c true iff @p g is an OR marked deterministic or an AND marked
 * decomposable (info = @c DNNF_CERT_INFO); see @c DNNF_CERT_INFO for the
 * semantics and trust model.
 *
 * @param g  Gate to test.
 * @return   Whether the gate carries the d-DNNF certificate.
 */
bool isDNNFCertified(gate_t g) const {
  const auto t = getGateType(g);
  return (t == BooleanGate::AND || t == BooleanGate::OR) &&
         getInfo(g) == DNNF_CERT_INFO;
}

private:
/**
 * @brief Recursive helper for @c rewriteMultivaluedGates().
 * @param muls              Gates in the MULVAR group being rewritten.
 * @param cumulated_probs   Cumulative probability thresholds for each MULIN.
 * @param start             First index in @p muls to process.
 * @param end               One past the last index in @p muls to process.
 * @param prefix            Current AND-chain prefix being built.
 */
void rewriteMultivaluedGatesRec(
  const std::vector<gate_t> &muls,
  const std::vector<double> &cumulated_probs,
  unsigned start,
  unsigned end,
  std::vector<gate_t> &prefix);

protected:
std::set<gate_t> inputs;     ///< Set of IN (input) gate IDs
std::set<gate_t> mulinputs;  ///< Set of MULVAR gate IDs
std::vector<double> prob;    ///< Per-gate probability (for IN gates)
std::map<gate_t, unsigned> info; ///< Per-gate integer info (for MULIN gates)
bool probabilistic=false;    ///< @c true if any gate has a non-unit probability

public:
/** @brief Construct an empty Boolean circuit. */
BooleanCircuit() {
}
virtual ~BooleanCircuit() {
}

/** @copydoc Circuit::addGate() */
gate_t addGate() override;
/** @copydoc Circuit::setGate(gateType) */
gate_t setGate(BooleanGate type) override;
/** @copydoc Circuit::setGate(const uuid&, gateType) */
gate_t setGate(const uuid &u, BooleanGate type) override;

/**
 * @brief Create a new gate with a probability annotation.
 * @param t  Gate type (typically @c BooleanGate::IN).
 * @param p  Probability of this gate being @c true.
 * @return   Gate identifier.
 */
gate_t setGate(BooleanGate t, double p);

/**
 * @brief Create (or update) a gate with a UUID and probability.
 * @param u  UUID string.
 * @param t  Gate type.
 * @param p  Probability of this gate being @c true.
 * @return   Gate identifier.
 */
gate_t setGate(const uuid &u, BooleanGate t, double p);

/**
 * @brief Return the set of input (IN) gate IDs.
 * @return Const reference to the set of IN gate identifiers.
 */
const std::set<gate_t> &getInputs() const {
  return inputs;
}

/**
 * @brief Return @c true if the circuit contains any MULIN gates.
 *
 * Multivalued inputs are normally rewritten into AND/OR/NOT/IN gates by
 * @c rewriteMultivaluedGates() before the circuit is consumed by an
 * evaluation method.  Algorithms that cannot handle multivalued inputs
 * directly can use this as a precondition check.
 *
 * @return @c true iff at least one @c MULIN gate is present.
 */
bool hasMultivaluedGates() const {
  return !mulinputs.empty();
}

/**
 * @brief Set the probability for gate @p g and mark the circuit as probabilistic.
 * @param g  Gate identifier.
 * @param p  Probability value in [0, 1].
 */
void setProb(gate_t g, double p) {
  if(!probabilistic && p!=1.)
    probabilistic=true;
  prob[static_cast<std::underlying_type<gate_t>::type>(g)]=p;
}

/**
 * @brief Return the probability stored for gate @p g.
 * @param g  Gate identifier.
 * @return   Probability value.
 */
double getProb(gate_t g) const {
  return prob[static_cast<std::underlying_type<gate_t>::type>(g)];
}

/**
 * @brief Return @c true if any gate has a non-trivial (< 1) probability.
 * @return @c true iff at least one gate has a probability strictly less than 1.
 */
bool isProbabilistic() const {
  return probabilistic;
}

/**
 * @brief Store an integer annotation on gate @p g.
 *
 * Used to record the index of a @c MULIN gate within its @c MULVAR group.
 *
 * @param g     Gate identifier.
 * @param info  Integer to store.
 */
void setInfo(gate_t g, unsigned info);

/**
 * @brief Return the integer annotation for gate @p g.
 * @param g  Gate identifier.
 * @return   Stored integer, or 0 if not set.
 */
unsigned getInfo(gate_t g) const;

/**
 * @brief Compute the probability by exact enumeration of all possible worlds.
 *
 * Only tractable for circuits with a small number of input gates.
 *
 * @param g  Root gate.
 * @return   Exact probability.
 */
double possibleWorlds(gate_t g) const;

/**
 * @brief Compile the sub-circuit rooted at @p g to a @c dDNNF via an external tool.
 *
 * Writes the circuit in DIMACS/DNNF format, invokes @p compiler as a
 * subprocess, and parses the resulting d-DNNF.
 *
 * @param g         Root gate.
 * @param compiler  Command to invoke (e.g. "d4", "c2d", "minic2d").  Empty
 *                  auto-selects the highest-preference available tool.
 * @param resolved  If non-null, set to the tool actually used (after the
 *                  empty -> auto-select resolution), so callers can report
 *                  WHICH compiler ran rather than just "compilation".
 * @return          The compiled @c dDNNF.
 */
dDNNF compilation(gate_t g, std::string compiler,
                  std::string *resolved = nullptr) const;

/**
 * @brief Parse a c2d/d4 NNF stream into a @c dDNNF over this circuit's input
 *        gates.
 *
 * Shared by the CLI @c compilation() path and the KCMCP client: both obtain
 * the same NNF text (from a temp file or a socket @c RESULT) and parse it
 * identically.  @p inputOrder maps d4 circuit-mode variables (1..k) to IN
 * gates; an empty vector means CNF mode, where d-DNNF variable @c v stands
 * for gate id @c v-1 (real only for IN gates, every other being a Tseytin
 * auxiliary that is projected out).
 *
 * @param in          NNF text stream.
 * @param inputOrder  Circuit-mode input-variable to IN-gate map (empty = CNF).
 * @return            The compiled @c dDNNF (empty if the formula is unsat).
 */
dDNNF parseDDNNF(std::istream &in,
                 const std::vector<gate_t> &inputOrder) const;

/**
 * @brief Estimate the probability via Monte Carlo sampling.
 *
 * @param g        Root gate.
 * @param samples  Number of independent worlds to sample.
 * @return         Estimated probability.
 */
double monteCarlo(gate_t g, unsigned samples) const;

/**
 * @brief Detect the DNF shape the Karp-Luby FPRAS requires.
 *
 * Recognises the two tractable regimes: (a) a single AND-of-leaves
 * clause, or (b) a top-level @c OR whose every child is an AND-only
 * sub-circuit over @c IN leaves (no @c OR below the root, no @c NOT, no
 * multivalued input).  Cross-clause leaf sharing is allowed and is the
 * normal Karp-Luby setting.
 *
 * On success, @p clauses receives one root per top-level disjunct (or the
 * singleton root @p g in regime (a)) and @p supports[i] the set of @c IN
 * leaves reachable from @p clauses[i] through its AND-only stratum -- the
 * support determines @c Pr[C_i] (the product of leaf marginals) and the
 * conditional sampler.
 *
 * @param g         Root gate.
 * @param clauses   Output: the top-level clause roots.
 * @param supports  Output: per-clause set of reachable @c IN leaves.
 * @return          @c true iff the circuit is DNF-shaped (regime (a)/(b)).
 */
bool dnfShape(gate_t g,
              std::vector<gate_t> &clauses,
              std::vector<std::set<gate_t> > &supports) const;

/**
 * @brief Cheap shape test: is the circuit DNF-shaped, and how many clauses?
 *
 * The @c O(circuit) half of @c dnfShape -- validates the OR-of-ANDs-of-leaves
 * shape with a single global visited-set (no per-clause re-walk) and returns the
 * clause count, WITHOUT materialising the per-clause supports (whose total size
 * can be @c O(m*N)).  Used by the chooser to rank @c sieve; the supports are
 * built only if @c sieve / @c karp-luby actually runs (via @c dnfShape).
 *
 * @param g            Root gate.
 * @param num_clauses  Output: number of top-level clauses.
 * @return             @c true iff DNF-shaped.
 */
bool dnfShapeInfo(gate_t g, std::size_t &num_clauses) const;

/**
 * @brief Karp-Luby FPRAS estimate of a DNF-shaped circuit's probability
 *        (fixed sample budget, stratified).
 *
 * Implements the Karp-Luby coverage estimator for the DNF-counting problem
 * (@c \#DNF) under tuple-independent inputs: with @c p_i the product of the
 * marginals of @p supports[i] and @c S the sum of the @c p_i (so @c S in
 * @c [Pr[F], m*Pr[F]]), the estimator over clause @c i samples a satisfying
 * assignment of @c C_i (its support forced true, every other leaf drawn from
 * its marginal), finds the smallest clause index @c j the assignment
 * satisfies, and accepts iff @c j == i; @c Pr[F] is then @c sum_i p_i times
 * the per-clause acceptance rate.  The acceptance probability is @c Pr[F]/S in
 * @c [1/m, 1], so the sample count for an @c (eps,delta) guarantee is
 * independent of @c Pr[F], unlike naive Monte Carlo.
 *
 * The @p samples rounds are spread across clauses by *stratified* allocation
 * (@c n_i proportional to @c p_i/S, every clause sampled at least once),
 * estimating each clause's acceptance rate separately and combining
 * @c sum_i p_i * acceptRate_i.  This removes the variance of the categorical
 * clause draw used by the textbook estimator (between-strata variance),
 * tightening the estimate at the same budget by up to a factor @c m.  When
 * @p samples @c < @c m there are too few rounds for one per clause, so the
 * method falls back to the unstratified categorical-draw estimator (still
 * unbiased for any budget).
 *
 * The @p clauses / @p supports are those returned by @c dnfShape.  The
 * @c mt19937_64 is seeded from @c provsql.monte_carlo_seed exactly as
 * @c monteCarlo, so the estimate is reproducible under a pinned seed.
 *
 * @param clauses   Top-level clause roots (from @c dnfShape).
 * @param supports  Per-clause reachable @c IN leaves (from @c dnfShape).
 * @param samples   Resolved number of sampling rounds.
 * @return          The Karp-Luby probability estimate.
 */
double karpLuby(const std::vector<gate_t> &clauses,
                const std::vector<std::set<gate_t> > &supports,
                unsigned long samples) const;

/**
 * @brief Exact probability of a monotone DNF by inclusion-exclusion (sieve).
 *
 * @c Pr[∨_i c_i] = Σ_{∅≠S⊆clauses} (-1)^{|S|+1} Pr[∧_{i∈S} c_i].  Each clause is
 * a conjunction of positive input leaves, so the conjunction of a set @c S of
 * clauses is the AND of the union of their supports, and over independent
 * inputs @c Pr[∧_{i∈S} c_i] = ∏_{leaf ∈ ∪supports(S)} getProb(leaf).  Exact, and
 * @c O(2^m) in the clause count @c m -- the portfolio member to pick when @c m
 * is small (a handful of clauses), where it beats the general compilers.
 *
 * @p clauses / @p supports are those returned by @c dnfShape (monotone DNF over
 * input leaves).  Throws when @c m exceeds @c kSieveMaxClauses (the @c 2^m
 * enumeration would be impractical) so the caller can pick another method.
 *
 * @param clauses   Top-level clause roots (from @c dnfShape).
 * @param supports  Per-clause reachable @c IN leaves (from @c dnfShape).
 * @return          The exact probability.
 */
double sieve(const std::vector<gate_t> &clauses,
             const std::vector<std::set<gate_t> > &supports) const;

/**
 * @brief Cheap certified probability interval @c [lower,upper] of a monotone
 *        DNF, without compiling it (Olteanu-Huang-Koch d-tree leaf bound).
 *
 * Implements the @c Independent heuristic of Olteanu, Huang & Koch,
 * "Approximate Confidence Computation in Probabilistic Databases" (ICDE 2010,
 * Fig. 3).  The clauses are greedily partitioned into @e buckets of pairwise
 * independent clauses (disjoint supports), clauses taken in descending
 * marginal-probability order so the most probable clauses anchor the buckets.
 * Each bucket's clauses are mutually independent, so its probability is the
 * independent-or @c 1-∏(1-P(d)) with @c P(d)=∏_{leaf∈supports[d]} getProb(leaf).
 * Then, since @c Φ is the disjunction of all buckets:
 *
 * - @c lower = max_i P(B_i): a sub-disjunction is a lower bound;
 * - @c upper = min(1, Σ_i P(B_i)): the union bound.
 *
 * Both bounds are sound for @e any partition (the greedy one only affects
 * tightness), so @c lower ≤ Pr[Φ] ≤ upper always holds.  When the clauses are
 * mutually independent (disjoint supports) they all land in a single bucket and
 * @c lower=upper=Pr[Φ], i.e. the interval collapses to the exact value.
 * @c O(m^2) in the clause count @c m.
 *
 * A monotone DNF is fully determined (for probability) by its per-clause input
 * supports, so this takes only the @c supports (the @c set per clause returned
 * by @c dnfShape, or a cofactor's residual clause set in the @c DTree engine);
 * the clause root gates are not needed.
 *
 * @param clauses  Per-clause input-leaf supports (a monotone DNF as a set of
 *                 clauses, each a set of @c IN leaves).
 * @param lower    [out] Certified lower bound on @c Pr[Φ].
 * @param upper    [out] Certified upper bound on @c Pr[Φ].
 */
void dnfBounds(const std::vector<std::set<gate_t> > &clauses,
               double &lower, double &upper) const;

/**
 * @brief Karp-Luby FPRAS with the self-adjusting stopping rule (adaptive
 *        sample count for a relative @c (eps,delta) guarantee).
 *
 * The Dagum-Karp-Luby-Ross stopping rule (SICOMP 2000, the optimal form of
 * the Karp-Luby-Madras 1989 self-adjusting rule): rather than fixing the
 * number of rounds from the worst-case acceptance probability @c 1/m, draw
 * coverage trials (clause @c i with probability @c p_i/S, then the
 * smallest-index coverage test of @c karpLuby) until the *accept count*
 * reaches the deterministic threshold
 * @c Y1 = 1 + (1+eps) * 4*(e-2)*ln(2/delta)/eps^2, then return
 * @c S * Y1 / N over the @c N rounds actually run.  That estimate is a
 * relative @c (eps,delta) approximation of @c Pr[F], and @c N adapts to the
 * true acceptance probability @c Pr[F]/S (expected @c N is @c Y1*S/Pr[F],
 * i.e. up to @c m times fewer rounds than the fixed bound when the clauses
 * barely overlap).
 *
 * Sampling stops early at @p max_samples rounds; @p reached_target is then
 * @c false and the return is the plain unbiased @c S*accepts/N estimate over
 * the spent budget (the @c (eps,delta) target was not met -- the caller
 * reports the weaker guarantee actually achieved).
 *
 * @param clauses         Top-level clause roots (from @c dnfShape).
 * @param supports        Per-clause reachable @c IN leaves (from @c dnfShape).
 * @param eps             Target relative error (in @c (0,1]).
 * @param delta           Target failure probability (in @c (0,1)).
 * @param max_samples     Hard cap on the number of rounds.
 * @param samples_used    Output: rounds actually run.
 * @param reached_target  Output: whether the stopping threshold was reached
 *                        before @p max_samples (i.e. the guarantee holds).
 * @return                The Karp-Luby probability estimate.
 */
double karpLubyStopping(const std::vector<gate_t> &clauses,
                        const std::vector<std::set<gate_t> > &supports,
                        double eps, double delta,
                        unsigned long max_samples,
                        unsigned long &samples_used,
                        bool &reached_target) const;

/**
 * @brief Weighted model counting through a registered external counter.
 *
 * Generic over the counter: @p tool names a registry record with the @c "wmc"
 * operation (today @c weightmc, @c ganak, @c sharpsat-td, @c dpmc, or any
 * tool an administrator registers).  The record's @c binary, @c dependencies,
 * @c argtpl and @c parser drive the whole call -- which weighted-CNF dialect
 * to write, the command to run, and how to read the count back -- so there is
 * no per-counter code path.  Two output/input conventions are understood, by
 * @c parser: @c "wmc-line" (MCC-2024 weighted DIMACS in, a @c "c s exact" /
 * @c "s wmc" count line out) and @c "weightmc" (weightmc's own dialect in, a
 * @c "mantissa x 2^exp" line out).
 *
 * @param g     Root gate of the sub-circuit.
 * @param tool  Logical name of the wmc tool to use.
 * @param opt   Tool options; for the approximate counters of the form
 *              @c "delta;epsilon" (drives the @c {pivotAC} placeholder).
 * @return      The weighted model count = P(formula).
 */
double wmcCount(gate_t g, const std::string &tool, const std::string &opt) const;

/**
 * @brief Compute the probability exactly when inputs are independent.
 *
 * Applicable when the circuit has no shared input gate (i.e., each
 * input appears at most once).
 *
 * @param g  Root gate.
 * @return   Exact probability.
 */
double independentEvaluation(gate_t g) const;

/**
 * @brief Rewrite all MULVAR/MULIN gate clusters into standard AND/OR/NOT circuits.
 *
 * Must be called before any evaluation method when the circuit contains
 * multivalued input gates.
 */
void rewriteMultivaluedGates();

/**
 * @brief Build a @c dDNNF directly from the Boolean circuit's structure.
 *
 * Used as a fallback when no external compiler is available and the
 * circuit is already in a form that can be interpreted as a d-DNNF.
 *
 * @param g  Root gate.
 * @return   A @c dDNNF wrapping the same structure.
 */
dDNNF interpretAsDD(gate_t g) const;

/**
 * @brief Dispatch to the appropriate d-DNNF construction method.
 *
 * @param g       Root gate.
 * @param method  Compilation method name (e.g. "tree-decomposition",
 *                "d4", "c2d"…).
 * @param args    Additional arguments forwarded to the chosen method.
 * @return        The constructed @c dDNNF.
 */
dDNNF makeDD(gate_t g, const std::string &method, const std::string &args) const;

/**
 * @brief Build a @c dDNNF from a single compiler/route name.
 *
 * Resolves the name the way a user (or Studio) thinks of it, with no
 * separate @c method / @c args split:
 * - the in-process meta-routes @c "tree-decomposition",
 *   @c "interpret-as-dd", @c "default" (and the empty string) go
 *   through @c makeDD;
 * - any other name is an external compiler (@c "d4", @c "d4v2",
 *   @c "c2d", @c "minic2d", @c "dsharp", @c "panini-*") and is passed
 *   straight to @c compilation.
 *
 * This is the single dispatch point shared by @c compile_to_ddnnf_dot,
 * @c compile_to_ddnnf (NNF) and @c ddnnf_stats, so they cannot drift on
 * which names they accept.
 *
 * @param g     Root gate.
 * @param name  Compiler or meta-route name.
 * @return      The constructed @c dDNNF.
 */
dDNNF makeDDByName(gate_t g, const std::string &name) const;

/** @copydoc Circuit::toString() */
virtual std::string toString(gate_t g) const override;

/**
 * @brief Render the sub-circuit at @p g, labelling input gates from a map.
 *
 * Same as @c toString(gate_t), but @c IN and @c MULIN gates whose
 * gate identifier is present in @p labels are rendered using the
 * mapped string instead of the default @c x@<id@> placeholder.  Gates
 * not found in @p labels fall back to the default rendering.
 *
 * @param g       Gate to render.
 * @param labels  Optional mapping from input/mulinput gate IDs to
 *                user-supplied labels.
 * @return        Human-readable string.
 */
std::string toString(
  gate_t g,
  const std::unordered_map<gate_t, std::string> &labels) const;

private:
/**
 * @brief Internal recursive helper for the two @c toString() variants.
 *
 * The @p parent parameter carries the gate type of the immediate
 * caller. It drives parenthesis elision in two cases: at the root
 * (parent set to @c UNDETERMINED) the outer wrap is omitted, and
 * when @p parent matches the current gate type (associative
 * AND/OR) the wrap is omitted to flatten same-op nesting. A 1-wire
 * AND/OR also bypasses the wrap and delegates to its child since
 * such single-element joins carry no information.
 *
 * @param g       Gate to render.
 * @param parent  Gate type of the caller, or @c UNDETERMINED at the root.
 * @param labels  Pointer to a label map, or @c nullptr for the
 *                unlabelled rendering.
 */
std::string toStringHelper(
  gate_t g,
  BooleanGate parent,
  const std::unordered_map<gate_t, std::string> *labels) const;

public:

/**
 * @brief Export the circuit in the textual format expected by external compilers.
 *
 * Produces a multi-line string encoding all gates reachable from the
 * circuit in the format used by the standalone @c tdkc tool and external
 * model counters.
 *
 * @param g  Root gate.
 * @return   Circuit description string.
 */
std::string exportCircuit(gate_t g) const;

/**
 * @brief Return the Tseytin transformation of the sub-circuit at @p g as a DIMACS string.
 *
 * Same encoding as the private @c Tseytin file-emitting overload, but
 * returned in memory without any file I/O. Useful for surfacing the
 * CNF to a user or to a knowledge-compilation tool over stdin.
 *
 * @param g            Root gate.
 * @param display_prob Include @c w lines listing each input's
 *                     probability (and its complement).
 * @param mapping      Prepend @c "c input <var> <uuid> <prob>" comment
 *                     lines, one per input gate, so the emitted DIMACS
 *                     is self-documenting (the comments are ignored by
 *                     every model counter / compiler).
 * @return             DIMACS CNF as a string.
 */
std::string TseytinCNF(gate_t g, bool display_prob, bool mapping = false) const;

/**
 * @brief One row of the Tseytin variable mapping.
 *
 * Ties a DIMACS variable index back to the provenance input it stands
 * for, so an external model count / satisfying assignment can be read
 * against the original circuit.
 */
struct CNFInputMapping {
  int variable;        ///< DIMACS variable index (= gate id + 1).
  std::string uuid;    ///< Original-circuit UUID (empty if unknown).
  double probability;  ///< Probability assigned to the input gate.
};

/**
 * @brief Map each input gate to its DIMACS variable, UUID, probability.
 *
 * The variable numbering matches @c TseytinCNF (and @c dDNNF::toNNF):
 * variable = gate id + 1. Inputs are listed in gate-id order so the
 * mapping is deterministic.
 *
 * @return One @c CNFInputMapping per input gate.
 */
std::vector<CNFInputMapping> tseytinVariableMapping() const;

/**
 * @brief Serialise the sub-circuit at @p g in d4's BC-S1.2 circuit format.
 *
 * Emits the Boolean circuit directly (inputs as @c "I" declarations,
 * AND/OR gates as @c "G name := A|O …" definitions, the root as @c "T"),
 * inlining @c NOT gates as literal sign flips. This is the input consumed by
 * d4v2's @c --input-type @c circuit mode, letting us skip the Tseytin
 * transform.
 *
 * Inputs are emitted first, so d4 (which numbers literals from 1 in
 * first-seen order) assigns them variables 1..k in declaration order;
 * every variable above k is an internal-gate variable. @p inputOrder is
 * filled so that d4 variable @c v (1-based) corresponds to input gate
 * @c inputOrder[v-1], which the d-DNNF parse-back uses to map decision
 * literals to the right @c IN gate.
 *
 * Throws @c CircuitException on a gate shape BC-S1.2 cannot express
 * (a nullary AND/OR, or a non-AND/OR/NOT/IN gate); the caller falls back
 * to the Tseytin CNF path.
 *
 * @param g          Root gate of the sub-circuit.
 * @param inputOrder Output: input gate for each d4 variable (1-based).
 * @return           BC-S1.2 circuit description as a string.
 */
std::string BCS12(gate_t g, std::vector<gate_t> &inputOrder) const;

/**
 * @brief Parse a Panini (KCBox) DD output file into a ProvSQL d-DNNF.
 *
 * The @c panini-dd output parser, selected by @c compilation() for the
 * @c panini-* records.  Those records run the generic compile path (a Tseytin
 * CNF written to the input, the record's @c argtpl run, the @c --lang carried
 * in the template); they differ from the @c nnf compilers only in this
 * parse-back.  Panini's DD output is over the variables of our Tseytin CNF:
 * decisions on input variables are translated to the corresponding @c IN
 * gates; decisions on Tseytin auxiliaries are dropped (their branches are
 * mutually exclusive over input assignments by Tseytin determinism, so the
 * input-projection is still a sound d-DNNF).  @c "R2-D2" / @c "CCDD" emit
 * @c K (kernelize) nodes that break decomposability, so those variants are
 * not registered and a @c K node here is an error.
 *
 * @param outfilename  Path to Panini's DD output file.
 * @return             The compiled d-DNNF.
 */
dDNNF parsePaniniDD(const std::string &outfilename) const;

/**
 * @brief Boost serialisation support.
 * @param ar       Boost archive (input or output).
 * @param version  Archive version (unused).
 */
template<class Archive>
void serialize (Archive & ar, const unsigned int version)
{
  ar & uuid2id;
  ar & id2uuid;
  ar & gates;
  ar & wires;
  ar & inputs;
  ar & mulinputs;
  ar & prob;
  ar & info;
  ar & probabilistic;
}

friend class dDNNFTreeDecompositionBuilder;
friend class boost::serialization::access;
};

#endif /* BOOLEAN_CIRCUIT_H */
