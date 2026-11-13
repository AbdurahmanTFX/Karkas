#pragma once
// What the bottom panels no longer show (docs/adr/0019): rows the user deleted or cleared
// from the tickets, agents, costs and build & tests panels. Display only — ticket files,
// run journals, test results and .karkas/karkas.log stay as they are, `karkas cost` still
// counts every run. An entry with newer activity than the moment it was cleared shows again.
// Stored per project in .karkas/cleared.json (not in git).
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "kk/core/error.hpp"
#include "kk/index/index.hpp"

namespace kk::gui {

struct Cleared {
  std::map<std::string, std::int64_t> tickets;  // id → time of the ticket file when cleared
  std::int64_t tickets_before = 0;              // "clear all": tickets unchanged since then
  std::set<std::string> runs;                   // journal file names
  std::int64_t runs_before = 0;                 // runs that ended at or before this time
  std::map<std::string, std::int64_t> tests;    // block → time of the cleared result
  std::int64_t tests_before = 0;
  std::set<std::string> changes;                // decided schema changes hidden from the side bar

  [[nodiscard]] bool hides(const index::TicketRow& t) const;
  [[nodiscard]] bool hides(const index::RunRow& r) const;
  [[nodiscard]] bool hides(const index::TestRow& t) const;
  [[nodiscard]] bool empty() const;

  void clear_ticket(const index::TicketRow& t);
  void clear_run(const index::RunRow& r);
  void clear_test(const index::TestRow& t);
  // "Clear all" of a panel: everything up to `now`; newer entries show.
  void clear_all_tickets(std::int64_t now);
  void clear_all_runs(std::int64_t now);
  void clear_all_tests(std::int64_t now);
  void restore_tickets();
  void restore_runs();
  void restore_tests();
};

std::filesystem::path cleared_path(const std::filesystem::path& project_root);
Cleared load_cleared(const std::filesystem::path& project_root);  // missing or broken file → nothing cleared
kk::VoidResult save_cleared(const std::filesystem::path& project_root, const Cleared& c);

}  // namespace kk::gui
