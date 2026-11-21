#include "kk/runner/runner.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>
#include <set>
#include <tuple>

#include "kk/core/platform.hpp"
#include "kk/core/text.hpp"

namespace kk::runner {

bool is_secret_name(std::string_view name) {
  auto n = core::to_lower(name);
  for (const char* s : {"key", "token", "secret", "password", "passwd", "credential", "auth", "cookie"}) {
    if (core::contains(n, s)) return true;
  }
  return false;
}

std::map<std::string, std::string> clean_environment(const std::map<std::string, std::string>& extra) {
  static const std::set<std::string> keep = {"PATH", "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "LC_CTYPE",
                                             "TERM", "TMPDIR", "SHELL", "CC", "CXX", "CFLAGS", "CXXFLAGS", "LDFLAGS",
                                             "CCACHE_DIR", "XDG_CACHE_HOME", "PKG_CONFIG_PATH", "CMAKE_PREFIX_PATH",
                                             "CMAKE_GENERATOR", "NINJA_STATUS", "SOURCE_DATE_EPOCH",
                                             // a compiler cache for the builds of agents and tests (ADR 0029)
                                             "CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER", "CCACHE_BASEDIR",
                                             "CCACHE_NOHASHDIR", "CCACHE_SLOPPINESS", "CCACHE_DISABLE",
#ifdef _WIN32
                                             // what programs on Windows need to start at all (docs/adr/0046)
                                             "SYSTEMROOT", "SYSTEMDRIVE", "WINDIR", "COMSPEC", "PATHEXT", "TEMP", "TMP",
                                             "USERPROFILE", "USERNAME", "HOMEDRIVE", "HOMEPATH", "APPDATA", "LOCALAPPDATA",
                                             "PROGRAMDATA", "PROGRAMFILES", "PROGRAMFILES(X86)", "PROGRAMW6432",
                                             "COMMONPROGRAMFILES", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE", "OS",
#endif
                                             };
  std::map<std::string, std::string> env;
  for (const auto& kv : core::environment()) {
    auto eq = kv.find('=');
    if (eq == std::string::npos) continue;
    std::string k = kv.substr(0, eq);
#ifdef _WIN32
    std::string name = k;  // names on Windows have no case ("Path")
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
#else
    const std::string& name = k;
#endif
    if (keep.contains(name) && !is_secret_name(k)) env[k] = kv.substr(eq + 1);
  }
  for (const auto& [k, v] : extra) {
    if (!is_secret_name(k)) env[k] = v;
  }
  return env;
}

namespace {

std::string find_program(const std::string& name) { return core::find_in_path(name); }

}  // namespace

kk::Result<RunResult> run(const RunOptions& opts) {
  if (opts.argv.empty()) return kk::fail("run", "empty command");
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::string> argv = opts.argv;
  argv[0] = find_program(argv[0]);
  reproc::process proc;
  reproc::options o;
  std::vector<std::pair<std::string, std::string>> env;
  if (opts.clean_env) {
    o.env.behavior = reproc::env::empty;
    for (const auto& [k, v] : clean_environment(opts.env)) env.emplace_back(k, v);
  } else if (!opts.unset_env.empty()) {
    // The inherited environment without the named variables.
    o.env.behavior = reproc::env::empty;
    std::set<std::string> drop(opts.unset_env.begin(), opts.unset_env.end());
    for (const auto& kv : core::environment()) {
      auto eq = kv.find('=');
      if (eq == std::string::npos) continue;
      std::string k = kv.substr(0, eq);
      if (!drop.contains(k) && !opts.env.contains(k)) env.emplace_back(k, kv.substr(eq + 1));
    }
    for (const auto& [k, v] : opts.env) env.emplace_back(k, v);
  } else {
    o.env.behavior = reproc::env::extend;
    for (const auto& [k, v] : opts.env) env.emplace_back(k, v);
  }
  o.env.extra = reproc::env(env);
  std::string cwd = opts.cwd.string();
  if (!cwd.empty()) o.working_directory = cwd.c_str();
  o.redirect.err.type = reproc::redirect::stdout_;
  o.redirect.in.type = reproc::redirect::discard;
  o.deadline = reproc::milliseconds(opts.timeout.count());
  o.stop = {{reproc::stop::terminate, reproc::milliseconds(2000)}, {reproc::stop::kill, reproc::milliseconds(2000)}, {}};
  std::error_code ec = proc.start(argv, o);
  if (ec) return kk::fail("run", fmt::format("cannot start {}: {}", opts.argv[0], ec.message()));
  RunResult r;
  auto sink = [&](reproc::stream, const uint8_t* buf, size_t n) -> std::error_code {
    if (r.output.size() < opts.max_output) r.output.append(reinterpret_cast<const char*>(buf), n);
    if (opts.on_output) opts.on_output(std::string_view(reinterpret_cast<const char*>(buf), n));
    return {};
  };
  ec = reproc::drain(proc, sink, reproc::sink::null);
  if (ec == std::errc::timed_out) r.timed_out = true;
  // Its output has ended: it is ending too; its own exit code. (A terminate sent to a process that has already
  // exited reports no exit code on Windows, docs/adr/0046.)
  int status = -1;
  std::error_code wec = std::make_error_code(std::errc::timed_out);
  if (!r.timed_out) std::tie(status, wec) = proc.wait(reproc::milliseconds(2000));
  if (wec) std::tie(status, wec) = proc.stop(o.stop);
  r.exit_code = r.timed_out ? -1 : status;
  r.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
  if (r.timed_out) r.output += fmt::format("\n[karkas] killed after {} ms timeout\n", opts.timeout.count());
  return r;
}

std::string compress_build_log(std::string_view log, const fs::path& root, std::size_t max_lines) {
  std::string prefix = root.empty() ? "" : root.string() + "/";
  std::vector<std::string> out;
  std::set<std::string> seen;
  std::size_t extra = 0;
  // Raw lines: core::split trims, and the indentation is what marks CMake's message lines.
  std::vector<std::string> all;
  for (std::size_t start = 0; start < log.size();) {
    auto end = log.find('\n', start);
    if (end == std::string_view::npos) end = log.size();
    auto raw = log.substr(start, end - start);
    if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
    if (!core::trim(raw).empty()) all.emplace_back(raw);
    start = end + 1;
  }
  for (std::size_t i = 0; i < all.size(); ++i) {
    auto line = all[i];
    auto l = core::to_lower(line);
    bool is_err = core::contains(l, "error:") || core::contains(l, "undefined reference") || core::contains(l, "ld returned") ||
                  core::contains(l, "fatal error") || core::contains(l, "cmake error") ||
                  core::contains(l, "no such file or directory") || core::contains(l, "killed after");
    if (!is_err) continue;
    // CMake puts the message on the indented lines under "CMake Error at <file>:<line> (<command>):";
    // without them the error says where, not what.
    if (core::contains(l, "cmake error")) {
      for (std::size_t k = i + 1, n = 0; k < all.size() && n < 4; ++k, ++n) {
        const auto& next = all[k];
        if (next.empty() || (next.front() != ' ' && next.front() != '\t')) break;
        line += " " + std::string(core::trim(next));
        i = k;
      }
    }
    if (!prefix.empty()) line = core::replace_all(line, prefix, "");
    line = std::string(core::trim(line));
    if (line.size() > 300) line = line.substr(0, 300) + "...";
    if (!seen.insert(line).second) continue;
    if (out.size() < max_lines) {
      out.push_back(line);
    } else {
      ++extra;
    }
  }
  if (extra > 0) out.push_back(fmt::format("... {} more error lines", extra));
  return core::join(out, "\n");
}

std::string compress_build_warnings(std::string_view log, const fs::path& root, std::size_t max_lines) {
  std::string prefix = root.empty() ? "" : root.string() + "/";
  std::vector<std::string> out;
  std::set<std::string> seen;
  std::size_t extra = 0;
  for (const auto& raw : core::split(log, '\n')) {
    auto line = std::string(core::trim(raw));
    if (!core::contains(core::to_lower(line), ": warning:")) continue;
    if (!prefix.empty()) line = core::replace_all(line, prefix, "");
    if (line.size() > 300) line = line.substr(0, 300) + "...";
    if (!seen.insert(line).second) continue;
    if (out.size() < max_lines) {
      out.push_back(line);
    } else {
      ++extra;
    }
  }
  if (extra > 0) out.push_back(fmt::format("... {} more warning lines", extra));
  return core::join(out, "\n");
}

std::vector<std::string> shell_argv(const std::string& command) {
#ifdef _WIN32
  return {"cmd.exe", "/d", "/s", "/c", command};
#else
  return {"sh", "-c", command};
#endif
}

kk::Result<BuildResult> build_command(const std::string& command, const fs::path& cwd, std::chrono::seconds timeout,
                                      const OutputSink& on_output) {
  BuildResult br;
  RunOptions ro{shell_argv(command), cwd, timeout};
  ro.on_output = on_output;
  auto r = run(ro);
  if (!r) return std::unexpected(r.error());
  br.log = r->output;
  br.ok = r->ok();
  br.timed_out = r->timed_out;
  br.duration = r->duration;
  br.warnings = compress_build_warnings(r->output, cwd);
  if (!br.ok) {
    br.summary = compress_build_log(r->output, cwd);
    if (br.summary.empty()) {
      br.summary = r->timed_out ? fmt::format("build command timed out after {} s: {}", timeout.count(), command)
                                : fmt::format("build command failed (exit {}): {}", r->exit_code, command);
    }
  }
  return br;
}

kk::Result<BuildResult> build(const BuildOptions& opts) {
  BuildResult br;
  auto bdir = opts.build_dir.empty() ? opts.source / "build" : opts.build_dir;
  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::string> conf = {"cmake", "-S", opts.source.string(), "-B", bdir.string(), "-G", "Ninja",
                                   "-DCMAKE_BUILD_TYPE=Debug"};
  if (!opts.doctest_include.empty()) conf.push_back("-DKARKAS_DOCTEST_DIR=" + opts.doctest_include);
  conf.insert(conf.end(), opts.cmake_args.begin(), opts.cmake_args.end());
  RunOptions co{conf, opts.source, opts.timeout};
  co.on_output = opts.on_output;
  auto c = run(co);
  if (!c) return std::unexpected(c.error());
  br.log = c->output;
  if (!c->ok()) {
    br.timed_out = c->timed_out;
    br.summary = compress_build_log(c->output, opts.source);
    if (br.summary.empty()) br.summary = "cmake configure failed";
    br.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    return br;
  }
  std::vector<std::string> b = {"cmake", "--build", bdir.string()};
  if (!opts.target.empty()) {
    b.push_back("--target");
    b.push_back(opts.target);
  }
  auto remaining = opts.timeout - std::chrono::duration_cast<std::chrono::seconds>(c->duration);
  if (remaining.count() < 10) remaining = std::chrono::seconds(10);
  RunOptions ro{b, opts.source, remaining};
  ro.on_output = opts.on_output;
  auto r = run(ro);
  if (!r) return std::unexpected(r.error());
  br.log += r->output;
  br.ok = r->ok();
  br.timed_out = r->timed_out;
  br.warnings = compress_build_warnings(r->output, opts.source);
  br.summary = br.ok ? "" : compress_build_log(r->output, opts.source);
  if (!br.ok && br.summary.empty()) br.summary = "build failed";
  br.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
  return br;
}

std::vector<BlockTests> parse_ctest_output(std::string_view out, const fs::path& root) {
  // ctest -V prints "Start N: name" and prefixes every line of test N with "N: ".
  std::map<int, std::string> names;
  std::map<int, std::vector<std::string>> lines;
  std::map<int, bool> passed;
  static const std::regex start_re(R"(^\s*Start\s+(\d+):\s+(\S+))");
  static const std::regex line_re(R"(^(\d+): (.*)$)");
  static const std::regex result_re(R"(Test\s+#(\d+):\s+(\S+)\s+\.*\s*(\*{0,3})(Passed|Failed|Not Run|Timeout|Exception|\S+))");
  static const std::regex summary_re(R"(\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed)");
  for (const auto& l : core::split(out, '\n', false)) {
    std::smatch m;
    if (std::regex_search(l, m, start_re)) {
      names[std::stoi(m[1])] = m[2];
    } else if (std::regex_search(l, m, result_re)) {
      int n = std::stoi(m[1]);
      names.emplace(n, m[2]);
      passed[n] = m[4] == "Passed";
    } else if (std::regex_match(l, m, line_re)) {
      lines[std::stoi(m[1])].push_back(m[2]);
    }
  }
  std::map<std::string, BlockTests> blocks;
  std::string prefix = root.empty() ? "" : root.string() + "/";
  for (const auto& [n, name] : names) {
    auto slash = name.rfind('/');
    std::string block = slash == std::string::npos ? name : name.substr(0, slash);
    std::string kind = slash == std::string::npos ? "unit" : name.substr(slash + 1);
    if (auto colon = kind.find(':'); colon != std::string::npos) kind.resize(colon);  // "unit:<program>", a standalone test
    auto& bt = blocks[block];
    bt.block = block;
    Counts* c = kind == "contract" ? &bt.contract : kind == "integration" ? &bt.integration : &bt.unit;
    bool found = false;
    std::string current_case;
    std::string pending_loc;
    for (const auto& l : lines[n]) {
      std::smatch m;
      if (std::regex_search(l, m, summary_re)) {
        int total = std::stoi(m[1]);
        int pass = std::stoi(m[2]);
        c->total += total;
        c->pass += pass;
        found = true;
      } else if (l.starts_with("TEST CASE:")) {
        current_case = std::string(core::trim(std::string_view(l).substr(10)));
      } else if (core::contains(l, ": ERROR: ") || core::contains(l, ": FATAL ERROR: ")) {
        auto loc = l.substr(0, l.find(": ERROR"));
        if (!prefix.empty()) loc = core::replace_all(loc, prefix, "");
        std::string entry = fmt::format("{}: {} ({})", kind, current_case.empty() ? "?" : current_case, loc);
        if (std::find(bt.failures.begin(), bt.failures.end(), entry) == bt.failures.end()) bt.failures.push_back(entry);
      }
    }
    bool ok = passed.contains(n) && passed[n];
    if (!found) {
      // Crash, missing executable or a non-doctest test: count it as one case.
      c->total += 1;
      c->pass += ok ? 1 : 0;
      bool missing = false;
      for (const auto& l : lines[n]) missing = missing || core::contains(l, "Could not find executable");
      if (missing) bt.build_failed = true;
      if (!ok) bt.failures.push_back(fmt::format("{}: {} (no doctest summary: crash or missing executable)", kind, name));
    }
  }
  std::vector<BlockTests> v;
  for (auto& [_, b] : blocks) v.push_back(std::move(b));
  return v;
}

std::string summarize(const std::vector<BlockTests>& blocks) {
  std::string out;
  for (const auto& b : blocks) {
    std::string line = "tests " + b.block;
    if (b.contract.total > 0) line += fmt::format(" contract {}/{}", b.contract.pass, b.contract.total);
    if (b.unit.total > 0) line += fmt::format(" unit {}/{}", b.unit.pass, b.unit.total);
    if (b.integration.total > 0) line += fmt::format(" integration {}/{}", b.integration.pass, b.integration.total);
    if (b.build_failed) line += " build=failed";
    out += line + "\n";
    std::size_t shown = 0;
    for (const auto& f : b.failures) {
      if (++shown > 10) {
        out += fmt::format("fail ... {} more\n", b.failures.size() - 10);
        break;
      }
      out += "fail " + f + "\n";
    }
  }
  return out.empty() ? "tests none\n" : out;
}

kk::Result<TestSummary> test(const TestOptions& opts) {
  TestSummary s;
  auto bdir = opts.build_dir.empty() ? opts.source / "build" : opts.build_dir;
  std::vector<std::string> argv = {"ctest", "--test-dir", bdir.string(), "-V", "--timeout", std::to_string(opts.timeout.count())};
  if (!opts.blocks.empty()) {
    std::vector<std::string> alts;
    for (const auto& b : opts.blocks) alts.push_back(core::replace_all(b, ".", "\\."));
    argv.push_back("-L");
    argv.push_back("^(" + core::join(alts, "|") + ")$");
  }
  std::map<std::string, std::string> env;
  if (!opts.probe_dir.empty()) {
    std::error_code ec;
    fs::create_directories(opts.probe_dir, ec);
    env["KARKAS_PROBE_DIR"] = opts.probe_dir.string();
    if (!opts.probe_run.empty()) env["KARKAS_PROBE_RUN"] = opts.probe_run;
  }
  auto total_timeout = std::chrono::milliseconds(opts.timeout) * 10;
  auto r = run({argv, opts.source, total_timeout, env});
  if (!r) return std::unexpected(r.error());
  s.log = r->output;
  s.blocks = parse_ctest_output(r->output, opts.source);
  s.ok = r->ok() && std::all_of(s.blocks.begin(), s.blocks.end(), [](const BlockTests& b) {
    return !b.build_failed && b.contract.pass == b.contract.total && b.unit.pass == b.unit.total &&
           b.integration.pass == b.integration.total;
  });
  s.text = summarize(s.blocks);
  if (core::contains(r->output, "No tests were found")) s.text = "tests none found\n";
  return s;
}

}  // namespace kk::runner
