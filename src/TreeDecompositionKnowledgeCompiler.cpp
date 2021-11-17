#include <iostream>
#include <fstream>
#include <iomanip>
#include <sys/time.h>

#include "dDNNFTreeDecompositionBuilder.h"

static double get_timestamp ()
{
  struct timeval now;
  gettimeofday (&now, NULL);
  return now.tv_usec * 1e-6 + now.tv_sec;
}

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

  for(unsigned i=0; i<nbGates;++i) {
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

    auto dnnf{dDNNFTreeDecompositionBuilder{c, "0", td}.build()};
    t1 = get_timestamp();
    std::cerr << "Computing dDNNF took " << (t1-t0) << "s" << std::endl;
    t0 = t1;

    std::cerr << "Probability: " << std::setprecision (15) << dnnf.dDNNFEvaluation(dnnf.getGate("root")) << std::endl;
    t1 = get_timestamp();
    std::cerr << "Evaluating dDNNF took " << (t1-t0) << "s" << std::endl;
  } catch(TreeDecompositionException&) {
    std::cerr << "Could not build tree decomposition of width <= " << TreeDecomposition::MAX_TREEWIDTH;
    exit(1);
  }

  return 0;
}
