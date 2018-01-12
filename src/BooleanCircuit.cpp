#include "BooleanCircuit.h"

extern "C" {
#include "provsql_utils.h"
#include <unistd.h>
}

#include <cassert>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>

using namespace std;

// Has to be redefined because of name hiding
unsigned BooleanCircuit::setGate(const uuid &u, BooleanGate type)
{
 unsigned id = Circuit::setGate(u, type);
  if(type == BooleanGate::IN)
    inputs.insert(id);
  return id;
}

unsigned BooleanCircuit::setGate(const uuid &u, BooleanGate type, double p)
{
  unsigned id = setGate(u, type);
  prob[id] = p;
  return id;
}

unsigned BooleanCircuit::addGate()
{
  unsigned id=Circuit::addGate();
  prob.push_back(1);
  return id;
}

std::string BooleanCircuit::toString(unsigned g) const
{
  std::string op;
  string result;

  switch(gates[g]) {
    case BooleanGate::IN:
      if(prob[g]==0.) {
        return "⊥";
      } else if(prob[g]==1.) {
        return "⊤";
      } else {
        return to_string(g)+"["+to_string(prob[g])+"]";
      }
    case BooleanGate::NOT:
      op="¬";
      break;
    case BooleanGate::UNDETERMINED:
      op="?";
      break;
    case BooleanGate::AND:
      op="∧";
      break;
    case BooleanGate::OR:
      op="∨";
      break;
  }

  if(wires[g].empty()) {
    if(gates[g]==BooleanGate::AND)
      return "⊤";
    else if(gates[g]==BooleanGate::OR)
      return "⊥";
    else return op;
  }

  for(auto s: wires[g]) {
    if(gates[g]==BooleanGate::NOT)
      result = op;
    else if(!result.empty())
      result+=" "+op+" ";
    result+=toString(s);
  }

  return "("+result+")";
}

double BooleanCircuit::dDNNFEvaluation(unsigned g) const
{
  switch(gates[g]) {
    case BooleanGate::IN:
      return prob[g];
    case BooleanGate::NOT:
      return 1-prob[g];
    case BooleanGate::AND:
      break;
    case BooleanGate::OR:
      break;
    case BooleanGate::UNDETERMINED:
      throw CircuitException("Incorrect gate type");
  }

  double result=(gates[g]==BooleanGate::AND?1:0);
  for(auto s: wires[g]) {
    double d = dDNNFEvaluation(s);
    if(gates[g]==BooleanGate::AND)
      result*=d;
    else
      result+=d;
  }

  return result;
}

bool BooleanCircuit::evaluate(unsigned g, const unordered_set<unsigned> &sampled) const
{
  bool disjunction=false;

  switch(gates[g]) {
    case BooleanGate::IN:
      return sampled.find(g)!=sampled.end();
    case BooleanGate::NOT:
      return !evaluate(*(wires[g].begin()), sampled);
    case BooleanGate::AND:
      disjunction = false;
      break;
    case BooleanGate::OR:
      disjunction = true;
      break;
    case BooleanGate::UNDETERMINED:
      throw CircuitException("Incorrect gate type");
  }

  for(auto s: wires[g]) {
    bool e = evaluate(s, sampled);
    if(disjunction && e)
      return true;
    if(!disjunction && !e)
      return false;
  }

  if(disjunction)
    return false;
  else
    return true;
}

double BooleanCircuit::monteCarlo(unsigned g, unsigned samples) const
{
  unsigned success=0;

  for(unsigned i=0; i<samples; ++i) {
    unordered_set<unsigned> sampled;
    for(unsigned in : inputs) {
      if(rand() *1. / RAND_MAX < prob[in]) {
        sampled.insert(in);
      }
    }

    if(evaluate(g, sampled))
      ++success;
    
    if(provsql_interrupted)
      throw CircuitException("Interrupted after "+to_string(i+1)+" samples");
  }

  return success*1./samples;
}

double BooleanCircuit::possibleWorlds(unsigned g) const
{ 
  if(inputs.size()>=8*sizeof(unsigned long long))
    throw CircuitException("Too many possible worlds to iterate over");

  unsigned long long nb=(1<<inputs.size());
  double totalp=0.;

  for(unsigned long long i=0; i < nb; ++i) {
    unordered_set<unsigned> s;
    double p = 1;

    unsigned j=0;
    for(unsigned in : inputs) {
      if(i & (1 << j)) {
        s.insert(in);
        p*=prob[in];
      } else {
        p*=1-prob[in];
      }
      ++j;
    }

    if(evaluate(g, s))
      totalp+=p;
   
    if(provsql_interrupted)
      throw CircuitException("Interrupted");
  }

  return totalp;
}

double BooleanCircuit::compilation(unsigned g, string compiler) const {
  vector<vector<int>> clauses;

  // Tseytin transformation
  for(unsigned i=0; i<gates.size(); ++i) {
    switch(gates[i]) {
      case BooleanGate::AND:
        {
          int id=i+1;
          vector<int> c = {id};
          for(int s: wires[i]) {
            clauses.push_back({-id, s+1});
            c.push_back(-s-1);
          }
          clauses.push_back(c);
          break;
        }

      case BooleanGate::OR:
        {
          int id=i+1;
          vector<int> c = {-id};
          for(int s: wires[i]) {
            clauses.push_back({id, -s-1});
            c.push_back(s+1);
          }
          clauses.push_back(c);
        }
        break;

      case BooleanGate::NOT:
        {
          int id=i+1;
          int s=*wires[i].begin();
          clauses.push_back({-id,-s-1});
          clauses.push_back({id,s+1});
          break;
        }

      case BooleanGate::IN:
      case BooleanGate::UNDETERMINED:
        ;
    }
  }
  clauses.push_back({(int)g+1});

  int fd;
  char cfilename[] = "/tmp/provsqlXXXXXX";
  fd = mkstemp(cfilename);
  close(fd);
  string filename=cfilename, outfilename=filename+".nnf";

  ofstream ofs(filename.c_str());

  ofs << "p cnf " << gates.size() << " " << clauses.size() << "\n";

  for(unsigned i=0;i<clauses.size();++i) {
    for(int x : clauses[i]) {
      ofs << x << " ";
    }
    ofs << "0\n";
  }

  ofs.close();

  string cmdline=compiler+" ";
  if(compiler=="d4") {
    cmdline+=filename+" -out="+outfilename;
  } else if(compiler=="c2d") {
    cmdline+="-in "+filename+" -silent";
  } else if(compiler=="dsharp") {
    cmdline+="-q -Fnnf "+outfilename+" "+filename;
  } else {
    throw CircuitException("Unknown compiler '"+compiler+"'");
  }

  int retvalue=system(cmdline.c_str());

  if(unlink(filename.c_str())) {
    throw CircuitException("Error removing "+filename);
  }

  if(retvalue)    
    throw CircuitException("Error executing "+compiler);
  
  ifstream ifs(outfilename.c_str());

  string nnf;
  getline(ifs, nnf, ' ');

  if(nnf!="nnf") // unsatisfiable formula
    return 0.;

  unsigned nb_nodes, foobar, nb_variables;
  ifs >> nb_nodes >> foobar >> nb_variables;

  BooleanCircuit dnnf;

  if(nb_variables!=gates.size())
    throw CircuitException("Unreadable d-DNNF (wrong number of variables: " + to_string(nb_variables) +" vs " + to_string(gates.size()) + ")");

  std::string line;
  getline(ifs,line);
  unsigned i=0;
  while(getline(ifs,line)) {
    stringstream ss(line);
    
    char c;
    ss >> c;

    if(c=='O') {
      int var, args;
      ss >> var >> args;
      unsigned id=dnnf.getGate(to_string(i));
      dnnf.setGate(to_string(i), BooleanGate::OR);
      int g;
      while(ss >> g) {
        unsigned id2=dnnf.getGate(to_string(g));
        dnnf.addWire(id,id2);
      }
    } else if(c=='A') {
      int args;
      ss >> args;
      unsigned id=dnnf.getGate(to_string(i));
      dnnf.setGate(to_string(i), BooleanGate::AND);
      int g;
      while(ss >> g) {
        unsigned id2=dnnf.getGate(to_string(g));
        dnnf.addWire(id,id2);
      }
    } else if(c=='L') {
      int leaf;
      ss >> leaf;
      if(gates[abs(leaf)-1]==BooleanGate::IN) {
        if(leaf<0) {
          dnnf.setGate(to_string(i), BooleanGate::IN, 1-prob[-leaf-1]);
        } else {
          dnnf.setGate(to_string(i), BooleanGate::IN, prob[leaf-1]);
        }
      } else
        dnnf.setGate(to_string(i), BooleanGate::IN, 1.);
    } else 
      throw CircuitException(string("Unreadable d-DNNF (unknown node type: ")+c+")");

    ++i;
  }

  ifs.close();
  if(unlink(outfilename.c_str())) {
    throw CircuitException("Error removing "+outfilename);
  }

//  throw CircuitException(toString(g) + "\n" + dnnf.toString(dnnf.getGate(to_string(i-1))));

  return dnnf.dDNNFEvaluation(dnnf.getGate(to_string(i-1)));
}
