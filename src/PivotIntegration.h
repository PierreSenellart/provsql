#ifndef PIVOT_INTEGRATION_H
#define PIVOT_INTEGRATION_H

/**
 * @file PivotIntegration.h
 * @brief Shared 1-D quadrature core for the pivot-conjunction and
 *        order-statistic closed forms.
 *
 * The exact evaluators for guard-partition shapes all reduce to the same
 * one-dimensional integral over a pivot RV's support — the layer-cake
 * order-statistic means, the RV-vs-RV conditional moments, the
 * pivot-conjunction moments @f$\int x^k f_X \prod_j W_j\,dx@f$ in
 * @c src/Expectation.cpp, and the analytic @f$2^k@f$ joint table over a
 * shared-pivot island in @c src/HybridEvaluator.cpp.  This header holds
 * the numeric core they share: the composite-Simpson driver (with the
 * common NaN-aborts-to-NaN convention, so an undefined density/CDF makes
 * the caller decline to Monte Carlo), the panel-count constant, the
 * @c binomial helper, and the central-moment binomial expansion over a
 * raw-moment closure.
 */

#include <cmath>
#include <limits>
#include <optional>

namespace provsql {

/** Panel count shared by every composite-Simpson quadrature over a
 *  distribution's integration range: exact for the low-degree polynomial
 *  integrands of the Uniform cases, high-accuracy otherwise. */
constexpr int kSimpsonPanels = 4000;

/**
 * @brief Composite-Simpson @f$\int_{lo}^{hi} f(x)\,dx@f$ with @p N panels.
 *
 * @p N must be even (every caller passes @c kSimpsonPanels).  A NaN from
 * @p f aborts the quadrature and returns NaN — the callers' shared
 * convention for "a density / CDF is undefined here, decline to MC".
 */
template <typename F>
double simpsonIntegrate(double lo, double hi, int N, F &&f)
{
  const double h = (hi - lo) / N;
  double acc = 0.0;
  for (int i = 0; i <= N; ++i) {
    const double x = lo + i * h;
    const double v = f(x);
    if (std::isnan(v)) return std::numeric_limits<double>::quiet_NaN();
    const double coeff = (i == 0 || i == N) ? 1.0 : (i % 2 == 1 ? 4.0 : 2.0);
    acc += coeff * v;
  }
  return acc * h / 3.0;
}

/** @brief Binomial coefficient @f$\binom{n}{k}@f$ as a double (exact for
 *  the small orders the moment expansions use). */
inline double binomial(unsigned n, unsigned k)
{
  if (k > n) return 0.0;
  if (k > n - k) k = n - k;
  double r = 1.0;
  for (unsigned i = 1; i <= k; ++i) {
    r *= static_cast<double>(n - i + 1);
    r /= static_cast<double>(i);
  }
  return r;
}

/**
 * @brief Central moment of order @p k from a raw-moment closure:
 *        @f$E[(X-\mu)^k] = \sum_{i=0}^{k} \binom{k}{i} (-\mu)^{k-i} E[X^i]@f$.
 *
 * @p raw maps a moment order to the (conditional) raw moment, returning
 * @c std::nullopt when it cannot be resolved — which propagates, so the
 * caller falls back exactly as it would for the raw moment itself.
 * @c raw(0) must be @c 1.
 */
template <typename Raw>
std::optional<double> centralFromRaw(unsigned k, Raw &&raw)
{
  auto mu_opt = raw(1);
  if (!mu_opt) return std::nullopt;
  const double mu = *mu_opt;
  if (k == 1) return 0.0;
  double total = 0.0;
  for (unsigned i = 0; i <= k; ++i) {
    auto m_i = raw(i);
    if (!m_i) return std::nullopt;
    total += binomial(k, i)
           * std::pow(-mu, static_cast<double>(k - i)) * (*m_i);
  }
  return total;
}

}  // namespace provsql

#endif /* PIVOT_INTEGRATION_H */
