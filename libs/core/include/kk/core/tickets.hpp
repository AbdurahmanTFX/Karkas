#pragma once
// Ticket files .karkas/tickets/T-NNNN.ksl (10): append-only except the status line,
// one agent per ticket (lock file), and the "path view" handed to agents (10.1, app. D).
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/model.hpp"

namespace kk::core {

class TicketStore {
 public:
  explicit TicketStore(std::filesystem::path project_root);

  [[nodiscard]] std::filesystem::path dir() const;
  [[nodiscard]] std::filesystem::path file(std::string_view id) const;

  Result<std::string> next_id() const;
  // kind bug: `text` is the symptom; other kinds: the task.
  Result<Ticket> create(TicketKind kind, std::string_view origin, std::string_view text,
                        const std::vector<std::string>& facts);
  Result<Ticket> load(std::string_view id) const;
  Result<std::vector<Ticket>> list() const;

  // Writes the ticket after checking it only appends to the file on disk:
  // symptom, task and original facts unchanged; old trail and result lines kept as a prefix.
  VoidResult save(const Ticket& t) const;

  // A human's edit of the whole ticket file (karkas-gui; docs/adr/0033): the text must parse as
  // a ticket with the same id; refused while an agent holds the ticket's lock. The append-only
  // rule of save() binds the agents, not the human who owns the ticket. Written canonically.
  Result<Ticket> edit_by_human(std::string_view id, std::string_view text) const;

  // Appends one trail entry plus up to two new facts (10.3).
  VoidResult append(std::string_view id, const TrailEntry& entry, const std::vector<std::string>& new_facts = {}) const;
  // Parses `line` as a trail line ("L1 navigator at=... ", "fact ...", "answer ...").
  VoidResult append_line(std::string_view id, std::string_view line) const;
  VoidResult set_status(std::string_view id, TicketStatus status) const;
  VoidResult add_result(std::string_view id, const std::vector<ResultLine>& lines) const;

  // Exclusive lock .karkas/tickets/T-NNNN.lock (6.4).
  class Lock {
   public:
    Lock() = default;
    explicit Lock(std::filesystem::path p) : path_(std::move(p)) {}
    Lock(Lock&& o) noexcept : path_(std::move(o.path_)) { o.path_.clear(); }
    Lock& operator=(Lock&& o) noexcept;
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    ~Lock();
    [[nodiscard]] bool held() const { return !path_.empty(); }

   private:
    std::filesystem::path path_;
  };
  Result<Lock> lock(std::string_view id, std::string_view owner) const;

 private:
  std::filesystem::path root_;
};

// The path view (10.1): original data + trail entries along root → `at`.
// Entries of other branches are dropped, suspects outside the path are removed,
// and when the view exceeds `token_budget` new facts are folded into
// "+N facts in ticket file" (10.4). `at` = "root" for the orchestrator.
std::string path_view(const Ticket& t, std::string_view at, std::size_t token_budget = 600);

// Next trail level for a block: root = 0, top-level block = 1, ...
int trail_level(std::string_view at);

}  // namespace kk::core
