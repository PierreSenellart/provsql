/**
 * @file RandomVariable.cpp
 * @brief Implementation of distribution parsing/formatting/moments.
 */
#include "RandomVariable.h"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <exception>
#include <string>

#include "Circuit.h"  // CircuitException

namespace provsql {

double parseDoubleStrict(const std::string &s)
{
  if (s.empty())
    throw CircuitException("Empty gate_value extra");
  std::size_t idx = 0;
  double v;
  try {
    v = std::stod(s, &idx);
  } catch (const std::exception &) {
    throw CircuitException("Cannot parse gate_value extra as double: " + s);
  }
  if (idx != s.size())
    throw CircuitException("Trailing characters in gate_value extra: " + s);
  return v;
}

namespace {

void strip(std::string &s)
{
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.pop_back();
}

bool parse_double(const std::string &raw, double &out)
{
  std::string s = raw;
  strip(s);
  if (s.empty()) return false;
  try {
    std::size_t idx = 0;
    out = std::stod(s, &idx);
    return idx == s.size();
  } catch (const std::exception &) {
    return false;
  }
}

}  // namespace

std::optional<DistributionSpec> parse_distribution_spec(const std::string &s)
{
  const auto colon = s.find(':');
  if (colon == std::string::npos) return std::nullopt;

  std::string kind_str = s.substr(0, colon);
  std::string params  = s.substr(colon + 1);
  strip(kind_str);
  strip(params);

  DistributionSpec out{};
  if (kind_str == "normal" || kind_str == "uniform") {
    const auto comma = params.find(',');
    if (comma == std::string::npos) return std::nullopt;
    if (!parse_double(params.substr(0, comma), out.p1)) return std::nullopt;
    if (!parse_double(params.substr(comma + 1), out.p2)) return std::nullopt;
    out.kind = (kind_str == "normal") ? DistKind::Normal : DistKind::Uniform;
    return out;
  }
  if (kind_str == "exponential") {
    if (!parse_double(params, out.p1)) return std::nullopt;
    out.p2 = 0.0;
    out.kind = DistKind::Exponential;
    return out;
  }
  return std::nullopt;
}

std::string format_distribution_spec(const DistributionSpec &d)
{
  switch (d.kind) {
    case DistKind::Normal:
      return "normal:" + std::to_string(d.p1) + "," + std::to_string(d.p2);
    case DistKind::Uniform:
      return "uniform:" + std::to_string(d.p1) + "," + std::to_string(d.p2);
    case DistKind::Exponential:
      return "exponential:" + std::to_string(d.p1);
  }
  return {};
}

double analytical_mean(const DistributionSpec &d)
{
  switch (d.kind) {
    case DistKind::Normal:      return d.p1;
    case DistKind::Uniform:     return 0.5 * (d.p1 + d.p2);
    case DistKind::Exponential: return 1.0 / d.p1;
  }
  return 0.0;
}

double analytical_variance(const DistributionSpec &d)
{
  switch (d.kind) {
    case DistKind::Normal: {
      const double sigma = d.p2;
      return sigma * sigma;
    }
    case DistKind::Uniform: {
      const double w = d.p2 - d.p1;
      return (w * w) / 12.0;
    }
    case DistKind::Exponential: {
      const double lambda = d.p1;
      return 1.0 / (lambda * lambda);
    }
  }
  return 0.0;
}

namespace {

double factorial(unsigned k)
{
  double r = 1.0;
  for (unsigned i = 2; i <= k; ++i) r *= static_cast<double>(i);
  return r;
}

double binomial_coeff(unsigned n, unsigned k)
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

// (j-1)!! with the empty-product convention (-1)!! = 1.
//   j = 0  ->  1
//   j = 2  ->  1!! = 1
//   j = 4  ->  3!! = 3
//   j = 6  ->  5!! = 15
double double_factorial_minus_one(unsigned j)
{
  if (j == 0) return 1.0;
  double r = 1.0;
  for (unsigned i = 1; i < j; i += 2) r *= static_cast<double>(i);
  return r;
}

}  // namespace

double analytical_raw_moment(const DistributionSpec &d, unsigned k)
{
  if (k == 0) return 1.0;
  if (k == 1) return analytical_mean(d);
  switch (d.kind) {
    case DistKind::Normal: {
      const double mu    = d.p1;
      const double sigma = d.p2;
      // E[X^k] = sum_{j=0,2,...}^{k} C(k,j) mu^(k-j) sigma^j (j-1)!!
      double total = 0.0;
      for (unsigned j = 0; j <= k; j += 2) {
        total += binomial_coeff(k, j)
               * std::pow(mu, static_cast<double>(k - j))
               * std::pow(sigma, static_cast<double>(j))
               * double_factorial_minus_one(j);
      }
      return total;
    }
    case DistKind::Uniform: {
      const double a   = d.p1;
      const double b   = d.p2;
      const double kp1 = static_cast<double>(k + 1);
      return (std::pow(b, kp1) - std::pow(a, kp1)) / (kp1 * (b - a));
    }
    case DistKind::Exponential: {
      const double lambda = d.p1;
      return factorial(k) / std::pow(lambda, static_cast<double>(k));
    }
  }
  return 0.0;
}

}  // namespace provsql
