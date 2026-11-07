#include "kk/core/tickets.hpp"

#include <fcntl.h>
#include <fmt/format.h>
#include <unistd.h>

#include <csignal>

#include <algorithm>
#include <charconv>
#include <map>
#include <set>

#include "kk/core/paths.hpp"
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
                                   const std::vector<std::string>& facts) {
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
    if (kind == TicketKind::Bug) {
      t.symptom = Scalar{std::string(text), {}, 0, 0, true};
    } else {
      t.task = Scalar{std::string(text), {}, 0, 0, true};
    }
    for (const auto& f : facts) t.facts.push_back(TextItem{f, {}, 0, 0});
    auto path = file(t.id);
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) continue;
    ::close(fd);
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
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    auto holder = read_file(path).value_or("?");
    // A lock whose owner process is gone is stale.
    int pid = 0;
    std::from_chars(holder.data(), holder.data() + holder.size(), pid);
    if (pid > 0 && ::kill(pid, 0) != 0) {
      fs::remove(path, ec);
      return lock(id, owner);
    }
    return fail("tickets", fmt::format("{} is locked by {}", id, trim(holder)));
  }
  auto line = fmt::format("{} {}\n", ::getpid(), owner);
  (void)!::write(fd, line.data(), line.size());
  ::close(fd);
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

  // For each block on the path keep its latest decision (earlier ones belong to
  // branches already ruled out); the current block keeps all its entries (attempts).
  std::map<std::string, std::size_t> latest;
  for (std::size_t i = 0; i < t.trail.size(); ++i) {
    const auto& e = t.trail[i];
    if (e.type == TrailEntry::Type::Level) latest[e.get("at")] = i;
  }
  std::vector<TrailEntry> kept;
  std::vector<std::size_t> new_fact_idx;  // indexes into kept
  bool attached = false;  // following lines belong to an included level entry
  for (std::size_t i = 0; i < t.trail.size(); ++i) {
    const auto& e = t.trail[i];
    if (e.type == TrailEntry::Type::Level) {
      auto a = e.get("at");
      bool include = in_path(a) && (a == at || latest[a] == i);
      attached = include;
      if (!include) continue;
      TrailEntry c = e;
      c.trivia = {};
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
      kept.push_back(std::move(c));
    } else if (e.type == TrailEntry::Type::Answer) {
      if (attached || in_path(e.get("at"))) {
        TrailEntry c = e;
        c.trivia = {};
        kept.push_back(std::move(c));
      }
    } else if (attached) {
      TrailEntry c = e;
      c.trivia = {};
      if (c.type == TrailEntry::Type::Fact) new_fact_idx.push_back(kept.size());
      kept.push_back(std::move(c));
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

}  // namespace kk::core
