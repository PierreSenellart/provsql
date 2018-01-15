extern "C" {

#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "utils/builtins.h"

#include "provsql_utils.h"
  
  PG_FUNCTION_INFO_V1(where_provenance);
}

#include <algorithm>
#include <utility>
#include <regex>
#include <sstream>

#include "WhereCircuit.h"
#include "provsql_utils_cpp.h"

using namespace std;

static vector<pair<int,int>> parse_array(string s)
{
  s=s.substr(1,s.size()-2); // Remove initial '{' and final '}'

  vector<pair<int,int>> result;
  regex reg("},?");

  sregex_token_iterator iter(s.begin(), s.end(), reg, -1);
  sregex_token_iterator end;

  for(sregex_token_iterator iter(s.begin(), s.end(), reg, -1), end; iter!=end; ++iter) {
    string p = *iter;
    int k=p.find(",",1);
    string s1=p.substr(1,k-1);
    int i1;
    if(s1=="NULL")
      i1=0;
    else
      i1=stoi(p.substr(1,k-1));
    int i2=stoi(p.substr(k+1,p.size()-k));
    result.push_back(make_pair(i1,i2));
  }

  return result;
}

static string where_provenance_internal
  (Datum token)
{
  constants_t constants;
  if(!initialize_constants(&constants)) {
    elog(ERROR, "Cannot find provsql schema");
  }

  Datum arguments[1]={token};
  Oid argtypes[1]={constants.OID_TYPE_PROVENANCE_TOKEN};
  char nulls[1] = {' '};
  
  SPI_connect();

  WhereCircuit c;

  if(SPI_execute_with_args(
      "SELECT * FROM provsql.sub_circuit_for_where($1)", 2, argtypes, arguments, nulls, true, 0)
      == SPI_OK_SELECT) {
    int proc = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;

    for (int i = 0; i < proc; i++)
    {
      HeapTuple tuple = tuptable->vals[i];

      string f = SPI_getvalue(tuple, tupdesc, 1);
      string type = SPI_getvalue(tuple, tupdesc, 3);
      if(type == "input") {
        string table = SPI_getvalue(tuple, tupdesc, 4);
        int nb_columns = stoi(SPI_getvalue(tuple, tupdesc, 5));

        c.setGateInput(f, table, nb_columns);
      } else {
        unsigned id=c.getGate(f);

        if(type == "times") {
          c.setGate(f, WhereGate::TIMES);
        } else if(type == "plus") {
          c.setGate(f, WhereGate::PLUS);
        } else if(type == "project" || type == "eq") {
          vector<pair<int,int>> v = parse_array(SPI_getvalue(tuple, tupdesc, 6));
          if(type=="eq") {
            if(v.size()!=1)
              elog(ERROR, "Incorrect extra information on eq gate");
            c.setGateEquality(f, v[0].first, v[0].second);
          } else {
            sort(v.begin(), v.end(), [](auto &left, auto &right) {
                return left.second < right.second;
                });
            vector<int> infos;
            for(auto p : v) {
              infos.push_back(p.first);
            }
            c.setGateProjection(f, move(infos));
          }
        } else if(type == "monusr" || type == "monusl" || type == "monus") {
          elog(ERROR, "Where-provenance of non-monotone query not supported");
        } else {
          elog(ERROR, "Wrong type of gate in circuit");
        }
        c.addWire(id, c.getGate(SPI_getvalue(tuple, tupdesc, 2)));
      }
    }
  } else {
    elog(ERROR, "SPI_execute_with_args failed on provsql.sub_circuit_for_where");
  }

  SPI_finish();
  
  unsigned gate = c.getGate(UUIDDatum2string(token));

  vector<set<WhereCircuit::Locator>> v = c.evaluate(gate);

  ostringstream os;
  os << "{";
  bool ofirst=true;
  for(auto s : v) {
    if(!ofirst)
      os << ",";
    os << "[";
    bool ifirst=true;
    for(auto l : s) {
      if(!ifirst)
        os << ";";
      os << l.toString();
      ifirst=false;
    }
    os << "]";
    ofirst=false;
  }
  os << "}";

  return os.str();
}

Datum where_provenance(PG_FUNCTION_ARGS)
{
  try {
    Datum token = PG_GETARG_DATUM(0);

    PG_RETURN_TEXT_P(cstring_to_text(where_provenance_internal(token).c_str()));
  } catch(const std::exception &e) {
    elog(ERROR, "where_provenance: %s", e.what());
  } catch(...) {
    elog(ERROR, "where_provenance: Unknown exception");
  }
}
