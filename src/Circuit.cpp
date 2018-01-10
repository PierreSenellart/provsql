#include "Circuit.h"

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

bool Circuit::hasGate(const uuid &u) const
{
  return uuid2id.find(u)!=uuid2id.end();
}

unsigned Circuit::getGate(const uuid &u)
{
  auto it=uuid2id.find(u);
  if(it==uuid2id.end()) {
    unsigned id=addGate();
    uuid2id[u]=id;
    return id;
  } else 
    return it->second;
}

unsigned Circuit::addGate()
{
  unsigned id=gates.size();
  gates.push_back(UNDETERMINED);
  prob.push_back(1);
  desc.push_back("");
  wires.resize(id+1);
  rwires.resize(id+1);
  return id;
}

Circuit::uuid Circuit::findGateUuid(const unsigned g) const
{
  for(auto it:uuid2id)
    if(it.second==g) return it.first;
  return "";
}

void Circuit::setGate(const uuid &u, gateType type, double p)
{
  unsigned id = getGate(u);
  gates[id] = type;
  prob[id] = p;
  if(type==IN)
    inputs.insert(id);
}

void Circuit::setGateWithDesc(const uuid &u, gateType type, std::string d)
{
  unsigned id = getGate(u);
  gates[id] = type;
  desc[id] = d;
  if(type==IN)
    inputs.insert(id);
}


void Circuit::addWire(unsigned f, unsigned t)
{
  wires[f].insert(t);
  rwires[t].insert(f);
}

//Outputs the gate in Graphviz dot format
std::string Circuit::toDot() const
{
  std::string op;
  string result="graph circuit{\n";

  //looping through the gates
  unsigned i=0;
  for(unsigned g:gates){
    result += to_string(i)+" [label=";
    switch(g) {
      case IN:
        if(prob[g]==0.) {
          result+="\"0\"";
        } else if(prob[g]==1.) {
          result+="\"1\"";
        } else {
          //result+="\""+to_string(i)+"\",shape=box"; //TODO add description
          result+="\""+desc[i]+"\",shape=box";
        }
        break;
      case NOT:
        result+="\"-\"";
        break;
      case UNDETERMINED:
        result+="\"?\"";
        break;
      case AND:
        result+="\"X\"";
        break;
      case OR:
        result+="\"+\"";
        break;
    }
    result+="];\n";
    i++;
  }

  //looping through the gates and their wires
  i=0;
  for(unsigned g:gates){
    for(auto s: wires[i])
      result += to_string(i)+" -- "+to_string(s)+";\n";
    i++;
  }
  return result+"}";
}



std::string Circuit::toString(unsigned g) const
{
  std::string op;
  string result;

  switch(gates[g]) {
    case IN:
      if(prob[g]==0.) {
        return "⊥";
      } else if(prob[g]==1.) {
        return "⊤";
      } else {
        return to_string(g)+"["+to_string(prob[g])+"]";
      }
    case NOT:
      op="¬";
      break;
    case UNDETERMINED:
      op="?";
      break;
    case AND:
      op="∧";
      break;
    case OR:
      op="∨";
      break;
  }

  if(wires[g].empty()) {
    if(gates[g]==AND)
      return "⊤";
    else if(gates[g]==OR)
      return "⊥";
    else return op;
  }

  for(auto s: wires[g]) {
    if(gates[g]==NOT)
      result = op;
    else if(!result.empty())
      result+=" "+op+" ";
    result+=toString(s);
  }

  return "("+result+")";
}

double Circuit::dDNNFEvaluation(unsigned g) const
{
  switch(gates[g]) {
    case IN:
      return prob[g];
    case NOT:
      return 1-prob[g];
    case AND:
      break;
    case OR:
      break;
    case UNDETERMINED:
      throw CircuitException("Incorrect gate type");
  }

  double result=(gates[g]==AND?1:0);
  for(auto s: wires[g]) {
    double d = dDNNFEvaluation(s);
    if(gates[g]==AND)
      result*=d;
    else
      result+=d;
  }

  return result;
}

bool Circuit::evaluate(unsigned g, const unordered_set<unsigned> &sampled) const
{
  bool disjunction=false;

  switch(gates[g]) {
    case IN:
      return sampled.find(g)!=sampled.end();
    case NOT:
      return !evaluate(*(wires[g].begin()), sampled);
    case AND:
      disjunction = false;
      break;
    case OR:
      disjunction = true;
      break;
    case UNDETERMINED:
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

double Circuit::monteCarlo(unsigned g, unsigned samples) const
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

double Circuit::possibleWorlds(unsigned g) const
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

int Circuit::dotRenderer() const {
  //Writing dot to a temporary file
  int fd;
  char cfilename[] = "/tmp/provsqlXXXXXX";
  fd = mkstemp(cfilename);
  close(fd);
  string filename=cfilename, outfilename=filename+".pdf";

  ofstream ofs(filename.c_str());
  ofs << toDot();
  ofs.close();

  //Executing the Graphviz dot renderer
  string cmdline="dot -Tpdf "+filename+" -o "+outfilename;

  int retvalue=system(cmdline.c_str());

  if(retvalue)    
    throw CircuitException("Error executing Graphviz dot"); 

  //Opening the PDF viewer
  retvalue = 0;

#ifdef __linux__
  //assuming evince on linux
  cmdline="export DISPLAY=':0'; xhost +; evince "+outfilename + " &";
  retvalue=system(cmdline.c_str());
#else
  throw CircuitException("Unsupported operating system for viewing");
#endif
  
  return 0;
}

double Circuit::compilation(unsigned g, string compiler) const {
  vector<vector<int>> clauses;

  // Tseytin transformation
  for(unsigned i=0; i<gates.size(); ++i) {
    switch(gates[i]) {
      case AND:
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

      case OR:
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

      case NOT:
        {
          int id=i+1;
          int s=*wires[i].begin();
          clauses.push_back({-id,-s-1});
          clauses.push_back({id,s+1});
          break;
        }

      case IN:
      case UNDETERMINED:
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

  Circuit dnnf;

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
      dnnf.setGate(to_string(i), OR);
      int g;
      while(ss >> g) {
        unsigned id2=dnnf.getGate(to_string(g));
        dnnf.addWire(id,id2);
      }
    } else if(c=='A') {
      int args;
      ss >> args;
      unsigned id=dnnf.getGate(to_string(i));
      dnnf.setGate(to_string(i), AND);
      int g;
      while(ss >> g) {
        unsigned id2=dnnf.getGate(to_string(g));
        dnnf.addWire(id,id2);
      }
    } else if(c=='L') {
      int leaf;
      ss >> leaf;
      if(gates[abs(leaf)-1]==IN) {
        if(leaf<0) {
          dnnf.setGate(to_string(i), IN, 1-prob[-leaf-1]);
        } else {
          dnnf.setGate(to_string(i), IN, prob[leaf-1]);
        }
      } else
        dnnf.setGate(to_string(i), IN, 1.);
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
