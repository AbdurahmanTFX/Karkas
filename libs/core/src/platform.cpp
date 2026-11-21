#include "kk/core/platform.hpp"

#include <cstdlib>

#include "kk/core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <io.h>
#include <process.h>

#include "kk/core/win_process.hpp"
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
extern char** environ;
#endif

namespace kk::core {

namespace {
std::string env_or(const char* name, const std::string& fallback = {}) {
  const char* v = std::getenv(name);
  return v != nullptr && *v != '\0' ? std::string(v) : fallback;
}
}  // namespace

#ifdef _WIN32

int current_pid() { return static_cast<int>(::GetCurrentProcessId()); }

bool pid_alive(int pid) {
  if (pid <= 0) return false;
  HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) return ::GetLastError() == ERROR_ACCESS_DENIED;  // exists, not ours
  bool alive = ::WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  ::CloseHandle(h);
  return alive;
}

bool pid_kill(int pid) {
  if (pid <= 0) return false;
  HANDLE h = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (h == nullptr) return false;
  bool ok = ::TerminateProcess(h, 137) != 0;
  ::CloseHandle(h);
  return ok;
}

bool pid_interrupt(int pid) { return pid_kill(pid); }

std::tm utc_tm(std::time_t t) {
  std::tm tm{};
  ::gmtime_s(&tm, &t);
  return tm;
}

std::time_t utc_time(std::tm tm) { return ::_mkgmtime(&tm); }

std::tm local_tm(std::time_t t) {
  std::tm tm{};
  ::localtime_s(&tm, &t);
  return tm;
}

void set_env(const std::string& name, const std::string& value) {
  ::SetEnvironmentVariableW(win::widen(name).c_str(), win::widen(value).c_str());
  ::_putenv_s(name.c_str(), value.c_str());  // the CRT's copy, which getenv reads
}

void unset_env(const std::string& name) {
  ::SetEnvironmentVariableW(win::widen(name).c_str(), nullptr);
  ::_putenv_s(name.c_str(), "");
}

std::vector<std::string> environment() {
  std::vector<std::string> out;
  wchar_t* block = ::GetEnvironmentStringsW();
  if (block == nullptr) return out;
  for (const wchar_t* p = block; *p != L'\0'; p += std::wcslen(p) + 1) {
    if (*p != L'=') out.push_back(win::narrow(p));  // "=C:=C:\..." entries are cmd.exe's
  }
  ::FreeEnvironmentStringsW(block);
  return out;
}

fs::path home_dir() {
  if (auto h = env_or("USERPROFILE"); !h.empty()) return fs::path(win::widen(h));
  return fs::path(win::widen(env_or("HOMEDRIVE", "C:") + env_or("HOMEPATH", "\\")));
}

fs::path config_home() {
  if (auto x = env_or("XDG_CONFIG_HOME"); !x.empty()) return fs::path(win::widen(x));
  if (auto a = env_or("APPDATA"); !a.empty()) return fs::path(win::widen(a));
  return home_dir() / "AppData" / "Roaming";
}

fs::path data_home() {
  if (auto x = env_or("XDG_DATA_HOME"); !x.empty()) return fs::path(win::widen(x));
  if (auto a = env_or("LOCALAPPDATA"); !a.empty()) return fs::path(win::widen(a));
  return home_dir() / "AppData" / "Local";
}

bool create_exclusive(const fs::path& p, const std::string& content) {
  HANDLE h = ::CreateFileW(p.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  if (!content.empty()) ::WriteFile(h, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
  ::CloseHandle(h);
  return true;
}

namespace {
std::vector<std::string> path_exts() {
  std::vector<std::string> exts;
  for (auto& e : split(env_or("PATHEXT", ".COM;.EXE;.BAT;.CMD"), ';')) {
    if (!e.empty()) exts.push_back(to_lower(e));
  }
  return exts;
}
}  // namespace

bool is_executable(const fs::path& p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) return false;
  auto ext = to_lower(p.extension().string());
  for (const auto& e : path_exts()) {
    if (ext == e) return true;
  }
  return false;
}

std::string find_in_path(const std::string& name) {
  if (name.find_first_of("/\\") != std::string::npos || name.find(':') != std::string::npos) return name;
  auto exts = path_exts();
  bool has_ext = false;
  auto lower = to_lower(name);
  for (const auto& e : exts) has_ext = has_ext || lower.ends_with(e);
  for (const auto& dir : split(env_or("PATH"), ';')) {
    if (dir.empty()) continue;
    auto base = fs::path(win::widen(dir)) / fs::path(win::widen(name));
    std::error_code ec;
    if (has_ext && fs::is_regular_file(base, ec)) return base.string();
    for (const auto& e : exts) {
      auto cand = base;
      cand += e;
      if (fs::is_regular_file(cand, ec)) return cand.string();
    }
  }
  return name;
}

char path_list_separator() { return ';'; }

FileLock::FileLock(const fs::path& p, const std::function<void()>& on_wait) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  HANDLE h = ::CreateFileW(p.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  handle_ = reinterpret_cast<std::intptr_t>(h);
  OVERLAPPED ov{};
  if (::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
    locked_ = true;
    return;
  }
  if (on_wait) on_wait();
  OVERLAPPED ov2{};
  locked_ = ::LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov2) != 0;
}

FileLock::~FileLock() {
  if (handle_ == -1) return;
  HANDLE h = reinterpret_cast<HANDLE>(handle_);
  if (locked_) {
    OVERLAPPED ov{};
    ::UnlockFileEx(h, 0, 1, 0, &ov);
  }
  ::CloseHandle(h);
}

bool open_with_desktop(const std::string& target) {
  auto r = reinterpret_cast<std::intptr_t>(::ShellExecuteW(nullptr, L"open", win::widen(target).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return r > 32;
}

#else  // POSIX

int current_pid() { return static_cast<int>(::getpid()); }

bool pid_alive(int pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno != ESRCH); }

bool pid_kill(int pid) { return pid > 0 && ::kill(pid, SIGKILL) == 0; }

bool pid_interrupt(int pid) { return pid > 0 && ::kill(pid, SIGINT) == 0; }

std::tm utc_tm(std::time_t t) {
  std::tm tm{};
  ::gmtime_r(&t, &tm);
  return tm;
}

std::time_t utc_time(std::tm tm) { return ::timegm(&tm); }

std::tm local_tm(std::time_t t) {
  std::tm tm{};
  ::localtime_r(&t, &tm);
  return tm;
}

void set_env(const std::string& name, const std::string& value) { ::setenv(name.c_str(), value.c_str(), 1); }

void unset_env(const std::string& name) { ::unsetenv(name.c_str()); }

std::vector<std::string> environment() {
  std::vector<std::string> out;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) out.emplace_back(*e);
  return out;
}

fs::path home_dir() { return env_or("HOME", "/tmp"); }

fs::path config_home() {
  if (auto x = env_or("XDG_CONFIG_HOME"); !x.empty()) return x;
  return home_dir() / ".config";
}

fs::path data_home() {
  if (auto x = env_or("XDG_DATA_HOME"); !x.empty()) return x;
  return home_dir() / ".local" / "share";
}

bool create_exclusive(const fs::path& p, const std::string& content) {
  int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  if (!content.empty()) (void)!::write(fd, content.data(), content.size());
  ::close(fd);
  return true;
}

bool is_executable(const fs::path& p) {
  struct stat st {};
  return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR) != 0;
}

std::string find_in_path(const std::string& name) {
  if (name.find('/') != std::string::npos) return name;
  for (const auto& dir : split(env_or("PATH", "/usr/bin:/bin"), ':')) {
    auto p = fs::path(dir) / name;
    if (::access(p.c_str(), X_OK) == 0) return p.string();
  }
  return name;
}

char path_list_separator() { return ':'; }

FileLock::FileLock(const fs::path& p, const std::function<void()>& on_wait) {
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  // Close-on-exec: neither the build's processes nor a program started after the build keep it.
  int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return;
  handle_ = fd;
  if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
    locked_ = true;
    return;
  }
  if (on_wait) on_wait();
  locked_ = ::flock(fd, LOCK_EX) == 0;
}

FileLock::~FileLock() {
  if (handle_ < 0) return;
  if (locked_) ::flock(static_cast<int>(handle_), LOCK_UN);
  ::close(static_cast<int>(handle_));
}

bool open_with_desktop(const std::string& target) {
  const char* argv[] = {"xdg-open", target.c_str(), nullptr};
  pid_t pid = 0;
  return ::posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0;
}

#endif

}  // namespace kk::core
