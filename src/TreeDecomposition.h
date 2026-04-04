/**
 * @file TreeDecomposition.h
 * @brief Tree decomposition of a Boolean circuit for knowledge compilation.
 *
 * A **tree decomposition** of a graph @f$G=(V,E)@f$ is a tree @f$T@f$
 * whose nodes are labelled with *bags* (subsets of @f$V@f$) such that:
 * 1. Every vertex of @f$G@f$ appears in at least one bag.
 * 2. For every edge @f$(u,v)\in E@f$, some bag contains both @f$u@f$
 *    and @f$v@f$.
 * 3. For every vertex @f$v@f$, the bags containing @f$v@f$ form a
 *    connected sub-tree of @f$T@f$.
 *
 * The **treewidth** is one less than the maximum bag size.  ProvSQL
 * builds a tree decomposition of the primal graph of a @c BooleanCircuit
 * (using the min-fill elimination heuristic via @c PermutationStrategy)
 * and then feeds it to @c dDNNFTreeDecompositionBuilder to construct a
 * d-DNNF.  Tractable compilation is guaranteed when the treewidth is
 * bounded (empirically ≤ @c MAX_TREEWIDTH).
 *
 * The decomposition can also be read from an external file in the
 * standard PACE challenge format (via the streaming @c operator>>).
 *
 * @c bag_t is a strongly-typed wrapper around @c size_t, analogous to
 * @c gate_t, used as a bag identifier within the tree.
 */
#ifndef TREE_DECOMPOSITION_H
#define TREE_DECOMPOSITION_H

#include <iostream>
#include <string>
#include <type_traits>
#include <vector>
#include <boost/container/static_vector.hpp>

#include "flat_set.hpp"
#include "BooleanCircuit.h"

// Forward declaration for friend
class dDNNFTreeDecompositionBuilder;

/**
 * @brief Strongly-typed bag identifier for a tree decomposition.
 *
 * Wraps a @c size_t, analogous to @c gate_t.
 */
enum class bag_t : size_t {};

/**
 * @brief Tree decomposition of a Boolean circuit's primal graph.
 *
 * Provides constructors that compute the decomposition from a
 * @c BooleanCircuit using the min-fill heuristic, or parse it from an
 * input stream.  After construction, @c makeFriendly() restructures the
 * tree into the "friendly" normal form required by
 * @c dDNNFTreeDecompositionBuilder.
 */
class TreeDecomposition {
public:
/** @brief Maximum supported treewidth.  Circuits exceeding this cause an error. */
static constexpr int MAX_TREEWIDTH = 10;
/** @brief Preferred maximum arity of bags in the friendly form. */
static constexpr int OPTIMAL_ARITY = 2;

/**
 * @brief Stack-allocated vector type bounded by @c MAX_TREEWIDTH+1 elements.
 *
 * Used for bag contents to avoid heap allocation for small, bounded sets.
 */
template<class T>
using small_vector = boost::container::static_vector<T, MAX_TREEWIDTH+1>;

/** @brief The type of a bag: a small flat set of gate IDs. */
using Bag = flat_set<gate_t, small_vector>;

private:
std::vector<Bag> bags;              ///< Bag contents, indexed by @c bag_t
std::vector<bag_t> parent;          ///< Parent of each bag (root points to itself)
std::vector<std::vector<bag_t> > children; ///< Children of each bag
bag_t root;                         ///< Identifier of the root bag
unsigned treewidth;                 ///< Treewidth of the decomposition

TreeDecomposition() = default;

/**
 * @brief Find the bag whose gate set is closest to gate @p v (for rooting).
 * @param v  Gate to search for.
 * @return   ID of the bag that contains or is nearest to @p v.
 */
bag_t findGateConnection(gate_t v) const;
/**
 * @brief Re-root the tree so that @p bag becomes the root.
 * @param bag  The bag to use as the new root.
 */
void reroot(bag_t bag);
/**
 * @brief Insert a new empty bag as a child of @p parent.
 * @param parent    Parent bag of the new empty bag.
 * @param children  Children to transfer from @p parent to the new bag.
 * @return          ID of the newly created bag.
 */
bag_t addEmptyBag(bag_t parent, const std::vector<bag_t> &children = std::vector<bag_t>());
/**
 * @brief Add gate @p g to the contents of bag @p b.
 * @param g  Gate to add.
 * @param b  Destination bag.
 */
void addGateToBag(gate_t g, bag_t b);

/**
 * @brief Mutable access to bag @p b.
 * @param b  Bag identifier.
 * @return   Reference to the bag's gate set.
 */
Bag &getBag(bag_t b)
{
  return bags[static_cast<std::underlying_type<bag_t>::type>(b)];
}
/**
 * @brief Const access to bag @p b.
 * @param b  Bag identifier.
 * @return   Const reference to the bag's gate set.
 */
const Bag &getBag(bag_t b) const
{
  return bags[static_cast<std::underlying_type<bag_t>::type>(b)];
}
/**
 * @brief Mutable access to the children of bag @p b.
 * @param b  Bag identifier.
 * @return   Reference to the vector of child bag IDs.
 */
std::vector<bag_t> &getChildren(bag_t b)
{
  return children[static_cast<std::underlying_type<bag_t>::type>(b)];
}
/**
 * @brief Const access to the children of bag @p b.
 * @param b  Bag identifier.
 * @return   Const reference to the vector of child bag IDs.
 */
const std::vector<bag_t> &getChildren(bag_t b) const
{
  return children[static_cast<std::underlying_type<bag_t>::type>(b)];
}
/**
 * @brief Return the parent of bag @p b.
 * @param b  Bag identifier.
 * @return   Parent bag identifier.
 */
bag_t getParent(bag_t b) const
{
  return parent[static_cast<std::underlying_type<bag_t>::type>(b)];
}
/**
 * @brief Set the parent of bag @p b to @p p.
 * @param b  Bag whose parent is to be set.
 * @param p  New parent bag.
 */
void setParent(bag_t b, bag_t p)
{
  parent[static_cast<std::underlying_type<bag_t>::type>(b)]=p;
}

public:
/**
 * @brief Compute a tree decomposition of the primal graph of @p bc.
 *
 * Uses the min-fill elimination ordering heuristic.  Throws
 * @c TreeDecompositionException if the computed treewidth exceeds
 * @c MAX_TREEWIDTH.
 *
 * @param bc  The Boolean circuit to decompose.
 */
TreeDecomposition(const BooleanCircuit &bc);

/**
 * @brief Parse a tree decomposition from a stream (PACE challenge format).
 *
 * @param in  Input stream containing the decomposition.
 */
TreeDecomposition(std::istream &in);

/** @brief Copy constructor. @param td Source decomposition. */
TreeDecomposition(const TreeDecomposition &td);
/**
 * @brief Copy assignment.
 * @param td  Source decomposition.
 * @return    Reference to this decomposition after assignment.
 */
TreeDecomposition &operator=(const TreeDecomposition &td);

/**
 * @brief Return the treewidth of this decomposition.
 * @return Treewidth (maximum bag size minus one).
 */
unsigned getTreewidth() const {
  return treewidth;
}

/**
 * @brief Restructure the tree into the friendly normal form.
 *
 * Reroots the tree at the bag that covers the circuit's root gate
 * and splits/merges bags so that @c dDNNFTreeDecompositionBuilder
 * can process them efficiently.
 *
 * @param root  The gate that should be covered by the root bag.
 */
void makeFriendly(gate_t root);

/**
 * @brief Render the tree decomposition as a GraphViz DOT string.
 * @return DOT @c digraph string for visualisation.
 */
std::string toDot() const;

friend std::istream& operator>>(std::istream& in, TreeDecomposition &td);
friend class dDNNFTreeDecompositionBuilder;
};

/**
 * @brief Read a tree decomposition in PACE challenge format.
 * @param in  Input stream.
 * @param td  Tree decomposition to populate.
 * @return    Reference to @p in.
 */
std::istream& operator>>(std::istream& in, TreeDecomposition &td);

/**
 * @brief Pre-increment operator for @c bag_t.
 * @param b  Bag to increment.
 * @return   Reference to the incremented bag.
 */
inline bag_t &operator++(bag_t &b) {
  return b=bag_t{static_cast<std::underlying_type<bag_t>::type>(b)+1};
}
/**
 * @brief Post-increment operator for @c bag_t.
 * @param b  Bag to increment.
 * @return   Original value of @p b before the increment.
 */
inline bag_t operator++(bag_t &b, int) {
  auto temp{b};
  b=bag_t{static_cast<std::underlying_type<bag_t>::type>(b)+1};
  return temp;
}

/**
 * @brief Compare a @c bag_t against a @c std::vector size type.
 * @param t  Bag identifier.
 * @param u  Size to compare against.
 * @return   @c true if the underlying integer of @p t is less than @p u.
 */
inline bool operator<(bag_t t, std::vector<bag_t>::size_type u)
{
  return static_cast<std::underlying_type<bag_t>::type>(t)<u;
}

/**
 * @brief Convert a @c bag_t to its decimal string representation.
 * @param b  Bag identifier.
 * @return   Decimal string of the underlying integer.
 */
inline std::string to_string(bag_t b) {
  return std::to_string(static_cast<std::underlying_type<bag_t>::type>(b));
}

/**
 * @brief Read a @c bag_t from an input stream.
 * @param i  Input stream.
 * @param b  Bag to populate.
 * @return   Reference to @p i.
 */
inline std::istream &operator>>(std::istream &i, bag_t &b)
{
  std::underlying_type<bag_t>::type u;
  i >> u;
  b=bag_t{u};
  return i;
}

/**
 * @brief Exception thrown when a tree decomposition cannot be constructed.
 *
 * Raised when the treewidth of the circuit exceeds @c MAX_TREEWIDTH or
 * when the input stream is malformed.
 */
class TreeDecompositionException : public std::exception {};

#endif /* TREE_DECOMPOSITION_H */
