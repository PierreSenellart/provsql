#include <cmath>

#include <boost/archive/binary_iarchive.hpp>
#include <boost/iostreams/device/array.hpp>
#include <boost/iostreams/stream.hpp>

#include "CircuitFromMMap.h"
#include "provsql_utils_cpp.h"

extern "C" {
#include "provsql_shmem.h"
#include "provsql_mmap.h"
}

template<typename C>
static C getCircuitFromMMap(pg_uuid_t token, char message_char)
{
  provsql_shmem_lock_exclusive();
  if(!WRITEM(&message_char, char) || !WRITEM(&token, pg_uuid_t))
    elog(ERROR, "Cannot write to pipe (message type %c)", message_char);

  unsigned long size;
  if(!READB(size, unsigned long))
    elog(ERROR, "Cannot read from pipe (message type %c)", message_char);

  char *buf = new char[size], *p = buf;
  ssize_t actual_read, remaining_size=size;
  while((actual_read=read(provsql_shared_state->pipembr, p, remaining_size))<remaining_size) {
    if(actual_read<=0) {
      provsql_shmem_unlock();
      elog(ERROR, "Cannot read from pipe (message type %c)", message_char);
    } else {
      remaining_size-=actual_read;
      p+=actual_read;
    }
  }
  provsql_shmem_unlock();

  boost::iostreams::stream<boost::iostreams::array_source> stream(buf, size);
  boost::archive::binary_iarchive ia(stream);
  C c;
  ia >> c;

  delete [] buf;

  return c;
}

BooleanCircuit getBooleanCircuit(pg_uuid_t token)
{
  return getCircuitFromMMap<BooleanCircuit>(token, 'b');
}
GenericCircuit getGenericCircuit(pg_uuid_t token)
{
  return getCircuitFromMMap<GenericCircuit>(token, 'g');
}
