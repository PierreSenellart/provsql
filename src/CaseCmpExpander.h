/**
 * @file CaseCmpExpander.h
 * @brief Expansion of a comparison one of whose operands is a guarded
 *        selection (@c gate_case) into the comparisons of its arms.
 *
 * A @c gate_case is the value of the first arm whose guard holds, and
 * nothing in the semiring combines the value gates of its arms: a
 * @c gate_cmp over one is what the Boolean translation meets and refuses
 * ("This semiring does not support value gates").  The comparison it stands
 * for is nonetheless an ordinary Boolean combination, since exactly one arm
 * is selected in each world:
 *
 * @code
 *   CASE(g₁,v₁, …, g_k,v_k, d) ⋈ O
 *     =  ⊕ᵢ (⊗ⱼ<ᵢ ¬gⱼ) ⊗ gᵢ ⊗ (vᵢ ⋈ O)  ⊕  (⊗ⱼ ¬gⱼ) ⊗ (d ⋈ O)
 * @endcode
 *
 * where @c ¬g is @c "𝟙 ⊖ g".  The terms are mutually exclusive by the prefix
 * of negated guards, so the @c ⊕ is a disjoint union in every semiring that
 * reads these gates as Boolean events.  Each arm comparison is then an
 * ordinary one -- an aggregate against the other operand, which the
 * closed-form pre-passes and the possible-world enumeration resolve as they
 * always have -- and a comparison between two constants is decided here.
 *
 * The pass runs first in @c resolveComparators so the comparisons it creates
 * are seen by every later pre-pass.  A comparison of two guarded selections
 * takes one round per side; nested ones, one round per level.
 */
#ifndef PROVSQL_CASE_CMP_EXPANDER_H
#define PROVSQL_CASE_CMP_EXPANDER_H

#include "GenericCircuit.h"

namespace provsql {

/**
 * @brief Expand every @c gate_cmp with a @c gate_case operand in @p gc.
 *
 * @param gc  Circuit to mutate in place.
 * @return    Number of comparisons expanded.
 */
unsigned runCaseCmpExpander(GenericCircuit &gc);

}  // namespace provsql

#endif  // PROVSQL_CASE_CMP_EXPANDER_H
