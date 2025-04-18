#ifndef SEMIRING_H
#define SEMIRING_H

#include <vector>

namespace semiring {
template<typename V>
class Semiring
{
public:
typedef V value_type;

virtual value_type zero() const = 0;
virtual value_type one() const = 0;
virtual value_type plus(const std::vector<value_type> &v) const = 0;
virtual value_type times(const std::vector<value_type> &v) const = 0;
virtual value_type monus(value_type x, value_type y) const = 0;
virtual value_type delta(value_type x) const = 0;

virtual ~Semiring() = default;
};
}

#endif /* SEMIRING_H */
