#include "kk/core/tickets.hpp"

#include <fmt/format.h>

#include <csignal>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <map>
#include <set>

#include "kk/core/paths.hpp"
#include "kk/core/platform.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"

namespace kk::core {

namespace fs = std::filesystem;

TicketStore::TicketStore(fs::path project_root) : root_(std::move(project_root)) {}

fs::path TicketStore::dir() const { return root_ / ".karkas" / "tickets"; }
fs::path TicketStore::file(std::string_view id) const { return dir() / (std::string(id) + ".ksl"); }

Result<std::string> TicketStore::next_id() const {
  int max = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir(), ec)) {
    auto stem = e.path().stem().string();
    if (e.path().extension() != ".ksl" || !is_record_id(stem) || stem[0] != 'T') continue;
    int n = 0;
    std::from_chars(stem.data() + 2, stem.data() + stem.size(), n);
    max = std::max(max, n);
  }
  return fmt::format("T-{:04d}", max + 1);
}

Result<Ticket> TicketStore::create(TicketKind kind, std::string_view origin, std::string_view text,
                                   const std::vector<std::string>& facts, std::string_view scope) {
  std::error_code ec;
  fs::create_directories(dir(), ec);
  // Retry on races: O_EXCL creation of the file reserves the id.
  for (int attempt = 0; attempt < 20; ++attempt) {
    auto id = next_id();
    if (!id) return std::unexpected(id.error());
    Ticket t;
    t.id = *id;
    t.kind = Scalar{std::string(to_string(kind)), {}, 0, 0, true};
    t.origin = Scalar{std::string(origin), {}, 0, 0, true};
    t.status = Scalar{"open", {}, 0, 0, true};
    if (!scope.empty() && scope != "root") t.scope = Scalar{std::string(scope), {}, 0, 0, true};
    if (kind == TicketKind::Bug) {
      t.symptom = Scalar{std::string(text), {}, 0, 0, true};
    } else {
      t.task = Scalar{std::string(text), {}, 0, 0, true};
    }
    for (const auto& f : facts) t.facts.push_back(TextItem{f, {}, 0, 0});
    auto path = file(t.id);
    if (!create_exclusive(path)) continue;
    if (auto r = write_file(path, write_ticket(t)); !r) return std::unexpected(r.error());
    return t;
  }
  return fail("tickets", "could not allocate a ticket id");
}

Result<Ticket> TicketStore::load(std::string_view id) const {
  if (!is_record_id(id) || id[0] != 'T') return fail("tickets", fmt::format("invalid ticket id '{}'", id));
  auto path = file(id);
  if (!fs::exists(path)) return fail("tickets", fmt::format("no ticket {}", id));
  auto text = read_file(path);
  if (!text) return std::unexpected(text.error());
  return parse_ticket_text(*text, rel_str(root_, path));
}

Result<std::vector<Ticket>> TicketStore::list() const {
  std::vector<Ticket> out;
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(dir(), ec)) {
    if (e.path().extension() == ".ksl") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    auto t = load(f.stem().string());
    if (!t) return std::unexpected(t.error());
    out.push_back(std::move(*t));
  }
  return out;
}

namespace {
bool same_text(const std::vector<TextItem>& a, const std::vector<TextItem>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].text != b[i].text) return false;
  }
  return true;
}
}  // namespace

VoidResult TicketStore::save(const Ticket& t) const {
  auto path = file(t.id);
  if (fs::exists(path)) {
    auto old = load(t.id);
    if (!old) return std::unexpected(old.error());
    auto immutable = [&](const char* what) {
      return fail("tickets", fmt::format("{}: {} is immutable, tickets are append-only (10.1)", t.id, what));
    };
    if (old->symptom.value != t.symptom.value) return immutable("symptom");
    if (old->task.value != t.task.value) return immutable("task");
    if (old->kind.value != t.kind.value) return immutable("kind");
    if (!same_text(old->facts, t.facts)) return immutable("the original facts");
    if (old->trail.size() > t.trail.size()) return immutable("the trail");
    for (std::size_t i = 0; i < old->trail.size(); ++i) {
      if (format_trail_entry(old->trail[i]) != format_trail_entry(t.trail[i])) return immutable("the trail");
    }
    if (old->result.size() > t.result.size()) return immutable("the result");
    for (std::size_t i = 0; i < old->result.size(); ++i) {
      if (old->result[i].key != t.result[i].key || old->result[i].text != t.result[i].text) {
        return immutable("the result");
      }
    }
  }
  return write_file(path, write_ticket(t));
}

Result<Ticket> TicketStore::edit_by_human(std::string_view id, std::string_view text) const {
  if (!fs::exists(file(id))) return fail("tickets", fmt::format("{} does not exist", id));
  auto t = parse_ticket_text(text, file(id).string());
  if (!t) return std::unexpected(t.error());
  if (t->id != id) return fail("tickets", fmt::format("the ticket is @ticket {}: its id cannot change", id));
  if (!t->kind_enum()) return fail("tickets", fmt::format("{}: unknown kind '{}'", id, t->kind.value));
  if (!t->status.value.empty() && !t->status_enum()) return fail("tickets", fmt::format("{}: unknown status '{}'", id, t->status.value));
  auto l = lock(id, "karkas-gui edit");
  if (!l) return fail("tickets", fmt::format("{} is being worked on ({}): stop its agents first", id, l.error().message));
  if (auto w = write_file(file(id), write_ticket(*t)); !w) return std::unexpected(w.error());
  return load(id);
}

VoidResult TicketStore::add_human_fact(std::string_view id, std::string_view text) const {
  auto t = load(id);
  if (!t) return std::unexpected(t.error());
  auto line = std::string(trim(text));
  if (line.empty()) return fail("tickets", "the message is empty");
  for (auto& c : line) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  t->facts.push_back(TextItem{"human: " + line, {}, 0, 0});
  return write_file(file(id), write_ticket(*t));
}

VoidResult TicketStore::append(std::string_view id, const TrailEntry& entry, const std::vector<std::string>& new_facts) const {
  if (new_facts.size() > 2) return fail("tickets", "at most two new facts per level (10.3)");
  auto t = load(id);
  if (!t) return std::unexpected(t.error());
  t->trail.push_back(entry);
  for (const auto& f : new_facts) {
    TrailEntry fe;
    fe.type = TrailEntry::Type::Fact;
    fe.text = f;
    t->trail.push_back(std::move(fe));
  }
  return save(*t);
}

VoidResult TicketStore::append_line(std::string_view id, std::string_view line) const {
  auto text = fmt::format("@ticket {}\n--- trail\n{}\n", id, line);
  auto parsed = parse_ticket_text(text, "line");
  if (!parsed) return fail("tickets", "not a valid trail line: " + parsed.error().message);
  if (parsed->trail.size() != 1) return fail("tickets", "expected exactly one trail line");
  return append(id, parsed->trail.front());
}

VoidResult TicketStore::set_status(std::string_view id, TicketStatus status) const {
  auto t = load(id);
  if (!t) return std::unexpected(t.error());
  t->status.value = std::string(to_string(status));
  t->status.set = true;
  return save(*t);
}

VoidResult TicketStore::add_result(std::string_view id, const std::vector<ResultLine>& lines) const {
  auto t = load(id);
  if (!t) return std::unexpected(t.error());
  for (const auto& l : lines) t->result.push_back(l);
  return save(*t);
}

TicketStore::Lock& TicketStore::Lock::operator=(Lock&& o) noexcept {
  if (this != &o) {
    if (!path_.empty()) {
      std::error_code ec;
      fs::remove(path_, ec);
    }
    path_ = std::move(o.path_);
    o.path_.clear();
  }
  return *this;
}

TicketStore::Lock::~Lock() {
  if (!path_.empty()) {
    std::error_code ec;
    fs::remove(path_, ec);
  }
}

Result<TicketStore::Lock> TicketStore::lock(std::string_view id, std::string_view owner) const {
  std::error_code ec;
  fs::create_directories(dir(), ec);
  auto path = dir() / (std::string(id) + ".lock");
  auto line = fmt::format("{} {}\n", current_pid(), owner);
  if (!create_exclusive(path, line)) {
    auto holder = read_file(path).value_or("?");
    // A lock whose owner process is gone is stale.
    int pid = 0;
    std::from_chars(holder.data(), holder.data() + holder.size(), pid);
    if (pid > 0 && !pid_alive(pid)) {
      fs::remove(path, ec);
      return lock(id, owner);
    }
    return fail("tickets", fmt::format("{} is locked by {}", id, trim(holder)));
  }
  return Lock(path);
}

// --- path view ----------------------------------------------------------------------

int trail_level(std::string_view at) {
  if (at.empty() || at == "root") return 0;
  return id_depth(at);
}

std::string path_view(const Ticket& t, std::string_view at, std::size_t token_budget) {
  std::set<std::string> on_path = {"root"};
  if (!at.empty() && at != "root") {
    std::string cur(at);
    while (!cur.empty()) {
      on_path.insert(cur);
      cur = parent_id(cur);
    }
  }
  auto in_path = [&](const std::string& b) { return on_path.contains(b); };
  const std::string here = at.empty() ? std::string("root") : std::string(at);

  // For each block on the path keep its latest decision (earlier ones belong to
  // branches already ruled out); the current block keeps its entries (attempts), but
  // repeats of one decision (same role, pick, outcome, model) fold into the latest one
  // with repeats=N: ten identical dispatches tell the agent nothing ten times.
  std::map<std::string, std::size_t> latest;
  std::map<std::string, std::size_t> own_latest;  // decision key → index of its latest entry
  std::map<std::string, int> own_count;
  auto own_key = [](const TrailEntry& e) {
    // Two proposals are two decisions ("proposed C-0006", "proposed C-0007"), not a repeat.
    std::string what = e.get("outcome") == "change" ? "|" + e.get("why") : std::string();
    return e.role + "|" + e.get("pick") + "|" + e.get("outcome") + "|" + e.get("model") + what;
  };
  for (std::size_t i = 0; i < t.trail.size(); ++i) {
    const auto& e = t.trail[i];
    if (e.type != TrailEntry::Type::Level) continue;
    latest[e.get("at")] = i;
    if (e.get("at") == here) {
      own_latest[own_key(e)] = i;
      ++own_count[own_key(e)];
    }
  }
  std::vector<TrailEntry> kept;
  std::vector<std::size_t> new_fact_idx;  // indexes into kept
  std::map<std::string, std::size_t> fact_last;  // a repeated observation is shown once (its latest copy)
  for (std::size_t i = 0; i < t.trail.size(); ++i) {
    if (t.trail[i].type == TrailEntry::Type::Fact) fact_last[t.trail[i].text] = i;
  }
  bool attached = false;  // following lines belong to an included level entry
  for (std::size_t i = 0; i < t.trail.size(); ++i) {
    const auto& e = t.trail[i];
    if (e.type == TrailEntry::Type::Level) {
      auto a = e.get("at");
      bool own = a == here;
      bool include = in_path(a) && (own || latest[a] == i);
      attached = include;
      if (!include) continue;
      if (own && own_latest[own_key(e)] != i) continue;  // folded into its latest repeat
      TrailEntry c = e;
      c.trivia = {};
      if (!own) {
        // Suspects on other branches mean nothing to an agent further down (app. D); the
        // agent's own earlier decisions keep their suspects.
        auto pick = c.get("pick");
        std::vector<std::string> sus;
        for (const auto& s : c.list("suspect")) {
          if (s != pick && in_path(s)) sus.push_back(s);
        }
        if (sus.empty()) {
          c.erase("suspect");
        } else {
          c.set("suspect", join(sus, ","));
        }
      }
      if (own && own_count[own_key(e)] > 1) c.set("repeats", std::to_string(own_count[own_key(e)]));
      kept.push_back(std::move(c));
    } else if (e.type == TrailEntry::Type::Answer) {
      if (attached || in_path(e.get("at"))) {
        TrailEntry c = e;
        c.trivia = {};
        kept.push_back(std::move(c));
      }
    } else if (attached) {
      if (e.type == TrailEntry::Type::Fact && fact_last[e.text] != i) continue;
      TrailEntry c = e;
      c.trivia = {};
      if (c.type == TrailEntry::Type::Fact) new_fact_idx.push_back(kept.size());
      kept.push_back(std::move(c));
    }
  }

  // What the earlier descents below this block ended with (oldest first, identical
  // consecutive ones folded): without it an agent above the leaves never learns that its
  // last pick already failed and picks it again.
  std::vector<std::string> tried;
  {
    struct Seg {
      std::vector<std::string> picks;
      std::string end;
      std::string detail;  // the fact the program wrote right after the outcome (its cause)
    };
    std::vector<Seg> segs;
    bool open = false;
    bool after_outcome = false;  // the previous line was the outcome of the open segment
    std::string outcome_at;
    for (const auto& e : t.trail) {
      bool was_after = after_outcome;
      after_outcome = false;
      if (e.type == TrailEntry::Type::Fact && was_after && !segs.empty() && e.text.starts_with(outcome_at + " ")) {
        segs.back().detail = e.text;
        continue;
      }
      if (e.type == TrailEntry::Type::Level) {
        auto a = e.get("at");
        if (a == here) {
          segs.push_back({});
          open = true;
          if (!e.get("outcome").empty()) {
            segs.back().end = e.get("outcome") + (e.get("why").empty() ? "" : ": " + e.get("why"));
            open = false;
          } else if (!e.get("pick").empty()) {
            segs.back().picks.push_back(e.get("pick"));
          }
          continue;
        }
        if (!open || !Schema::is_under(here, a)) {
          open = false;
          continue;
        }
        if (!e.get("pick").empty()) segs.back().picks.push_back(e.get("pick"));
        if (!e.get("outcome").empty()) {
          segs.back().end = e.role + " at " + a + ": " + e.get("outcome");
          segs.back().detail.clear();
          after_outcome = true;
          outcome_at = a;
        }
      } else if (e.type == TrailEntry::Type::Note && open && !segs.empty() && e.text.starts_with("stopped by the user")) {
        segs.back().end = "stopped by the user";
      }
    }
    bool below = std::any_of(segs.begin(), segs.end(), [](const Seg& s) { return !s.picks.empty(); });
    struct Fold {
      std::string line, detail;
      int n = 1;
    };
    std::vector<Fold> folded;
    for (const auto& s : segs) {
      if (!below) break;  // a leaf: its own attempts are in the trail already
      if (s.end.empty()) continue;  // still running, or ended with no recorded outcome
      std::string line = (s.picks.empty() ? std::string() : join(s.picks, " > ") + " -> ") + s.end;
      if (!folded.empty() && folded.back().line == line) {
        ++folded.back().n;
        if (!s.detail.empty()) folded.back().detail = s.detail;  // the latest cause
      } else {
        folded.push_back({line, s.detail, 1});
      }
    }
    for (const auto& f : folded) {
      std::string l = f.n > 1 ? fmt::format("{} (x{})", f.line, f.n) : f.line;
      if (!f.detail.empty()) l += " | " + truncate_utf8(f.detail, 240);
      tried.push_back(l);
    }
  }

  // The latest result block (what the last run reported to the human) for the agents on
  // the path of the block where the last descent ended: its cause is the most useful line
  // of the whole ticket and it lives outside the trail.
  std::vector<ResultLine> last_result;
  {
    std::string last_at;
    for (const auto& e : t.trail) {
      if (e.type == TrailEntry::Type::Level && !e.get("outcome").empty()) last_at = e.get("at");
    }
    std::size_t start = t.result.size();
    for (std::size_t i = 0; i < t.result.size(); ++i) {
      if (t.result[i].key == "status") start = i;
    }
    bool relevant = here == "root" || (!last_at.empty() && Schema::is_under(here, last_at));
    if (relevant) {
      for (std::size_t i = start; i < t.result.size(); ++i) {
        if (t.result[i].key != "cost" && t.result[i].key != "state") last_result.push_back(t.result[i]);
      }
    }
    // Below the root, a result of a feature over blocks ("fix  config: ...") shows only this block's
    // lines: the others' are noise to its agent (ADR 0026); none of them means nothing to show.
    auto prefix_of = [](const std::string& text) -> std::string {
      auto colon = text.find(": ");
      if (colon == std::string::npos) return {};
      auto id = text.substr(0, colon);
      return is_block_path(id) ? id : std::string();
    };
    if (here != "root" && std::any_of(last_result.begin(), last_result.end(), [&](const ResultLine& r) { return !prefix_of(r.text).empty(); })) {
      std::vector<ResultLine> mine;
      for (const auto& r : last_result) {
        auto p = prefix_of(r.text);
        if (p.empty() ? r.key == "status" : Schema::is_under(here, p)) mine.push_back(r);
      }
      if (std::none_of(mine.begin(), mine.end(), [](const ResultLine& r) { return r.key != "status"; })) mine.clear();
      last_result = std::move(mine);
    }
  }

  auto render = [&](const std::vector<TrailEntry>& entries, std::size_t folded) {
    std::string out = fmt::format("@ticket {} view=path at={}\n", t.id, at.empty() ? "root" : at);
    out += format_line("kind", t.kind.value) + "\n";
    if (t.symptom.set) out += format_line("symptom", format_free_text(t.symptom.value)) + "\n";
    if (t.task.set) out += format_line("task", format_free_text(t.task.value)) + "\n";
    for (const auto& f : t.facts) out += format_line("fact", format_free_text(f.text)) + "\n";
    if (!entries.empty() || folded > 0) {
      out += "--- trail\n";
      for (const auto& e : entries) out += format_trail_entry(e) + "\n";
      if (folded > 0) out += format_line("note", fmt::format("+{} facts in ticket file", folded)) + "\n";
    }
    if (!tried.empty()) {
      out += "--- earlier descents from here (oldest first)\n";
      for (const auto& l : tried) out += format_line("tried", format_free_text(l)) + "\n";
    }
    if (!last_result.empty()) {
      out += "--- last result\n";
      for (const auto& r : last_result) out += format_line(r.key, format_free_text(r.text)) + "\n";
    }
    return out;
  };
  std::string out = render(kept, 0);
  // Budget (10.4): keep the original facts and the latest new ones, fold older new facts.
  std::size_t folded = 0;
  while (estimate_tokens(out) > token_budget && folded < new_fact_idx.size()) {
    ++folded;
    std::vector<TrailEntry> slim;
    std::set<std::size_t> drop(new_fact_idx.begin(), new_fact_idx.begin() + static_cast<long>(folded));
    for (std::size_t i = 0; i < kept.size(); ++i) {
      if (!drop.contains(i)) slim.push_back(kept[i]);
    }
    out = render(slim, folded);
  }
  return out;
}

// --- known failures (docs/adr/0050) -------------------------------------------------------

namespace {

bool id_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }
bool path_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '.' || c == '/' || c == '-'; }

// `needle` occurs in `text` where `before_ok` accepts the character before it (or it starts the text)
// and `after_ok` the character after it (or it ends the text).
template <class Before, class After>
bool occurs(std::string_view text, std::string_view needle, Before before_ok, After after_ok) {
  if (needle.empty()) return false;
  for (auto pos = text.find(needle); pos != std::string_view::npos; pos = text.find(needle, pos + 1)) {
    const auto end = pos + needle.size();
    if ((pos == 0 || before_ok(text[pos - 1])) && (end >= text.size() || after_ok(text[end]))) return true;
  }
  return false;
}

}  // namespace

std::string block_named_in(std::string_view text, const Schema& schema) {
  // A folder of a block as a path: "blocks/dsp/echo/impl/echo.cpp" names dsp.echo, not the group dsp.
  const Block* best = nullptr;
  for (const auto& b : schema.blocks) {
    bool found = occurs(
        text, b.dir, [](char c) { return !path_char(c); }, [](char c) { return c == '/' || c == ':' || !path_char(c); });
    if (found && (best == nullptr || b.dir.size() > best->dir.size())) best = &b;
  }
  if (best != nullptr) return best->id;
  // A block id as a whole word outside a path; the longest wins (stream.ring over stream).
  for (const auto& b : schema.blocks) {
    bool found = occurs(
        text, b.id, [](char c) { return !(id_char(c) || c == '.' || c == '/'); }, [](char c) { return !id_char(c); });
    if (found && (best == nullptr || b.id.size() > best->id.size())) best = &b;
  }
  return best != nullptr ? best->id : std::string();
}

std::string ticket_block(const Ticket& t, const Schema& schema) {
  for (const auto& f : t.facts) {
    std::string_view x = f.text;
    if (!x.starts_with("block ")) continue;
    auto id = trim(x.substr(6));
    if (schema.find(id) != nullptr) return std::string(id);
  }
  for (const auto& f : t.facts) {
    std::string_view x = f.text;
    if (!x.starts_with("found at ")) continue;
    if (auto b = block_named_in(x.substr(9), schema); !b.empty()) return b;
  }
  if (auto b = block_named_in(t.symptom.value, schema); !b.empty()) return b;
  // Other facts (a human's "contract stream.ring/no_drop FAIL", karkas debug --fact ...).
  for (const auto& f : t.facts) {
    std::string_view x = f.text;
    if (x.starts_with("block ") || x.starts_with("found at ")) continue;
    if (auto b = block_named_in(x, schema); !b.empty()) return b;
  }
  return {};
}

Result<std::map<std::string, std::vector<Ticket>>> open_bugs(const TicketStore& store, const Schema& schema) {
  auto all = store.list();
  if (!all) return std::unexpected(all.error());
  std::map<std::string, std::vector<Ticket>> out;
  for (auto& t : *all) {
    if (t.kind_enum() != TicketKind::Bug || t.status_enum() == TicketStatus::Closed) continue;
    auto b = ticket_block(t, schema);
    if (!b.empty()) out[b].push_back(std::move(t));
  }
  return out;
}

std::string known_failures_text(const std::vector<std::string>& failing,
                                const std::map<std::string, std::vector<Ticket>>& bugs, const Schema& schema) {
  (void)schema;
  std::vector<std::string> blocks = failing;
  std::sort(blocks.begin(), blocks.end());
  blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
  std::string known, none;
  for (const auto& f : blocks) {
    std::vector<const Ticket*> tickets;
    for (const auto& [b, list] : bugs) {
      if (!Schema::is_under(f, b)) continue;
      for (const auto& t : list) tickets.push_back(&t);
    }
    std::sort(tickets.begin(), tickets.end(), [](const Ticket* a, const Ticket* b) { return a->id < b->id; });
    if (tickets.empty()) none += f + "\n";
    for (const auto* t : tickets) known += fmt::format("{}  {}  {}\n", f, t->id, t->symptom.value);
  }
  std::string out;
  if (!known.empty()) out += "--- known: these failures already have an open ticket\n" + known;
  if (!none.empty()) out += "--- no ticket: investigate them only if they are your task\n" + none;
  if (!known.empty()) out += "If one of the known failures is the bug of your task, fix it; otherwise leave it.\n";
  return out;
}

}  // namespace kk::core
