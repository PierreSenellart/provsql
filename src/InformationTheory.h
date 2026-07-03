#ifndef INFORMATION_THEORY_H
#define INFORMATION_THEORY_H

/**
 * @file InformationTheory.h
 * @brief Information-theoretic readouts over scalar RV sub-circuits:
 *        entropy, Kullback-Leibler divergence, mutual information.
 *
 * The exact paths resolve a gate to a closed *density view* — a bare
 * @c gate_rv (its family @c pdf over the integration range), a
 * @c gate_value / categorical mixture (a finite pmf), or a Bernoulli
 * mixture tree over independent such arms (e.g. the @c gmm
 * constructor's cascade) — and evaluate the defining integral / sum
 * directly.  Entropy of a discrete view is Shannon entropy; of a
 * continuous view, differential entropy (both in nats).  Shapes with
 * no density view fall back to Monte Carlo plug-in estimators at the
 * @c provsql.rv_mc_samples budget (a histogram density for entropy, a
 * 2-D histogram over coupled joint draws for mutual information;
 * KL has no density-free estimator and raises instead).
 */

#include <optional>

#include "GenericCircuit.h"

namespace provsql {

/** @brief Entropy of @p root in nats: Shannon for a discrete view,
 *  differential for a continuous one.  With @p event, the entropy of
 *  the conditional distribution (MC plug-in estimate). */
double computeEntropy(const GenericCircuit &gc, gate_t root,
                      std::optional<gate_t> event);

/** @brief Kullback-Leibler divergence KL(P || Q) in nats.  @c Infinity
 *  when P is not absolutely continuous w.r.t. Q (mismatched kinds, an
 *  atom / region of P outside Q's support).  Both roots must resolve
 *  to density views; raises otherwise. */
double computeKL(const GenericCircuit &gc, gate_t p_root, gate_t q_root);

/** @brief Mutual information I(X; Y) in nats: exactly @c 0 for
 *  structurally independent roots, @c H(X) / @c Infinity for identical
 *  discrete / continuous roots, and the 2-D histogram plug-in estimate
 *  over coupled joint draws otherwise. */
double computeMutualInformation(const GenericCircuit &gc, gate_t x_root,
                                gate_t y_root);

}  // namespace provsql

#endif /* INFORMATION_THEORY_H */
