extern "C"
{
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"
#include "storage/fd.h"
#include "math.h"


  PG_FUNCTION_INFO_V1(dump_data);
}
#include <string>
#include <map>
#include <fstream>
#include <unordered_map>
#include <tuple>
#include <vector>
#include <cassert>
#include "provsql_utils_cpp.h"

char* print_shared_state_constants(constants_t &constants, char* buffer)
{
  sprintf(buffer, "Constants :\n OID_SCHEMA_PROVSQL = %d \n"
  "OID_TYPE_PROVENANCE_TOKEN = %d\n"
  "OID_TYPE_GATE_TYPE = %d\n"
  "OID_TYPE_AGG_TOKEN = %d\n"
  "OID_TYPE_UUID = %d\n"
  "OID_TYPE_UUID_ARRAY = %d\n"
  "OID_TYPE_INT = %d\n"
  "OID_TYPE_INT_ARRAY = %d\n"
  "OID_TYPE_VARCHAR = %d\n"
  "OID_FUNCTION_ARRAY_AGG = %d\n"
  "OID_FUNCTION_PROVENANCE_PLUS = %d\n"
  "OID_FUNCTION_PROVENANCE_TIMES = %d\n"
  "OID_FUNCTION_PROVENANCE_MONUS = %d\n"
  "OID_FUNCTION_PROVENANCE_PROJECT = %d\n"
  "OID_FUNCTION_PROVENANCE_EQ = %d\n"
  "OID_FUNCTION_PROVENANCE = %d\n"
  "GATE_TYPE_TO_OID[nb_gate_types] = %p\n"
  "OID_FUNCTION_PROVENANCE_DELTA = %d\n"
  "OID_FUNCTION_PROVENANCE_AGGREGATE = %d\n"
  "OID_FUNCTION_PROVENANCE_SEMIMOD = %d\n"
  "OID_FUNCTION_GATE_ZERO = %d \n"
  "OID_OPERATOR_NOT_EQUAL_UUID = %d \n"
  "OID_FUNCTION_NOT_EQUAL_UUID = %d"
  ,
  constants.OID_SCHEMA_PROVSQL,
  constants.OID_TYPE_PROVENANCE_TOKEN,
  constants.OID_TYPE_GATE_TYPE,
  constants.OID_TYPE_AGG_TOKEN,
  constants.OID_TYPE_UUID,
  constants.OID_TYPE_UUID_ARRAY,
  constants.OID_TYPE_INT,
  constants.OID_TYPE_INT_ARRAY,
  constants.OID_TYPE_VARCHAR,
  constants.OID_FUNCTION_ARRAY_AGG,
  constants.OID_FUNCTION_PROVENANCE_PLUS,
  constants.OID_FUNCTION_PROVENANCE_TIMES,
  constants.OID_FUNCTION_PROVENANCE_MONUS,
  constants.OID_FUNCTION_PROVENANCE_PROJECT,
  constants.OID_FUNCTION_PROVENANCE_EQ,
  constants.OID_FUNCTION_PROVENANCE,
  constants.GATE_TYPE_TO_OID,
  constants.OID_FUNCTION_PROVENANCE_DELTA,
  constants.OID_FUNCTION_PROVENANCE_AGGREGATE,
  constants.OID_FUNCTION_PROVENANCE_SEMIMOD,
  constants.OID_FUNCTION_GATE_ZERO,
  constants.OID_OPERATOR_NOT_EQUAL_UUID,
  constants.OID_FUNCTION_NOT_EQUAL_UUID

  );

  return buffer;
}


char* print_hash_entry(provsqlHashEntry* hash, char* buffer)
{
  sprintf(buffer, "Hash :\n"
  //"Key = %16u \n"
  "Type = %d \n"
  "nb_children = %u \n"
  "children_idx = %u \n"
  "prob = %f\n"
  "info1 = %u\n"
  "info2 = %u\n"
  ,
  //&hash->key,
  *&hash->type,
  *&hash->nb_children,
  *&hash->children_idx,
  *&hash->prob,
  *&hash->info1,
  *&hash->info2
  );

  return buffer;
}


int provsql_serialize(const char* filename)
{
  FILE *file;
  int32 num_entries;
  provsqlHashEntry *entry;
  HASH_SEQ_STATUS hash_seq;

  file = AllocateFile(filename, PG_BINARY_W);
  if (file == NULL)
  {
    return 1;
  }

  num_entries = hash_get_num_entries(provsql_hash);
  hash_seq_init(&hash_seq, provsql_hash);


  if(! fwrite(&num_entries, sizeof(int32), 1, file)){
    if (FreeFile(file))
    {
      file = NULL;
      return 4;
    }
    return 2;
  }

  while ( (entry = (provsqlHashEntry*)hash_seq_search(&hash_seq) )  != NULL )
  {
    if (!fwrite(entry, sizeof(provsqlHashEntry), 1, file))
    {
      if (FreeFile(file))
      {
       file = NULL;
       return 4;
      }
      return 2;
    }

  }


  if ( !fwrite( &(provsql_shared_state->nb_wires), sizeof(unsigned int), 1, file ))
  {
    if (FreeFile(file))
    {
      file = NULL;
      return 4;
    }
    return 1;
  }
  if (provsql_shared_state->nb_wires  > 0)
  {

    if (!fwrite( &(provsql_shared_state->wires), sizeof(pg_uuid_t)*  (unsigned long int)(provsql_shared_state->nb_wires), 1, file))
    {
     if (FreeFile(file))
     {
       file = NULL;
       return 4;
     }
     return 2;
    }

  }

  if (FreeFile(file))
  {
    file = NULL;
    return 3;
  }

  //TODO locks

  return 0;

}


int provsql_deserialize(const char* filename)
{
  FILE *file;
  int32 num;
  provsqlHashEntry tmp;
  provsqlHashEntry *entry;
  bool found;

  file = AllocateFile(filename, PG_BINARY_R);
  if (file == NULL)
  {
    return 1;
  }


  if (!fread(&num, sizeof(int32),1,file))
  {
    return 2;
  }

  for (int i = 0; i < num; i++)
  {
    if (!fread(&tmp, sizeof(provsqlHashEntry), 1, file))
    {
      return 2;
    }

    // Deleting the entry if it already exists is important, otherwhise the HASH_ENTER will just ignore it and we will be stuck with the curent value, even if the serialized one was different.
    hash_search(provsql_hash, &(tmp.key), HASH_REMOVE, &found);
    entry = (provsqlHashEntry *) hash_search(provsql_hash, &(tmp.key), HASH_ENTER, &found);

    if (!found)
    {
      *entry = tmp;
    }


  }

  if (! fread(&provsql_shared_state->nb_wires, sizeof(unsigned int), 1, file ))
  {
    return 2;
  }

  if (provsql_shared_state->nb_wires  > 0) {
    if (! fread(&provsql_shared_state->wires, sizeof(pg_uuid_t),(unsigned long int) (provsql_shared_state->nb_wires), file))
    {
      return 2;
    }
  }


  if (FreeFile(file))
  {
    file = NULL;
    return 3;
  }

  return 0;

}

Datum dump_data(PG_FUNCTION_ARGS)
{


  switch (provsql_serialize("provsql_test.tmp")) //TODO Find a better name for the file
  {
  case 0:
    elog(INFO,"serializing completed without error");
    break;

  case 1:
    elog(INFO, "Error while opening the file during serialization");
    break;

  case 2:
    elog(INFO, "Error while writing to the file during serialization");
    break;

  case 3:
    elog(INFO, "Error while closing the file during serialization");
    break;

  case 4:
    elog(INFO, "Error while closing the file when a writing error happened");
    break;

  }


  PG_RETURN_NULL();
}



extern "C"
{
  PG_FUNCTION_INFO_V1(read_data_dump);
}

Datum read_data_dump(PG_FUNCTION_ARGS){

  switch(provsql_deserialize("provsql_test.tmp"))
  {
    case 0:
    elog(INFO,"deserialization completed without error");
    break;

  case 1:
    elog(INFO, "Error while opening the file during deserialization");
    break;

  case 2:
    elog(INFO, "Error while reading the file during deserialization");
    break;

  case 3:
    elog(INFO, "Error while closing the file during deserialization");
    break;

  }

  PG_RETURN_NULL();
}


//TODO The files are curently written in /var/lib/postgresql/12/main/noncnfFromCircuit.noncnf, which requires some permissions in order to fetch.
//     It would be nice if the file was created directly in the current directory or in a specified path and/or file
int circuit_to_noncnf_internal(Datum token){
  static int file_id = 0;
  Datum arguments[1]= {token};
  const constants_t constants = initialize_constants(true);
  Oid argtypes[1]= {constants.OID_TYPE_PROVENANCE_TOKEN};
  char nulls[1] = {' '};
  int proc = 0;
  SPI_connect();
  if (SPI_execute_with_args(
          "SELECT * FROM provsql.sub_circuit_without_desc($1)",
          2,argtypes,arguments,nulls, true, 0
  ) == SPI_OK_SELECT)
  {
    std::string filename = "./noncnfFromCircuit" + std::to_string(file_id++)+".noncnf";
    std::ofstream of(filename.c_str());
    assert(of);

    //TODO: change the type in order to reduce the memory used : replace uuid in string form to pgu_uuid_t form;
    // maybe replace the input type string to a more compact form. There are less than 16 different type, so an int8 would already be more than enough. Maybe an Enum for clarity

    //this map is defined as follows :
    // the key is an UID
    // the value linked to the key is a tuple constitued of :
    // - an Integer, used to represent the variable this UID is linked to in the cnf
    // - a String, that receives the gate type of the current UID (with
    // "input" being considered a gate type) -- TODO: make it a char
    // - A vector that contains the UID of the inputs of the current gate. Possibly empty. Those UIDs can be used as Keys for this map
    //TODO: the first item of the value doesn't seem to be useful
    std::unordered_map  <int,   std::tuple<std::string, std::vector<int>> > compactm;

    //these maps are mapping integers to pg_uuid_t because pg_uuid_t are too big, which leads to Out of Memory issues when compiling big circuits.
    //TODO: this should really be a vector
    std::unordered_map<int,std::string> mapOfUUID;
    std::unordered_map<std::string, int> mapOfUUID2;
    int current_pg_uuid_id = 1;

    proc = SPI_processed;
    TupleDesc tupdesc = SPI_tuptable->tupdesc;
    SPITupleTable *tuptable = SPI_tuptable;

    for (int i = 0; i < proc; i++)
    {
      HeapTuple tuple = tuptable->vals[i];
      std::string gates[2];      // from and to
      int gates_var[2] = {-1, -1};
      std::string type;

      if (SPI_getvalue(tuple,tupdesc,1) != NULL)
        gates[0] = SPI_getvalue(tuple,tupdesc,1);

      if (SPI_getvalue(tuple,tupdesc,2) != NULL)
        gates[1] = SPI_getvalue(tuple,tupdesc,2);

      if (SPI_getvalue(tuple,tupdesc,3) != NULL)
        type = SPI_getvalue(tuple,tupdesc,3);

      assert(gates[0] != "");

      for(unsigned k=0;k<2;++k) {
        if(gates[k].empty())
          continue;

        auto it = mapOfUUID2.find(gates[k]);
        if (it == mapOfUUID2.end())
        {
          gates_var[k] = current_pg_uuid_id;

          mapOfUUID[current_pg_uuid_id]=gates[k];
          mapOfUUID2[gates[k]]=current_pg_uuid_id;

          compactm[current_pg_uuid_id]=std::make_tuple(type,std::vector<int>());
          current_pg_uuid_id++;
        } else {
          gates_var[k] = it->second;
        }
      }

      if(gates_var[1] != -1)
        std::get<1>(compactm.find(gates_var[0])->second).push_back(gates_var[1]);
    }

    of << "p noncnf ";
    of << (current_pg_uuid_id-1) << "\n";

    for (auto iter = compactm.begin(); iter != compactm.end(); ++iter)
    {
      std::string s = std::get<0>(iter->second);
      if (s == "plus")
      {
        of << "6 -1 ";
      } else if (s == "times"){
        of << "4 -1 ";
      } else if (s == "input") {
        of << "c p weight ";
      } else {
        assert(false);
        // TODO: write a monus b as a AND NOT b
      }

      of << std::to_string(iter->first) + " "; //write the gate variable

      if(s != "input") {
        for (auto a :  std::get<1>(iter->second) )
          of << a << " "; //write the gate's input variables
        of << "0\n";
      } else {
        double prob = 1.;
        bool found;
        LWLockAcquire(provsql_shared_state->lock, LW_SHARED);
        auto tmp = mapOfUUID.find(iter->first)->second;
        pg_uuid_t token = string2uuid(tmp);
        provsqlHashEntry* entry = (provsqlHashEntry *) hash_search(provsql_hash, &token, HASH_FIND, &found);
        if(found){
          prob = entry->prob;
        }
        LWLockRelease(provsql_shared_state->lock);
        of << prob << "\n";
      }
    }

    SPI_finish();

    of.close();
  }

  return 0;
}

extern "C"
{
  PG_FUNCTION_INFO_V1(circuit_to_noncnf);
}
Datum circuit_to_noncnf(PG_FUNCTION_ARGS){
  try
  {
    Datum token = PG_GETARG_DATUM(0);
    circuit_to_noncnf_internal(token);

  }
  catch(const std::exception& e)
  {
    elog(ERROR, "circuit_to_noncnf : %s" , e.what());
  }

  PG_RETURN_NULL();


}
/*
extern "C"
{
  PG_FUNCTION_INFO_V1(provsql_memory_used);
}
DATUM provsql_memory_used(PG_FUNCTION_ARGS){
  PG_RETURN_NULL();
}*/
