#ifndef DOT_CIRCUIT_H
#define DOT_CIRCUIT_H

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <vector>

#include "Circuit.hpp"

enum class DotGate
{
  UNDETERMINED,
  OTIMES,
  OPLUS,
  OMINUS,
  OMINUSR,
  OMINUSL,
  PROJECT,
  EQ,
  IN,
  DELTA
};

class DotCircuit : public Circuit<DotGate> {
 private:
  std::set<gate_t> inputs;
  std::vector<std::string> desc;
  
 public:
  gate_t addGate() override;
  gate_t setGate(const uuid &u, DotGate t) override;
  gate_t setGate(const uuid &u, DotGate t, std::string d);

  std::string render() const;
  
  virtual std::string toString(gate_t g) const override;
};

#endif /* DOT_CIRCUIT_H */
