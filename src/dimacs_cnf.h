/**
 * @file dimacs_cnf.h
 * @brief Parse a DIMACS CNF into a ProvSQL @c BooleanCircuit.
 *
 * Reads the @c "p cnf" header, the clause lines, and the Model Counting
 * Competition weight lines (@c "c p weight <lit> <w> 0").  Builds an
 * AND-of-OR-of-literal Boolean circuit: one @c IN gate per variable, a shared
 * @c NOT per negated variable, an @c OR per clause, and an @c AND root.
 *
 * For weighted model counting (@p weighted) each input gate's probability is
 * set to the weight of its positive literal, defaulting to 0.5 when no weight
 * is given; ProvSQL's evaluator then takes the complementary weight @c 1-p for
 * the negative literal (the probability model the @c "c p weight" lines
 * ProvSQL itself emits already satisfy).  Used by the @c tdkc KCMCP server.
 */
#ifndef PROVSQL_DIMACS_CNF_H
#define PROVSQL_DIMACS_CNF_H

#include <string>

#include "BooleanCircuit.h"

/**
 * @brief Parse @p text (a DIMACS CNF) into @p c and return the root gate.
 * @throws std::runtime_error on a malformed input.
 */
gate_t parse_dimacs_cnf(const std::string &text, BooleanCircuit &c,
                        bool weighted);

#endif
