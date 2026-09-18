// Oversubscription Governor — child-process supervision.
//
// The distributed proof runs real OS processes: evidence publishers and the
// coordinator are separate executables, and the tests kill and restart them. A
// handle here is a real process handle, not a thread pretending to be one.
#ifndef OVERSUB_PROCESS_HPP
#define OVERSUB_PROCESS_HPP

#include <string>
#include <vector>

#include "oversub/checked.hpp"
#include "oversub/status.hpp"

namespace oversub {

struct ProcessSpec {
  std::string executable;
  std::vector<std::string> arguments;
  std::string working_directory;   // empty => inherit
  bool inherit_output{false};      // true => child shares this process's stdout/stderr
};

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  Status spawn(const ProcessSpec& spec);
  bool running();
  // Blocks until the child exits. Never returns before the exit code is known.
  Status wait(u32& exit_code);
  // Forcefully terminates the child (the crash/kill path used by tests).
  Status terminate();
  u32 pid() const { return pid_; }
  bool valid() const;

 private:
  void reset();
#if defined(_WIN32)
  void* handle_{nullptr};
#else
  int os_pid_{-1};
#endif
  u32 pid_{0};
};

// Absolute path of the running executable. Used by tests to re-execute
// themselves in a different role.
Status current_executable_path(std::string& out);
// Directory containing the running executable.
Status current_executable_directory(std::string& out);

}  // namespace oversub

#endif  // OVERSUB_PROCESS_HPP
