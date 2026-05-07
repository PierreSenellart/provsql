/**
 * @file having_semantics.hpp
 * @brief Provenance evaluation helpers for HAVING-clause circuits.
 *
 * When a query includes a HAVING clause, ProvSQL creates a special
 * sub-circuit that encodes the aggregate predicate.  Before the main
 * provenance circuit can be evaluated over a semiring, these HAVING
 * sub-circuits must first be evaluated to determine which groups
 * pass the filter.
 *
 * This header declares one evaluation helper per semiring variant.
 * Each function evaluates the HAVING sub-circuit rooted at gate @p g
 * over the appropriate semiring, writing results into @p mapping.
 * On successful evaluation, @p mapping is populated with entries
 * for input gates reachable from @p g.  If the HAVING gate type is
 * incompatible with the requested semiring the function is a no-op.
 */
#ifndef PROVSQL_HAVING_SEMANTICS_HPP
#define PROVSQL_HAVING_SEMANTICS_HPP
#include <string>
#include <unordered_map>

#include "GenericCircuit.hpp"
#include "BooleanCircuit.h"
#include "semiring/BoolExpr.h"
#include "semiring/Why.h"
#include "semiring/Which.h"

/**
 * @brief Evaluate the HAVING sub-circuit at @p g over the Formula (symbolic representation).
 *
 * @param c        The generic circuit containing gate @p g.
 * @param g        Root gate of the HAVING sub-circuit.
 * @param mapping  Map from input gates to their formula string values;
 *                 populated on successful evaluation.
 */
void provsql_try_having_formula(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, std::string> &mapping
  );

/**
 * @brief Evaluate the HAVING sub-circuit at @p g over the Counting semiring.
 *
 * @param c        The generic circuit containing gate @p g.
 * @param g        Root gate of the HAVING sub-circuit.
 * @param mapping  Map from input gates to their count values;
 *                 populated on successful evaluation.
 */
void provsql_try_having_counting(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, unsigned> &mapping
  );

/**
 * @brief Evaluate the HAVING sub-circuit at @p g over the Why-provenance semiring.
 *
 * @param c        The generic circuit containing gate @p g.
 * @param g        Root gate of the HAVING sub-circuit.
 * @param mapping  Map from input gates to their why-provenance values;
 *                 populated on successful evaluation.
 */
void provsql_try_having_why(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, semiring::why_provenance_t> &mapping
  );

/**
 * @brief Evaluate the HAVING sub-circuit at @p g over the Which-provenance semiring.
 *
 * @param c        The generic circuit containing gate @p g.
 * @param g        Root gate of the HAVING sub-circuit.
 * @param mapping  Map from input gates to their which-provenance values;
 *                 populated on successful evaluation.
 */
void provsql_try_having_which(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, semiring::which_provenance_t> &mapping
  );

/**
 * @brief Evaluate the HAVING sub-circuit at @p g over the BoolExpr semiring.
 *
 * @param c        The generic circuit containing gate @p g.
 * @param be       The @c BoolExpr semiring instance (shared circuit).
 * @param g        Root gate of the HAVING sub-circuit.
 * @param mapping  Map from input gates to their @c gate_t values in @c be;
 *                 populated on successful evaluation.
 */
void provsql_try_having_boolexpr(
  GenericCircuit &c,
  semiring::BoolExpr &be,
  gate_t g,
  std::unordered_map<gate_t, gate_t> &mapping
  );

/**
 * @brief Evaluate the HAVING sub-circuit at @p g over the Boolean semiring.
 *
 * @param c        The generic circuit containing gate @p g.
 * @param g        Root gate of the HAVING sub-circuit.
 * @param mapping  Map from input gates to their Boolean values;
 *                 populated on successful evaluation.
 */
void provsql_try_having_boolean(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, bool> &mapping
  );

/**
 * @brief Evaluate the HAVING sub-circuit at @p g over the Tropical semiring.
 *
 * @param c        The generic circuit containing gate @p g.
 * @param g        Root gate of the HAVING sub-circuit.
 * @param mapping  Map from input gates to their tropical (cost) values;
 *                 populated on successful evaluation.
 */
void provsql_try_having_tropical(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, double> &mapping
  );

/**
 * @brief Evaluate the HAVING sub-circuit at @p g over the Viterbi semiring.
 *
 * @param c        The generic circuit containing gate @p g.
 * @param g        Root gate of the HAVING sub-circuit.
 * @param mapping  Map from input gates to their Viterbi (probability) values;
 *                 populated on successful evaluation.
 */
void provsql_try_having_viterbi(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, double> &mapping
  );

#if PG_VERSION_NUM >= 140000
/**
 * @brief Evaluate the HAVING sub-circuit at @p g over the Temporal semiring.
 *
 * @param c        The generic circuit containing gate @p g.
 * @param g        Root gate of the HAVING sub-circuit.
 * @param mapping  Map from input gates to their tstzmultirange Datum values;
 *                 populated on successful evaluation.
 */
void provsql_try_having_temporal(
  GenericCircuit &c,
  gate_t g,
  std::unordered_map<gate_t, Datum> &mapping
  );
#endif

#endif
