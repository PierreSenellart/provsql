#include "WhereCircuit.h"

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

unsigned WhereCircuit::setGate(const uuid &u, WhereGate type)
{
  return Circuit::setGate(u, type);
}

unsigned WhereCircuit::setGateProjection(const uuid &u, vector<int> &&infos)
{
  unsigned id = setGate(u, WhereGate::PROJECT);
  projection_info[id]=infos;
  return id;
}
  
unsigned WhereCircuit::setGateEquality(const uuid &u, int pos1, int pos2)
{
  unsigned id = setGate(u, WhereGate::EQ);
  equality_info[id]=make_pair(pos1,pos2);
  return id;
}

unsigned WhereCircuit::setGateInput(const uuid &u, string table, int nb_columns)
{
  unsigned id = setGate(u, WhereGate::IN);
  input_token[id]=u;
  input_info[id]=make_pair(table, nb_columns);
  return id;
}

string WhereCircuit::toString(unsigned g) const
{
  std::string op;
  string result;

  switch(gates[g]) {
    case WhereGate::IN:
      return input_info.find(g)->second.first+":"+to_string(input_info.find(g)->second.second)+":"+input_token.find(g)->second;
    case WhereGate::UNDETERMINED:
      op="?";
      break;
    case WhereGate::TIMES:
      op="⊗";
      break;
    case WhereGate::PLUS:
      op="⊕";
      break;
    case WhereGate::PROJECT:
      op="Π[";
      {
        bool first=true;
        for(auto i : projection_info.find(g)->second) {
          if(!first)
            op+=",";
          op+=to_string(i);
          first=false;
        }
      }
      op+="]";
      break;
    case WhereGate::EQ:
      op="=["+to_string(equality_info.find(g)->second.first)+","+to_string(equality_info.find(g)->second.second)+"]";  
  }

  for(auto s: wires[g]) {
    if(gates[g]==WhereGate::PROJECT || gates[g]==WhereGate::EQ)
      result = op;
    else if(!result.empty())
      result+=" "+op+" ";
    result+=toString(s);
  }

  return "("+result+")";
}
  
vector<set<WhereCircuit::Locator>> WhereCircuit::evaluate(unsigned g) const
{
  vector<set<Locator>> v;

  switch(gates[g]) {
    case WhereGate::IN:
      {
        string table=input_info.find(g)->second.first;
        uuid tid=input_token.find(g)->second;
        int nb_columns=input_info.find(g)->second.second;
        for(int i=0;i<nb_columns;++i) {
          set<Locator> s;
          s.insert(Locator(table,tid,i+1));
          v.push_back(s);
        }
      }
      break;

    case WhereGate::TIMES:
      if(wires[g].empty())
        throw CircuitException("No wire connected to ⊗ gate");

      for(auto g2 : wires[g]) {
        if(v.empty())
          v=evaluate(g2);
        else {
          vector<set<Locator>> w=evaluate(g2);
          v.insert(v.end(), w.begin(), w.end());
        }
      }
      break;

    case WhereGate::PLUS:
      if(wires[g].empty())
        throw CircuitException("No wire connected to ⊕ gate");

      for(auto g2 : wires[g]) {
        if(v.empty())
          v=evaluate(g2);
        else {
          vector<set<Locator>> w=evaluate(g2);
          if(w.size()!=v.size())
            throw CircuitException("Incompatible inputs for ⊕ gate");

          for(size_t k=0;k<v.size();++k) {
            v[k].insert(w[k].begin(), w[k].end());
          }
        }
      }
      break;

    case WhereGate::PROJECT:
      if(wires[g].size()!=1)
        throw CircuitException("Not exactly one wire connected to Π gate");

      {
        vector<set<Locator>> w=evaluate(*wires[g].begin());
        vector<int> positions=projection_info.find(g)->second;
        for(auto i : positions) {
          if(i==0)
            v.push_back(set<Locator>());
          else
            v.push_back(w[i-1]);
        }
      }
      break;

    case WhereGate::EQ:
      if(wires[g].size()!=1)
        throw CircuitException("Not exactly one wire connected to = gate");

      v=evaluate(*wires[g].begin());
      {
        pair<int,int> positions=equality_info.find(g)->second;
        v[positions.first-1].insert(v[positions.second-1].begin(), v[positions.second-1].end());
        v[positions.second-1].insert(v[positions.first-1].begin(), v[positions.first-1].end());
      }
      break;

    default:
      throw CircuitException("Wrong type of gate");
  }

  return v;
}
    
bool WhereCircuit::Locator::operator<(WhereCircuit::Locator that) const
{
  if(this->table<that.table)
    return true;
  if(this->tid<that.tid)
    return true;
  return this->position<that.position;
}

std::string WhereCircuit::Locator::toString() const
{
  return table + ":" + tid + ":" +to_string(position);
}
