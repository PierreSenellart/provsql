extern "C"
{
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(view_circuit);
}

#include "DotCircuit.h"
#include <csignal>
#include <utility>
#include <sstream>
#include <algorithm>

using namespace std;

static vector<pair<int, int> > parse_array(string s)
{
  s = s.substr(1, s.size() - 2); // Remove initial '{' and final '}'

  vector<pair<int, int> > result;

  istringstream iss(s);
  string p;

  while(getline(iss, p, '}'))
  {
    if(!p.empty() && p[0]==',')
      p=p.substr(1);

    int k = p.find(",", 1);
    string s1 = p.substr(1, k - 1);
    int i1;
    if (s1 == "NULL")
      i1 = 0;
    else
      i1 = stoi(p.substr(1, k - 1));
    int i2 = stoi(p.substr(k + 1, p.size() - k));
    result.push_back(make_pair(i1, i2));
  }

  return result;
}

static std::string view_circuit_internal(Datum token, Datum token2prob, Datum is_debug)
{
  Datum arguments[2]={token,token2prob};
  constants_t constants=initialize_constants(true);
  Oid argtypes[2]={constants.OID_TYPE_UUID,REGCLASSOID};
  char nulls[2] = {' ',' '};

  SPI_connect();

  DotCircuit c;

  int proc = 0;

  if (SPI_execute_with_args(
        "SELECT * FROM provsql.sub_circuit_with_desc($1,$2)", 2, argtypes, arguments, nulls, true, 0) == SPI_OK_SELECT)
  {
    proc = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;

    for (int i = 0; i < proc; i++)
    {
      HeapTuple tuple = tuptable->vals[i];

      string f = SPI_getvalue(tuple, tupdesc, 1);
      string type = SPI_getvalue(tuple, tupdesc, 3);
      if (type == "input")
      {
        c.setGate(f, DotGate::IN, SPI_getvalue(tuple, tupdesc, 4));

      } else {
        auto id=c.getGate(f);

        if (type == "times")
        {
          c.setGate(f, DotGate::OTIMES);
        }
        else if (type == "plus")
        {
          c.setGate(f, DotGate::OPLUS);
        }
        else if (type == "monus")
        {
          c.setGate(f, DotGate::OMINUS);
        }
        else if (type == "monusr")
        {
          c.setGate(f, DotGate::OMINUSR);
        }
        else if (type == "monusl")
        {
          c.setGate(f, DotGate::OMINUSL);
        }
        else if (type == "delta")
        {
          c.setGate(f, DotGate::DELTA);
        }
        else if (type == "eq")
        {
          vector<pair<int, int> > v = parse_array(SPI_getvalue(tuple, tupdesc, 5));
          if (v.size() != 1)
            elog(ERROR, "Incorrect extra information on eq gate");
          std::string cond = std::to_string(v[0].first) + std::string("=") + std::to_string(v[0].second);
          c.setGate(f, DotGate::EQ, cond);
        }
        else if (type == "project")
        {
          vector<pair<int, int> > v = parse_array(SPI_getvalue(tuple, tupdesc, 5));
          sort(v.begin(), v.end(), [](auto &left, auto &right) {
            return left.second < right.second;
          });
          std::string cond("(");
          for (auto p : v)
          {
            cond += std::to_string(p.first) + ",";
          }
          std::string cond_final(cond.substr(0, cond.size() - 1) + ")");
          //elog(WARNING, "PROJECT cond: %s", cond_final.c_str());
          c.setGate(f, DotGate::PROJECT, cond_final);
        }
        else
        {
          elog(ERROR, "Wrong type of gate in circuit");
        }
        //elog(WARNING, "%d -- %d", id, c.getGate(SPI_getvalue(tuple, tupdesc, 2)));
        c.addWire(id, c.getGate(SPI_getvalue(tuple, tupdesc, 2)));
      }
    }
  }

  SPI_finish();

  // Display the circuit for debugging:
  int display = DatumGetInt64(is_debug);
  if(display)
    elog(WARNING, "%s", c.toString(gate_t{0}).c_str());

  //Calling the dot renderer
  return c.render();
}

Datum view_circuit(PG_FUNCTION_ARGS)
{
  try
  {
    Datum token = PG_GETARG_DATUM(0);
    Datum token2prob = PG_GETARG_DATUM(1);
    Datum is_debug = PG_GETARG_DATUM(2);

    if (PG_ARGISNULL(1))
      PG_RETURN_NULL();

    std::string s = view_circuit_internal(token, token2prob, is_debug);

    text *result = (text *) palloc(VARHDRSZ + s.size() + 1);
    SET_VARSIZE(result, VARHDRSZ + s.size());

    memcpy((void *) VARDATA(result),
           s.c_str(),
           s.size());
    PG_RETURN_TEXT_P(result);
  } catch(const std::exception &e) {
    elog(ERROR, "view_circuit: %s", e.what());
  }
  catch (...)
  {
    elog(ERROR, "view_circuit: Unknown exception");
  }

  PG_RETURN_NULL();
}
