#ifndef PROVSQL_MMAP_H
#define PROVSQL_MMAP_H

#include "postgres.h"
#include "provsql_utils.h"

void provsql_mmap_worker(Datum);
void RegisterProvSQLMMapWorker(void);
void initialize_provsql_mmap(void);
void destroy_provsql_mmap(void);
void provsql_mmap_main_loop(void);

#define READM(var, type) (read(provsql_shared_state->pipebmr, &var, sizeof(type))-sizeof(type)>=0) // flawfinder: ignore
#define READB(var, type) (read(provsql_shared_state->pipembr, &var, sizeof(type))-sizeof(type)>=0) // flawfinder: ignore
#define WRITEB(pvar, type) (write(provsql_shared_state->pipembw, pvar, sizeof(type))!=-1)
#define WRITEM(pvar, type) (write(provsql_shared_state->pipebmw, pvar, sizeof(type))!=-1)

#endif /* PROVSQL_COLUMN_NAME */
