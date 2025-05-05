#ifndef WHY_H
#define WHY_H

#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include "Semiring.h"

namespace semiring {

using label_t = std::string;
using label_set = std::set<label_t>;
using why_provenance_t = std::set<label_set>;

class Why : public Semiring<why_provenance_t> {
public:
// Additive identity
value_type zero() const {
  return {};
}

// Multiplicative identity: empty set (⊗(x,{{}}) means "don't change")
value_type one() const {
  return { {} };
}

// Union of all input sets
value_type plus(const std::vector<value_type> &vec) const {
  value_type result;
  for (const auto &v : vec) {
    result.insert(v.begin(), v.end());
  }
  return result;
}

// Cartesian product: union each inner set with each other
value_type times(const std::vector<value_type> &vec) const {
  if (vec.empty()) return one();

  value_type result = vec[0];
  for (size_t i = 1; i < vec.size(); ++i) {
    value_type temp;
    for (const auto &s1 : result) {
      for (const auto &s2 : vec[i]) {
        label_set combined = s1;
        combined.insert(s2.begin(), s2.end());
        temp.insert(std::move(combined));
      }
    }
    result = std::move(temp);
  }
  return result;
}


virtual value_type monus(value_type x, value_type y) const {
  for (auto const &s : y) {
    x.erase(s);
  }
  return x;
}



value_type delta(value_type x) const {
  return x.empty() ? zero() : x;
}
};

}

#endif // WHY_H
