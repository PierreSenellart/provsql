#include "BooleanCircuit.h"
#include <type_traits>

extern "C" {
#include <unistd.h>
#include <math.h>
}

#include <cassert>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "dDNNF.h"

// "provsql_utils.h"
#ifdef TDKC
constexpr bool provsql_interrupted = false;
constexpr int provsql_verbose = 0;
#define elog(level, ...) {;}
#else
#include "provsql_utils.h"
extern "C" {
#include "utils/elog.h"
}
#endif

gate_t BooleanCircuit::setGate(BooleanGate type)
{
  auto id = Circuit::setGate(type);
  if(type == BooleanGate::IN) {
    setProb(id,1.);
    inputs.insert(id);
  } else if(type == BooleanGate::MULIN) {
    mulinputs.insert(id);
  }
  return id;
}

gate_t BooleanCircuit::setGate(const uuid &u, BooleanGate type)
{
 auto id = Circuit::setGate(u, type);
  if(type == BooleanGate::IN) {
    setProb(id,1.);
    inputs.insert(id);
  } else if(type == BooleanGate::MULIN) {
    mulinputs.insert(id);
  }
  return id;
}

gate_t BooleanCircuit::setGate(const uuid &u, BooleanGate type, double p)
{
  auto id = setGate(u, type);
  setProb(id,p);
  return id;
}

gate_t BooleanCircuit::setGate(BooleanGate type, double p)
{
  auto id = setGate(type);
  setProb(id,p);
  return id;
}

gate_t BooleanCircuit::addGate()
{
  auto id=Circuit::addGate();
  prob.push_back(1);
  return id;
}

std::string BooleanCircuit::toString(gate_t g) const
{
  std::string op;
  std::string result;

  switch(getGateType(g)) {
    case BooleanGate::IN:
      if(getProb(g)==0.) {
        return "⊥";
      } else if(getProb(g)==1.) {
        return "⊤";
      } else {
        return to_string(g)+"["+std::to_string(getProb(g))+"]";
      }
    case BooleanGate::MULIN:
      return "{" + to_string(*getWires(g).begin()) + "=" + std::to_string(getInfo(g)) + "}[" + std::to_string(getProb(g)) + "]";
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
    case BooleanGate::MULVAR:
      ; // already dealt with in MULIN
  }

  if(getWires(g).empty()) {
    if(getGateType(g)==BooleanGate::AND)
      return "⊤";
    else if(getGateType(g)==BooleanGate::OR)
      return "⊥";
    else return op;
  }

  for(auto s: getWires(g)) {
    if(getGateType(g)==BooleanGate::NOT)
      result = op;
    else if(!result.empty())
      result+=" "+op+" ";
    result+=toString(s);
  }

  return "("+result+")";
}

bool BooleanCircuit::evaluate(gate_t g, const std::unordered_set<gate_t> &sampled) const
{
  bool disjunction=false;

  switch(getGateType(g)) {
    case BooleanGate::IN:
      return sampled.find(g)!=sampled.end();
    case BooleanGate::MULIN:
    case BooleanGate::MULVAR:
      throw CircuitException("Monte-Carlo sampling not implemented on multivalued inputs");
    case BooleanGate::NOT:
      return !evaluate(*(getWires(g).begin()), sampled);
    case BooleanGate::AND:
      disjunction = false;
      break;
    case BooleanGate::OR:
      disjunction = true;
      break;
    case BooleanGate::UNDETERMINED:
      throw CircuitException("Incorrect gate type");
  }

  for(auto s: getWires(g)) {
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

double BooleanCircuit::monteCarlo(gate_t g, unsigned samples) const
{
  auto success{0u};

  for(unsigned i=0; i<samples; ++i) {
    std::unordered_set<gate_t> sampled;
    for(auto in: inputs) {
      if(rand() *1. / RAND_MAX < getProb(in)) {
        sampled.insert(in);
      }
    }

    if(evaluate(g, sampled))
      ++success;
    
    if(provsql_interrupted)
      throw CircuitException("Interrupted after "+std::to_string(i+1)+" samples");
  }

  return success*1./samples;
}

double BooleanCircuit::possibleWorlds(gate_t g) const
{ 
  if(inputs.size()>=8*sizeof(unsigned long long))
    throw CircuitException("Too many possible worlds to iterate over");

  unsigned long long nb=(1<<inputs.size());
  double totalp=0.;

  for(unsigned long long i=0; i < nb; ++i) {
    std::unordered_set<gate_t> s;
    double p = 1;

    unsigned j=0;
    for(gate_t in : inputs) {
      if(i & (1 << j)) {
        s.insert(in);
        p*=getProb(in);
      } else {
        p*=1-getProb(in);
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

std::string BooleanCircuit::Tseytin(gate_t g, bool display_prob=false) const {
  std::vector<std::vector<int>> clauses;
  
  // Tseytin transformation
  for(gate_t i{0}; i<gates.size(); ++i) {
    switch(getGateType(i)) {
      case BooleanGate::AND:
        {
          int id{static_cast<int>(i)+1};
          std::vector<int> c = {id};
          for(auto s: getWires(i)) {
            clauses.push_back({-id, static_cast<int>(s)+1});
            c.push_back(-static_cast<int>(s)-1);
          }
          clauses.push_back(c);
          break;
        }

      case BooleanGate::OR:
        {
          int id{static_cast<int>(i)+1};
          std::vector<int> c = {-id};
          for(auto s: getWires(i)) {
            clauses.push_back({id, -static_cast<int>(s)-1});
            c.push_back(static_cast<int>(s)+1);
          }
          clauses.push_back(c);
        }
        break;

      case BooleanGate::NOT:
        {
          int id=static_cast<int>(i)+1;
          auto s=*getWires(i).begin();
          clauses.push_back({-id,-static_cast<int>(s)-1});
          clauses.push_back({id,static_cast<int>(s)+1});
          break;
        }

      case BooleanGate::MULIN:
        throw CircuitException("Multivalued inputs should have been removed by then.");  
      case BooleanGate::MULVAR:
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

  std::string filename=cfilename;
  std::ofstream ofs(filename.c_str());

  ofs << "p cnf " << gates.size() << " " << clauses.size() << "\n";

  for(unsigned i=0;i<clauses.size();++i) {
    for(int x : clauses[i]) {
      ofs << x << " ";
    }
    ofs << "0\n";
  }
  if(display_prob) {
    for(gate_t in: inputs) {
      ofs << "w " << (static_cast<std::underlying_type<gate_t>::type>(in)+1) << " " << getProb(in) << "\n";
      ofs << "w -" << (static_cast<std::underlying_type<gate_t>::type>(in)+1) << " " << (1. - getProb(in)) << "\n";
    }
  }

  ofs.close();

  return filename;
}

double BooleanCircuit::compilation(gate_t g, std::string compiler) const {
  std::string filename=BooleanCircuit::Tseytin(g);
  std::string outfilename=filename+".nnf";

  if(provsql_verbose>=20) {
    elog(NOTICE, "Tseytin circuit in %s", filename.c_str());
  }

  bool new_d4 {false};
  std::string cmdline=compiler+" ";
  if(compiler=="d4") {
    cmdline+="-dDNNF "+filename+" -out="+outfilename;
    new_d4 = true;
  } else if(compiler=="c2d") {
    cmdline+="-in "+filename+" -silent";
  } else if(compiler=="minic2d") {
    cmdline+="-in "+filename;
  } else if(compiler=="dsharp") {
    cmdline+="-q -Fnnf "+outfilename+" "+filename;
  } else {
    throw CircuitException("Unknown compiler '"+compiler+"'");
  }

  int retvalue=system(cmdline.c_str());

  if(retvalue && compiler=="d4") {
    // Temporary support for older version of d4
    new_d4 = false;
    cmdline = "d4 "+filename+" -out="+outfilename;
    retvalue=system(cmdline.c_str());
  }

  if(retvalue)
    throw CircuitException("Error executing "+compiler);
  
  if(provsql_verbose<20) {
    if(unlink(filename.c_str())) {
      throw CircuitException("Error removing "+filename);
    }
  }
  
  std::ifstream ifs(outfilename.c_str());

  std::string line;
  getline(ifs,line);

  if(line.rfind("nnf", 0) != 0) {
    // New d4 does not include this magic line

    if(compiler != "d4") {
      // unsatisfiable formula
      return 0.;
    }
  } else {
    std::string nnf;
    unsigned nb_nodes, nb_edges, nb_variables;

    std::stringstream ss(line);
    ss >> nnf >> nb_nodes >> nb_edges >> nb_variables;
  
    if(nb_variables!=gates.size())
      throw CircuitException("Unreadable d-DNNF (wrong number of variables: " + std::to_string(nb_variables) +" vs " + std::to_string(gates.size()) + ")");
  
    getline(ifs,line);
  }

  dDNNF dnnf;

  unsigned i=0;
  do {
    std::stringstream ss(line);
    
    std::string c;
    ss >> c;

    if(c=="O") {
      int var, args;
      ss >> var >> args;
      auto id=dnnf.getGate(std::to_string(i));
      dnnf.setGate(std::to_string(i), BooleanGate::OR);
      int g;
      while(ss >> g) {
        auto id2=dnnf.getGate(std::to_string(g));
        dnnf.addWire(id,id2);
      }
    } else if(c=="A") {
      int args;
      ss >> args;
      auto id=dnnf.getGate(std::to_string(i));
      dnnf.setGate(std::to_string(i), BooleanGate::AND);
      int g;
      while(ss >> g) {
        auto id2=dnnf.getGate(std::to_string(g));
        dnnf.addWire(id,id2);
      }
    } else if(c=="L") {
      int leaf;
      ss >> leaf;
      if(gates[abs(leaf)-1]==BooleanGate::IN) {
        if(leaf<0) {
          dnnf.setGate(std::to_string(i), BooleanGate::IN, 1-prob[-leaf-1]);
        } else {
          dnnf.setGate(std::to_string(i), BooleanGate::IN, prob[leaf-1]);
        }
      } else
        dnnf.setGate(std::to_string(i), BooleanGate::IN, 1.);
    } else if(c=="f" || c=="o") {
      // d4 extended format
      // A FALSE gate is an OR gate without wires
      int var;
      ss >> var;
      dnnf.setGate(std::to_string(var), BooleanGate::OR);
    } else if(c=="t" || c=="a") {
      // d4 extended format
      // A TRUE gate is an AND gate without wires
      int var;
      ss >> var;
      dnnf.setGate(std::to_string(var), BooleanGate::AND);
    } else if(dnnf.hasGate(c)) {
      // d4 extended format
      int var;
      ss >> var;
      auto id2=dnnf.getGate(std::to_string(var));

      std::vector<int> decisions;
      int decision;
      while(ss >> decision) {
        if(decision==0)
          break;
        if(gates[abs(decision)-1]==BooleanGate::IN)
          decisions.push_back(decision);
      }

      if(decisions.empty()) {
        dnnf.addWire(dnnf.getGate(c), id2);
      } else {
        auto and_gate = dnnf.setGate(BooleanGate::AND);
        dnnf.addWire(dnnf.getGate(c), and_gate);
        dnnf.addWire(and_gate, id2);
        for(auto leaf : decisions) {
           gate_t leaf_gate;
          if(leaf<0) {
            leaf_gate = dnnf.setGate("i"+std::to_string(leaf), BooleanGate::IN, 1-prob[-leaf-1]);
          } else {
            leaf_gate = dnnf.setGate("i"+std::to_string(leaf), BooleanGate::IN, prob[leaf-1]);
          }
          dnnf.addWire(and_gate, leaf_gate);
        }
      }
    } else
      throw CircuitException(std::string("Unreadable d-DNNF (unknown node type: ")+c+")");

    ++i;
  } while(getline(ifs, line));

  ifs.close();

  if(provsql_verbose<20) {
    if(unlink(outfilename.c_str())) {
      throw CircuitException("Error removing "+outfilename);
    }
  } else
    elog(NOTICE, "Compiled d-DNNF in %s", outfilename.c_str());

  return dnnf.dDNNFEvaluation(dnnf.getGate(new_d4?"1":std::to_string(i-1)));
}

double BooleanCircuit::WeightMC(gate_t g, std::string opt) const {
  std::string filename=BooleanCircuit::Tseytin(g, true);

  //opt of the form 'delta;epsilon'
  std::stringstream ssopt(opt); 
  std::string delta_s, epsilon_s;
  getline(ssopt, delta_s, ';');
  getline(ssopt, epsilon_s, ';');

  double delta = 0;
  try { 
    delta=stod(delta_s); 
  } catch (std::invalid_argument &e) {
    delta=0;
  }
  double epsilon = 0;
  try {
    epsilon=stod(epsilon_s);
  } catch (std::invalid_argument &e) {
    epsilon=0;
  }
  if(delta == 0) delta=0.2;
  if(epsilon == 0) epsilon=0.8;

  //TODO calcul numIterations

  //calcul pivotAC
  const double pivotAC=2*ceil(exp(3./2)*(1+1/epsilon)*(1+1/epsilon));

  std::string cmdline="weightmc --startIteration=0 --gaussuntil=400 --verbosity=0 --pivotAC="+std::to_string(pivotAC)+ " "+filename+" > "+filename+".out";

  int retvalue=system(cmdline.c_str());
  if(retvalue) {
    throw CircuitException("Error executing weightmc");
  }

  //parsing
  std::ifstream ifs((filename+".out").c_str());
  std::string line, prev_line;
  while(getline(ifs,line))
    prev_line=line;

  std::stringstream ss(prev_line);
  std::string result;
  ss >> result >> result >> result >> result >> result;
  
  std::istringstream iss(result);
  std::string val, exp;
  getline(iss, val, 'x');
  getline(iss, exp);
  double value=stod(val);
  exp=exp.substr(2);
  double exponent=stod(exp);
  double ret=value*(pow(2.0,exponent));

  if(unlink(filename.c_str())) {
    throw CircuitException("Error removing "+filename);
  }

  if(unlink((filename+".out").c_str())) {
    throw CircuitException("Error removing "+filename+".out");
  }

  return ret;
}

double BooleanCircuit::independentEvaluationInternal(
    gate_t g, std::set<gate_t> &seen) const
{
  double result=1.;

  switch(getGateType(g)) {
    case BooleanGate::AND:
      for(const auto &c: getWires(g)) {
        result*=independentEvaluationInternal(c, seen);
      }
      break;

    case BooleanGate::OR:
      {
        // We collect probability among each group of children, where we
        // group MULIN gates with the same key var together
        std::map<gate_t, double> groups;
        std::set<gate_t> local_mulins;
        std::set<std::pair<gate_t, unsigned>> mulin_seen;

        for(const auto &c: getWires(g)) {
          auto group = c;
          if(getGateType(c) == BooleanGate::MULIN) {
            group = *getWires(c).begin();
            if(local_mulins.find(g)==local_mulins.end()) {
              if(seen.find(g)!=seen.end())
                throw CircuitException("Not an independent circuit");
              else
                seen.insert(g);
            }
            auto p = std::make_pair(group, getInfo(c));
            if(mulin_seen.find(p)==mulin_seen.end()) {
              groups[group] += getProb(c);
              mulin_seen.insert(p);
            }
          } else 
            groups[group] = independentEvaluationInternal(c, seen);
        }

        for(const auto [k, v]: groups)
          result *= 1-v;
        result = 1-result;
      }
      break;

    case BooleanGate::NOT:
      result=1-independentEvaluationInternal(*getWires(g).begin(), seen);
      break;

    case BooleanGate::IN:
      if(seen.find(g)!=seen.end())
        throw CircuitException("Not an independent circuit");
      seen.insert(g);
      result=getProb(g);
      break;
    
    case BooleanGate::MULIN:
      { 
        auto child = *getWires(g).begin();
        if(seen.find(child)!=seen.end())
          throw CircuitException("Not an independent circuit");
        seen.insert(child);
        result=getProb(g);
      }
      break;

    case BooleanGate::UNDETERMINED:
    case BooleanGate::MULVAR:
      throw CircuitException("Bad gate");
  }

  return result;
}

double BooleanCircuit::independentEvaluation(gate_t g) const
{
  std::set<gate_t> seen;
  return independentEvaluationInternal(g, seen);
}

void BooleanCircuit::setInfo(gate_t g, unsigned int i)
{
  info[g] = i;
}

unsigned BooleanCircuit::getInfo(gate_t g) const
{
  auto it = info.find(g);

  if(it==info.end())
    return 0;
  else
    return it->second;
}

void BooleanCircuit::rewriteMultivaluedGatesRec(
    const std::vector<gate_t> &muls,
    const std::vector<double> &cumulated_probs,
    unsigned start,
    unsigned end,
    std::vector<gate_t> &prefix)
{
  if(start==end) {
    getWires(muls[start]) = prefix;
    return;
  }

  unsigned mid = (start+end)/2;
  auto g = setGate(
      BooleanGate::IN,
      (cumulated_probs[mid+1] - cumulated_probs[start]) / 
      (cumulated_probs[end] - cumulated_probs[start]));
  auto not_g = setGate(BooleanGate::NOT);
  getWires(not_g).push_back(g);

  prefix.push_back(g);
  rewriteMultivaluedGatesRec(muls, cumulated_probs, start, mid, prefix);
  prefix.pop_back();
  prefix.push_back(not_g);
  rewriteMultivaluedGatesRec(muls, cumulated_probs, mid+1, end, prefix);
  prefix.pop_back();
}

static constexpr bool almost_equals(double a, double b)
{
  double diff = a - b;
  constexpr double epsilon = std::numeric_limits<double>::epsilon() * 10;

  return (diff < epsilon && diff > -epsilon);
}

void BooleanCircuit::rewriteMultivaluedGates()
{
  std::map<gate_t,std::vector<gate_t>> var2mulinput;
  for(auto mul: mulinputs) {
    var2mulinput[*getWires(mul).begin()].push_back(mul);
  }
  mulinputs.clear();

  for(const auto &[var, muls]: var2mulinput)
  {
    const unsigned n = muls.size();
    std::vector<double> cumulated_probs(n);
    double cumulated_prob=0.;
    
    for(unsigned i=0; i<n; ++i) {
      cumulated_prob += getProb(muls[i]);
      cumulated_probs[i] = cumulated_prob;
      gates[static_cast<std::underlying_type<gate_t>::type>(muls[i])] = BooleanGate::AND;
      getWires(muls[i]).clear();
    }
      
    std::vector<gate_t> prefix;
    prefix.reserve(static_cast<unsigned>(log(n)/log(2)+2));
    if(!almost_equals(cumulated_probs[n-1],1.)) {
      prefix.push_back(setGate(BooleanGate::IN, cumulated_probs[n-1]));
    }
    rewriteMultivaluedGatesRec(muls, cumulated_probs, 0, n-1, prefix);
  }
}
