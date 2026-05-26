/**
 * @file tool_registry_sync.h
 * @brief Reload the in-memory external-tool registry from its persistent
 *        overrides.
 *
 * The default tools are compiled in (seeded in @ref ToolRegistry.h); an
 * administrator's changes are stored in the @c provsql.tool_overrides table.
 * @ref provsql_sync_tool_registry rebuilds the per-backend in-memory registry
 * as "compiled seed, overlaid with the override rows", so a registration made
 * in one backend is honoured by every other.  It must be called at the top of
 * any SQL function that consults the registry (the probability / compile /
 * shapley / visualise dispatchers, the @c provsql.tools listing, and the
 * mutators themselves).
 *
 * It uses SPI (nesting-safe) and is a no-op leaving just the compiled seed
 * when the overrides table is absent (an extension older than 1.8.0, or the
 * upgrade not yet applied), so older databases keep working unchanged.
 *
 * Extension-only: not linked into the standalone @c tdkc tool, which invokes
 * no external tool.
 */
#ifndef PROVSQL_TOOL_REGISTRY_SYNC_H
#define PROVSQL_TOOL_REGISTRY_SYNC_H

void provsql_sync_tool_registry();

#endif
