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
#include <utility>

namespace provsql {

namespace {

/* Function-local statics so registration (dynamic initialisation of the
 * per-family registrar objects) never observes an uninitialised map,
 * whatever the TU initialisation order. */
std::map<std::pair<DistKind, DistKind>, ComparatorRule> &comparatorRules()
{
  static std::map<std::pair<DistKind, DistKind>, ComparatorRule> rules;
  return rules;
}

std::map<std::pair<DistKind, DistKind>, ClosureRule> &closureRules()
{
  static std::map<std::pair<DistKind, DistKind>, ClosureRule> rules;
  return rules;
}

struct FamilyRecord {
  DistributionFamily descriptor;
  DistributionFactory factory;
};

std::map<DistKind, FamilyRecord> &familiesByKind()
{
  static std::map<DistKind, FamilyRecord> families;
  return families;
}

std::map<std::string, FamilyRecord> &familiesByName()
{
  static std::map<std::string, FamilyRecord> families;
  return families;
}

/* Registry-miss default: P(X < Y) by the 1-D quadrature
 * P(X<Y) = ∫ (1 - F_Y(t)) f_X(t) dt over X's integration range (composite
 * Simpson).  Family-agnostic -- needs only pdf / cdf / integrationRange.
 * NaN when a density / CDF is undefined (e.g. a non-integer Erlang shape),
 * so the caller falls back to Monte Carlo. */
double quadraturePairLess(const Distribution &X, const Distribution &Y)
{
  double lo, hi;
  if (!X.integrationRange(lo, hi))
    return kNaN;
  const int N = 4000;
  const double h = (hi - lo) / N;
  double acc = 0.0;
  for (int i = 0; i <= N; ++i) {
    const double t = lo + i * h;
    const double fX = X.pdf(t);
    const double FY = Y.cdf(t);
    if (std::isnan(fX) || std::isnan(FY))
      return kNaN;
    const double coeff = (i == 0 || i == N) ? 1.0 : (i % 2 == 1 ? 4.0 : 2.0);
    acc += coeff * (1.0 - FY) * fX;
  }
  return acc * h / 3.0;
}

}  // namespace

void registerComparatorRule(DistKind x, DistKind y, ComparatorRule rule)
{
  comparatorRules()[{x, y}] = rule;
}

void registerClosureRule(DistKind x, DistKind y, ClosureRule rule)
{
  closureRules()[{x, y}] = rule;
}

std::unique_ptr<Distribution> closePlusTerms(
  const std::vector<ClosureTerm> &terms)
{
  const auto &rules = closureRules();
  ClosureRule rule = nullptr;
  bool first = true;
  DistKind k0{};
  for (const auto &t : terms) {
    if (!t.dist) continue;
    if (first) {
      first = false;
      k0 = t.dist->kind();
      const auto it = rules.find({k0, k0});
      if (it == rules.end()) return nullptr;
      rule = it->second;
    } else {
      const auto it = rules.find({k0, t.dist->kind()});
      if (it == rules.end() || it->second != rule) return nullptr;
    }
  }
  if (!rule) return nullptr;   /* no RV term: the constant fold's job */
  return rule(terms);
}

double comparatorPairLess(const Distribution &X, const Distribution &Y)
{
  const auto &rules = comparatorRules();
  const auto it = rules.find({X.kind(), Y.kind()});
  double pLess = kNaN;
  if (it != rules.end())
    pLess = it->second(X, Y);
  /* Registry miss, or a rule that declined (parameter guard, or a shape
   * outside its closed form): generic quadrature. */
  if (std::isnan(pLess))
    pLess = quadraturePairLess(X, Y);
  return pLess;
}

void registerDistributionFamily(DistKind kind, const char *name,
                                unsigned nparams,
                                DistributionFactory factory)
{
  const FamilyRecord record{{kind, nparams}, factory};
  familiesByKind()[kind] = record;
  familiesByName()[name] = record;
}

std::optional<DistributionFamily> lookupDistributionFamily(
  const std::string &name)
{
  const auto &families = familiesByName();
  const auto it = families.find(name);
  if (it == families.end()) return std::nullopt;
  return it->second.descriptor;
}

std::unique_ptr<Distribution> makeDistribution(const DistributionSpec &spec)
{
  const auto &families = familiesByKind();
  const auto it = families.find(spec.kind);
  if (it == families.end()) return nullptr;
  return it->second.factory(spec.p1, spec.p2);
}

}  // namespace provsql
