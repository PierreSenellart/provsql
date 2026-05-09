/**
 * @file RandomVariable.cpp
 * @brief Implementation of distribution parsing/formatting/moments.
 */
#include "RandomVariable.h"

#include <cctype>
#include <cstddef>
#include <exception>
#include <string>

namespace provsql {

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

}  // namespace provsql
