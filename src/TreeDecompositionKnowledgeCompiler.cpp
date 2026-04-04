/**
 * @file TreeDecompositionKnowledgeCompiler.cpp
 * @brief Standalone @c tdkc tool: tree-decomposition-based knowledge compiler.
 *
 * This file contains the @c main() entry point for the standalone
 * @c tdkc (Tree-Decomposition Knowledge Compiler) binary.  It is built
 * separately from the PostgreSQL extension (see @c Makefile target @c tdkc).
 *
 * Usage:
 * @code
 * tdkc <circuit_file>
 * @endcode
 *
 * The circuit file is a text file produced by @c BooleanCircuit::exportCircuit()
 * listing the number of gates followed by one line per gate describing its
 * type, probability, and children.
 *
 * The tool:
 * 1. Reads the circuit from the file.
 * 2. Rewrites multivalued gates.
 * 3. Computes a tree decomposition.
 * 4. Builds a d-DNNF via @c dDNNFTreeDecompositionBuilder.
 * 5. Evaluates the probability and prints it to @c stdout.
 *
 * Timing information is printed to @c stderr.
 */
#include <iostream>
#include <fstream>
#include <iomanip>

extern "C" {
#include <sys/time.h>
}

#include "dDNNFTreeDecompositionBuilder.h"
#include "Circuit.hpp"

/**
 * @brief Return the current time as a floating-point number of seconds.
 * @return Current time (seconds since the epoch, with microsecond precision).
 */
static double get_timestamp ()
{
  struct timeval now;
  gettimeofday (&now, NULL);
  return now.tv_usec * 1e-6 + now.tv_sec;
}

/**
 * @brief Entry point for the standalone tree-decomposition knowledge compiler.
 * @param argc  Argument count; must be 2.
 * @param argv  Argument vector; @c argv[1] is the path to the circuit file.
 * @return      0 on success, non-zero on error.
 */
int main(int argc, char **argv) {
  if(argc != 2) {
    std::cerr << "Usage: " << argv[0] << " circuit" << std::endl;
    exit(1);
  }

  std::ifstream g(argv[1]);
  BooleanCircuit c;
  unsigned nbGates;

  g >> nbGates;
  std::string line;
  std::getline(g,line);

  for(unsigned i=0; i<nbGates; ++i) {
    std::getline(g, line);
    if(line=="IN")
      c.setGate(std::to_string(i), BooleanGate::IN, 0.001);
    else if(line=="OR")
      c.setGate(std::to_string(i), BooleanGate::OR);
    else if(line=="AND")
      c.setGate(std::to_string(i), BooleanGate::AND);
    else if(line=="NOT")
      c.setGate(std::to_string(i), BooleanGate::NOT);
    else {
      std::cerr << "Wrong line type: " << line << std::endl;
      exit(1);
    }
  }

  gate_t u,v;
  while(g >> u >> v)
    c.addWire(u,v);
  g.close();

  try {
    double t0, t1;
    t0 = get_timestamp();

    TreeDecomposition td(c);
    std::cerr << "Treewidth: " << td.getTreewidth() << std::endl;
    t1 = get_timestamp();
    std::cerr << "Computing tree decomposition took " << (t1-t0) << "s" << std::endl;
    t0 = t1;

    auto dnnf{dDNNFTreeDecompositionBuilder{c, c.getGate("0"), td}.build()};
    t1 = get_timestamp();
    std::cerr << "Computing dDNNF took " << (t1-t0) << "s" << std::endl;
    t0 = t1;

    std::cerr << "Probability: " << std::setprecision (15) << dnnf.probabilityEvaluation() << std::endl;
    t1 = get_timestamp();
    std::cerr << "Evaluating dDNNF took " << (t1-t0) << "s" << std::endl;
  } catch(TreeDecompositionException&) {
    std::cerr << "Could not build tree decomposition of width <= " << TreeDecomposition::MAX_TREEWIDTH;
    exit(1);
  }

  return 0;
}
