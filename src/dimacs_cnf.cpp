/**
 * @file dimacs_cnf.cpp
 * @brief Implementation of the DIMACS-CNF to BooleanCircuit parser.
 */
#include "dimacs_cnf.h"
#include "Circuit.hpp"

#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// A MCC weight line: "c p weight <lit> <w> 0".  Returns true and fills
// (lit, w) if @p line is one.
bool parse_weight_line(const std::string &line, long &lit, double &w)
{
  std::istringstream iss(line);
  std::string c, p, weight;
  if (!(iss >> c >> p >> weight) || c != "c" || p != "p" || weight != "weight")
    return false;
  long zero = 1;
  if (!(iss >> lit >> w >> zero))
    return false;
  return true;
}

} // namespace

gate_t parse_dimacs_cnf(const std::string &text, BooleanCircuit &c,
                        bool weighted)
{
  std::map<long, double> weights;   // positive literal -> weight
  std::vector<int> tokens;          // clause literals, 0-terminated, flattened
  bool saw_header = false;

  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    // Trim leading whitespace to classify the line.
    size_t s = line.find_first_not_of(" \t\r");
    if (s == std::string::npos)
      continue;
    char lead = line[s];
    if (lead == 'c') {
      long lit; double w;
      if (parse_weight_line(line.substr(s), lit, w) && lit > 0)
        weights[lit] = w;
      continue;  // other comments (incl. "c t wmc", "c p show") ignored
    }
    if (lead == 'p') {
      std::istringstream iss(line.substr(s));
      std::string p, cnf;
      iss >> p >> cnf;  // "p cnf <nvars> <nclauses>"; counts not needed
      saw_header = true;
      continue;
    }
    // A clause line: whitespace-separated literals (a clause may span lines,
    // terminated by 0).
    std::istringstream iss(line.substr(s));
    int lit;
    while (iss >> lit)
      tokens.push_back(lit);
  }
  if (!saw_header)
    throw std::runtime_error("DIMACS: missing 'p cnf' header");

  // Materialise the circuit. Input / NOT gates are created on first use.
  std::map<int, gate_t> in_gate;   // var -> IN gate
  std::map<int, gate_t> neg_gate;  // var -> NOT gate

  auto literal_gate = [&](int lit) -> gate_t {
    int v = std::abs(lit);
    auto it = in_gate.find(v);
    if (it == in_gate.end()) {
      double p = 0.5;
      if (weighted) {
        auto w = weights.find(v);
        if (w != weights.end()) p = w->second;
      }
      gate_t g = c.setGate("v" + std::to_string(v), BooleanGate::IN, p);
      it = in_gate.emplace(v, g).first;
    }
    if (lit > 0)
      return it->second;
    auto n = neg_gate.find(v);
    if (n == neg_gate.end()) {
      gate_t ng = c.setGate("n" + std::to_string(v), BooleanGate::NOT);
      c.addWire(ng, it->second);
      n = neg_gate.emplace(v, ng).first;
    }
    return n->second;
  };

  gate_t root = c.setGate("root", BooleanGate::AND);
  unsigned clause_no = 0;
  gate_t clause = c.setGate("cl0", BooleanGate::OR);
  bool clause_open = false;
  for (int lit : tokens) {
    if (lit == 0) {
      c.addWire(root, clause);
      clause = c.setGate("cl" + std::to_string(++clause_no), BooleanGate::OR);
      clause_open = false;
    } else {
      c.addWire(clause, literal_gate(lit));
      clause_open = true;
    }
  }
  // A trailing clause not terminated by 0 is still honoured.
  if (clause_open)
    c.addWire(root, clause);

  return root;
}
