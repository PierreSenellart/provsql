#include "DotCircuit.h"

extern "C" {
#include "provsql_utils.h"
#include <unistd.h>
}

#include <cassert>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>


// Has to be redefined because of name hiding
unsigned DotCircuit::setGate(const uuid &u, DotGate type)
{
 unsigned id = Circuit::setGate(u, type);
  if(type == DotGate::IN)
    inputs.insert(id);
  return id;
}

unsigned DotCircuit::setGate(const uuid &u, DotGate type, std::string d)
{
  unsigned id = setGate(u, type);
  desc[id] = d;
  return id;
}

unsigned DotCircuit::addGate()
{
  unsigned id=Circuit::addGate();
  desc.push_back("");
  return id;
}

//Outputs the gate in Graphviz dot format
std::string DotCircuit::toString(unsigned ) const
{
  std::string op;
  std::string result="graph circuit{\n node [shape=plaintext];\n";
  
  //looping through the gates
  //eliminating the minusr and minusl gates
  unsigned i=0;
  for(auto g:gates){
    if(g != DotGate::OMINUSR && g != DotGate::OMINUSL){
      result += std::to_string(i)+" [label=";
      switch(g) {
        case DotGate::IN:
          result+="\""+desc[i]+"\"";
          break;
        case DotGate::OMINUS:
          result+="\"⊖\"";
          break;
        case DotGate::UNDETERMINED:
          result+="\"?\"";
          break;
        case DotGate::OTIMES:
          result+="\"⊗\"";
          break;
        case DotGate::OPLUS:
          result+="\"⊕\"";
          break;
        case DotGate::EQ:
          result+="\""+desc[i]+"\"";
          break;
        case DotGate::PROJECT:
          result+="\"Π"+desc[i]+"\"";
          break;
        case DotGate::OMINUSR:
        case DotGate::OMINUSL:
          break;
      }
      result+="];\n";
    }
    i++;
  }

  //looping through the gates and their wires
  for(size_t i=0;i<wires.size();++i){
    if(gates[i] != DotGate::OMINUSR && gates[i] != DotGate::OMINUSL){
      std::unordered_map<unsigned, unsigned> number_gates;
      for(auto s: wires[i]){
        if(number_gates.find(s)!=number_gates.end()){
          number_gates[s] = number_gates[s]+1;
        }
        else {
          number_gates[s] = 1;
        }
      }
      for(auto el: number_gates)
      {
        unsigned s = el.first;
        unsigned n = el.second;
        if(gates[s] == DotGate::OMINUSR || gates[s] == DotGate::OMINUSL) {
          for(auto t: wires[s]) {
            result += std::to_string(i)+" -- "+std::to_string(t);
            if(gates[s] == DotGate::OMINUSR)
              result += " [label=\"R\"];\n";
            else
              result += " [label=\"L\"];\n";
          }
        }
        else {
          result += std::to_string(i)+" -- "+std::to_string(s);
          if(n==1) {
            result += ";\n";
          }
          else {
            result += " [label=\""+std::to_string(n)+"\"];\n";
          }
        }
      }
    }
  }
  return result+"}";
}

void DotCircuit::render() const {
  //Writing dot to a temporary file
  int fd;
  char cfilename[] = "/tmp/provsqlXXXXXX";
  fd = mkstemp(cfilename);
  close(fd);
  std::string filename=cfilename, outfilename=filename+".pdf";

  std::ofstream ofs(filename.c_str());
  ofs << toString(0);
  ofs.close();

  //Executing the Graphviz dot renderer
  std::string cmdline="dot -Tpdf "+filename+" -o "+outfilename;

  int retvalue=system(cmdline.c_str());

  if(retvalue)    
    throw CircuitException("Error executing Graphviz dot"); 

  //Opening the PDF viewer
#ifdef __linux__
  //assuming evince on linux
  cmdline="export DISPLAY=':0'; evince "+outfilename + " &";
  retvalue=system(cmdline.c_str());
#else
  throw CircuitException("Unsupported operating system for viewing");
#endif
}
