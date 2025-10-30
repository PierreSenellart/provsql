extern "C"
{
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

PG_FUNCTION_INFO_V1(to_provxml);
}

#include <set>
#include <csignal>
#include <utility>
#include <sstream>
#include <algorithm>

#include "provenance_evaluate_compiled.hpp"

using namespace std;

static string xmlEscape(const std::string& input) {
  string output;
  output.reserve(input.size());
  for (char c : input) {
    switch (c) {
    case '&':  output += "&amp;";  break;
    case '<':  output += "&lt;";   break;
    default:   output += c;        break;
    }
  }
  return output;
}

/* From provenance_evaluate_compiled.cpp */
extern const char *drop_temp_table;
bool join_with_temp_uuids(Oid table, const std::vector<std::string> &uuids);

static string to_provxml_internal(Datum tokenDatum, Datum table)
{
  pg_uuid_t token = *DatumGetUUIDP(tokenDatum);

  stringstream ss;

  ss << "<?xml version='1.0' encoding='UTF-8'?>\n";
  ss << "<prov:document xmlns:prov='http://www.w3.org/ns/prov#' xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance' xmlns:xsd='http://www.w3.org/2001/XMLSchema' xmlns:provsql='https://provsql.org/'>\n";

  GenericCircuit c = getGenericCircuit(token);
  auto root = c.getGate(uuid2string(token));

  std::unordered_map<gate_t, std::string> provenance_mapping;
  if(table) {
    auto inputs = c.getInputs();
    std::vector<std::string> inputs_uuid;
    std::transform(inputs.begin(), inputs.end(), std::back_inserter(inputs_uuid), [&c](auto x) {
      return c.getUUID(x);
    });
    bool drop_table = join_with_temp_uuids(table, inputs_uuid);
    constants_t constants = get_constants(true);
    initialize_provenance_mapping<std::string>(constants, c, provenance_mapping, [](const char *v) {
      return std::string(v);
    }, drop_table);
  }

  std::set<gate_t> to_process { root };
  std::set<gate_t> processed;

  while(!to_process.empty()) {
    auto g = *to_process.begin();
    to_process.erase(to_process.begin());
    auto uuid = c.getUUID(g);
    gate_type type = c.getGateType(g);
    processed.insert(g);

    ss << "  <prov:entity prov:id='provsql:" + uuid + "'>\n";
    ss << "    <prov:type xsi:type='xsd:QName'>provsql:" + std::string(gate_type_name[type]) + "</prov:type>\n";
    if (type == gate_input && table && provenance_mapping.find(g)!=provenance_mapping.end()) {
      ss << "    <prov:value>" + xmlEscape(provenance_mapping[g]) + "</prov:value>\n";
    }
    ss << "  </prov:entity>\n";

    bool first=true;
    for(auto h: c.getWires(g)) {
      ss << "  <prov:wasDerivedFrom>\n";
      ss << "    <prov:generatedEntity prov:ref='provsql:" + uuid + "' />\n";
      ss << "    <prov:usedEntity prov:ref='provsql:" + c.getUUID(h) + "' />\n";
      if(type == gate_monus) {
        if(first)
          ss << "    <prov:label>left</prov:label>\n";
        else
          ss << "    <prov:label>right</prov:label>\n";
      }

      ss << "  </prov:wasDerivedFrom>\n";

      if(processed.find(h) == processed.end())
        to_process.insert(h);

      first=false;
    }
  }

  ss << "</prov:document>\n";

  return ss.str();
}

Datum to_provxml(PG_FUNCTION_ARGS)
{
  try
  {
    Datum token = PG_GETARG_DATUM(0);
    Datum table = PG_GETARG_DATUM(1);

    std::string s = to_provxml_internal(token, table);

    text *result = (text *) palloc(VARHDRSZ + s.size() + 1);
    SET_VARSIZE(result, VARHDRSZ + s.size());

    memcpy((void *) VARDATA(result),
           s.c_str(),
           s.size());
    PG_RETURN_TEXT_P(result);
  } catch(const exception &e) {
    elog(ERROR, "to_provxml: %s", e.what());
  }
  catch (...)
  {
    elog(ERROR, "to_provxml: Unknown exception");
  }

  PG_RETURN_NULL();
}
