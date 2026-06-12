/**
 * @file ToolRegistry.cpp
 * @brief Seed and mutation logic for the external-tool registry.
 *
 * The seed lists the tools ProvSQL knows about, with
 * their default executable names and a preference order that keeps
 * @c d4 first among compilers, matching the
 * @c provsql.fallback_compiler default.  See @ref ToolRegistry.h.
 */
#include "ToolRegistry.h"

#include <algorithm>

namespace provsql {

bool ToolRecord::hasOperation(const std::string &op) const
{
  return std::find(operations.begin(), operations.end(), op)
         != operations.end();
}

bool ToolRecord::acceptsInput(const std::string &fmt) const
{
  return std::find(input_formats.begin(), input_formats.end(), fmt)
         != input_formats.end();
}

std::string ToolRecord::buildCommand(
    const std::string &in, const std::string &out,
    const std::string &binary_override,
    const std::vector<std::pair<std::string, std::string>> &extra) const
{
  return expandCommandTemplate(argtpl, binary_override, in, out, extra);
}

void ToolRegistry::seed()
{
  records_.clear();

  // Fields: name, kind, binary, operations, input_formats, output_format,
  //         parser, preference, enabled, dependencies, argtpl.
  // operations / input_formats / output_format use the KCMCP shared-registry
  // names so CLI and (future) kcmcp-server records are comparable; `parser`
  // is the CLI-only "how to decode this tool's raw output" tag (a kcmcp
  // server returns output_format directly, so its records leave parser "").
  //
  // Knowledge compilers reading a Tseytin DIMACS CNF and emitting a d-DNNF.
  // The single tolerant `nnf` parser auto-detects the c2d/d4 magic header,
  // so it reads both the d4-family (header-less) and classic forms.
  // Preference keeps @c d4 as the default compiler.
  records_.push_back({"d4",      "cli", "d4",      {"compile"}, {"dimacs-cnf"},
                      "ddnnf-nnf", "nnf", 100, true, {}, "-dDNNF {in} -out={out}"});
  // d4v2 also accepts a BC-S1.2 circuit (native, structure-preserving): it is
  // listed first so the dispatcher prefers it, falling back to dimacs-cnf.
  // argtpl is the CNF command; argtpl_circuit the native-circuit one (`-t
  // pcnf` projects gate variables out so the d-DNNF branches only on inputs).
  records_.push_back({"d4v2",    "cli", "d4v2",    {"compile"},
                      {"circuit-bcs12", "dimacs-cnf"},
                      "ddnnf-nnf", "nnf", 90, true, {}, "-i {in} --dump-file {out}",
                      "-i {in} --input-type circuit -t pcnf --dump-file {out}"});
  records_.push_back({"c2d",     "cli", "c2d",     {"compile"}, {"dimacs-cnf"},
                      "ddnnf-nnf", "nnf", 80, true, {}, "-in {in} -silent"});
  records_.push_back({"minic2d", "cli", "minic2d", {"compile"}, {"dimacs-cnf"},
                      "ddnnf-nnf", "nnf", 70, true, {}, "-c {in}"});
  records_.push_back({"dsharp",  "cli", "dsharp",  {"compile"}, {"dimacs-cnf"},
                      "ddnnf-nnf", "nnf", 60, true, {}, "-q -Fnnf {out} {in}"});

  // Panini (KCBox): one binary, three logical variants selected by the
  // --lang argument baked into each template.  Its compiled form is its own
  // DD format (no KCMCP code), so output_format/parser are the local
  // "panini-dd"; paniniCompile() reads it.
  records_.push_back({"panini-obdd",     "cli", "panini", {"compile"}, {"dimacs-cnf"},
                      "panini-dd", "panini-dd", 52, true, {},
                      "Panini --lang \"OBDD\" --out {out} --quiet {in}"});
  records_.push_back({"panini-obdd-and", "cli", "panini", {"compile"}, {"dimacs-cnf"},
                      "panini-dd", "panini-dd", 51, true, {},
                      "Panini --lang \"OBDD[AND]\" --out {out} --quiet {in}"});
  records_.push_back({"panini-decdnnf",  "cli", "panini", {"compile"}, {"dimacs-cnf"},
                      "panini-dd", "panini-dd", 50, true, {},
                      "Panini --lang \"Decision-DNNF\" --out {out} --quiet {in}"});

  // Weighted model counters: read a (weighted) DIMACS CNF and return a number
  // (KCMCP output_format "decimal").  sharpsat-td needs the flow_cutter_pace17
  // helper (invoked by relative path, hence the cd) and a scratch {tmpdir};
  // dpmc is a two-binary pipeline (htb | dmc) with no single binary of its
  // own.  The `wmc-line` parser scrapes the "c s exact" line; weightmc's
  // mantissa x 2^e output needs its own `weightmc` parser.
  records_.push_back({"sharpsat-td", "cli", "sharpsat-td", {"wmc"}, {"dimacs-cnf"},
                      "decimal", "wmc-line", 90, true, {"flow_cutter_pace17"},
                      "cd \"$(dirname \"$(command -v flow_cutter_pace17)\")\" && "
                      "{binary} -WE -decot 1 -decow 100 -tmpdir {tmpdir} "
                      "-cs 3500 -prec 20 {in} > {out} 2>&1"});
  records_.push_back({"ganak",       "cli", "ganak",       {"wmc"}, {"dimacs-cnf"},
                      "decimal", "wmc-line", 80, true, {},
                      "--mode 7 {in} > {out} 2>&1"});
  records_.push_back({"weightmc",    "cli", "weightmc",    {"wmc"}, {"dimacs-cnf"},
                      "decimal", "weightmc", 70, true, {},
                      "--startIteration=0 --gaussuntil=400 --verbosity=0 "
                      "--pivotAC={pivotAC} {in} > {out}"});
  records_.push_back({"dpmc",        "cli", "",            {"wmc"}, {"dimacs-cnf"},
                      "decimal", "wmc-line", 60, true, {"htb", "dmc"},
                      "htb --cf={in} | dmc --cf={in} > {out} 2>&1"});

  // Visualisation: a ProvSQL-local operation (no KCMCP counterpart); reads a
  // GraphViz DOT and returns graph-easy's ASCII art verbatim.
  records_.push_back({"graph-easy", "cli", "graph-easy", {"render"}, {"dot"},
                      "ascii", "ascii", 100, true, {},
                      "--as=boxart --output={out} {in}"});
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
