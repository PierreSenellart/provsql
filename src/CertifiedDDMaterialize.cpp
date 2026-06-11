/**
 * @file CertifiedDDMaterialize.cpp
 * @brief Implementation of certified-d-D materialisation into the store.
 *
 * See @c CertifiedDDMaterialize.h.  Extracted from the reachability
 * compiler's SQL glue so the joint-width UCQ compiler shares the same
 * content-addressed materialisation (and so probability / Shapley /
 * expectation all go through the standard @c probability_evaluate path on
 * the materialised token).
 */
extern "C" {
#include "postgres.h"
#include "miscadmin.h"
#include "utils/uuid.h"

#include "provsql_utils.h"
#include "provsql_mmap.h"
}

#include "CertifiedDDMaterialize.h"
#include "BooleanCircuit.h"
#include "provsql_utils_cpp.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

/**
 * @brief Minimal SHA-1 (RFC 3174), used for RFC 4122 version-5 UUIDs.
 *
 * Self-contained so the content addressing builds on every supported
 * PostgreSQL version.
 */
void sha1(const unsigned char *data, std::size_t len, unsigned char out[20])
{
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                   0xC3D2E1F0u};
  const std::size_t total = ((len + 8) / 64 + 1) * 64;
  std::vector<unsigned char> buf(total, 0);
  std::copy(data, data + len, buf.begin());
  buf[len] = 0x80;
  const uint64_t bits = static_cast<uint64_t>(len) * 8;
  for (int i = 0; i < 8; ++i)
    buf[total - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));

  for (std::size_t chunk = 0; chunk < total; chunk += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (static_cast<uint32_t>(buf[chunk + 4*i]) << 24) |
             (static_cast<uint32_t>(buf[chunk + 4*i + 1]) << 16) |
             (static_cast<uint32_t>(buf[chunk + 4*i + 2]) << 8) |
             static_cast<uint32_t>(buf[chunk + 4*i + 3]);
    for (int i = 16; i < 80; ++i) {
      const uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
      w[i] = (v << 1) | (v >> 31);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
      e = d;
      d = c;
      c = (b << 30) | (b >> 2);
      b = a;
      a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  for (int i = 0; i < 5; ++i) {
    out[4*i]     = static_cast<unsigned char>(h[i] >> 24);
    out[4*i + 1] = static_cast<unsigned char>(h[i] >> 16);
    out[4*i + 2] = static_cast<unsigned char>(h[i] >> 8);
    out[4*i + 3] = static_cast<unsigned char>(h[i]);
  }
}

} // namespace

pg_uuid_t provsqlUuidV5(const std::string &name)
{
  // uuid_ns_provsql() = 920d4f02-8718-5319-9532-d4ab83a64489
  static const unsigned char ns[16] = {
    0x92, 0x0d, 0x4f, 0x02, 0x87, 0x18, 0x53, 0x19,
    0x95, 0x32, 0xd4, 0xab, 0x83, 0xa6, 0x44, 0x89
  };
  std::vector<unsigned char> data(16 + name.size());
  std::copy(ns, ns + 16, data.begin());
  std::copy(name.begin(), name.end(), data.begin() + 16);
  unsigned char digest[20];
  sha1(data.data(), data.size(), digest);
  pg_uuid_t u;
  std::copy(digest, digest + 16, u.data);
  u.data[6] = static_cast<unsigned char>((u.data[6] & 0x0F) | 0x50);
  u.data[8] = static_cast<unsigned char>((u.data[8] & 0x3F) | 0x80);
  return u;
}

std::unordered_map<gate_t, pg_uuid_t, hash_gate_t> materializeCertifiedDD(
  const dDNNF &dd, const std::vector<gate_t> &roots)
{
  std::unordered_map<gate_t, pg_uuid_t, hash_gate_t> uuid_of;
  /* Tokens this backend has already materialised, across calls: the
   * store is append-only, so a create this backend has sent once never
   * needs re-sending.  Per-backend, bounded. */
  static std::unordered_set<std::string> created;
  constexpr std::size_t kCreatedCap = 4u << 20;
  if (created.size() > kCreatedCap)
    created.clear();
  pg_uuid_t one_uuid;
  bool have_one = false;

  const auto ensureOne = [&]() {
                           if (!have_one) {
                             one_uuid = provsqlUuidV5("one");
                             provsql_internal_create_gate(&one_uuid, gate_one,
                                                          0, NULL);
                             have_one = true;
                           }
                           return one_uuid;
                         };
  const auto createOnce = [&](const pg_uuid_t &token, gate_type type,
                              const std::vector<pg_uuid_t> &children,
                              bool certified) {
                            const std::string key = uuid2string(token);
                            if (!created.insert(key).second)
                              return;
                            provsql_internal_create_gate(
                              &token, type,
                              static_cast<unsigned>(children.size()),
                              children.empty() ? NULL : children.data());
                            if (certified)
                              provsql_internal_set_infos(&token,
                                                         DNNF_CERT_INFO, 0);
                          };

  std::vector<gate_t> stack(roots);
  while (!stack.empty()) {
    CHECK_FOR_INTERRUPTS();
    const gate_t g = stack.back();
    if (uuid_of.find(g) != uuid_of.end()) {
      stack.pop_back();
      continue;
    }

    const auto t = dd.getGateType(g);

    if (t == BooleanGate::IN || t == BooleanGate::MULIN) {
      const std::string tok = dd.getUUID(g);
      if (!tok.empty() && tok[0] == '\x01') {
        // A synthetic stick-breaking coin: the joint-width compiler
        // introduces these (token "\x01mulsb:N") when it expands a
        // repair_key / BID exclusion block into shared independent
        // events; they carry a probability but no store UUID.  Materialise
        // each as a fresh independent gate_input with that probability.
        // The UUID is content-addressed on the synthetic token, so coins
        // shared across a block's values (the same "\x01mulsb:N") collapse
        // to one store gate, preserving the mutual exclusion.
        const pg_uuid_t token = provsqlUuidV5(tok);
        if (created.insert(uuid2string(token)).second) {
          provsql_internal_create_gate(&token, gate_input, 0, NULL);
          provsql_internal_set_prob(&token, dd.getProb(g));
        }
        uuid_of[g] = token;
      } else {
        // Existing tokens (the leaf provenance): never re-created.
        uuid_of[g] = string2uuid(tok);
      }
      stack.pop_back();
      continue;
    }

    bool ready = true;
    for (const auto &c : dd.getWires(g))
      if (uuid_of.find(c) == uuid_of.end()) {
        stack.push_back(c);
        ready = false;
      }
    if (!ready)
      continue;

    const bool certified = dd.isDNNFCertified(g);
    pg_uuid_t token;

    switch (t) {
    case BooleanGate::NOT:
    {
      const pg_uuid_t child = uuid_of[dd.getWires(g)[0]];
      const pg_uuid_t one = ensureOne();
      token = provsqlUuidV5("monus" + uuid2string(one) + uuid2string(child));
      createOnce(token, gate_monus, {one, child}, false);
    }
    break;

    case BooleanGate::AND:
    case BooleanGate::OR:
    {
      const auto &wires = dd.getWires(g);
      if (wires.empty()) {
        if (t == BooleanGate::AND)
          token = ensureOne();
        else {
          token = provsqlUuidV5("zero");
          createOnce(token, gate_zero, {}, false);
        }
      } else if (wires.size() == 1) {
        token = uuid_of[wires[0]];
      } else {
        std::vector<std::string> texts;
        texts.reserve(wires.size());
        for (const auto &c : wires)
          texts.push_back(uuid2string(uuid_of[c]));
        std::sort(texts.begin(), texts.end());
        std::string name = (t == BooleanGate::AND ? "times{" : "plus{");
        for (std::size_t i = 0; i < texts.size(); ++i) {
          if (i)
            name += ",";
          name += texts[i];
        }
        name += "}";
        token = provsqlUuidV5(name);
        std::vector<pg_uuid_t> children;
        children.reserve(wires.size());
        for (const auto &c : wires)
          children.push_back(uuid_of[c]);
        createOnce(token, t == BooleanGate::AND ? gate_times : gate_plus,
                   children, certified);
      }
    }
    break;

    default:
      provsql_error("materializeCertifiedDD: unsupported gate type");
      throw std::runtime_error("unreachable");
    }

    uuid_of[g] = token;
    stack.pop_back();
  }

  return uuid_of;
}

pg_uuid_t wrapAssumedAbsorptive(const pg_uuid_t &child)
{
  static std::unordered_set<std::string> created;
  constexpr std::size_t kCreatedCap = 1u << 20;
  if (created.size() > kCreatedCap)
    created.clear();

  const pg_uuid_t token =
    provsqlUuidV5("assumedabsorptive" + uuid2string(child));
  if (created.insert(uuid2string(token)).second) {
    provsql_internal_create_gate(&token, gate_assumed, 1, &child);
    provsql_internal_set_extra(&token, "absorptive");
  }
  return token;
}
