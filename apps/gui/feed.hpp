#pragma once
// The live feed of agent runs (ADR 0021): every step of a run journal (.karkas/runs/*.jsonl)
// as readable items — what the model thought, which tools it called with which arguments,
// what came back — for any run, also ones started from a terminal. A journal is read once and
// then only what was appended to it, so watching an active run stays cheap.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace kk::gui {

struct FeedStep {
  enum class Kind { Start, Think, Text, Call, Result, End, Error, Cancelled };
  Kind kind = Kind::Text;
  int n = 0;              // step number of the journal
  std::int64_t ts = 0;    // unix seconds
  std::string title;      // tool name, or a short label
  std::string text;       // reasoning, arguments, result, message
  bool error = false;     // a tool result marked as error
  std::int64_t out_tokens = 0, reasoning_tokens = 0;  // of a model answer (Think/Call/Text)
};

struct FeedRun {
  std::string journal;  // file name
  std::string ticket, role, block, model, tier;
  int attempt = 0;
  std::int64_t start = 0, end = 0;  // unix seconds; end = 0 while running
  std::string outcome;
  double cost = 0;
  std::int64_t in_tokens = 0, out_tokens = 0, cache_read = 0;
  std::string task;  // what the agent was asked (the task text of its first step)
  std::vector<FeedStep> steps;
};

// Parses one journal line into `run` (run_start, step, run_end); unknown lines are ignored.
void feed_line(FeedRun& run, const std::string& line);

class JournalFeed {
 public:
  explicit JournalFeed(std::filesystem::path runs_dir) : dir_(std::move(runs_dir)) {}
  // The run of `journal` (a file name in the runs folder) with everything written so far.
  const FeedRun& load(const std::string& journal);

 private:
  struct Entry {
    std::uintmax_t offset = 0;  // bytes parsed
    std::string partial;        // an unfinished last line
    FeedRun run;
  };
  std::filesystem::path dir_;
  std::map<std::string, Entry> cache_;
};

}  // namespace kk::gui
