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

  PG_FUNCTION_INFO_V1(dump_data);
}



char* print_shared_state_constants(constants_t &constants, char* buffer)
{
  sprintf(buffer, "Constants :\n OID_SCHEMA_PROVSQL = %d \n"
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


  switch (provsql_serialize("provsql_test.tmp"))
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
