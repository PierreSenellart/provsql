/**
 * @file process_memory.cpp
 * @brief Resident memory of a process, or of a process group, read from the
 *        operating system.
 *
 * See @c process_memory.h.
 */
#include "process_memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#elif defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>
#endif

#if defined(__linux__)
/** @brief The contents of the small file @p path, or an empty string. */
static std::string read_small_file(const char *path)
{
  char buf[512];
  int fd = open(path, O_RDONLY);
  ssize_t len;
  if (fd < 0)
    return std::string();
  len = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (len <= 0)
    return std::string();
  return std::string(buf, (std::size_t)len);
}
#endif

std::size_t provsql_process_rss(pid_t pid)
{
#if defined(__linux__)
  char path[64];
  unsigned long size, resident;
  snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);
  std::string s = read_small_file(path);
  if (s.empty() || sscanf(s.c_str(), "%lu %lu", &size, &resident) != 2)
    return 0;
  return (std::size_t)resident * (std::size_t)sysconf(_SC_PAGESIZE);
#elif defined(__APPLE__)
  struct proc_taskinfo ti;
  if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) != sizeof(ti))
    return 0;
  return (std::size_t)ti.pti_resident_size;
#elif defined(__FreeBSD__)
  struct kinfo_proc kp;
  std::size_t len = sizeof(kp);
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid};
  if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len == 0)
    return 0;
  return (std::size_t)kp.ki_rssize * (std::size_t)getpagesize();
#else
  (void)pid;
  return 0;
#endif
}

std::size_t provsql_process_group_rss(pid_t pgid)
{
#if defined(__linux__)
  /* The group of each process is the fifth field of its stat, after the
   * command, which is in parentheses and may contain anything */
  std::size_t total = 0;
  DIR *proc = opendir("/proc");
  struct dirent *e;
  if (proc == NULL)
    return 0;
  while ((e = readdir(proc)) != NULL) {
    char path[300];               /* "/proc/" + a directory name + "/stat" */
    const char *close_paren;
    char state;
    int ppid, pgrp;
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    snprintf(path, sizeof(path), "/proc/%s/stat", e->d_name);
    std::string s = read_small_file(path);
    close_paren = strrchr(s.c_str(), ')');
    if (close_paren == NULL ||
        sscanf(close_paren + 1, " %c %d %d", &state, &ppid, &pgrp) != 3)
      continue;
    if (pgrp == (int)pgid)
      total += provsql_process_rss((pid_t)atoi(e->d_name));
  }
  closedir(proc);
  return total;
#elif defined(__APPLE__)
  int bytes = proc_listpids(PROC_PGRP_ONLY, (uint32_t)pgid, NULL, 0);
  std::size_t total = 0;
  if (bytes <= 0)
    return 0;
  std::vector<pid_t> pids((std::size_t)bytes / sizeof(pid_t) + 16);
  bytes = proc_listpids(PROC_PGRP_ONLY, (uint32_t)pgid, pids.data(),
                        (int)(pids.size() * sizeof(pid_t)));
  if (bytes <= 0)
    return 0;
  for (std::size_t i = 0; i < (std::size_t)bytes / sizeof(pid_t); ++i)
    if (pids[i] > 0)
      total += provsql_process_rss(pids[i]);
  return total;
#elif defined(__FreeBSD__)
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PGRP, (int)pgid};
  std::size_t len = 0, total = 0;
  if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0 || len == 0)
    return 0;
  std::vector<struct kinfo_proc> procs(len / sizeof(struct kinfo_proc) + 4);
  len = procs.size() * sizeof(struct kinfo_proc);
  if (sysctl(mib, 4, procs.data(), &len, NULL, 0) != 0)
    return 0;
  for (std::size_t i = 0; i < len / sizeof(struct kinfo_proc); ++i)
    total += (std::size_t)procs[i].ki_rssize * (std::size_t)getpagesize();
  return total;
#else
  (void)pgid;
  return 0;
#endif
}
