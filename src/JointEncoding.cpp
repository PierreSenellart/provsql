/**
 * @file JointEncoding.cpp
 * @brief The data-graph fast-path encoding.
 *
 * See @c JointEncoding.h for the design.  This translation unit
 * implements the §3.5 regime (independent @c gate_input tokens, joint
 * graph = Gaifman graph of the facts); it does not handle the
 * correlated regime's circuit slice extraction.
 */
#include "JointEncoding.h"

#include <algorithm>
#include <unordered_map>

#include "TreeDecomposition.h"

JointEncoding JointEncoding::fromFacts(const std::vector<FactRow> &rows)
{
  JointEncoding enc;

  // Dedup independent facts by token: the same provenance token over the
  // same relation and element tuple is one variable serving several
  // atoms (the standard self-join case).  A token over a *different*
  // tuple is perfectly correlated with the first occurrence -- treating
  // the two as independent would be silently wrong -- so it routes to
  // the general path.
  std::unordered_map<std::string, std::size_t> token_to_fact;
  // Certain facts (untracked relations) dedup by (relation, elements).
  std::unordered_map<std::string, std::size_t> certain_to_fact;

  for (const auto &row : rows) {
    for (unsigned long e : row.elements)
      enc.n_elements = std::max(enc.n_elements, e + 1);

    if (row.token.empty()) {
      // Certain fact: always present.
      std::string key = std::to_string(row.relation_id) + ":";
      for (unsigned long e : row.elements)
        key += std::to_string(e) + ",";
      if (certain_to_fact.count(key))
        continue;
      certain_to_fact[key] = enc.facts.size();
      Fact f;
      f.relation_id = row.relation_id;
      f.elements = row.elements;
      f.kind = FactGateKind::CERTAIN;
      enc.facts.push_back(std::move(f));
      continue;
    }

    auto it = token_to_fact.find(row.token);
    if (it != token_to_fact.end()) {
      const Fact &existing = enc.facts[it->second];
      if (existing.relation_id != row.relation_id ||
          existing.elements != row.elements)
        throw JointCompilerException(
                "provenance token " + row.token +
                " gates facts over different element tuples; "
                "the joint-width data-graph path requires distinct tokens");
      continue;   // duplicate occurrence of the same fact (self-join)
    }

    if (row.prob < 0. || row.prob > 1.)
      throw JointCompilerException("fact probability out of [0,1]");

    Fact f;
    f.relation_id = row.relation_id;
    f.elements = row.elements;
    f.kind = FactGateKind::INDEP;
    f.event = enc.events.size();
    token_to_fact[row.token] = enc.facts.size();
    enc.facts.push_back(std::move(f));
    enc.events.push_back(Event{row.token, row.prob});
  }

  // Diagnostics: the data-graph degeneracy lower bound is the joint
  // screen in this regime; no circuit slice, so its bound is 0.
  Graph g = enc.buildGraph();
  unsigned max_degree = 0;
  enc.data_treewidth_lb =
    TreeDecomposition::degeneracyLowerBound(g, max_degree);
  enc.circuit_treewidth_lb = 0;

  return enc;
}

JointEncoding JointEncoding::fromCorrelated(std::vector<Fact> facts,
                                            std::vector<SliceGate> slice,
                                            unsigned long n_elements)
{
  JointEncoding enc;
  enc.facts = std::move(facts);
  enc.slice = std::move(slice);
  enc.n_elements = n_elements;
  enc.correlated = true;

  // The world variables are the slice's INPUT leaves; mirror them into
  // events for the statistics count.
  for (const auto &sg : enc.slice)
    if (sg.type == SliceGateType::INPUT)
      enc.events.push_back(Event{sg.token, sg.prob});

  // Diagnostics: the data-only degeneracy lower bound (Gaifman of the
  // facts, no gate vertices) and the slice-only degeneracy lower bound
  // (gate cliques only) -- the two "separate screens" of thesis
  // Prop. 4.2.11, both of which can be small while the joint width is
  // large.
  {
    Graph data;
    for (const auto &f : enc.facts) {
      for (std::size_t i = 0; i < f.elements.size(); ++i)
        for (std::size_t j = i + 1; j < f.elements.size(); ++j)
          if (f.elements[i] != f.elements[j])
            data.add_edge(f.elements[i], f.elements[j]);
      if (!f.elements.empty())
        data.add_node(f.elements[0]);
    }
    unsigned md = 0;
    enc.data_treewidth_lb = TreeDecomposition::degeneracyLowerBound(data, md);
  }
  {
    Graph circ;
    for (std::size_t i = 0; i < enc.slice.size(); ++i) {
      const SliceGate &sg = enc.slice[i];
      std::vector<unsigned long> cl;
      for (unsigned c : sg.children)
        cl.push_back(enc.n_elements + c);
      cl.push_back(enc.n_elements + i);
      for (std::size_t a = 0; a < cl.size(); ++a)
        for (std::size_t b = a + 1; b < cl.size(); ++b)
          if (cl[a] != cl[b])
            circ.add_edge(cl[a], cl[b]);
      if (cl.size() == 1)
        circ.add_node(cl[0]);
    }
    unsigned md = 0;
    enc.circuit_treewidth_lb =
      TreeDecomposition::degeneracyLowerBound(circ, md);
  }

  return enc;
}

Graph JointEncoding::buildGraph() const
{
  Graph g;
  for (const auto &f : facts) {
    // Fact clique: {elements} ∪ {fact gate} (the gate vertex is present
    // only in the correlated regime; an independent/certain fact's clique
    // is over its elements alone).
    std::vector<unsigned long> cl = f.elements;
    if (correlated && f.kind == FactGateKind::GATE)
      cl.push_back(n_elements + f.gate);
    for (std::size_t i = 0; i < cl.size(); ++i)
      for (std::size_t j = i + 1; j < cl.size(); ++j)
        if (cl[i] != cl[j])
          g.add_edge(cl[i], cl[j]);
    if (!cl.empty() && !g.has_node(cl[0]))
      g.add_node(cl[0]);
  }
  // Gate cliques: {gate} ∪ {children} per internal slice gate (the
  // stronger ternary co-occurrence, so every gate is confirmable in a
  // single bag).
  if (correlated)
    for (std::size_t i = 0; i < slice.size(); ++i) {
      const SliceGate &sg = slice[i];
      if (sg.children.empty()) {
        g.add_node(n_elements + i);   // INPUT leaf
        continue;
      }
      std::vector<unsigned long> cl;
      for (unsigned c : sg.children)
        cl.push_back(n_elements + c);
      cl.push_back(n_elements + i);
      for (std::size_t a = 0; a < cl.size(); ++a)
        for (std::size_t b = a + 1; b < cl.size(); ++b)
          if (cl[a] != cl[b])
            g.add_edge(cl[a], cl[b]);
    }
  return g;
}
