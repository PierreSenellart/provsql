/**
 * @file StructuredDNNF.h
 * @brief In-process reduced-OBDD construction over a query-derived variable
 *        order, for the inversion-free UCQ(OBDD) probability path.
 *
 * Given a @c BooleanCircuit (AND / OR / NOT / IN over independent inputs) and a
 * total order on its input variables, build a reduced OBDD of the function and
 * read off its probability (inputs independent Bernoulli).  For an
 * inversion-free query the query-derived (Prop. 4.5) order yields an OBDD of
 * size linear in the lineage, where the generic methods (tree-decomposition,
 * d4) blow up.
 *
 * This phase validates the construction (size + probability); emitting the
 * result as a ProvSQL @c dDNNF for the in-server evaluator is a later phase.
 * Pure C++ (BooleanCircuit + STL, no PostgreSQL headers) so it builds both in
 * the extension and the standalone @c -DTDKC harness.
 */
#ifndef STRUCTURED_DNNF_H
#define STRUCTURED_DNNF_H

#include "BooleanCircuit.h"
#include "dDNNF.h"

#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <cstddef>

class StructuredDNNF {
public:
  /**
   * @param bc          Boolean circuit (no multivalued inputs).
   * @param root        Root gate of the function to compile.
   * @param input_rank  Maps every IN gate reachable from @p root to a distinct
   *                    rank in @c [0,ninputs).  Lower rank = tested earlier
   *                    (nearer the OBDD root).
   * @throws CircuitException if @p bc has multivalued gates, or a reachable
   *         input lacks a rank, or an unsupported gate type is met.
   */
  StructuredDNNF(const BooleanCircuit &bc, gate_t root,
                 const std::map<gate_t, int> &input_rank);

  /** @brief Probability that the function is true (independent inputs). */
  double probability() const;

  /** @brief Decision nodes reachable from the root (the reduced-OBDD size). */
  std::size_t size() const;

  /** @brief Total decision nodes created during apply (work proxy, >= size()). */
  std::size_t workSize() const { return nodes_.size() >= 2 ? nodes_.size() - 2 : 0; }

private:
  /* OBDD node table.  Index 0 = FALSE terminal, 1 = TRUE terminal, >=2 =
   * decision node.  @c var is the variable rank (terminals use a +inf
   * sentinel so they sort below every variable). */
  struct Node { int var; int lo; int hi; };
  std::vector<Node> nodes_;

  std::map<std::tuple<int,int,int>, int> unique_;       // (var,lo,hi) -> node
  std::map<std::tuple<int,int,int>, int> apply_cache_;  // (op,f,g)    -> node (op:0=AND,1=OR)
  std::map<int,int> negate_cache_;                      // f -> ¬f
  std::vector<double> prob_by_rank_;                    // P(variable) per rank

  int root_;

  int varOf(int node) const;
  int mk(int var, int lo, int hi);
  int applyOp(int op, int f, int g);
  int negate(int f);
  int build(const BooleanCircuit &bc, gate_t g,
            const std::map<gate_t,int> &rank, std::map<gate_t,int> &memo);
};

/**
 * @brief Top-down structured-d-DNNF builder over a query-derived variable order.
 *
 * This is the §4 construction of the inversion-free plan: rather than the
 * bottom-up @c apply of @c StructuredDNNF (kept above as a reference oracle), it
 * compiles the monotone lineage **top-down** into a genuine ProvSQL @c dDNNF
 * with two node kinds, placed by structure:
 *
 * - **Decomposable AND** at independence points.  When the residual function is
 *   a product of variable-disjoint sub-functions @f$\varphi_1\wedge\dots@f$
 *   (the consistent-unification self-join's A-part and B-part once the shared
 *   separator variables are fixed; or a pure product term), emit a flat @c AND
 *   and recurse per factor.  Decomposability (children share no variable) holds
 *   by construction and is the structural win an OBDD cannot express.
 * - **Deterministic OR** at Shannon decisions.  Otherwise pick the lowest-rank
 *   variable still present and branch on it, emitting
 *   @c OR(AND(v,hi),AND(¬v,lo)).  Determinism (children mutually exclusive)
 *   holds because both branches come from a decision on the same variable.
 *
 * A component cache (keyed on the canonical residual) shares equal
 * sub-d-DNNFs, which is what keeps the result linear in the lineage under the
 * Prop. 4.5 order (and bounds the work).  The function is taken as a monotone
 * DNF expanded from the @c BooleanCircuit (AND distributes, OR unions, IN is a
 * single literal; @c NOT and multivalued inputs are rejected — out of scope for
 * the TID inversion-free path).
 *
 * Output is a finished @c dDNNF (root set, @c simplify() applied) ready for
 * @c dDNNF::probabilityEvaluation().  Pure C++ so it builds in the extension and
 * the standalone @c -DTDKC harness.
 */
class StructuredDNNFBuilder {
public:
  /**
   * @param bc          Boolean circuit (monotone AND/OR/IN, no multivalued
   *                    inputs, no NOT).
   * @param root        Root gate of the function to compile.
   * @param input_rank  Maps every IN gate reachable from @p root to a distinct
   *                    rank in @c [0,ninputs).  Lower rank = decided earlier
   *                    (a Prop. 4.5-consistent order, derived by the caller).
   * @param max_nodes   Size guard: throw @c CircuitException once this many
   *                    d-DNNF gates have been created (0 = no guard).
   * @throws CircuitException on multivalued/NOT/unsupported gates, a reachable
   *         input without a rank, or the size guard tripping.
   */
  StructuredDNNFBuilder(const BooleanCircuit &bc, gate_t root,
                        const std::map<gate_t, int> &input_rank,
                        std::size_t max_nodes = 0);

  /**
   * @brief Structured per-input order key (the task-11 key, derived by the
   *        caller for now).
   *
   * Locates an input variable in the query hierarchy of a consistent-unification
   * self-join: @c root is the root-class value (one independent block per
   * value), @c sec the secondary-class value (one tile per value within a
   * block), and @c factor which quantified factor (clause) the variable's atom
   * belongs to, or @c GUARD_FACTOR for a self-join atom shared by every factor
   * of its tile.  For the witness @c S(x,y),A(x,y),S(x,z),B(x,z): the @c S tuple
   * is @c {a,v,GUARD}, @c A is @c {a,v,0}, @c B is @c {a,v,1}.
   */
  struct InputKey { int root; int sec; int factor; };
  static constexpr int GUARD_FACTOR = -1;

  /**
   * @brief Linear-build constructor over the factored (clause-set) hierarchy.
   *
   * Uses the structured keys to keep each block factored as an AND of clauses
   * (one disjunction per factor, sharing the self-join guard), OR-chained across
   * blocks without ever flattening to the @f$n^2@f$ cross-terms.  The bounded
   * atom-frontier then falls out of caching the compact clause-set cofactors, so
   * build time is linear in the lineage for fixed query.  Same d-DNNF output
   * contract as the order-only constructor.
   *
   * @param keys  One @c InputKey per IN gate reachable from @p root.
   * @throws CircuitException as the order-only constructor, plus on a key set
   *         that does not describe a well-formed block/tile/factor hierarchy.
   */
  StructuredDNNFBuilder(const BooleanCircuit &bc, gate_t root,
                        const std::map<gate_t, InputKey> &keys,
                        std::size_t max_nodes = 0);

  /** @brief The constructed d-DNNF (root set, simplified). */
  const dDNNF &dnnf() const { return dd_; }

  /** @brief Root gate of the constructed d-DNNF (current after simplify). */
  gate_t rootGate() const { return dd_.root; }

  /** @brief Probability that the function is true (independent inputs). */
  double probability() const { return dd_.probabilityEvaluation(); }

  /** @brief d-DNNF gates reachable from the root. */
  std::size_t size() const;

private:
  using Var  = int;                 ///< Variable rank in @c [0,ninputs).
  using Term = std::vector<Var>;    ///< A product: sorted, duplicate-free vars.
  using DNF  = std::vector<Term>;   ///< A sum of products: canonicalised.
  using ClauseSet = std::vector<DNF>; ///< A conjunction of clauses (each a DNF).

  dDNNF dd_;
  gate_t root_;
  std::size_t max_nodes_;

  std::vector<double> prob_;        ///< prob_[rank] = P(variable)
  std::vector<gate_t> in_gate_;     ///< rank -> shared dDNNF IN gate
  std::vector<gate_t> not_gate_;    ///< rank -> shared dDNNF NOT(IN) gate (lazy)
  gate_t true_gate_, false_gate_;   ///< empty AND / empty OR terminals

  /* Component cache: (residual, false-sink) -> gate.  The sink is part of the
   * key because the same residual built against a different OR-chain tail (§
   * @c build) yields a different node.  Hashed so a lookup costs one O(|d|)
   * fingerprint rather than O(log N) comparisons of large DNF keys. */
  struct CacheKey { DNF d; gate_t fs; bool operator==(const CacheKey &o) const; };
  struct CacheKeyHash { std::size_t operator()(const CacheKey &k) const; };
  std::unordered_map<CacheKey, gate_t, CacheKeyHash> cache_;

  /* Clause-set cofactor cache for the factored (linear-build) path. */
  struct CSKey { ClauseSet cs; gate_t fs; bool operator==(const CSKey &o) const; };
  struct CSKeyHash { std::size_t operator()(const CSKey &k) const; };
  std::unordered_map<CSKey, gate_t, CSKeyHash> csCache_;

  /* Expand the circuit's function under @p root into a canonical monotone DNF,
   * memoised per gate. */
  DNF extract(const BooleanCircuit &bc, gate_t g,
              const std::map<gate_t, int> &rank,
              std::map<gate_t, DNF> &memo) const;

  static DNF canonical(DNF d);                 ///< sort, dedup, drop supersets
  static DNF condition(const DNF &d, Var v, bool value);

  /* Split @p d into variable-disjoint OR-components (terms sharing a variable,
   * transitively, stay together), ordered by least variable; @c {d} if the
   * co-occurrence graph is connected.  These are OR'd, so they are chained, not
   * AND'd -- see @c build. */
  std::vector<DNF> orDecompose(const DNF &d) const;

  /* If @p d is a product of variable-disjoint sub-DNFs, return the factors
   * (size > 1); otherwise return @c {d}.  Sound: only splits when an exact
   * cross-product is verified, so an AND node is always decomposable. */
  std::vector<DNF> andDecompose(const DNF &d) const;

  gate_t mkAnd(const std::vector<gate_t> &children);
  gate_t mkLit(Var v);                         ///< shared positive literal IN
  gate_t mkNeg(Var v);                         ///< shared negative literal NOT(IN)
  gate_t newGate(BooleanGate type);            ///< setGate + size guard

  /* Compile @p d into a d-DNNF node whose FALSE leaf is @p false_sink (rather
   * than the global FALSE terminal).  Threading a sink lets an OR-chain build
   * each disjoint component against the node for "the rest of the disjunction",
   * so the inert components are not dragged through the residual DNF. */
  gate_t build(const DNF &d, gate_t false_sink);

  /* Factored path (keyed constructor).  Compile a conjunction of clauses with
   * FALSE leaf @p false_sink.  Clauses are kept small (one factor each) so the
   * cofactor cache realises the bounded atom-frontier, giving linear build. */
  gate_t buildBlock(const ClauseSet &cs, gate_t false_sink);
  static ClauseSet condition(const ClauseSet &cs, Var v, bool value, bool &is_false);
  /* Partition clauses into variable-disjoint groups (decomposable AND over the
   * conjunction); @c {cs} if they all share variables transitively. */
  std::vector<ClauseSet> csAndDecompose(const ClauseSet &cs) const;
};

#endif /* STRUCTURED_DNNF_H */
