/**
 * @file ToolRegistry.cpp
 * @brief Seed and mutation logic for the external-tool registry.
 *
 * The seed mirrors exactly the tools ProvSQL has always known about, with
 * their default executable names and a preference order that keeps the
 * historical selection (@c d4 first among compilers, matching the
 * @c provsql.fallback_compiler default).  See @ref ToolRegistry.h.
 */
#include "ToolRegistry.h"

#include <algorithm>

namespace provsql {

bool ToolRecord::hasOperation(const std::string &op) const
{
  return std::find(operations.begin(), operations.end(), op)
         != operations.end();
}

void ToolRegistry::seed()
{
  records_.clear();

  // Knowledge compilers whose output the compilation() NNF parser reads.
  // Preference order reproduces the historical "d4 is the default" bias;
  // it is only a default the admin can override, not behaviour today (no
  // dispatcher currently walks a multi-compiler preference chain).
  records_.push_back({"d4",      "cli", "d4",      {"compile"}, 100, true});
  records_.push_back({"d4v2",    "cli", "d4v2",    {"compile"},  90, true});
  records_.push_back({"c2d",     "cli", "c2d",     {"compile"},  80, true});
  records_.push_back({"minic2d", "cli", "minic2d", {"compile"},  70, true});
  records_.push_back({"dsharp",  "cli", "dsharp",  {"compile"},  60, true});

  // Panini (KCBox): one binary, three logical variants selected by the
  // --lang argument.  They are the compiler names provsql.fallback_compiler
  // and the 'compilation' method already accept.
  records_.push_back({"panini-obdd",     "cli", "panini", {"compile"}, 52, true});
  records_.push_back({"panini-obdd-and", "cli", "panini", {"compile"}, 51, true});
  records_.push_back({"panini-decdnnf",  "cli", "panini", {"compile"}, 50, true});

  // Weighted model counters.  sharpsat-td additionally needs the
  // flow_cutter_pace17 helper; dpmc is a two-binary pipeline (htb | dmc)
  // with no single binary of its own.
  records_.push_back({"sharpsat-td", "cli", "sharpsat-td", {"wmc"}, 90, true,
                      {"flow_cutter_pace17"}});
  records_.push_back({"ganak",       "cli", "ganak",       {"wmc"}, 80, true});
  records_.push_back({"weightmc",    "cli", "weightmc",    {"wmc"}, 70, true});
  records_.push_back({"dpmc",        "cli", "",            {"wmc"}, 60, true,
                      {"htb", "dmc"}});

  // Visualisation.
  records_.push_back({"graph-easy", "cli", "graph-easy", {"render"}, 100, true});
}

void ToolRegistry::reset()
{
  seed();
}

ToolRegistry &ToolRegistry::instance()
{
  static ToolRegistry registry;
  return registry;
}

const ToolRecord *ToolRegistry::find(const std::string &name) const
{
  for (const auto &rec : records_)
    if (rec.name == name)
      return &rec;
  return nullptr;
}

bool ToolRegistry::provides(const std::string &name, const std::string &op) const
{
  const ToolRecord *rec = find(name);
  return rec != nullptr && rec->enabled && rec->hasOperation(op);
}

std::vector<const ToolRecord *>
ToolRegistry::byOperation(const std::string &op) const
{
  std::vector<const ToolRecord *> out;
  for (const auto &rec : records_)
    if (rec.enabled && rec.hasOperation(op))
      out.push_back(&rec);

  std::sort(out.begin(), out.end(),
            [](const ToolRecord *a, const ToolRecord *b) {
              if (a->preference != b->preference)
                return a->preference > b->preference;
              return a->name < b->name;
            });
  return out;
}

void ToolRegistry::upsert(const ToolRecord &rec)
{
  for (auto &existing : records_) {
    if (existing.name == rec.name) {
      existing = rec;
      return;
    }
  }
  records_.push_back(rec);
}

bool ToolRegistry::remove(const std::string &name)
{
  auto it = std::find_if(records_.begin(), records_.end(),
                         [&](const ToolRecord &r) { return r.name == name; });
  if (it == records_.end())
    return false;
  records_.erase(it);
  return true;
}

bool ToolRegistry::setEnabled(const std::string &name, bool enabled)
{
  for (auto &rec : records_) {
    if (rec.name == name) {
      rec.enabled = enabled;
      return true;
    }
  }
  return false;
}

bool ToolRegistry::setPreference(const std::string &name, int preference)
{
  for (auto &rec : records_) {
    if (rec.name == name) {
      rec.preference = preference;
      return true;
    }
  }
  return false;
}

} // namespace provsql
