/**
 * @file process_memory.h
 * @brief Resident memory of a process, or of a process group.
 *
 * Used by @c provsql.max_memory (the backend's own memory, see
 * @c provsql_interrupt.h) and @c provsql.tool_max_memory (an external tool
 * and the processes it forks, see @c external_tool.cpp).  Read from the
 * operating system on Linux, macOS and FreeBSD; elsewhere the functions
 * return 0, and the budgets they serve have no effect.
 */
#ifndef PROVSQL_PROCESS_MEMORY_H
#define PROVSQL_PROCESS_MEMORY_H

#include <cstddef>
#include <sys/types.h>

/** @brief The resident memory of process @p pid, in bytes, or 0 where the
 *  platform does not say. */
std::size_t provsql_process_rss(pid_t pid);

/** @brief The resident memory of the processes of group @p pgid, in bytes,
 *  or 0 where the platform does not say. */
std::size_t provsql_process_group_rss(pid_t pgid);

#endif /* PROVSQL_PROCESS_MEMORY_H */
