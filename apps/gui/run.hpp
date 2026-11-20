#pragma once
// "Build and run" in karkas-gui (docs/adr/0037). The green triangle runs `karkas run` — the command a
// user would type — as a child of the GUI in a terminal of its own (a pseudo-terminal: the program
// writes its lines as it would to a terminal and reads what is typed into the Run tab). The build's
// output and then the program's stream into the Run tab; Stop ends the whole process group (karkas,
// the build, the program), killing it if it does not end in a few seconds.
#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kk::gui {

struct RunLine {
  enum class Kind { Out, Info, Warning, Error };
  Kind kind = Kind::Out;
  std::string text;
};

enum class RunPhase { Idle, Building, Running };

// How the last run ended.
struct RunEnd {
  bool ended = false;         // a run has ended (false: none yet, or one runs now)
  bool started = false;       // the program started (karkas said "running: ...")
  bool build_failed = false;  // karkas ended before the program: the build failed
  bool nothing_to_run = false;  // karkas found no program to run ([project] run_command)
  bool stopped = false;       // by Stop
  int exit_code = -1;         // of the program (of karkas when the program never started)
  int signal = 0;             // the process was ended by this signal
  double seconds = 0;         // the whole run, build included
};

// The karkas arguments of a run.
std::vector<std::string> run_args(bool build);

// The output of a terminal as lines: "\n" ends a line ("\r\n" too), a lone '\r' starts the line
// again (progress bars), backspace takes a character back, tabs become spaces, escape sequences
// (colours, cursor moves) and other control characters are dropped. A sequence or a line may be
// cut between two pieces.
class TerminalText {
 public:
  static constexpr std::size_t kMaxLine = 4000;  // a longer line is cut into lines of this size
  void feed(std::string_view chunk, std::vector<std::string>& lines);
  [[nodiscard]] const std::string& partial() const { return line_; }  // the line not ended yet
  std::string take_partial() { return std::exchange(line_, {}); }

 private:
  enum class Esc { None, Start, Csi, Osc, OscEsc };
  std::string line_;
  Esc esc_ = Esc::None;
  bool cr_ = false;  // '\r' came last: anything but '\n' writes the line anew
};

class RunSession {
 public:
  static constexpr std::size_t kMaxLines = 5000;
  static constexpr double kKillAfter = 3.0;  // seconds after Stop

  // `cli`: the karkas executable (tests pass a fake one); `wake` redraws the GUI.
  RunSession(std::filesystem::path root, std::filesystem::path cli, std::function<void()> wake);
  ~RunSession();  // a running program is ended (SIGTERM, then SIGKILL)
  RunSession(const RunSession&) = delete;
  RunSession& operator=(const RunSession&) = delete;

  // Starts `karkas run` (with the build, or the program as it was built); false with an Error line
  // when a run is going on or it cannot start. The lines of the previous run are cleared.
  bool start(bool build);
  void stop();  // SIGTERM to the process group; SIGKILL kKillAfter seconds later
  // A line typed into the program's terminal (Enter included); false when nothing runs.
  bool send_input(const std::string& line);
  [[nodiscard]] RunPhase phase() const;
  [[nodiscard]] bool busy() const { return phase() != RunPhase::Idle; }
  [[nodiscard]] bool stopping() const;
  [[nodiscard]] double elapsed() const;       // seconds: of the run going on, else of the last one
  [[nodiscard]] std::string program() const;  // what karkas said it runs ("" before that)
  [[nodiscard]] RunEnd last_end() const;
  [[nodiscard]] std::uint64_t serial() const;  // changes with every line and state change
  [[nodiscard]] std::vector<RunLine> lines() const;
  [[nodiscard]] std::string partial_line() const;  // the line the program writes now (a prompt)
  void clear();  // forgets the lines (not while a run is going on)
  bool wait(std::chrono::milliseconds timeout);  // until the run ends (tests); false on timeout

 private:
  void reader(int master, pid_t pid);
  void add_locked(RunLine l);
  void line_locked(std::string text);  // one line of the terminal: its kind, the phase it starts
  std::filesystem::path root_, cli_;
  std::function<void()> wake_;
  mutable std::mutex mu_;
  std::vector<RunLine> lines_;
  TerminalText term_;
  std::uint64_t serial_ = 0;
  RunPhase phase_ = RunPhase::Idle;
  pid_t pid_ = 0;
  int master_ = -1;
  std::string program_;
  RunEnd end_;
  std::chrono::steady_clock::time_point started_at_{};
  std::optional<std::chrono::steady_clock::time_point> stop_at_;
  bool killed_ = false;
  bool quit_ = false;
  std::thread thread_;
};

}  // namespace kk::gui
