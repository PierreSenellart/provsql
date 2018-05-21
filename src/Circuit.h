#ifndef CIRCUIT_H
#define CIRCUIT_H

#include "pg_config.h" // for PG_VERSION_NUM
#include "c.h" // for int16

extern "C" {
#include "utils/uuid.h"

#if PG_VERSION_NUM < 100000
/* In versions of PostgreSQL < 10, pg_uuid_t is declared to be an opaque
 * struct pg_uuid_t in uuid.h, so we have to give the definition of
 * struct pg_uuid_t; this problem is resolved in PostgreSQL 10 */
#define UUID_LEN 16
  struct pg_uuid_t
  {
    unsigned char data[UUID_LEN];
  };
#endif /* PG_VERSION_NUM */
}  

#include <unordered_map>
#include <unordered_set>
#include <vector>

template<class gateType>
class Circuit {
 public:
  using uuid = std::string;

 private:
  std::unordered_map<uuid, unsigned> uuid2id;
  
 protected:
  std::vector<gateType> gates;
  std::vector<std::vector<unsigned>> wires;
    
 public:
  virtual unsigned addGate();
  virtual unsigned setGate(const uuid &u, gateType t);
  bool hasGate(const uuid &u) const;
  unsigned getGate(const uuid &u);
  void addWire(unsigned f, unsigned t);

  virtual std::string toString(unsigned g) const = 0;
};

class CircuitException : public std::exception
{
  std::string message;

 public: 
  CircuitException(const std::string &m) : message(m) {}
  virtual char const * what() const noexcept { return message.c_str(); }
};

#endif /* CIRCUIT_H */
