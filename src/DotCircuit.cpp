#include "DotCircuit.h"
#include <type_traits>

extern "C"
{
#include "provsql_utils.h"
#include <unistd.h>
}

#include <cassert>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>

// Has to be redefined because of name hiding
gate_t DotCircuit::setGate(const uuid &u, DotGate type)
{
 auto id = Circuit::setGate(u, type);
  if(type == DotGate::IN)
    inputs.insert(id);
  return id;
}

gate_t DotCircuit::setGate(const uuid &u, DotGate type, std::string d)
{
  auto id = setGate(u, type);
  desc[static_cast<std::underlying_type<gate_t>::type>(id)] = d;
  return id;
}

gate_t DotCircuit::addGate()
{
  auto id=Circuit::addGate();
  desc.push_back("");
  return id;
}

//Outputs the gate in Graphviz dot format
std::string DotCircuit::toString(gate_t) const
{
  std::string op;
  std::string result="digraph circuit{\n graph [rankdir=UD] ;\n";
  
  //looping through the gates
  //eliminating the minusr and minusl gates
  unsigned i = 0;
  for (auto g : gates)
  {
    if (g != DotGate::OMINUSR && g != DotGate::OMINUSL)
    {
      result += std::to_string(i) + " [label=";
      switch (g)
      {
      case DotGate::IN:
        result += "\"" + desc[i] + "\"";
        break;
      case DotGate::OMINUS:
        result += "\"⊖\"";
        break;
      case DotGate::UNDETERMINED:
        result += "\"?\"";
        break;
      case DotGate::OTIMES:
        result += "\"⊗\"";
        break;
      case DotGate::OPLUS:
        result += "\"⊕\"";
        break;
      case DotGate::DELTA:
        result += "\"δ\"";
        break;
      case DotGate::EQ:
        result += "\"" + desc[i] + "\"";
        break;
      case DotGate::PROJECT:
        result += "\"Π" + desc[i] + "\"";
        break;
      case DotGate::OMINUSR:
      case DotGate::OMINUSL:
        break;
      }
      if(i==0)
        result+=",shape=\"double\"";
      result+="];\n";
    }
    i++;
  }

  //looping through the gates and their wires
  for(size_t i=0;i<wires.size();++i){
    if(gates[i] != DotGate::OMINUSR && gates[i] != DotGate::OMINUSL){
      std::unordered_map<gate_t, unsigned> number_gates;
      for(auto s: wires[i]){
        if(number_gates.find(s)!=number_gates.end()){
          number_gates[s] = number_gates[s]+1;
        }
        else
        {
          number_gates[s] = 1;
        }
      }
      for(auto [s,n]: number_gates)
      {
        if(getGateType(s) == DotGate::OMINUSR || getGateType(s) == DotGate::OMINUSL) {
          for(auto t: getWires(s)) {
            result += std::to_string(i)+" -> "+to_string(t);
            if(getGateType(s) == DotGate::OMINUSR)
              result += " [label=\"R\"];\n";
            else
              result += " [label=\"L\"];\n";
          }
        }
        else {
          result += std::to_string(i)+" -> "+to_string(s);
          if(n==1) {
            result += ";\n";
          }
          else
          {
            result += " [label=\"" + std::to_string(n) + "\"];\n";
          }
        }
      }
    }
  }
  return result + "}";
}

std::string DotCircuit::render() const {
  //Writing dot to a temporary file
  int fd;
  char cfilename[] = "/tmp/provsqlXXXXXX";
  fd = mkstemp(cfilename);
  close(fd);
  std::string filename=cfilename, outfilename=filename+".out";

  std::ofstream ofs(filename.c_str());
  ofs << toString(gate_t{0});
  ofs.close();

  //Executing the Graphviz dot renderer through graph-easy for ASCII
  //output
  std::string cmdline="graph-easy --as=boxart --output="+outfilename+" "+filename;

  int retvalue = system(cmdline.c_str());
  
  if(provsql_verbose<20) {
    if(unlink(filename.c_str())) {
      throw CircuitException("Error removing "+filename);
    }
  }

  if(retvalue)    
    throw CircuitException("Error executing graph-easy"); 

  std::ifstream ifs(outfilename.c_str());
  std::string str((std::istreambuf_iterator<char>(ifs)),
                  std::istreambuf_iterator<char>());
  
  if(provsql_verbose<20) {
    if(unlink(outfilename.c_str())) {
      throw CircuitException("Error removing "+outfilename);
    }
  }
  
  return str;
}
