#include "cleared.hpp"

#include <nlohmann/json.hpp>

#include "kk/core/paths.hpp"

namespace kk::gui {

using json = nlohmann::json;

bool Cleared::hides(const index::TicketRow& t) const {
  if (tickets_before > 0 && t.created_at <= tickets_before) return true;
  auto it = tickets.find(t.id);
  return it != tickets.end() && t.created_at <= it->second;
}

bool Cleared::hides(const index::RunRow& r) const {
  if (runs_before > 0 && r.ts <= runs_before) return true;
  return !r.journal.empty() && runs.contains(r.journal);
}

bool Cleared::hides(const index::TestRow& t) const {
  if (tests_before > 0 && t.ts <= tests_before) return true;
  auto it = tests.find(t.block);
  return it != tests.end() && t.ts <= it->second;
}

bool Cleared::empty() const {
  return tickets.empty() && tickets_before == 0 && runs.empty() && runs_before == 0 && tests.empty() && tests_before == 0;
}

void Cleared::clear_ticket(const index::TicketRow& t) { tickets[t.id] = t.created_at; }
void Cleared::clear_run(const index::RunRow& r) {
  if (!r.journal.empty()) runs.insert(r.journal);
}
void Cleared::clear_test(const index::TestRow& t) { tests[t.block] = t.ts; }

void Cleared::clear_all_tickets(std::int64_t now) {
  tickets_before = now;
  tickets.clear();
}
void Cleared::clear_all_runs(std::int64_t now) {
  runs_before = now;
  runs.clear();
}
void Cleared::clear_all_tests(std::int64_t now) {
  tests_before = now;
  tests.clear();
}
void Cleared::restore_tickets() {
  tickets.clear();
  tickets_before = 0;
}
void Cleared::restore_runs() {
  runs.clear();
  runs_before = 0;
}
void Cleared::restore_tests() {
  tests.clear();
  tests_before = 0;
}

std::filesystem::path cleared_path(const std::filesystem::path& project_root) { return project_root / ".karkas" / "cleared.json"; }

Cleared load_cleared(const std::filesystem::path& project_root) {
  Cleared c;
  auto text = core::read_file(cleared_path(project_root));
  if (!text) return c;
  try {
    auto j = json::parse(*text);
    c.tickets = j.value("tickets", std::map<std::string, std::int64_t>{});
    c.tickets_before = j.value("tickets_before", std::int64_t{0});
    c.runs = j.value("runs", std::set<std::string>{});
    c.runs_before = j.value("runs_before", std::int64_t{0});
    c.tests = j.value("tests", std::map<std::string, std::int64_t>{});
    c.tests_before = j.value("tests_before", std::int64_t{0});
  } catch (const json::exception&) {
    return Cleared{};
  }
  return c;
}

kk::VoidResult save_cleared(const std::filesystem::path& project_root, const Cleared& c) {
  if (c.empty()) {
    std::error_code ec;
    std::filesystem::remove(cleared_path(project_root), ec);
    return {};
  }
  json j = {{"tickets", c.tickets}, {"tickets_before", c.tickets_before}, {"runs", c.runs},
            {"runs_before", c.runs_before}, {"tests", c.tests},           {"tests_before", c.tests_before}};
  return core::write_file(cleared_path(project_root), j.dump(2) + "\n");
}

}  // namespace kk::gui
