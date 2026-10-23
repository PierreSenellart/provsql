/**
 * @file FormulaRegions.h
 * @brief The parts of a circuit @c sr_formula_rows renders a few rows from.
 *
 * @c sr_formula_rows writes the formula of a token as rows of bounded size,
 * a gate whose formula is too large being named and given a row of its own
 * (see @c FormulaRows in @c provenance_evaluate_compiled.cpp).  A row only
 * reads the gates below its own down to the named ones, so the circuit is
 * not loaded whole: the store answers, for a few rows at a time, the
 * @em region of each, the named gates in it left out as @em stubs.
 *
 * The size of a formula is counted in symbols, a gate for one, written out
 * at each use; a named descendant counts for one, its name.  A gate whose
 * size exceeds the @c cap of the request is named.  The store decides it,
 * from the circuit alone, stopping the count of a gate once past the cap.
 */
#ifndef FORMULA_REGIONS_H
#define FORMULA_REGIONS_H

#include <string>
#include <vector>

#include <boost/serialization/string.hpp>
#include <boost/serialization/vector.hpp>

#include "GenericCircuit.h"

/** @brief What a region is asked for. */
struct FormulaRequest {
  pg_uuid_t gate;       ///< The gate
  unsigned long from;   ///< First child of the range asked for
  /** Number of children asked for, from @c from; 0 for the region of the
   *  gate itself */
  unsigned long count;
};

/** @brief A gate left out of a region: a named one. */
struct FormulaStub {
  std::string uuid;           ///< The gate
  gate_type type;             ///< Its type
  unsigned info1;             ///< Its first integer annotation
  unsigned info2;             ///< Its second integer annotation
  unsigned long nb_children;  ///< Its number of children

  /** @brief Boost serialisation. */
  template<class Archive> void serialize(Archive &ar, const unsigned int)
  {
    ar & uuid & type & info1 & info2 & nb_children;
  }
};

/**
 * @brief The regions of a few requests, in one circuit.
 *
 * The region of a gate (a request of @c count 0) is the gate and the gates
 * below it down to the named ones, which are stubs: in @c circuit as
 * variable leaves, so that the load-time folds keep them as they are, and
 * listed in @c stubs.  A gate whose own row would still exceed the wide cap
 * of the request, with its named children read as their names, is itself
 * a stub, its children to be asked for by ranges.  The region of a range
 * of children is that of each of them, a named one being a stub; the
 * children, in order, are listed in @c children.
 */
struct FormulaRegions {
  GenericCircuit circuit;             ///< The gates of the regions
  std::vector<FormulaStub> stubs;     ///< The gates left out
  std::vector<std::string> children;  ///< The children asked for, in order

  /** @brief Boost serialisation. */
  template<class Archive> void serialize(Archive &ar, const unsigned int)
  {
    ar & circuit & stubs & children;
  }
};

#endif /* FORMULA_REGIONS_H */
