#pragma once
// The orchestrator chat of karkas-gui (docs/adr/0019). A message is turned into the same
// command a user would type — `karkas task "<text>"`, `karkas debug "<text>"`, or any
// "/<agent command>" — and run as a child process of the GUI, so the protocol is exactly
// the CLI's: a ticket, the descent by the orchestrator and navigators, workers, approvals.
// Its progress lines stream into the chat; Stop sends SIGINT (agents end gracefully, the
// ticket stays open), Kill ends the command's process group. The conversation is kept in
// .karkas/chat.jsonl (not in git); tickets, journals and karkas.log are never touched.
#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace kk::gui {

enum class ChatMode { Task, Bug };

struct ChatEntry {
  enum class Kind { User, Progress, Log, Result, Error, Info };
  Kind kind = Kind::Info;
  std::string text;
  std::string mode;    // user messages: "task" | "bug" | "command"
  std::string ticket;  // the ticket the command worked on (when known)
  std::int64_t ts = 0;
};

// The karkas arguments of a message: Task → task "<text>", Bug → debug "<text>",
// "/<command> args..." → that agent command (quotes keep spaces). Empty with `error` set
// for an empty message or a command the chat does not run.
std::vector<std::string> chat_command(ChatMode mode, const std::string& text, std::string* error);
// "T-0004" from "created T-0004" / "@ticket T-0004 ..."; "" when the line names none.
std::string ticket_in(const std::string& line);
// Change ids (C-NNNN) named in a text, in order, without repeats.
std::vector<std::string> changes_in(const std::string& text);
// The karkas CLI next to karkas-gui (build tree or install), else "karkas" from PATH.
std::filesystem::path find_karkas_cli();

class Chat {
 public:
  // `cli`: the karkas executable (tests pass a fake one); `wake` redraws the GUI.
  Chat(std::filesystem::path root, std::filesystem::path cli, std::function<void()> wake);
  ~Chat();  // a running command is stopped (SIGINT, then killed after a few seconds)
  Chat(const Chat&) = delete;
  Chat& operator=(const Chat&) = delete;

  // Runs the command of a message; false (with an Error entry) when busy or it cannot start.
  bool send(ChatMode mode, const std::string& text);
  void stop();  // SIGINT: like Ctrl+C in a terminal
  void kill();  // SIGKILL to the whole process group of the command
  [[nodiscard]] bool busy() const;
  // Seconds since stop() while the command still runs; nullopt when not stopping.
  [[nodiscard]] std::optional<double> stopping_for() const;
  [[nodiscard]] std::string current_ticket() const;
  [[nodiscard]] std::uint64_t serial() const;  // changes with every new entry
  [[nodiscard]] std::vector<ChatEntry> entries() const;
  void clear();  // forgets the conversation (chat.jsonl only)
  // Blocks until the command ends (tests); false on timeout.
  bool wait(std::chrono::milliseconds timeout);

 private:
  void add(ChatEntry e);
  void add_locked(ChatEntry e);
  void reader(int out_fd, int err_fd, pid_t pid);
  std::filesystem::path root_, cli_, file_;
  std::function<void()> wake_;
  mutable std::mutex mu_;
  std::vector<ChatEntry> entries_;
  std::uint64_t serial_ = 0;
  pid_t pid_ = 0;
  bool busy_ = false;
  std::optional<std::chrono::steady_clock::time_point> stop_at_;
  std::string ticket_;
  bool quit_ = false;  // the GUI is closing: the reader stops waiting
  std::thread thread_;
};

}  // namespace kk::gui
