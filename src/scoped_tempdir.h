#ifndef PROVSQL_SCOPED_TEMPDIR_H
#define PROVSQL_SCOPED_TEMPDIR_H

extern "C" {
#include <unistd.h>
}

#include <string>
#include <vector>

#include "provsql_error.h"

namespace provsql {

/**
 * @brief RAII guard around a freshly mkdtemp'd /tmp directory.
 *
 * Wraps the boilerplate every external-tool launch shares: create a
 * private 0700 directory, write the Tseytin CNF and any sibling files
 * into it, and on scope exit unlink each tracked file and rmdir the
 * directory. If the body throws (missing tool, runtime failure,
 * malformed output), the destructor still runs and the /tmp leak is
 * avoided. Pass @c keep() in the verbose-debug arm to leave the
 * artifacts on disk for inspection.
 *
 * Best-effort: the destructor swallows unlink/rmdir errors because it
 * must not throw during stack unwinding.
 */
class ScopedTempDir {
 public:
  ScopedTempDir() {
    char buf[] = "/tmp/provsqlXXXXXX";
    if (mkdtemp(buf) == nullptr)
      throw CircuitException("Cannot create temporary directory");
    path_ = buf;
  }
  ~ScopedTempDir() {
    if (keep_) return;
    for (const auto &f : files_) (void)::unlink(f.c_str());
    (void)::rmdir(path_.c_str());
  }
  ScopedTempDir(const ScopedTempDir &) = delete;
  ScopedTempDir &operator=(const ScopedTempDir &) = delete;

  /// Build a path under the temp dir and register it for cleanup.
  std::string file(const std::string &basename) {
    std::string p = path_ + "/" + basename;
    files_.push_back(p);
    return p;
  }
  /// Also track a file we did not create through @c file() (e.g. a
  /// helper tool's scratch output sitting in @c path_).
  void track(const std::string &p) { files_.push_back(p); }
  /// Leave the directory on disk; cleanup is skipped at scope exit.
  void keep() { keep_ = true; }
  const std::string &path() const { return path_; }

 private:
  std::string path_;
  std::vector<std::string> files_;
  bool keep_ = false;
};

}  // namespace provsql

#endif  // PROVSQL_SCOPED_TEMPDIR_H
