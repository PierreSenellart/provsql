extern "C"
{
#include "postgres.h"
#include "fmgr.h"
#include "catalog/pg_type.h"
#include "utils/uuid.h"
#include "executor/spi.h"
#include "provsql_shmem.h"
#include "provsql_utils.h"

  PG_FUNCTION_INFO_V1(dump_data);
}



char* print_shared_state_constants(constants_t &constants, char* buffer){
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


char* print_hash_entry(provsqlHashEntry* hash, char* buffer){
  sprintf(buffer, "Hash :\n"
  "Key = %16u \n"
  "Type = %d \n"
  "nb_children = %u \n"
  "children_idx = %u \n"
  "prob = %f\n"
  "info1 = %u\n"
  "info2 = %u\n"
  ,
  &hash->key,
  *&hash->type,
  *&hash->nb_children,
  *&hash->children_idx,
  *&hash->prob,
  *&hash->info1,
  *&hash->info2
  );

  return buffer;
}


Datum dump_data(PG_FUNCTION_ARGS)
{
  char buffer[1024];
  FILE *file;
  int32 num_entries;
  provsqlHashEntry *entry;
  HASH_SEQ_STATUS hash_seq;

  file = AllocateFile("provsql.tmp", PG_BINARY_W);
  if (file == NULL)
  {    /* TODO error */

    elog(ERROR, "error while allocating the file");
    PG_RETURN_NULL();

  }

  num_entries = hash_get_num_entries(provsql_hash);
  hash_seq_init(&hash_seq, provsql_hash);

  if(! fwrite(&num_entries, sizeof(int32), 1, file)){
    elog(ERROR, "error while writing num entries");
    PG_RETURN_NULL();
  }

  while ( (entry = (provsqlHashEntry*)hash_seq_search(&hash_seq) )  != NULL )
  {
    if (!fwrite(entry, sizeof(provsqlHashEntry), 1, file))
    {
      elog(ERROR, "error while writing hash entries");
      PG_RETURN_NULL();
    }

    elog(INFO,"%s", print_hash_entry(entry,buffer));
    
  }



  if( !fwrite(&provsql_shared_state->constants, sizeof(char), sizeof(constants_t), file) ){
    elog(ERROR, "error while writing shared state to file");
    PG_RETURN_NULL();
  }
  //elog(INFO,"Shared state constants : ");
  //elog(INFO,"%s", print_shared_state_constants(provsql_shared_state->constants, buffer));

  if ( !fwrite(&provsql_shared_state->nb_wires, sizeof(char), sizeof(unsigned), file )){
    PG_RETURN_NULL();
  }
  elog(INFO,"Nb_Wires : ");
  elog(INFO,"%u",provsql_shared_state->nb_wires);



  if (!fwrite(&provsql_shared_state->wires, sizeof(char), sizeof(pg_uuid_t)*provsql_shared_state->nb_wires, file)){
    PG_RETURN_NULL();
  } 


  if (FreeFile(file))
  {
    file = NULL;
    /* TODO error */
  }

  elog(INFO,"serializing complete");
  PG_RETURN_NULL();
}





extern "C"
{
  PG_FUNCTION_INFO_V1(read_data_dump);
}

Datum read_data_dump(PG_FUNCTION_ARGS){

  FILE *file;
  constants_t constants_buffer;
  char buffer[1024];
  unsigned nb_wires_buffer;
  int32 num;
  provsqlHashEntry tmp;
  provsqlHashEntry *entry;
  bool found;

  file = AllocateFile("provsql.tmp", PG_BINARY_R);


  if (!fread(&num, sizeof(int32), 1, file))
  {
    PG_RETURN_NULL();
  }

for (int i = 0; i < num; i++)
  {
    if (!fread(&tmp, sizeof(provsqlHashEntry), 1, file))
    {
      PG_RETURN_NULL();
    }
    entry = (provsqlHashEntry *) hash_search(provsql_hash, &(tmp.key), HASH_ENTER, &found);
    elog(INFO,"%s", print_hash_entry(entry,buffer));

    if (!found)
    {
      *entry = tmp;
      // entry->key = tmp.key;
      // entry->type = tmp.type;
      // entry->nb_children = tmp.nb_children;
      // entry->children_idx = tmp.children_idx;
      // entry->prob = tmp.prob;
      // entry->info1 = tmp.info1;
      // entry->info2 = tmp.info2;

      elog(INFO,"%s", print_hash_entry(entry,buffer));


    }

    
  }

  if(! fread(&constants_buffer, sizeof(constants_t), 1, file)){
    PG_RETURN_NULL();
  }
  //elog(INFO,"After Reading : \n");
  //elog(INFO,"%s",print_shared_state_constants(constants_buffer, buffer));


  if (! fread(&nb_wires_buffer, sizeof(unsigned), 1, file ))
  {
    PG_RETURN_NULL();
  }
  
  //elog(INFO,"NB_wires : %u",nb_wires_buffer);


  if (FreeFile(file))
  {
    file = NULL;
    /* TODO error */
  }

  PG_RETURN_NULL();
}
