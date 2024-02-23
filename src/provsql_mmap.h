#ifndef PROVSQL_MMAP_H
#define PROVSQL_MMAP_H

#include "postgres.h"

void provsql_mmap_worker(Datum);
void RegisterProvSQLMMapWorker(void);
void initialize_provsql_mmap(void);
void destroy_provsql_mmap(void);

#endif /* PROVSQL_COLUMN_NAME */
