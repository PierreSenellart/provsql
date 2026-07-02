/**
 * @file Distribution.cpp
 * @brief The family-agnostic side of the @c Distribution hierarchy: the
 *        registries (family descriptors / factories, pairwise comparator
 *        and closure rules), their dispatch drivers, and the generic
 *        comparator quadrature fallback.
 *
 * The per-family closed forms live in one file per family next to this
 * one (@c normal.cpp, @c uniform.cpp, @c exponential.cpp,
 * @c erlang.cpp); each registers itself at static initialisation, so
 * adding a family means adding a file -- nothing here changes.
 */
#include "DistributionCommon.h"

#include <cmath>
#include <map>
#include <string>
#include <utility>

namespace provsql {

namespace {

/* Function-local statics so registration (dynamic initialisation of the
 * per-family registrar objects) never observes an uninitialised map,
 * whatever the TU initialisation order. */
using FamilyPair = std::pair<std::string, std::string>;

std::map<FamilyPair, ComparatorRule> &comparatorRules()
{
  static std::map<FamilyPair, ComparatorRule> rules;
  return rules;
}

std::map<FamilyPair, ClosureRule> &closureRules()
{
  static std::map<FamilyPair, ClosureRule> rules;
  return rules;
}

std::map<FamilyPair, ProductRule> &productRules()
{
  static std::map<FamilyPair, ProductRule> rules;
  return rules;
}

std::map<FamilyPair, TransformRule> &transformRules()
{
  static std::map<FamilyPair, TransformRule> rules;
  return rules;
}

std::map<std::string, const DistributionFamily *> &familiesByName()
{
  static std::map<std::string, const DistributionFamily *> families;
  return families;
}

/* Registry-miss default: P(X < Y) by a 1-D composite-Simpson quadrature.
 * Two equivalent identities are available --
 *   P(X<Y) = ∫ (1 - F_Y(t)) f_X(t) dt   over X's integration range,
 *   P(X<Y) = ∫ F_X(t) f_Y(t) dt         over Y's integration range --
 * and the fixed panel budget resolves the narrower window better, so the
 * side with the narrower range is the one integrated over.  A heavy tail
 * (Pareto with small α) blows its own window up to xₘ·10^{9/α}, leaving
 * ~30-unit panels across the density peak; integrating the *other*,
 * well-conditioned density against the heavy tail's exact CDF instead
 * keeps the answer exact-grade.  Family-agnostic -- needs only pdf /
 * cdf / integrationRange.  NaN when a density / CDF is undefined (e.g.
 * a non-integer Erlang shape), so the caller falls back to Monte Carlo. */
double quadraturePairLess(const Distribution &X, const Distribution &Y)
{
  double xlo, xhi, ylo, yhi;
  const bool xok = X.integrationRange(xlo, xhi);
  const bool yok = Y.integrationRange(ylo, yhi);
  if (!xok && !yok)
    return kNaN;
  const bool overX = xok && (!yok || (xhi - xlo) <= (yhi - ylo));
  const double lo = overX ? xlo : ylo;
  const double hi = overX ? xhi : yhi;
  const int N = 4000;
  const double h = (hi - lo) / N;
  double acc = 0.0;
  for (int i = 0; i <= N; ++i) {
    const double t = lo + i * h;
    const double f = overX ? X.pdf(t) : Y.pdf(t);
    const double F = overX ? Y.cdf(t) : X.cdf(t);
    if (std::isnan(f) || std::isnan(F))
      return kNaN;
    const double coeff = (i == 0 || i == N) ? 1.0 : (i % 2 == 1 ? 4.0 : 2.0);
    acc += coeff * (overX ? (1.0 - F) : F) * f;
  }
  return acc * h / 3.0;
}

}  // namespace

void registerComparatorRule(const char *x, const char *y,
                            ComparatorRule rule)
{
  comparatorRules()[{x, y}] = rule;
}

void registerClosureRule(const char *x, const char *y, ClosureRule rule)
{
  closureRules()[{x, y}] = rule;
}

void registerProductRule(const char *x, const char *y, ProductRule rule)
{
  productRules()[{x, y}] = rule;
}

void registerTransformRule(const char *transform, const char *family,
                           TransformRule rule)
{
  transformRules()[{transform, family}] = rule;
}

std::unique_ptr<Distribution> closeProductFactors(
  const std::vector<const Distribution *> &factors)
{
  const auto &rules = productRules();
  ProductRule rule = nullptr;
  const char *n0 = nullptr;
  for (const Distribution *f : factors) {
    if (!n0) {
      n0 = f->family().name;
      const auto it = rules.find({n0, n0});
      if (it == rules.end()) return nullptr;
      rule = it->second;
    } else {
      const auto it = rules.find({n0, f->family().name});
      if (it == rules.end() || it->second != rule) return nullptr;
    }
  }
  if (!rule) return nullptr;
  return rule(factors);
}

std::unique_ptr<Distribution> closeTransform(const char *transform,
                                             const Distribution &x)
{
  const auto &rules = transformRules();
  const auto it = rules.find({transform, x.family().name});
  return it == rules.end() ? nullptr : it->second(x);
}

std::unique_ptr<Distribution> closePlusTerms(
  const std::vector<ClosureTerm> &terms)
{
  const auto &rules = closureRules();
  ClosureRule rule = nullptr;
  const char *n0 = nullptr;
  for (const auto &t : terms) {
    if (!t.dist) continue;
    if (!n0) {
      n0 = t.dist->family().name;
      const auto it = rules.find({n0, n0});
      if (it == rules.end()) return nullptr;
      rule = it->second;
    } else {
      const auto it = rules.find({n0, t.dist->family().name});
      if (it == rules.end() || it->second != rule) return nullptr;
    }
  }
  if (!rule) return nullptr;   /* no RV term: the constant fold's job */
  return rule(terms);
}

double numericQuantile(const Distribution &d, double p)
{
  double lo, hi;
  if (!d.integrationRange(lo, hi))
    return kNaN;
  const double f_lo = d.cdf(lo), f_hi = d.cdf(hi);
  if (std::isnan(f_lo) || std::isnan(f_hi) || !(f_lo <= f_hi))
    return kNaN;
  if (p <= f_lo) return lo;
  if (p >= f_hi) return hi;
  /* The CDF is monotone, so plain bisection is unconditionally
   * convergent; 200 halvings exhaust double precision from any
   * starting window long before the bound. */
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (!(mid > lo && mid < hi))
      break;   /* interval narrowed to adjacent doubles */
    const double f = d.cdf(mid);
    if (std::isnan(f))
      return kNaN;
    if (f < p) lo = mid;
    else       hi = mid;
  }
  return 0.5 * (lo + hi);
}

double comparatorPairLess(const Distribution &X, const Distribution &Y)
{
  const auto &rules = comparatorRules();
  const auto it = rules.find({X.family().name, Y.family().name});
  double pLess = kNaN;
  if (it != rules.end())
    pLess = it->second(X, Y);
  /* Registry miss, or a rule that declined (parameter guard, or a shape
   * outside its closed form): generic quadrature. */
  if (std::isnan(pLess))
    pLess = quadraturePairLess(X, Y);
  return pLess;
}

void registerDistributionFamily(const DistributionFamily &descriptor)
{
  familiesByName()[descriptor.name] = &descriptor;
}

std::vector<const DistributionFamily *> listDistributionFamilies()
{
  std::vector<const DistributionFamily *> out;
  for (const auto &entry : familiesByName())
    out.push_back(entry.second);
  return out;   /* std::map iteration order: sorted by name */
}

const DistributionFamily *lookupDistributionFamily(const std::string &name)
{
  const auto &families = familiesByName();
  const auto it = families.find(name);
  return it == families.end() ? nullptr : it->second;
}

std::unique_ptr<Distribution> makeDistribution(const DistributionSpec &spec)
{
  if (!spec.family) return nullptr;
  return spec.family->factory(spec.p1, spec.p2);
}

}  // namespace provsql
