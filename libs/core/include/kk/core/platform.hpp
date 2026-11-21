#pragma once
// What differs between Linux and Windows (docs/adr/0046), in one place. On Linux each function is the
// POSIX call the code used before; on Windows its Win32 counterpart.
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace kk::core {

namespace fs = std::filesystem;

int current_pid();
// A process with this id exists (a lock or a marker whose owner is gone is stale).
bool pid_alive(int pid);
// Ends the process at once (SIGKILL; Windows: TerminateProcess).
bool pid_kill(int pid);
// Asks the process to stop like Ctrl+C (SIGINT). Windows has no such signal for another console: there it
// ends the process (pid_kill).
bool pid_interrupt(int pid);

std::tm utc_tm(std::time_t t);      // gmtime_r
std::time_t utc_time(std::tm tm);   // timegm
std::tm local_tm(std::time_t t);    // localtime_r

void set_env(const std::string& name, const std::string& value);
void unset_env(const std::string& name);
// "NAME=value" of this process.
std::vector<std::string> environment();

// The user's home (HOME; Windows: USERPROFILE).
fs::path home_dir();
// Where settings go: $XDG_CONFIG_HOME, else ~/.config; Windows: $XDG_CONFIG_HOME when set (tests), else %APPDATA%.
fs::path config_home();
// Where data goes: $XDG_DATA_HOME, else ~/.local/share; Windows: $XDG_DATA_HOME when set, else %LOCALAPPDATA%.
fs::path data_home();

// Creates the file only if it does not exist (O_CREAT | O_EXCL): true when this call made it.
bool create_exclusive(const fs::path& p, const std::string& content = {});

// An executable file (Linux: the x bit; Windows: a file with an extension of PATHEXT).
bool is_executable(const fs::path& p);
// `name` looked up in PATH (Windows: also with the extensions of PATHEXT); `name` itself when not found or
// when it names a path.
std::string find_in_path(const std::string& name);
char path_list_separator();  // ':' / ';'

// An exclusive lock on a file, held while the object lives (flock; Windows: LockFileEx). `on_wait` is called
// once when another process holds it (then this one waits).
class FileLock {
 public:
  explicit FileLock(const fs::path& p, const std::function<void()>& on_wait = {});
  ~FileLock();
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  [[nodiscard]] bool locked() const { return locked_; }

 private:
  std::intptr_t handle_ = -1;
  bool locked_ = false;
};

// Opens a file, a folder or a web address with the desktop's program (xdg-open; Windows: ShellExecute).
// The caller never waits. False when it cannot start.
bool open_with_desktop(const std::string& target);

}  // namespace kk::core
