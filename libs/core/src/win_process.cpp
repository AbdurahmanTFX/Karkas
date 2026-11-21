// Child processes on Windows (docs/adr/0046). Built on Windows only.
#ifdef _WIN32
#include "kk/core/win_process.hpp"

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <utility>

#include "kk/core/platform.hpp"
#include "kk/core/text.hpp"

namespace kk::core::win {

std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string narrow(std::wstring_view w) {
  if (w.empty()) return {};
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::filesystem::path utf8_path(std::string_view utf8) { return std::filesystem::path(widen(utf8)); }

std::wstring quote_arg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (auto it = arg.begin();; ++it) {
    std::size_t slashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++slashes;
    }
    if (it == arg.end()) {
      out.append(slashes * 2, L'\\');  // before the closing quote: doubled
      break;
    }
    if (*it == L'"') {
      out.append(slashes * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(slashes, L'\\');
      out.push_back(*it);
    }
  }
  out.push_back(L'"');
  return out;
}

namespace {

// Handles are made inheritable only around CreateProcess, under this lock, and closed right after: a child
// started at the same time by another thread does not inherit (and keep open) the pipes of this one.
std::mutex g_spawn_mu;

HANDLE open_null(bool write) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
  return ::CreateFileW(L"NUL", write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
}

HANDLE open_append(const std::filesystem::path& p) {
  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  return ::CreateFileW(p.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

void close_handle(void*& h) {
  if (h != nullptr && h != INVALID_HANDLE_VALUE) ::CloseHandle(static_cast<HANDLE>(h));
  h = nullptr;
}

std::string last_error_text(DWORD code) {
  wchar_t* buf = nullptr;
  ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::string s = buf != nullptr ? narrow(buf) : std::string();
  if (buf != nullptr) ::LocalFree(buf);
  return std::string(trim(s));
}

long read_now(void* h, char* buf, std::size_t n) {
  if (h == nullptr) return -1;
  DWORD avail = 0;
  if (!::PeekNamedPipe(static_cast<HANDLE>(h), nullptr, 0, nullptr, &avail, nullptr)) return -1;  // closed (broken pipe)
  if (avail == 0) return 0;
  DWORD got = 0;
  DWORD want = static_cast<DWORD>(std::min<std::size_t>(n, avail));
  if (!::ReadFile(static_cast<HANDLE>(h), buf, want, &got, nullptr)) return -1;
  return static_cast<long>(got);
}

}  // namespace

Child::~Child() { close_all(); }

Child::Child(Child&& o) noexcept { *this = std::move(o); }

Child& Child::operator=(Child&& o) noexcept {
  if (this == &o) return *this;
  close_all();
  process_ = std::exchange(o.process_, nullptr);
  job_ = std::exchange(o.job_, nullptr);
  in_ = std::exchange(o.in_, nullptr);
  out_ = std::exchange(o.out_, nullptr);
  err_ = std::exchange(o.err_, nullptr);
  pid_ = std::exchange(o.pid_, 0);
  code_ = std::exchange(o.code_, std::nullopt);
  return *this;
}

void Child::close_all() {
  close_handle(in_);
  close_handle(out_);
  close_handle(err_);
  close_handle(process_);
  close_handle(job_);  // a job made with KILL_ON_JOB_CLOSE ends what is left in it
}

kk::Result<Child> Child::spawn(const SpawnSpec& spec) {
  if (spec.argv.empty()) return kk::fail("spawn", "empty command");
  using Io = SpawnSpec::Io;
  Child c;
  // The program: a .cmd or .bat runs through cmd.exe (CreateProcess runs only real programs).
  std::string program = find_in_path(spec.argv[0]);
  std::wstring cmdline;
  auto ext = to_lower(std::filesystem::path(widen(program)).extension().string());
  const bool batch = ext == ".cmd" || ext == ".bat";
  for (std::size_t i = 0; i < spec.argv.size(); ++i) {
    if (i > 0) cmdline += L' ';
    cmdline += quote_arg(widen(i == 0 ? program : spec.argv[i]));
  }
  std::wstring app;
  if (!spec.command_line.empty()) {
    app = widen(program);
    cmdline = widen(spec.command_line);
  } else if (batch) {
    const char* comspec = std::getenv("COMSPEC");
    app = widen(comspec != nullptr ? comspec : "C:\\Windows\\System32\\cmd.exe");
    cmdline = L"cmd.exe /d /s /c \"" + cmdline + L"\"";
  }

  HANDLE child_in = nullptr, child_out = nullptr, child_err = nullptr;
  HANDLE our_in = nullptr, our_out = nullptr, our_err = nullptr;
  auto pipe = [](HANDLE* read, HANDLE* write) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    return ::CreatePipe(read, write, &sa, 1 << 16) != 0;
  };
  auto cleanup = [&] {
    for (HANDLE* h : {&child_in, &child_out, &child_err, &our_in, &our_out, &our_err}) {
      if (*h != nullptr && *h != INVALID_HANDLE_VALUE) ::CloseHandle(*h);
      *h = nullptr;
    }
  };
  switch (spec.in) {
    case Io::Pipe: if (!pipe(&child_in, &our_in)) { cleanup(); return kk::fail("spawn", "cannot make a pipe"); } break;
    case Io::Inherit: child_in = ::GetStdHandle(STD_INPUT_HANDLE); break;
    default: child_in = open_null(false); break;
  }
  switch (spec.out) {
    case Io::Pipe: if (!pipe(&our_out, &child_out)) { cleanup(); return kk::fail("spawn", "cannot make a pipe"); } break;
    case Io::Inherit: child_out = ::GetStdHandle(STD_OUTPUT_HANDLE); break;
    case Io::File: child_out = open_append(spec.out_file); break;
    default: child_out = open_null(true); break;
  }
  switch (spec.err) {
    case Io::Pipe: if (!pipe(&our_err, &child_err)) { cleanup(); return kk::fail("spawn", "cannot make a pipe"); } break;
    case Io::Inherit: child_err = ::GetStdHandle(STD_ERROR_HANDLE); break;
    case Io::File: child_err = open_append(spec.err_file); break;
    case Io::ToStdout: ::DuplicateHandle(::GetCurrentProcess(), child_out, ::GetCurrentProcess(), &child_err, 0, FALSE, DUPLICATE_SAME_ACCESS); break;
    default: child_err = open_null(true); break;
  }
  // Inherited std handles of this process are not ours to close.
  const bool own_in = spec.in != Io::Inherit, own_out = spec.out != Io::Inherit, own_err = spec.err != Io::Inherit;

  std::wstring env_block;
  if (spec.env) {
    for (const auto& kv : *spec.env) {
      env_block += widen(kv);
      env_block.push_back(L'\0');
    }
    env_block.push_back(L'\0');
  }
  std::wstring cwd = spec.cwd.empty() ? std::wstring() : spec.cwd.wstring();

  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = child_in;
  si.hStdOutput = child_out;
  si.hStdError = child_err;
  PROCESS_INFORMATION pi{};
  DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
  const bool inherits_console = spec.in == Io::Inherit || spec.out == Io::Inherit || spec.err == Io::Inherit;
  if (spec.new_console) {
    flags |= CREATE_NEW_CONSOLE;
  } else if (!inherits_console) {
    flags |= CREATE_NO_WINDOW;  // a console program started by the window shows no console of its own
  }
  BOOL ok = FALSE;
  DWORD err = 0;
  {
    std::lock_guard lk(g_spawn_mu);
    for (HANDLE h : {child_in, child_out, child_err}) {
      if (h != nullptr && h != INVALID_HANDLE_VALUE) ::SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    ok = ::CreateProcessW(app.empty() ? nullptr : app.c_str(), cmdline.data(), nullptr, nullptr, TRUE, flags,
                          spec.env ? env_block.data() : nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    err = ok ? 0 : ::GetLastError();
    for (HANDLE h : {child_in, child_out, child_err}) {
      if (h != nullptr && h != INVALID_HANDLE_VALUE) ::SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
    }
  }
  if (own_in && child_in != nullptr && child_in != INVALID_HANDLE_VALUE) ::CloseHandle(child_in);
  if (own_out && child_out != nullptr && child_out != INVALID_HANDLE_VALUE) ::CloseHandle(child_out);
  if (own_err && child_err != nullptr && child_err != INVALID_HANDLE_VALUE) ::CloseHandle(child_err);
  child_in = child_out = child_err = nullptr;
  if (!ok) {
    cleanup();
    return kk::fail("spawn", "cannot start " + spec.argv[0] + ": " + last_error_text(err));
  }
  if (spec.own_job) {
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
      li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
      if (::AssignProcessToJobObject(job, pi.hProcess)) {
        c.job_ = job;
      } else {
        ::CloseHandle(job);  // a job this process may not nest: the process alone then
      }
    }
  }
  ::ResumeThread(pi.hThread);
  ::CloseHandle(pi.hThread);
  c.process_ = pi.hProcess;
  c.pid_ = pi.dwProcessId;
  c.in_ = our_in;
  c.out_ = our_out;
  c.err_ = our_err;
  return c;
}

std::optional<int> Child::try_wait() { return wait(0); }

std::optional<int> Child::wait(int timeout_ms) {
  if (code_) return code_;
  if (process_ == nullptr) return std::nullopt;
  DWORD r = ::WaitForSingleObject(static_cast<HANDLE>(process_), timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms));
  if (r != WAIT_OBJECT_0) return std::nullopt;
  DWORD code = 0;
  ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
  code_ = static_cast<int>(code);
  return code_;
}

long Child::read_out(char* buf, std::size_t n) { return read_now(out_, buf, n); }

long Child::read_err(char* buf, std::size_t n) { return read_now(err_, buf, n); }

bool Child::write_in(std::string_view data) {
  if (in_ == nullptr) return false;
  while (!data.empty()) {
    DWORD put = 0;
    if (!::WriteFile(static_cast<HANDLE>(in_), data.data(), static_cast<DWORD>(data.size()), &put, nullptr)) return false;
    data.remove_prefix(put);
  }
  return true;
}

void Child::close_in() { close_handle(in_); }

void Child::kill_tree(unsigned code) {
  if (job_ != nullptr) {
    ::TerminateJobObject(static_cast<HANDLE>(job_), code);
  } else if (process_ != nullptr) {
    ::TerminateProcess(static_cast<HANDLE>(process_), code);
  }
}

void Child::detach() {
  if (job_ != nullptr) {
    // Without KILL_ON_JOB_CLOSE the processes outlive the handle.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    ::SetInformationJobObject(static_cast<HANDLE>(job_), JobObjectExtendedLimitInformation, &li, sizeof li);
  }
  close_all();
  pid_ = 0;
}

}  // namespace kk::core::win
#endif
