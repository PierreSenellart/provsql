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
  provsql_shmem_lock_exclusive();
  if(!WRITEM("g", char) || !WRITEM(&token, pg_uuid_t))
    elog(ERROR, "Cannot write to pipe (message type g)");

  unsigned long size;
  if(!READB(size, unsigned long))
    elog(ERROR, "Cannot read from pipe (message type g)");

  char *buf = new char[size], *p = buf;
  ssize_t actual_read, remaining_size=size;
  while((actual_read=read(provsql_shared_state->pipembr, p, remaining_size))<remaining_size) {
    if(actual_read<=0) {
      provsql_shmem_unlock();
      elog(ERROR, "Cannot read from pipe (message type g)");
    } else {
      remaining_size-=actual_read;
      p+=actual_read;
    }
  }
  provsql_shmem_unlock();

  boost::iostreams::stream<boost::iostreams::array_source> stream(buf, size);
  boost::archive::binary_iarchive ia(stream);
  BooleanCircuit bc;
  ia >> bc;

  delete [] buf;

  return bc;
}
