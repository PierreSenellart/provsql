/**
 * @file ToolRegistry.h
 * @brief In-memory catalog of the external tools ProvSQL can invoke.
 *
 * ProvSQL shells out to several knowledge compilers / model counters /
 * visualisers (@c d4, @c d4v2, @c c2d, @c minic2d, @c dsharp, @c ganak,
 * @c weightmc, @c graph-easy).  Rather than compiling the set of tools,
 * their executable names, and which one is preferred for a given operation
 * in as string literals scattered across many call sites, this
 * registry holds a single in-memory table of @ref provsql::ToolRecord, so
 * the dispatchers query metadata instead of testing literals.
 *
 * The registry is seeded at first use with the tools ProvSQL knows
 * about, with their default invocations and a default
 * preference order, so a fresh backend works out of the box with no
 * registration call.  An administrator
 * may then add / repoint / reorder / disable tools at run time through the
 * SQL surface (@c provsql.register_tool, @c provsql.unregister_tool,
 * @c provsql.set_tool_enabled, @c provsql.set_tool_preference, and the
 * read-only @c provsql.tools view).
 *
 * @par Lifetime
 * The catalog is process-local (one copy per PostgreSQL backend), seeded
 * from compiled-in defaults and mutated in memory.  It is therefore
 * **per-session and transient**: registrations are visible only within the
 * backend that made them and are reset when the session ends.  This is the
 * deliberate first-stage backing; a future stage may back it with a shared
 * catalog table without changing this interface.
 *
 * @par Standalone tdkc
 * The standalone @c tdkc tool deliberately uses no external tool, so it does
 * not link this registry: every reference to it in @c BooleanCircuit.cpp is
 * guarded by @c \#ifndef @c TDKC.  Keep this header free of any PostgreSQL or
 * external-tool dependency so it stays a self-contained piece of metadata.
 */
#ifndef PROVSQL_TOOL_REGISTRY_H
#define PROVSQL_TOOL_REGISTRY_H

#include <string>
#include <utility>
#include <vector>

namespace provsql {

/**
 * @brief Expand a command template into a runnable shell command line.
 *
 * Replaces @c {binary} / @c {in} / @c {out} and any @p extra placeholders
 * (e.g. @c {tmpdir}, @c {pivotAC}) in @p tpl.  When @p tpl contains no
 * @c {binary} placeholder, a non-empty @p binary is prepended (the common
 * "<binary> <args>" shape); a template that places the binaries itself (the
 * @c dpmc pipeline) is returned as-is.
 *
 * Header-only and dependency-free on purpose, so the standalone @c tdkc
 * build can expand a template without linking the registry.
 */
inline std::string expandCommandTemplate(
    const std::string &tpl, const std::string &binary,
    const std::string &in, const std::string &out,
    const std::vector<std::pair<std::string, std::string>> &extra = {})
{
  auto sub = [](std::string &s, const std::string &key,
                const std::string &value) {
    const std::string token = "{" + key + "}";
    std::string::size_type pos = 0;
    while ((pos = s.find(token, pos)) != std::string::npos) {
      s.replace(pos, token.size(), value);
      pos += value.size();
    }
  };
  std::string cmd = tpl;
  sub(cmd, "binary", binary);
  sub(cmd, "in", in);
  sub(cmd, "out", out);
  for (const auto &kv : extra)
    sub(cmd, kv.first, kv.second);
  if (tpl.find("{binary}") == std::string::npos && !binary.empty())
    return binary + " " + cmd;
  return cmd;
}

/**
 * @brief One registered external tool.
 *
 * @c name is the logical id used by the dispatchers and the
 * @c provsql.fallback_compiler GUC (e.g. @c "d4").  @c binary is the
 * executable resolved through @c find_external_tool; it defaults to
 * @c name but may be repointed (e.g. an absolute path to a specific build)
 * without changing the logical id.  Distinct logical ids may share one
 * @c binary: the three @c panini-* compiler variants all run @c "panini"
 * with a different @c --lang.  @c operations advertises what the tool can
 * do: @c "compile" (knowledge compilation to a d-DNNF / NNF the
 * @c compilation() parser reads), @c "wmc" (weighted model counting), or
 * @c "render" (DOT visualisation); a record with no operation would be
 * unselectable, so every record advertises at least one.  @c preference
 * orders candidates within an operation (higher first); @c enabled lets an
 * admin keep a record but stop the dispatchers from selecting it.
 *
 * @c dependencies lists extra executables the tool needs at run time beyond
 * @c binary: @c sharpsat-td needs @c flow_cutter_pace17, and @c dpmc is a
 * two-binary pipeline (@c htb @c | @c dmc) with an empty @c binary and
 * @c dependencies @c = @c {htb, dmc}.  A tool is "available" iff @c binary
 * (when non-empty) and every dependency resolve on PATH.
 *
 * The capability triple @c operations / @c input_formats / @c output_format
 * uses the **KCMCP shared registry names** (see
 * doc/source/dev/kc-server-protocol.rst), so a CLI record and a future
 * @c kind="kcmcp" server record are directly comparable: @c operations is
 * @c "compile" / @c "wmc" (the ProvSQL-local @c "render" has no KCMCP
 * counterpart); @c input_formats is drawn from @c "dimacs-cnf" /
 * @c "circuit-bcs12"; @c output_format from @c "ddnnf-nnf" / @c "decimal" /
 * @c "rational" (with ProvSQL-local @c "panini-dd" / @c "ascii" where KCMCP
 * has no code).  A KCMCP server *discovers* this triple at the handshake; a
 * CLI tool *declares* it here.
 *
 * @c parser is **CLI-only** -- it says how to decode this tool's raw output
 * into @c output_format, a job a KCMCP server does on the wire and so leaves
 * empty: the single tolerant @c "nnf" reader yields @c output_format
 * @c "ddnnf-nnf" (it auto-detects the c2d/d4 magic header, covering both the
 * d4-family and classic forms); @c "wmc-line" (the @c "c s exact" line) and
 * @c "weightmc" (mantissa x 2^e) both yield @c "decimal"; @c "panini-dd" and
 * @c "ascii" are their own.
 *
 * @c argtpl is the command the dispatcher runs, with @c {in} / @c {out}
 * substituted by the input/output temp files and a few tool-specific
 * placeholders (@c {binary}, @c {tmpdir}, @c {pivotAC}).  When @c argtpl
 * contains no @c {binary}, the resolved @c binary is prepended; otherwise
 * the template is the whole command (used by the @c dpmc pipeline and the
 * @c sharpsat-td @c cd-prefix).  @c argtpl_circuit is the alternative command
 * used when the @c "circuit-bcs12" input is selected (the input is then a
 * BC-S1.2 circuit rather than a Tseytin CNF); only a tool that accepts that
 * input needs it.  Together these make a CLI tool's whole invocation data: a
 * new tool whose output is a known parser/format can be registered without
 * recompiling.
 */
struct ToolRecord {
  std::string name;
  std::string kind;                      ///< "cli" (spawn a binary) or "kcmcp"
                                         ///< (talk to a socket server at @c endpoint).
  std::string binary;
  std::vector<std::string> operations;
  std::vector<std::string> input_formats;
  std::string output_format;
  std::string parser;
  int preference = 0;
  bool enabled = true;
  std::vector<std::string> dependencies;
  std::string argtpl;
  std::string argtpl_circuit;
  std::string endpoint;                  ///< KCMCP server address for kind
                                         ///< "kcmcp": "unix:/path" or "host:port".

  bool hasOperation(const std::string &op) const;
  bool acceptsInput(const std::string &fmt) const;

  /**
   * @brief Build the command line for this tool.
   *
   * Expands @c {in} / @c {out} (and @p extra placeholders, e.g.
   * @c {tmpdir}, @c {pivotAC}) in @c argtpl.  @p binary_override is the
   * executable to use (the registry-resolved @c binary, possibly repointed);
   * when @c argtpl references @c {binary} it is substituted inline, otherwise
   * a non-empty binary is prepended.
   */
  std::string buildCommand(
      const std::string &in, const std::string &out,
      const std::string &binary_override,
      const std::vector<std::pair<std::string, std::string>> &extra = {}) const;
};

/**
 * @brief The process-local registry singleton.
 *
 * Not thread-safe; PostgreSQL backends are single-threaded, which is the
 * only context that touches it.
 */
class ToolRegistry {
public:
  /// Access the per-process registry, seeding it on first use.
  static ToolRegistry &instance();

  /// Find a record by logical name, or @c nullptr if none is registered.
  const ToolRecord *find(const std::string &name) const;

  /// True iff a record named @p name exists, is enabled, and advertises @p op.
  bool provides(const std::string &name, const std::string &op) const;

  /**
   * @brief Enabled tools advertising @p op, ordered by descending
   *        preference then name.
   */
  std::vector<const ToolRecord *> byOperation(const std::string &op) const;

  /// All records, in registration order (used by the @c provsql.tools view).
  const std::vector<ToolRecord> &records() const {
    return records_;
  }

  /// Register a new tool or replace the record with the same name.
  void upsert(const ToolRecord &rec);

  /// Remove the record named @p name; returns false if none existed.
  bool remove(const std::string &name);

  /// Flip the @c enabled flag of @p name; returns false if none existed.
  bool setEnabled(const std::string &name, bool enabled);

  /// Set the @c preference of @p name; returns false if none existed.
  bool setPreference(const std::string &name, int preference);

  /// Discard all records and re-seed the compiled-in defaults.
  void reset();

private:
  ToolRegistry() {
    seed();
  }
  void seed();

  std::vector<ToolRecord> records_;
};

/// Shorthand for @c ToolRegistry::instance().
inline ToolRegistry &tool_registry() {
  return ToolRegistry::instance();
}

} // namespace provsql

#endif
