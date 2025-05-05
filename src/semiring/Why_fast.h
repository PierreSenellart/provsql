#ifndef WHY_H
#define WHY_H

#include <unordered_set>
#include <string>
#include "Semiring.h"  

namespace semiring {

// A single provenance label set: a set of strings
using label_set = std::unordered_set<std::string>;

// Hash function for label_set: allows using label_set as key in unordered containers
struct LabelSetHasher {
    std::size_t operator()(label_set const &s) const noexcept {
        std::size_t h = 0;
        std::hash<std::string> string_hash;
        // Sum the hash of each label 
        for (auto const &lbl : s) {
            h += string_hash(lbl);
        }
        return h;
    }
};


// Equality comparator for label_set: checks two sets contain the same labels
struct LabelSetEqual {
    bool operator()(label_set const &a, label_set const &b) const noexcept {
        if (a.size() != b.size()) return false;
        for (auto const &lbl : a) {
            if (b.find(lbl) == b.end()) return false;
        }
        return true;
    }
};

// The Why semiring value type: a set of label_sets (i.e., 2^(2^X))
using why_provenance_t = std::unordered_set<
    label_set,
    LabelSetHasher,
    LabelSetEqual
>;

class Why : public Semiring<why_provenance_t> {
public:
    // Additive identity: empty set (no justifications)
    virtual value_type zero() const {
        return {};  
    }

    // Multiplicative identity: set containing the empty label_set
    virtual value_type one() const {
        return why_provenance_t{ label_set{} };
    }

    // Plus = union of all tokens
    virtual value_type plus(std::vector<value_type> const &v) const {
        value_type result;
        for (auto const &subset : v) {
            result.insert(subset.begin(), subset.end());
        }
        return result;
    }

    // Times = Cartesian product followed by union of inner label_sets
    // A ⋓ B = { a ∪ b | a ∈ A, b ∈ B }
    virtual value_type times(std::vector<value_type> const &v) const {
        if (v.empty()) return one();
        value_type result = v.front();
        for (size_t i = 1; i < v.size(); ++i) {
            value_type next;
            for (auto const &a : result) {
                for (auto const &b : v[i]) {
                    label_set u = a;            
                    u.insert(b.begin(), b.end());
                    next.insert(std::move(u));
                }
            }
            result = std::move(next);
        }
        return result;
    }

    // Monus = set difference of tokens
    virtual value_type monus(value_type x, value_type y) const  {
        for (auto const &b : y) {
            x.erase(b);
        }
        return x;
    }

    // Delta = zero->zero, nonzero->one
    virtual value_type delta(value_type x) const {
        return x.empty() ? zero() : x;
    }
};

} 


//Why hash version
// static Datum pec_why(
//   const constants_t &constants,
//   GenericCircuit &c,
//   gate_t g,
//   const std::set<gate_t> &inputs,
//   const std::string &semiring,
//   bool drop_table)
// {
//   
//   std::unordered_map<gate_t, semiring::why_provenance_t> provenance_mapping;
//   initialize_provenance_mapping<semiring::why_provenance_t>(
//       constants,
//       c,
//       provenance_mapping,
//       /* parser: char* ------ why_provenance_t */
//       [](const char *v) {
//           semiring::why_provenance_t result;
//           
//           semiring::label_set single;
//           single.insert(std::string(v));
//           
//           result.insert(std::move(single));
//           return result;
//       },
//       drop_table
//   );

//   if (semiring == "why") {
//       
//       auto prov = c.evaluate<semiring::Why>(g, provenance_mapping);

//    
//       std::ostringstream oss;
//       oss << "{";
//       bool firstOuter = true;
//       for (auto const &inner : prov) {
//           if (!firstOuter) oss << ",";
//           firstOuter = false;
//           oss << "{";
//           bool firstInner = true;
//           for (auto const &lbl : inner) {
//               if (!firstInner) oss << ",";
//               firstInner = false;
//               oss << lbl;
//           }
//           oss << "}";
//       }
//       oss << "}";

//       std::string out = oss.str();
//       text *result = (text *) palloc(VARHDRSZ + out.size() + 1);
//       SET_VARSIZE(result, VARHDRSZ + out.size());
//       memcpy(VARDATA(result), out.c_str(), out.size());
//       PG_RETURN_TEXT_P(result);
//   } else {
//       throw CircuitException("Unknown semiring for type varchar: " + semiring);
//   }
// }

#endif // WHY_H
