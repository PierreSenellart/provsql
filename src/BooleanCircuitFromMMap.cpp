#include <cmath>

#include <boost/archive/text_iarchive.hpp>

#include "BooleanCircuit.h"
#include "provsql_utils_cpp.h"

extern "C" {
#include "provsql_shmem.h"
#include "provsql_mmap.h"
}

BooleanCircuit getBooleanCircuit(pg_uuid_t token)
{
  LWLockAcquire(provsql_shared_state->lock, LW_EXCLUSIVE);
  if(!WRITEM("g", char) || !WRITEM(&token, pg_uuid_t))
    elog(ERROR, "Cannot write to pipe (message type G)");

  const unsigned BUFFER_SIZE=1024;
  char buf[BUFFER_SIZE]; // flawfinder: ignore
  std::stringstream ss;
  unsigned nb_read;
  while((nb_read=read(provsql_shared_state->pipembr, buf, BUFFER_SIZE))>0) { // flawfinder: ignore
    bool finished = false;
    if(buf[nb_read-1] == '\0') {
      finished=true;
      nb_read--;
    }
    ss << std::string(buf, nb_read);
    if(finished)
      break;
  }
  LWLockRelease(provsql_shared_state->lock);

  boost::archive::text_iarchive ia(ss);
  BooleanCircuit bc;
  ia >> bc;

  return bc;
}
