/**
 * @file provsql_migrate_mmap.cpp
 * @brief Migrate old flat provsql mmap files to the per-database layout.
 *
 * Old format: $PGDATA/provsql_*.mmap (no 16-byte header)
 * New format: $PGDATA/base/\<db_oid\>/provsql_*.mmap (16-byte header)
 *
 * For each database that has provsql installed, the tool:
 *   1. Enumerates provenance-tracked tables via libpq.
 *   2. Collects root UUIDs from those tables.
 *   3. BFS-traverses the old flat circuit from those roots.
 *   4. Writes per-database new-format files.
 *
 * Usage: provsql_migrate_mmap -D \<pgdata\> -c \<connstr\>
 *
 * connstr should point to the postgres / template1 database so the tool
 * can enumerate all databases.  The tool then re-connects per database.
 * If connstr already contains "dbname=", append " dbname=\<target\>" to
 * override it (libpq uses the last occurrence).
 *
 * Gates belonging to tables in more than one database are written to
 * every relevant database.  UUID v4 collisions across databases are
 * negligible.
 *
 * The tool skips any database whose $PGDATA/base/\<oid\>/provsql_gates.mmap
 * already exists.
 */

/* Include libpq first — it provides Oid (unsigned int) via postgres_ext.h
 * before any PostgreSQL server headers are pulled in. */
#include <libpq-fe.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// ── Portable types ────────────────────────────────────────────────────────────

struct pg_uuid_t { uint8_t data[16]; };

struct UUIDHash {
  size_t operator()(pg_uuid_t u) const noexcept {
    uint64_t h; memcpy(&h, u.data, 8); return static_cast<size_t>(h);
  }
};
struct UUIDEq {
  bool operator()(pg_uuid_t a, pg_uuid_t b) const noexcept {
    return memcmp(a.data, b.data, 16) == 0;
  }
};
using UUIDSet = std::unordered_set<pg_uuid_t, UUIDHash, UUIDEq>;

// gate_type must match provsql_utils.h exactly (enum values are on-disk ABI).
enum gate_type {
  gate_input, gate_plus, gate_times, gate_monus, gate_project,
  gate_zero, gate_one, gate_eq, gate_agg, gate_semimod,
  gate_cmp, gate_delta, gate_value, gate_mulinput, gate_update,
  gate_invalid, nb_gate_types
};

// GateInformation must match MMappedCircuit.h exactly (on-disk ABI).
struct GateInformation {
  gate_type     type;
  unsigned      nb_children;
  unsigned long children_idx;
  double        prob;
  unsigned      info1;
  unsigned      info2;
  unsigned long extra_idx;
  unsigned      extra_len;

  GateInformation()
    : type(gate_input), nb_children(0), children_idx(0),
      prob(1.0), info1(0), info2(0), extra_idx(0), extra_len(0) {}
  GateInformation(gate_type t, unsigned n, unsigned long idx)
    : type(t), nb_children(n), children_idx(idx),
      prob(1.0), info1(0), info2(0), extra_idx(0), extra_len(0) {}
};

// ── Old-format readers ────────────────────────────────────────────────────────
//
// Old MMappedVector<T> on-disk:
//   unsigned long nb_elements;
//   unsigned long capacity;
//   T d[];
//
// Old MMappedUUIDHashTable on-disk:
//   unsigned      log_size;
//   (4-byte implicit padding)
//   unsigned long nb_elements;
//   unsigned long next_value;
//   value_t t[];              where value_t = { pg_uuid_t uuid; unsigned long value; }

static constexpr unsigned long NOTHING = static_cast<unsigned long>(-1);

struct OldVecHdr {
  unsigned long nb_elements;
  unsigned long capacity;
};

struct OldHashSlot {
  pg_uuid_t     uuid;
  unsigned long value;
};

struct OldTableHdr {
  unsigned      log_size;
  /* 4-byte implicit padding */
  unsigned long nb_elements;
  unsigned long next_value;
  /* OldHashSlot t[] follows */
};

class OldMMapVec {
  void          *base_     = nullptr;
  size_t         sz_       = 0;
  const OldVecHdr *hdr_    = nullptr;
  const uint8_t  *elems_   = nullptr;
  size_t          esz_     = 0;
public:
  void open(const std::string &path, size_t elem_size) {
    esz_ = elem_size;
    int fd = ::open(path.c_str(), O_RDONLY); // flawfinder: ignore
    if (fd < 0)
      throw std::runtime_error("Cannot open " + path + ": " + strerror(errno));
    sz_ = static_cast<size_t>(lseek(fd, 0, SEEK_END));
    base_ = ::mmap(nullptr, sz_, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (base_ == MAP_FAILED)
      throw std::runtime_error("mmap(" + path + "): " + strerror(errno));
    hdr_   = reinterpret_cast<const OldVecHdr *>(base_);
    elems_ = reinterpret_cast<const uint8_t *>(hdr_ + 1);
  }
  ~OldMMapVec() { if (base_ && base_ != MAP_FAILED) munmap(base_, sz_); }
  unsigned long size() const { return hdr_ ? hdr_->nb_elements : 0; }
  const void *at(unsigned long k) const { return elems_ + k * esz_; }
};

class OldMMapHash {
  void             *base_     = nullptr;
  size_t            sz_       = 0;
  const OldTableHdr *hdr_     = nullptr;
  const OldHashSlot *slots_   = nullptr;
  unsigned long      cap_     = 0;
public:
  void open(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDONLY); // flawfinder: ignore
    if (fd < 0)
      throw std::runtime_error("Cannot open " + path + ": " + strerror(errno));
    sz_ = static_cast<size_t>(lseek(fd, 0, SEEK_END));
    base_ = ::mmap(nullptr, sz_, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (base_ == MAP_FAILED)
      throw std::runtime_error("mmap(" + path + "): " + strerror(errno));
    hdr_   = reinterpret_cast<const OldTableHdr *>(base_);
    slots_ = reinterpret_cast<const OldHashSlot *>(hdr_ + 1);
    cap_   = 1UL << hdr_->log_size;
  }
  ~OldMMapHash() { if (base_ && base_ != MAP_FAILED) munmap(base_, sz_); }

  unsigned long lookup(pg_uuid_t u) const {
    if (!hdr_) return NOTHING;
    uint64_t h; memcpy(&h, u.data, 8);
    unsigned long k = h % cap_;
    while (slots_[k].value != NOTHING && memcmp(slots_[k].uuid.data, u.data, 16))
      k = (k + 1) % cap_;
    return slots_[k].value;
  }

  /* Return all stored (uuid, index) pairs. */
  std::vector<std::pair<pg_uuid_t, unsigned long>> allEntries() const {
    std::vector<std::pair<pg_uuid_t, unsigned long>> out;
    if (!hdr_) return out;
    out.reserve(hdr_->nb_elements);
    for (unsigned long i = 0; i < cap_; ++i)
      if (slots_[i].value != NOTHING)
        out.emplace_back(slots_[i].uuid, slots_[i].value);
    return out;
  }
};

struct OldCircuit {
  OldMMapHash mapping;
  OldMMapVec  gates;
  OldMMapVec  wires;
  OldMMapVec  extra;

  void open(const std::string &pgdata) {
    mapping.open(pgdata + "/provsql_mapping.mmap");
    gates.open  (pgdata + "/provsql_gates.mmap",   sizeof(GateInformation));
    wires.open  (pgdata + "/provsql_wires.mmap",   sizeof(pg_uuid_t));
    extra.open  (pgdata + "/provsql_extra.mmap",   sizeof(char));
  }

  const GateInformation *getGate(pg_uuid_t u) const {
    unsigned long idx = mapping.lookup(u);
    if (idx == NOTHING) return nullptr;
    return reinterpret_cast<const GateInformation *>(gates.at(idx));
  }

  std::vector<pg_uuid_t> getChildren(const GateInformation *gi) const {
    std::vector<pg_uuid_t> result;
    for (unsigned long k = gi->children_idx; k < gi->children_idx + gi->nb_children; ++k)
      result.push_back(*reinterpret_cast<const pg_uuid_t *>(wires.at(k)));
    return result;
  }

  std::string getExtra(const GateInformation *gi) const {
    std::string s;
    s.reserve(gi->extra_len);
    for (unsigned long k = gi->extra_idx; k < gi->extra_idx + gi->extra_len; ++k)
      s += *reinterpret_cast<const char *>(extra.at(k));
    return s;
  }
};

// ── New-format writers ────────────────────────────────────────────────────────
//
// Replicates the on-disk layout of MMappedVector<T> and MMappedUUIDHashTable
// without depending on PostgreSQL server headers.
//
// New MMappedVector<T> on-disk:
//   uint64_t magic; uint16_t version; uint16_t elem_size; uint32_t _reserved;
//   unsigned long nb_elements; unsigned long capacity;
//   T d[];
//
// New MMappedUUIDHashTable on-disk:
//   uint64_t magic; uint16_t version; uint16_t elem_size; uint32_t _reserved;
//   unsigned log_size; (4-byte implicit padding)
//   unsigned long nb_elements; unsigned long next_value;
//   value_t t[];

static constexpr uint64_t MAGIC_GATES =
  uint64_t('P')       | uint64_t('v') <<  8 | uint64_t('S') << 16 | uint64_t('G') << 24 |
  uint64_t('a') << 32 | uint64_t('t') << 40 | uint64_t('e') << 48 | uint64_t('s') << 56;
static constexpr uint64_t MAGIC_WIRES =
  uint64_t('P')       | uint64_t('v') <<  8 | uint64_t('S') << 16 | uint64_t('W') << 24 |
  uint64_t('i') << 32 | uint64_t('r') << 40 | uint64_t('e') << 48 | uint64_t('s') << 56;
static constexpr uint64_t MAGIC_MAPPING =
  uint64_t('P')       | uint64_t('v') <<  8 | uint64_t('S') << 16 | uint64_t('M') << 24 |
  uint64_t('a') << 32 | uint64_t('p') << 40 | uint64_t('n') << 48 | uint64_t('g') << 56;
static constexpr uint64_t MAGIC_EXTRA =
  uint64_t('P')       | uint64_t('v') <<  8 | uint64_t('S') << 16 | uint64_t('E') << 24 |
  uint64_t('x') << 32 | uint64_t('t') << 40 | uint64_t('r') << 48 | uint64_t('a') << 56;

struct NewVecHdr {
  uint64_t      magic;
  uint16_t      version;
  uint16_t      elem_size;
  uint32_t      _reserved;
  unsigned long nb_elements;
  unsigned long capacity;
};

struct NewHashSlot {
  pg_uuid_t     uuid;
  unsigned long value;
};

struct NewTableHdr {
  uint64_t      magic;
  uint16_t      version;
  uint16_t      elem_size;
  uint32_t      _reserved;
  unsigned      log_size;
  /* 4-byte implicit padding */
  unsigned long nb_elements;
  unsigned long next_value;
};

static void writeNewVector(const std::string &path, uint64_t magic,
                           size_t elem_size, const uint8_t *raw, unsigned long nb)
{
  unsigned long cap = 1UL << 16; /* STARTING_CAPACITY */
  while (cap < nb) cap *= 2;

  size_t fsz = sizeof(NewVecHdr) + elem_size * cap;
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600); // flawfinder: ignore
  if (fd < 0) throw std::runtime_error("Cannot create " + path + ": " + strerror(errno));
  if (ftruncate(fd, static_cast<off_t>(fsz))) {
    ::close(fd);
    throw std::runtime_error("ftruncate(" + path + "): " + strerror(errno));
  }
  void *base = ::mmap(nullptr, fsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (base == MAP_FAILED) throw std::runtime_error("mmap(" + path + "): " + strerror(errno));

  auto *hdr = reinterpret_cast<NewVecHdr *>(base);
  hdr->magic       = magic;
  hdr->version     = 1;
  hdr->elem_size   = static_cast<uint16_t>(elem_size);
  hdr->_reserved   = 0;
  hdr->nb_elements = nb;
  hdr->capacity    = cap;
  if (nb > 0) memcpy(hdr + 1, raw, nb * elem_size);

  msync(base, fsz, MS_SYNC);
  munmap(base, fsz);
}

static void writeNewHashTable(const std::string &path,
                              const std::vector<std::pair<pg_uuid_t, unsigned long>> &entries)
{
  unsigned log_size = 16;
  while ((1UL << log_size) < entries.size() * 2)
    ++log_size;
  unsigned long cap = 1UL << log_size;

  size_t fsz = sizeof(NewTableHdr) + cap * sizeof(NewHashSlot);
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600); // flawfinder: ignore
  if (fd < 0) throw std::runtime_error("Cannot create " + path + ": " + strerror(errno));
  if (ftruncate(fd, static_cast<off_t>(fsz))) {
    ::close(fd);
    throw std::runtime_error("ftruncate(" + path + "): " + strerror(errno));
  }
  void *base = ::mmap(nullptr, fsz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (base == MAP_FAILED) throw std::runtime_error("mmap(" + path + "): " + strerror(errno));

  auto *hdr   = reinterpret_cast<NewTableHdr *>(base);
  hdr->magic       = MAGIC_MAPPING;
  hdr->version     = 1;
  hdr->elem_size   = static_cast<uint16_t>(sizeof(NewHashSlot));
  hdr->_reserved   = 0;
  hdr->log_size    = log_size;
  hdr->nb_elements = entries.size();
  hdr->next_value  = entries.size();

  auto *slots = reinterpret_cast<NewHashSlot *>(hdr + 1);
  for (unsigned long i = 0; i < cap; ++i)
    slots[i].value = NOTHING;

  for (auto &[u, v] : entries) {
    uint64_t h; memcpy(&h, u.data, 8);
    unsigned long k = h % cap;
    while (slots[k].value != NOTHING)
      k = (k + 1) % cap;
    slots[k].uuid  = u;
    slots[k].value = v;
  }

  msync(base, fsz, MS_SYNC);
  munmap(base, fsz);
}

// ── libpq RAII helpers ────────────────────────────────────────────────────────

struct Conn {
  PGconn *c;
  explicit Conn(const std::string &cs) : c(PQconnectdb(cs.c_str())) {
    if (PQstatus(c) != CONNECTION_OK)
      throw std::runtime_error(std::string("libpq connect: ") + PQerrorMessage(c));
  }
  ~Conn() { PQfinish(c); }
  PGconn *get() const { return c; }
};

struct Res {
  PGresult *r;
  explicit Res(PGresult *r) : r(r) {}
  ~Res() { PQclear(r); }
  bool ok() const { return PQresultStatus(r) == PGRES_TUPLES_OK; }
  int  ntuples() const { return PQntuples(r); }
  const char *get(int row, int col) const { return PQgetvalue(r, row, col); }
};

// ── UUID parsing ──────────────────────────────────────────────────────────────

static pg_uuid_t parseUUID(const char *s)
{
  auto hex = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
    return static_cast<uint8_t>(c - 'A' + 10);
  };
  pg_uuid_t u{};
  int b = 0;
  while (*s && b < 16) {
    if (*s == '-') { ++s; continue; }
    u.data[b++] = static_cast<uint8_t>((hex(s[0]) << 4) | hex(s[1]));
    s += 2;
  }
  return u;
}

// ── BFS to collect reachable gates ───────────────────────────────────────────

struct GateData {
  GateInformation        gi;
  std::vector<pg_uuid_t> children;
  std::string            extra_str;
};

/* Returns map: old_gate_index -> GateData for all reachable gates. */
static std::unordered_map<unsigned long, GateData>
collectReachable(const OldCircuit &old, const UUIDSet &roots)
{
  std::unordered_map<unsigned long, GateData> result;
  std::queue<pg_uuid_t> todo;
  UUIDSet visited;

  for (auto &u : roots)
    if (!visited.count(u)) { visited.insert(u); todo.push(u); }

  while (!todo.empty()) {
    pg_uuid_t u = todo.front(); todo.pop();

    unsigned long idx = old.mapping.lookup(u);
    if (idx == NOTHING) continue;   /* implicit input gate, no children */
    if (result.count(idx)) continue;

    const GateInformation *gi = reinterpret_cast<const GateInformation *>(old.gates.at(idx));
    GateData gd;
    gd.gi       = *gi;
    gd.children = old.getChildren(gi);
    gd.extra_str = old.getExtra(gi);

    auto children_copy = gd.children;  /* save before move */
    result[idx] = std::move(gd);

    for (auto &child : children_copy)
      if (!visited.count(child)) { visited.insert(child); todo.push(child); }
  }
  return result;
}

// ── Per-database migration ────────────────────────────────────────────────────

static void migrateDatabase(const OldCircuit &old, const std::string &pgdata,
                            Oid db_oid, const UUIDSet &roots)
{
  std::string dir        = pgdata + "/base/" + std::to_string(db_oid);
  std::string gates_path = dir + "/provsql_gates.mmap";

  struct stat st;
  if (stat(gates_path.c_str(), &st) == 0) {
    std::cerr << "  Skipping db " << db_oid << " (provsql_gates.mmap already exists)\n";
    return;
  }

  auto reachable = collectReachable(old, roots);
  std::cerr << "  db " << db_oid << ": " << reachable.size() << " reachable gates\n";

  /* Build reverse map: old_index -> UUID, from the full old hash table. */
  std::unordered_map<unsigned long, pg_uuid_t> old_idx_to_uuid;
  old_idx_to_uuid.reserve(old.mapping.allEntries().size());
  for (auto &[u, v] : old.mapping.allEntries())
    old_idx_to_uuid[v] = u;

  /* Assign compact new indices.
   * Pass 1: explicitly stored gates in reachable.
   * Pass 2: implicit input children (referenced but absent from mapping). */
  std::unordered_map<pg_uuid_t, unsigned long, UUIDHash, UUIDEq> uuid_to_new;
  unsigned long next_new = 0;

  for (auto &[old_idx, gd] : reachable) {
    auto it = old_idx_to_uuid.find(old_idx);
    if (it != old_idx_to_uuid.end() && !uuid_to_new.count(it->second))
      uuid_to_new[it->second] = next_new++;
  }
  for (auto &[old_idx, gd] : reachable) {
    for (auto &child : gd.children)
      if (!uuid_to_new.count(child))
        uuid_to_new[child] = next_new++;
  }
  /* Also include roots that have no explicit gate record (rare but safe). */
  for (auto &u : roots)
    if (!uuid_to_new.count(u))
      uuid_to_new[u] = next_new++;

  unsigned long N = next_new;
  if (N == 0) { std::cerr << "  No gates to write, skipping.\n"; return; }

  /* Build UUID-indexed view: new_idx -> uuid. */
  std::vector<pg_uuid_t> new_idx_to_uuid(N);
  for (auto &[u, ni] : uuid_to_new)
    new_idx_to_uuid[ni] = u;

  /* Build new gates, wires, extra arrays. */
  std::vector<GateInformation> new_gates(N);
  std::vector<pg_uuid_t>       new_wires;
  std::vector<uint8_t>         new_extra;

  for (unsigned long ni = 0; ni < N; ++ni) {
    pg_uuid_t u = new_idx_to_uuid[ni];
    unsigned long old_idx = old.mapping.lookup(u);

    if (old_idx == NOTHING) {
      new_gates[ni] = GateInformation(gate_input, 0, new_wires.size());
      continue;
    }

    auto rit = reachable.find(old_idx);
    if (rit == reachable.end()) {
      new_gates[ni] = GateInformation(gate_input, 0, new_wires.size());
      continue;
    }

    const GateData &gd = rit->second;
    unsigned long wires_start = new_wires.size();

    for (auto &child : gd.children) {
      /* Remap child UUID to its new index, stored as the UUID itself —
       * wires still store UUIDs in both old and new format. */
      new_wires.push_back(child);
    }

    new_gates[ni]              = gd.gi;
    new_gates[ni].children_idx = wires_start;
    new_gates[ni].extra_idx    = 0;
    new_gates[ni].extra_len    = 0;

    if (!gd.extra_str.empty()) {
      new_gates[ni].extra_idx = new_extra.size();
      new_gates[ni].extra_len = static_cast<unsigned>(gd.extra_str.size());
      for (char c : gd.extra_str)
        new_extra.push_back(static_cast<uint8_t>(c));
    }
  }

  /* Build mapping entries for the new hash table. */
  std::vector<std::pair<pg_uuid_t, unsigned long>> mapping_entries(
    uuid_to_new.begin(), uuid_to_new.end());

  /* Write the four files. */
  writeNewVector(gates_path, MAGIC_GATES, sizeof(GateInformation),
                 reinterpret_cast<const uint8_t *>(new_gates.data()), N);

  writeNewVector(dir + "/provsql_wires.mmap", MAGIC_WIRES, sizeof(pg_uuid_t),
                 reinterpret_cast<const uint8_t *>(new_wires.data()), new_wires.size());

  writeNewVector(dir + "/provsql_extra.mmap", MAGIC_EXTRA, sizeof(char),
                 new_extra.data(), new_extra.size());

  writeNewHashTable(dir + "/provsql_mapping.mmap", mapping_entries);

  std::cerr << "  Wrote " << N << " gates to " << dir << "\n";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
  std::string pgdata, connstr;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-D") == 0 && i + 1 < argc) pgdata  = argv[++i];
    else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) connstr = argv[++i];
  }

  if (pgdata.empty() || connstr.empty()) {
    std::cerr << "Usage: provsql_migrate_mmap -D <pgdata> -c <connstr>\n";
    return 1;
  }

  struct stat st;
  if (stat((pgdata + "/provsql_gates.mmap").c_str(), &st) != 0) {
    std::cerr << "No provsql_gates.mmap found in " << pgdata << " — nothing to migrate.\n";
    return 0;
  }

  std::cerr << "Opening old circuit files in " << pgdata << "...\n";
  OldCircuit old;
  try {
    old.open(pgdata);
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  std::cerr << "Connecting to cluster...\n";
  try {
    Conn conn(connstr);

    Res dbs(PQexec(conn.get(),
      "SELECT oid, datname FROM pg_database "
      "WHERE datallowconn AND NOT datistemplate "
      "ORDER BY oid"));
    if (!dbs.ok()) {
      std::cerr << "pg_database query failed: " << PQerrorMessage(conn.get()) << "\n";
      return 1;
    }

    for (int i = 0; i < dbs.ntuples(); ++i) {
      Oid         db_oid = static_cast<Oid>(atol(dbs.get(i, 0)));
      std::string dbname = dbs.get(i, 1);
      std::cerr << "Database " << dbname << " (oid=" << db_oid << ")...\n";

      /* Connect to this specific database (libpq uses last dbname= value). */
      PGconn *dbc = PQconnectdb((connstr + " dbname=" + dbname).c_str());
      if (PQstatus(dbc) != CONNECTION_OK) {
        std::cerr << "  Cannot connect: " << PQerrorMessage(dbc) << "\n";
        PQfinish(dbc);
        continue;
      }

      /* Check provsql is installed. */
      {
        PGresult *r = PQexec(dbc, "SELECT 1 FROM pg_extension WHERE extname='provsql'");
        bool has_provsql = PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0;
        PQclear(r);
        if (!has_provsql) { PQfinish(dbc); continue; }
      }
      std::cerr << "  provsql is installed\n";

      /* Find provenance-tracked tables (those with a 'provsql' column
       * outside the provsql schema itself). */
      PGresult *r = PQexec(dbc,
        "SELECT attrelid::regclass::text "
        "FROM pg_attribute "
        "JOIN pg_class ON attrelid = pg_class.oid "
        "JOIN pg_namespace ON relnamespace = pg_namespace.oid "
        "WHERE attname = 'provsql' AND attisdropped = false "
        "  AND relkind = 'r' AND nspname <> 'provsql'");

      if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        std::cerr << "  Attribute query failed: " << PQerrorMessage(dbc) << "\n";
        PQclear(r); PQfinish(dbc); continue;
      }

      std::vector<std::string> tables;
      for (int j = 0; j < PQntuples(r); ++j)
        tables.emplace_back(PQgetvalue(r, j, 0));
      PQclear(r);

      UUIDSet roots;
      for (const auto &tname : tables) {
        /* Safe: tname comes from pg_class via regclass cast, not user input. */
        std::string q = "SELECT provsql FROM " + tname;
        PGresult *tr = PQexec(dbc, q.c_str());
        if (PQresultStatus(tr) != PGRES_TUPLES_OK) {
          std::cerr << "  Query on " << tname << " failed, skipping\n";
          PQclear(tr); continue;
        }
        for (int k = 0; k < PQntuples(tr); ++k) {
          const char *s = PQgetvalue(tr, k, 0);
          if (s && s[0]) roots.insert(parseUUID(s));
        }
        PQclear(tr);
      }
      PQfinish(dbc);

      std::cerr << "  " << roots.size() << " root UUIDs across "
                << tables.size() << " table(s)\n";

      if (!roots.empty())
        migrateDatabase(old, pgdata, db_oid, roots);
    }
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  /* All databases processed successfully: remove the old flat files so
   * the new background worker does not find them and so that the upgrade
   * script warning is not triggered again. */
  static const char *old_files[] = {
    "provsql_gates.mmap",
    "provsql_wires.mmap",
    "provsql_mapping.mmap",
    "provsql_extra.mmap",
    nullptr
  };
  std::cerr << "Removing old flat circuit files from " << pgdata << ":\n";
  for (int i = 0; old_files[i]; ++i) {
    std::string path = pgdata + "/" + old_files[i];
    if (unlink(path.c_str()) == 0)
      std::cerr << "  Deleted " << path << "\n";
    else if (errno != ENOENT)
      std::cerr << "  Warning: could not delete " << path << ": " << strerror(errno) << "\n";
  }

  std::cerr << "Migration complete.\n"
               "Please restart the PostgreSQL server.\n";
  return 0;
}
