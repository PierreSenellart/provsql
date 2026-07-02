/**
 * @file RandomVariable.cpp
 * @brief Implementation of distribution parsing/formatting/moments.
 */
#include "RandomVariable.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>

#include "Circuit.h"  // CircuitException
#include "distributions/Distribution.h"  // makeDistribution (per-family closed forms)

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

/* std::ostringstream is used rather than std::snprintf in the fallback
 * because including <cstdio> after PostgreSQL's port.h would expand
 * std::snprintf to the non-existent std::pg_snprintf via the
 * #define snprintf macro. */
std::string double_to_text(double v)
{
  std::array<char, 32> buf;
  auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
  if (ec == std::errc{}) return std::string(buf.data(), ptr);
  std::ostringstream oss;
  oss << std::setprecision(17) << v;
  return oss.str();
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

  /* Resolve the family name through the DistributionRegistry so a new
   * family's token is recognised without touching this parser. */
  const DistributionFamily *family = lookupDistributionFamily(kind_str);
  if (!family) return std::nullopt;

  DistributionSpec out{};
  out.family = family;
  if (family->nparams == 2) {
    const auto comma = params.find(',');
    if (comma == std::string::npos) return std::nullopt;
    if (!parse_double(params.substr(0, comma), out.p1)) return std::nullopt;
    if (!parse_double(params.substr(comma + 1), out.p2)) return std::nullopt;
  } else {
    if (!parse_double(params, out.p1)) return std::nullopt;
    out.p2 = 0.0;
  }
  return out;
}

/* analytical_mean / analytical_variance / analytical_raw_moment are thin
 * wrappers over the per-family Distribution closed forms (src/Distribution.*).
 * The family-specific formulas live in the Distribution subclasses; these
 * free functions stay as the stable call surface for existing consumers. */
double analytical_mean(const DistributionSpec &d)
{
  return makeDistribution(d)->mean();
}

double analytical_variance(const DistributionSpec &d)
{
  return makeDistribution(d)->variance();
}

double analytical_raw_moment(const DistributionSpec &d, unsigned k)
{
  return makeDistribution(d)->rawMoment(k);
}

}  // namespace provsql
