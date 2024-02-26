#include <cmath>

#include <boost/archive/binary_iarchive.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>

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

  unsigned size;
  if(!READB(size, unsigned))
    elog(ERROR, "Cannot read from pipe (message type G)");

  char *buf = new char[size];
  if(read(provsql_shared_state->pipembr, buf, size)<size)
    elog(ERROR, "Cannot read from pipe (message type G)");
  LWLockRelease(provsql_shared_state->lock);

  boost::iostreams::stream<boost::iostreams::array_source> stream(buf, size);
  boost::archive::binary_iarchive ia(stream);
  BooleanCircuit bc;
  ia >> bc;

  delete [] buf;

  return bc;
}
