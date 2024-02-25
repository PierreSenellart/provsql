#ifndef PROVSQL_MMAP_H
#define PROVSQL_MMAP_H

#include "limits.h"

#include "postgres.h"
#include "provsql_utils.h"

void provsql_mmap_worker(Datum);
void RegisterProvSQLMMapWorker(void);
void initialize_provsql_mmap(void);
void destroy_provsql_mmap(void);
void provsql_mmap_main_loop(void);

extern char buffer[PIPE_BUF];
extern unsigned bufferpos;

#define READM(var, type) (read(provsql_shared_state->pipebmr, &var, sizeof(type))-sizeof(type)>=0) // flawfinder: ignore
#define READB(var, type) (read(provsql_shared_state->pipembr, &var, sizeof(type))-sizeof(type)>=0) // flawfinder: ignore
#define WRITEB(pvar, type) (write(provsql_shared_state->pipembw, pvar, sizeof(type))!=-1)
#define WRITEM(pvar, type) (write(provsql_shared_state->pipebmw, pvar, sizeof(type))!=-1)

#define STARTWRITEM() (bufferpos=0)
#define ADDWRITEM(pvar, type) (memcpy(buffer+bufferpos, pvar, sizeof(type)), bufferpos+=sizeof(type))
#define SENDWRITEM() (write(provsql_shared_state->pipebmw, buffer, bufferpos)!=-1)

#endif /* PROVSQL_COLUMN_NAME */
