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

#include <map>
#include <tuple>
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

#endif /* STRUCTURED_DNNF_H */
