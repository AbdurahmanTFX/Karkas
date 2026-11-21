#pragma once
// Child processes on Windows (docs/adr/0046): what posix_spawn, pipes, poll, waitpid and kill(-pgid) do for
// the Linux code. Only on Windows; the Linux code keeps its POSIX calls.
#ifdef _WIN32
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/error.hpp"

namespace kk::core::win {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);
std::filesystem::path utf8_path(std::string_view utf8);

// One argument of a command line as the MSVC runtime splits it again (CommandLineToArgvW rules).
std::wstring quote_arg(const std::wstring& arg);

struct SpawnSpec {
  enum class Io { Null, Pipe, Inherit, File, ToStdout };
  std::vector<std::string> argv;  // argv[0]: a path or a name looked up in PATH with PATHEXT
  std::string command_line;       // when set: the command line as it is (argv[0] is still the program), e.g. for cmd.exe
  std::filesystem::path cwd;      // empty: this process's
  std::optional<std::vector<std::string>> env;  // "NAME=value"; nullopt: this process's
  Io in = Io::Null;
  Io out = Io::Pipe;
  Io err = Io::Pipe;
  std::filesystem::path out_file, err_file;  // Io::File: appended to
  bool own_job = true;     // in a job of its own: kill_tree() ends it with every process it started
  bool new_console = false;  // a console window of its own (else none, or this process's when inheriting)
};

class Child {
 public:
  Child() = default;
  ~Child();
  Child(Child&& o) noexcept;
  Child& operator=(Child&& o) noexcept;
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  static kk::Result<Child> spawn(const SpawnSpec& spec);

  [[nodiscard]] int pid() const { return static_cast<int>(pid_); }
  [[nodiscard]] bool valid() const { return process_ != nullptr; }
  // The exit code once it ended.
  std::optional<int> try_wait();
  std::optional<int> wait(int timeout_ms = -1);
  // What is in the pipe now, without waiting: bytes read (0: nothing yet), -1 when the pipe is closed.
  long read_out(char* buf, std::size_t n);
  long read_err(char* buf, std::size_t n);
  bool write_in(std::string_view data);
  void close_in();
  // Ends it and everything it started (its job), or the process alone without a job.
  void kill_tree(unsigned code = 137);
  // Lets it run on its own: the handles are closed, the job does not end it.
  void detach();

 private:
  void close_all();
  void* process_ = nullptr;
  void* job_ = nullptr;
  void* in_ = nullptr;
  void* out_ = nullptr;
  void* err_ = nullptr;
  unsigned long pid_ = 0;
  std::optional<int> code_;
};

}  // namespace kk::core::win
#endif
