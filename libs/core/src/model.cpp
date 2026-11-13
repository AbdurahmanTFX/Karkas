#include "kk/core/model.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <map>

#include "kk/core/text.hpp"

namespace kk::core {

// --- enums ---------------------------------------------------------------------------

namespace {
template <class E, std::size_t N>
std::optional<E> lookup(const std::array<std::pair<E, std::string_view>, N>& table,
                        std::string_view s) {
  for (const auto& [e, name] : table) {
    if (name == s) return e;
  }
  return std::nullopt;
}
template <class E, std::size_t N>
std::string_view name_of(const std::array<std::pair<E, std::string_view>, N>& table, E e) {
  for (const auto& [v, name] : table) {
    if (v == e) return name;
  }
  return "?";
}

constexpr std::array<std::pair<BlockKind, std::string_view>, 4> kBlockKinds = {{
    {BlockKind::Group, "group"},
    {BlockKind::Leaf, "leaf"},
    {BlockKind::Entry, "entry"},
    {BlockKind::Shared, "shared"},
}};
constexpr std::array<std::pair<LinkKind, std::string_view>, 5> kLinkKinds = {{
    {LinkKind::Data, "data"},
    {LinkKind::Event, "event"},
    {LinkKind::Call, "call"},
    {LinkKind::State, "state"},
    {LinkKind::Use, "use"},
}};
constexpr std::array<std::pair<PortDir, std::string_view>, 3> kPortDirs = {{
    {PortDir::In, "in"},
    {PortDir::Out, "out"},
    {PortDir::Fn, "fn"},
}};
constexpr std::array<std::pair<BlockStatus, std::string_view>, 8> kBlockStatuses = {{
    {BlockStatus::Design, "design"},
    {BlockStatus::ApiFrozen, "api_frozen"},
    {BlockStatus::TestsReady, "tests_ready"},
    {BlockStatus::Implemented, "implemented"},
    {BlockStatus::Passing, "passing"},
    {BlockStatus::Failing, "failing"},
    {BlockStatus::Drift, "drift"},
    {BlockStatus::NeedsReview, "needs_review"},
}};
constexpr std::array<std::pair<TicketKind, std::string_view>, 4> kTicketKinds = {{
    {TicketKind::Bug, "bug"},
    {TicketKind::Feature, "feature"},
    {TicketKind::Change, "change"},
    {TicketKind::Question, "question"},
}};
constexpr std::array<std::pair<TicketStatus, std::string_view>, 6> kTicketStatuses = {{
    {TicketStatus::Open, "open"},
    {TicketStatus::Descending, "descending"},
    {TicketStatus::Fixing, "fixing"},
    {TicketStatus::Verifying, "verifying"},
    {TicketStatus::Closed, "closed"},
    {TicketStatus::Escalated, "escalated"},
}};
constexpr std::array<std::pair<ReportStatus, std::string_view>, 6> kReportStatuses = {{
    {ReportStatus::Fixed, "fixed"},
    {ReportStatus::Done, "done"},
    {ReportStatus::Failed, "failed"},
    {ReportStatus::NeedsContractChange, "needs_contract_change"},
    {ReportStatus::NeedsInfo, "needs_info"},
    {ReportStatus::NotHere, "not_here"},
}};
}  // namespace

std::string_view to_string(BlockKind k) { return name_of(kBlockKinds, k); }
std::string_view to_string(LinkKind k) { return name_of(kLinkKinds, k); }
std::string_view to_string(PortDir d) { return name_of(kPortDirs, d); }
std::string_view to_string(BlockStatus s) { return name_of(kBlockStatuses, s); }
std::string_view to_string(TicketKind k) { return name_of(kTicketKinds, k); }
std::string_view to_string(TicketStatus s) { return name_of(kTicketStatuses, s); }
std::string_view to_string(ReportStatus s) { return name_of(kReportStatuses, s); }
std::optional<BlockKind> parse_block_kind(std::string_view s) { return lookup(kBlockKinds, s); }
std::optional<LinkKind> parse_link_kind(std::string_view s) { return lookup(kLinkKinds, s); }
std::optional<BlockStatus> parse_block_status(std::string_view s) {
  return lookup(kBlockStatuses, s);
}
std::optional<TicketKind> parse_ticket_kind(std::string_view s) { return lookup(kTicketKinds, s); }
std::optional<TicketStatus> parse_ticket_status(std::string_view s) {
  return lookup(kTicketStatuses, s);
}
std::optional<ReportStatus> parse_report_status(std::string_view s) {
  return lookup(kReportStatuses, s);
}

int badness(BlockStatus s) {
  switch (s) {
    case BlockStatus::Passing: return 0;
    case BlockStatus::Implemented: return 1;
    case BlockStatus::TestsReady: return 2;
    case BlockStatus::ApiFrozen: return 3;
    case BlockStatus::Design: return 4;
    case BlockStatus::NeedsReview: return 5;
    case BlockStatus::Drift: return 6;
    case BlockStatus::Failing: return 7;
  }
  return 4;
}

std::optional<PortRef> parse_port_ref(std::string_view s) {
  auto colon = s.find(':');
  if (colon == std::string_view::npos) return std::nullopt;
  auto block = s.substr(0, colon);
  auto port = s.substr(colon + 1);
  if (!is_block_path(block) || !is_ident(port)) return std::nullopt;
  return PortRef{std::string(block), std::string(port)};
}

// --- small model methods ---------------------------------------------------------------

std::string Port::signature() const {
  if (dir != PortDir::Fn) return type;
  std::string s = name + "(" + join(params, ", ") + ")";
  if (!ret.empty()) s += " -> " + ret;
  return s;
}

const Port* Block::port(std::string_view name) const {
  for (const auto& p : ports) {
    if (p.name == name) return &p;
  }
  return nullptr;
}

bool Block::has_code() const {
  return kind && (*kind == BlockKind::Leaf || *kind == BlockKind::Entry || *kind == BlockKind::Shared);
}

std::string_view Block::kind_name() const { return kind ? to_string(*kind) : "?"; }

std::vector<std::string> Block::file_list() const {
  std::vector<std::string> out;
  for (const auto& f : files) out.insert(out.end(), f.paths.begin(), f.paths.end());
  return out;
}

void Block::add_hist(std::string ticket, std::string text) {
  hist.push_back(HistEntry{std::move(ticket), std::move(text), {}, 0});
  std::size_t real = 0;
  for (const auto& h : hist) real += h.ticket == "summary" ? 0 : 1;
  if (real <= kMaxHistEntries) return;
  // Fold the oldest real entries into the summary line (always the first line).
  std::size_t fold = real - kMaxHistEntries;
  long long count = 0;
  std::string first;
  std::string last;
  auto it = std::find_if(hist.begin(), hist.end(), [](const HistEntry& h) { return h.ticket == "summary"; });
  if (it != hist.end()) {
    // "<n> earlier entries <first>..<last>"
    auto parts = split_ws(it->text);
    if (!parts.empty()) {
      std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(), count);
    }
    if (parts.size() >= 4) {
      auto dots = parts[3].find("..");
      if (dots != std::string::npos) first = parts[3].substr(0, dots);
    }
    hist.erase(it);
  }
  std::vector<HistEntry> kept;
  std::size_t folded = 0;
  for (auto& h : hist) {
    if (folded < fold) {
      if (first.empty()) first = h.ticket;
      last = h.ticket;
      ++folded;
      ++count;
      continue;
    }
    kept.push_back(std::move(h));
  }
  HistEntry summary;
  summary.ticket = "summary";
  summary.text = fmt::format("{} earlier entries {}..{}", count, first, last);
  kept.insert(kept.begin(), std::move(summary));
  hist = std::move(kept);
}

std::string TrailEntry::get(std::string_view key) const {
  for (const auto& a : attrs) {
    if (a.key == key) return a.value;
  }
  return {};
}

std::vector<std::string> TrailEntry::list(std::string_view key) const {
  return split(get(key), ',');
}

void TrailEntry::set(std::string_view key, std::string value) {
  for (auto& a : attrs) {
    if (a.key == key) {
      a.value = std::move(value);
      return;
    }
  }
  attrs.push_back(Attr{std::string(key), std::move(value)});
}

void TrailEntry::erase(std::string_view key) {
  std::erase_if(attrs, [&](const Attr& a) { return a.key == key; });
}

std::optional<TicketKind> Ticket::kind_enum() const { return parse_ticket_kind(kind.value); }
std::optional<TicketStatus> Ticket::status_enum() const { return parse_ticket_status(status.value); }
std::string Ticket::result_value(std::string_view key) const {
  std::string v;
  for (const auto& r : result) {
    if (r.key == key) v = r.text;
  }
  return v;
}

// --- directive table -------------------------------------------------------------------

const std::vector<DirectiveSpec>& directive_table() {
  static const std::vector<DirectiveSpec> table = {
      // @block (4.4)
      {"block", "purpose", 1, "<text>", ""},
      {"block", "kind", 1, "group|leaf|entry|shared", ""},
      {"block", "thread", 1, "<ident>", "leaf,entry,shared"},
      {"block", "est", 1, "S|M|L", "leaf,entry"},
      {"block", "files", -1, "<path> [<path>...]", "leaf,entry,shared"},
      {"block", "in", -1, "<name> <Type> [from=<block>:<port>] [link=data|event] [via=] [sync=] [rate=]",
       "leaf,entry,shared"},
      {"block", "out", -1, "<name> <Type> [via=] [sync=] [rate=]", "leaf,entry,shared"},
      {"block", "fn", -1, "<name>(<params>) [-> <Type>]", "leaf,shared"},
      {"block", "call", -1, "<block>:<fn> [via=] [sync=] [rate=]", "leaf,entry"},
      {"block", "use", -1, "<shared block>", "leaf,entry,shared"},
      {"block", "state", -1, "<name> <Type> with=<block> [via=] [sync=]", "leaf"},
      {"block", "inv", -1, "<text>", "leaf,shared"},
      {"block", "note", 3, "<text>", ""},
      {"block", "hist", static_cast<int>(kMaxHistEntries) + 1, "<T-NNNN> <text>", "leaf,entry"},
      // @project (4.3)
      {"project", "idea", 1, "<text>", ""},
      {"project", "lang", 1, "<ident>", ""},
      {"project", "build", 1, "<word>", ""},
      {"project", "std", 1, "<word>", ""},
      {"project", "test", 1, "<word>", ""},
      {"project", "dirs", 1, "[blocks=] [api=] [impl=] [tests=] [mocks=]", ""},
      {"project", "rule", -1, "<text>", ""},
      {"project", "note", 3, "<text>", ""},
      // @ticket (10.2)
      {"ticket", "kind", 1, "bug|feature|change|question", ""},
      {"ticket", "origin", 1, "human|tester|orchestrator", ""},
      {"ticket", "status", 1, "open|descending|fixing|verifying|closed|escalated", ""},
      {"ticket", "symptom", 1, "<text>", ""},
      {"ticket", "task", 1, "<text>", ""},
      {"ticket", "fact", -1, "<text>", ""},
      // @report (10.6)
      {"report", "status", 1, "fixed|done|failed|needs_contract_change|needs_info|not_here", ""},
      {"report", "cause", 1, "<text>", ""},
      {"report", "fix", 1, "<text>", ""},
      {"report", "files", 1, "<text>", ""},
      {"report", "tests", 1, "<text>", ""},
      {"report", "api", 1, "unchanged|change_requested", ""},
      // @change (10.7)
      {"change", "from", 1, "<text>", ""},
      {"change", "reason", 1, "<text>", ""},
      {"change", "status", 1, "pending|approved|rejected", ""},
  };
  return table;
}

const DirectiveSpec* find_directive(std::string_view record, std::string_view name) {
  for (const auto& d : directive_table()) {
    if (d.record == record && d.name == name) return &d;
  }
  return nullptr;
}

// --- parsing helpers ---------------------------------------------------------------------

namespace {

SourceLoc loc_of(const Record& rec, const Line& l, int col = 0) {
  return SourceLoc{rec.file, l.line, col > 0 ? col : l.col};
}

bool is_type(std::string_view t) {
  if (t.empty()) return false;
  char c = t[0];
  bool ok_first = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
  if (!ok_first) return false;
  return t.find('=') == std::string_view::npos && t.find('"') == std::string_view::npos;
}

// Counts occurrences and enforces the table's multiplicity (S05) and existence (S03).
struct DirectiveCounter {
  std::map<std::string, int> counts;
  VoidResult check(const Record& rec, const Line& l, std::string_view record_kind) {
    const auto* spec = find_directive(record_kind, l.directive);
    if (spec == nullptr) {
      return fail("S03", fmt::format("unknown directive '{}' in @{}", l.directive, record_kind),
                  loc_of(rec, l));
    }
    int n = ++counts[l.directive];
    if (spec->max >= 0 && n > spec->max) {
      return fail("S05",
                  fmt::format("'{}' may appear at most {} time{} in @{}", l.directive, spec->max,
                              spec->max == 1 ? "" : "s", record_kind),
                  loc_of(rec, l));
    }
    return {};
  }
};

Result<std::vector<Token>> toks(const Record& rec, const Line& l) { return tokenize(l, rec.file); }

Result<Scalar> scalar_text(const Record& rec, const Line& l) {
  auto t = free_text(l, rec.file);
  if (!t) return std::unexpected(t.error());
  if (trim(*t).empty()) {
    return fail("S04", fmt::format("'{}' needs text", l.directive), loc_of(rec, l));
  }
  return Scalar{*t, l.trivia, l.line, l.col, true};
}

Result<TextItem> text_item(const Record& rec, const Line& l) {
  auto s = scalar_text(rec, l);
  if (!s) return std::unexpected(s.error());
  return TextItem{s->value, l.trivia, l.line, l.col};
}

// One bare word argument, optionally restricted to a set of values.
Result<Scalar> scalar_word(const Record& rec, const Line& l,
                           std::initializer_list<std::string_view> allowed = {},
                           bool ident = false) {
  auto t = toks(rec, l);
  if (!t) return std::unexpected(t.error());
  if (t->size() != 1 || (*t)[0].kind != Token::Kind::Word) {
    return fail("S04", fmt::format("'{}' takes exactly one word", l.directive),
                loc_of(rec, l, l.text_col));
  }
  const auto& w = (*t)[0].text;
  if (allowed.size() > 0 && std::find(allowed.begin(), allowed.end(), w) == allowed.end()) {
    std::vector<std::string> names(allowed.begin(), allowed.end());
    return fail("S04", fmt::format("'{}' must be one of {}, got '{}'", l.directive, join(names, "|"), w),
                loc_of(rec, l, (*t)[0].col));
  }
  if (ident && !is_ident(w)) {
    return fail("S04", fmt::format("'{}' must be an identifier [a-z][a-z0-9_]*, got '{}'", l.directive, w),
                loc_of(rec, l, (*t)[0].col));
  }
  return Scalar{w, l.trivia, l.line, l.col, true};
}

VoidResult link_attrs(const Record& rec, const Line& l, const Token& t, std::string& via,
                      std::string& sync, std::string& rate) {
  if (t.key == "via") {
    via = t.value;
  } else if (t.key == "sync") {
    sync = t.value;
  } else if (t.key == "rate") {
    rate = t.value;
  } else {
    return fail("S04", fmt::format("unknown attribute '{}' for '{}'", t.key, l.directive),
                loc_of(rec, l, t.col));
  }
  return {};
}

Result<Port> parse_in_out(const Record& rec, const Line& l, PortDir dir) {
  auto t = toks(rec, l);
  if (!t) return std::unexpected(t.error());
  if (t->size() < 2 || (*t)[0].kind != Token::Kind::Word || (*t)[1].kind != Token::Kind::Word) {
    return fail("S04", fmt::format("'{}' expects '<name> <Type>'", l.directive),
                loc_of(rec, l, l.text_col));
  }
  Port p;
  p.dir = dir;
  p.name = (*t)[0].text;
  p.type = (*t)[1].text;
  p.trivia = l.trivia;
  p.line = l.line;
  p.col = l.col;
  if (!is_ident(p.name)) {
    return fail("S04", fmt::format("port name '{}' is not an identifier", p.name),
                loc_of(rec, l, (*t)[0].col));
  }
  if (!is_type(p.type)) {
    return fail("S04", fmt::format("invalid type '{}'", p.type), loc_of(rec, l, (*t)[1].col));
  }
  for (std::size_t i = 2; i < t->size(); ++i) {
    const auto& a = (*t)[i];
    if (a.kind != Token::Kind::Attr) {
      return fail("S04", fmt::format("unexpected '{}', expected key=value", a.text),
                  loc_of(rec, l, a.col));
    }
    if (dir == PortDir::In && a.key == "from") {
      auto ref = parse_port_ref(a.value);
      if (!ref) {
        return fail("S04", fmt::format("from= expects <block>:<port>, got '{}'", a.value),
                    loc_of(rec, l, a.col));
      }
      p.from = *ref;
    } else if (dir == PortDir::In && a.key == "link") {
      auto k = parse_link_kind(a.value);
      if (!k || (*k != LinkKind::Data && *k != LinkKind::Event)) {
        return fail("S04", fmt::format("link= must be data or event, got '{}'", a.value),
                    loc_of(rec, l, a.col));
      }
      p.link = *k;
      p.link_written = true;
    } else {
      auto r = link_attrs(rec, l, a, p.via, p.sync, p.rate);
      if (!r) return std::unexpected(r.error());
    }
  }
  return p;
}

// Splits "a, b<c, d>, e" at top-level commas.
std::vector<std::string> split_params(std::string_view s) {
  std::vector<std::string> out;
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '(' || c == '<' || c == '[') ++depth;
    if (c == ')' || c == '>' || c == ']') --depth;
    if (c == ',' && depth == 0) {
      out.emplace_back(trim(s.substr(start, i - start)));
      start = i + 1;
    }
  }
  auto last = trim(s.substr(start));
  if (!last.empty() || !out.empty()) out.emplace_back(last);
  return out;
}

Result<Port> parse_fn(const Record& rec, const Line& l) {
  std::string_view s = l.text;
  Port p;
  p.dir = PortDir::Fn;
  p.trivia = l.trivia;
  p.line = l.line;
  p.col = l.col;
  std::size_t i = 0;
  while (i < s.size() && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) ++i;
  p.name.assign(s.substr(0, i));
  if (!is_ident(p.name)) {
    return fail("S04", "fn expects '<name>(<params>) [-> <Type>]'", loc_of(rec, l, l.text_col));
  }
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  if (i >= s.size() || s[i] != '(') {
    return fail("S04", fmt::format("fn '{}' needs a parameter list '(...)'", p.name),
                loc_of(rec, l, l.text_col + static_cast<int>(i)));
  }
  int depth = 0;
  std::size_t open = i;
  std::size_t close = std::string_view::npos;
  for (; i < s.size(); ++i) {
    if (s[i] == '(') ++depth;
    if (s[i] == ')' && --depth == 0) {
      close = i;
      break;
    }
  }
  if (close == std::string_view::npos) {
    return fail("S04", "unbalanced '(' in fn signature", loc_of(rec, l, l.text_col + static_cast<int>(open)));
  }
  p.params = split_params(s.substr(open + 1, close - open - 1));
  std::string_view rest = trim(s.substr(close + 1));
  if (!rest.empty()) {
    if (!rest.starts_with("->")) {
      return fail("S04", fmt::format("unexpected '{}' after fn parameters", rest),
                  loc_of(rec, l, l.text_col + static_cast<int>(s.size() - rest.size())));
    }
    p.ret = std::string(trim(rest.substr(2)));
    if (!is_type(p.ret)) {
      return fail("S04", "fn return type missing or invalid after '->'",
                  loc_of(rec, l, l.text_col + static_cast<int>(s.size() - rest.size())));
    }
  }
  return p;
}

Result<CallDecl> parse_call(const Record& rec, const Line& l) {
  auto t = toks(rec, l);
  if (!t) return std::unexpected(t.error());
  if (t->empty() || (*t)[0].kind != Token::Kind::Word) {
    return fail("S04", "call expects '<block>:<fn>'", loc_of(rec, l, l.text_col));
  }
  auto ref = parse_port_ref((*t)[0].text);
  if (!ref) {
    return fail("S04", fmt::format("call expects <block>:<fn>, got '{}'", (*t)[0].text),
                loc_of(rec, l, (*t)[0].col));
  }
  CallDecl c;
  c.target = *ref;
  c.trivia = l.trivia;
  c.line = l.line;
  c.col = l.col;
  for (std::size_t i = 1; i < t->size(); ++i) {
    const auto& a = (*t)[i];
    if (a.kind != Token::Kind::Attr) {
      return fail("S04", fmt::format("unexpected '{}', expected key=value", a.text), loc_of(rec, l, a.col));
    }
    auto r = link_attrs(rec, l, a, c.via, c.sync, c.rate);
    if (!r) return std::unexpected(r.error());
  }
  return c;
}

Result<UseDecl> parse_use(const Record& rec, const Line& l) {
  auto t = toks(rec, l);
  if (!t) return std::unexpected(t.error());
  if (t->size() != 1 || (*t)[0].kind != Token::Kind::Word || !is_block_path((*t)[0].text)) {
    return fail("S04", "use expects one block path", loc_of(rec, l, l.text_col));
  }
  return UseDecl{(*t)[0].text, l.trivia, l.line, l.col};
}

// Project-relative path of an existing file: no absolute paths, no "..", '/' separators.
bool is_project_path(std::string_view p) {
  if (p.empty() || p.front() == '/' || p.find('\\') != std::string_view::npos) return false;
  for (const auto& part : split(p, '/', false)) {
    if (part.empty() || part == "." || part == "..") return false;
  }
  return true;
}

Result<FilesDecl> parse_files(const Record& rec, const Line& l) {
  auto t = toks(rec, l);
  if (!t) return std::unexpected(t.error());
  FilesDecl f;
  f.trivia = l.trivia;
  f.line = l.line;
  f.col = l.col;
  for (const auto& w : *t) {
    if (w.kind != Token::Kind::Word || !is_project_path(w.text)) {
      return fail("S04", fmt::format("files expects project-relative paths, got '{}'", w.text), loc_of(rec, l, w.col));
    }
    f.paths.push_back(w.text);
  }
  if (f.paths.empty()) return fail("S04", "files expects at least one path", loc_of(rec, l, l.text_col));
  return f;
}

Result<StateDecl> parse_state(const Record& rec, const Line& l) {
  auto t = toks(rec, l);
  if (!t) return std::unexpected(t.error());
  if (t->size() < 2 || (*t)[0].kind != Token::Kind::Word || (*t)[1].kind != Token::Kind::Word) {
    return fail("S04", "state expects '<name> <Type> with=<block>'", loc_of(rec, l, l.text_col));
  }
  StateDecl s;
  s.name = (*t)[0].text;
  s.type = (*t)[1].text;
  s.trivia = l.trivia;
  s.line = l.line;
  s.col = l.col;
  if (!is_ident(s.name)) {
    return fail("S04", fmt::format("state name '{}' is not an identifier", s.name), loc_of(rec, l, (*t)[0].col));
  }
  for (std::size_t i = 2; i < t->size(); ++i) {
    const auto& a = (*t)[i];
    if (a.kind != Token::Kind::Attr) {
      return fail("S04", fmt::format("unexpected '{}', expected key=value", a.text), loc_of(rec, l, a.col));
    }
    if (a.key == "with") {
      if (!is_block_path(a.value)) {
        return fail("S04", fmt::format("with= expects a block path, got '{}'", a.value), loc_of(rec, l, a.col));
      }
      s.with = a.value;
    } else if (a.key == "via") {
      s.via = a.value;
    } else if (a.key == "sync") {
      s.sync = a.value;
    } else {
      return fail("S04", fmt::format("unknown attribute '{}' for 'state'", a.key), loc_of(rec, l, a.col));
    }
  }
  if (s.with.empty()) return fail("S04", "state needs with=<block>", loc_of(rec, l, l.text_col));
  return s;
}

Result<HistEntry> parse_hist(const Record& rec, const Line& l) {
  std::string_view s = l.text;
  std::size_t sp = 0;
  while (sp < s.size() && s[sp] != ' ' && s[sp] != '\t') ++sp;
  std::string id(s.substr(0, sp));
  if (!(is_record_id(id) && id[0] == 'T') && id != "summary") {
    return fail("S04", "hist expects '<T-NNNN> <text>'", loc_of(rec, l, l.text_col));
  }
  Line rest = l;
  rest.text = std::string(trim(s.substr(sp)));
  rest.text_col = l.text_col + static_cast<int>(s.size() - rest.text.size());
  auto text = free_text(rest, rec.file);
  if (!text) return std::unexpected(text.error());
  return HistEntry{id, *text, l.trivia, l.line};
}

VoidResult no_sections(const Record& rec) {
  if (!rec.sections.empty()) {
    const auto& s = rec.sections.front();
    return fail("S07", fmt::format("unexpected section '{}' in @{}", s.name, rec.kind),
                SourceLoc{rec.file, s.line, 1});
  }
  return {};
}

template <class T>
VoidResult assign(Result<T>&& r, T& out) {
  if (!r) return std::unexpected(r.error());
  out = std::move(*r);
  return {};
}

template <class T>
VoidResult push(Result<T>&& r, std::vector<T>& out) {
  if (!r) return std::unexpected(r.error());
  out.push_back(std::move(*r));
  return {};
}

}  // namespace

// --- parse: block --------------------------------------------------------------------------

Result<Block> parse_block(const Record& rec) {
  if (rec.kind != "block") return fail("S02", "expected @block", SourceLoc{rec.file, rec.line, 1});
  Block b;
  b.id = rec.id;
  b.file = rec.file;
  b.line = rec.line;
  b.header = rec.trivia;
  b.tail = rec.tail;
  if (auto r = no_sections(rec); !r) return std::unexpected(r.error());
  DirectiveCounter counter;
  for (const auto& l : rec.lines) {
    if (auto r = counter.check(rec, l, "block"); !r) return std::unexpected(r.error());
    const auto& d = l.directive;
    VoidResult r;
    if (d == "purpose") {
      r = assign(scalar_text(rec, l), b.purpose);
    } else if (d == "kind") {
      r = assign(scalar_word(rec, l, {"group", "leaf", "entry", "shared"}), b.kind_text);
      if (r) b.kind = parse_block_kind(b.kind_text.value);
    } else if (d == "thread") {
      r = assign(scalar_word(rec, l, {}, true), b.thread);
    } else if (d == "est") {
      r = assign(scalar_word(rec, l, {"S", "M", "L"}), b.est);
    } else if (d == "in") {
      r = push(parse_in_out(rec, l, PortDir::In), b.ports);
    } else if (d == "out") {
      r = push(parse_in_out(rec, l, PortDir::Out), b.ports);
    } else if (d == "fn") {
      r = push(parse_fn(rec, l), b.ports);
    } else if (d == "call") {
      r = push(parse_call(rec, l), b.calls);
    } else if (d == "use") {
      r = push(parse_use(rec, l), b.uses);
    } else if (d == "state") {
      r = push(parse_state(rec, l), b.states);
    } else if (d == "inv") {
      r = push(text_item(rec, l), b.invs);
    } else if (d == "note") {
      r = push(text_item(rec, l), b.notes);
    } else if (d == "hist") {
      r = push(parse_hist(rec, l), b.hist);
    } else if (d == "files") {
      r = push(parse_files(rec, l), b.files);
    }
    if (!r) return std::unexpected(r.error());
  }
  return b;
}

// --- parse: project ------------------------------------------------------------------------

Result<Project> parse_project(const Record& rec) {
  if (rec.kind != "project") return fail("S02", "expected @project", SourceLoc{rec.file, rec.line, 1});
  Project p;
  p.name = rec.id;
  p.file = rec.file;
  p.line = rec.line;
  p.header = rec.trivia;
  p.tail = rec.tail;
  if (auto r = no_sections(rec); !r) return std::unexpected(r.error());
  DirectiveCounter counter;
  for (const auto& l : rec.lines) {
    if (auto r = counter.check(rec, l, "project"); !r) return std::unexpected(r.error());
    const auto& d = l.directive;
    VoidResult r;
    if (d == "idea") {
      r = assign(scalar_text(rec, l), p.idea);
    } else if (d == "lang") {
      r = assign(scalar_word(rec, l, {}, true), p.lang);
    } else if (d == "build") {
      r = assign(scalar_word(rec, l), p.build);
    } else if (d == "std") {
      r = assign(scalar_word(rec, l), p.std);
    } else if (d == "test") {
      r = assign(scalar_word(rec, l), p.test);
    } else if (d == "rule") {
      r = push(text_item(rec, l), p.rules);
    } else if (d == "note") {
      r = push(text_item(rec, l), p.notes);
    } else if (d == "dirs") {
      auto t = toks(rec, l);
      if (!t) return std::unexpected(t.error());
      p.dirs.written = true;
      p.dirs.trivia = l.trivia;
      p.dirs.line = l.line;
      for (const auto& a : *t) {
        std::string* dst = nullptr;
        if (a.kind == Token::Kind::Attr) {
          if (a.key == "blocks") dst = &p.dirs.blocks;
          if (a.key == "api") dst = &p.dirs.api;
          if (a.key == "impl") dst = &p.dirs.impl;
          if (a.key == "tests") dst = &p.dirs.tests;
          if (a.key == "mocks") dst = &p.dirs.mocks;
        }
        if (dst == nullptr) {
          return fail("S04", fmt::format("dirs: unexpected '{}'", a.text), loc_of(rec, l, a.col));
        }
        *dst = a.value;
      }
    }
    if (!r) return std::unexpected(r.error());
  }
  return p;
}

// --- parse: ticket -------------------------------------------------------------------------

namespace {

Result<TrailEntry> parse_trail_line(const Record& rec, const Line& l) {
  TrailEntry e;
  e.trivia = l.trivia;
  e.line = l.line;
  const auto& d = l.directive;
  if (d.size() >= 2 && d[0] == 'L' &&
      std::all_of(d.begin() + 1, d.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    e.type = TrailEntry::Type::Level;
    std::from_chars(d.data() + 1, d.data() + d.size(), e.level);
    auto t = toks(rec, l);
    if (!t) return std::unexpected(t.error());
    if (t->empty() || (*t)[0].kind != Token::Kind::Word || !is_ident((*t)[0].text)) {
      return fail("S04", "trail entry expects 'L<n> <role> key=value ...'", loc_of(rec, l, l.text_col));
    }
    e.role = (*t)[0].text;
    for (std::size_t i = 1; i < t->size(); ++i) {
      const auto& a = (*t)[i];
      if (a.kind != Token::Kind::Attr) {
        return fail("S04", fmt::format("unexpected '{}' in trail entry, expected key=value", a.text),
                    loc_of(rec, l, a.col));
      }
      e.attrs.push_back(Attr{a.key, a.value});
    }
    return e;
  }
  if (d == "fact" || d == "note") {
    e.type = d == "fact" ? TrailEntry::Type::Fact : TrailEntry::Type::Note;
    auto t = free_text(l, rec.file);
    if (!t) return std::unexpected(t.error());
    e.text = *t;
    return e;
  }
  if (d == "answer") {
    e.type = TrailEntry::Type::Answer;
    auto t = toks(rec, l);
    if (!t) return std::unexpected(t.error());
    for (const auto& a : *t) {
      if (a.kind == Token::Kind::Attr) {
        e.attrs.push_back(Attr{a.key, a.value});
      } else {
        if (!e.text.empty()) e.text += ' ';
        e.text += a.text;
      }
    }
    return e;
  }
  return fail("S03", fmt::format("unknown directive '{}' in --- trail", d), loc_of(rec, l));
}

}  // namespace

Result<Ticket> parse_ticket(const Record& rec) {
  if (rec.kind != "ticket") return fail("S02", "expected @ticket", SourceLoc{rec.file, rec.line, 1});
  Ticket t;
  t.id = rec.id;
  t.header_attrs = rec.attrs;
  t.file = rec.file;
  t.line = rec.line;
  t.header = rec.trivia;
  t.tail = rec.tail;
  DirectiveCounter counter;
  for (const auto& l : rec.lines) {
    if (auto r = counter.check(rec, l, "ticket"); !r) return std::unexpected(r.error());
    const auto& d = l.directive;
    VoidResult r;
    if (d == "kind") {
      r = assign(scalar_word(rec, l, {"bug", "feature", "change", "question"}), t.kind);
    } else if (d == "origin") {
      r = assign(scalar_word(rec, l, {}, true), t.origin);
    } else if (d == "status") {
      r = assign(scalar_word(rec, l, {"open", "descending", "fixing", "verifying", "closed", "escalated"}),
                 t.status);
    } else if (d == "symptom") {
      r = assign(scalar_text(rec, l), t.symptom);
    } else if (d == "task") {
      r = assign(scalar_text(rec, l), t.task);
    } else if (d == "fact") {
      r = push(text_item(rec, l), t.facts);
    }
    if (!r) return std::unexpected(r.error());
  }
  for (const auto& s : rec.sections) {
    if (s.name == "trail") {
      t.trail_trivia = s.trivia;
      for (const auto& l : s.lines) {
        auto e = parse_trail_line(rec, l);
        if (!e) return std::unexpected(e.error());
        t.trail.push_back(std::move(*e));
      }
    } else if (s.name == "result") {
      t.result_trivia = s.trivia;
      for (const auto& l : s.lines) {
        static constexpr std::array<std::string_view, 10> kKeys = {
            "status", "cause", "fix", "files", "tests", "api", "verify", "cost", "note", "state"};
        if (std::find(kKeys.begin(), kKeys.end(), l.directive) == kKeys.end()) {
          return fail("S03", fmt::format("unknown directive '{}' in --- result", l.directive), loc_of(rec, l));
        }
        auto text = free_text(l, rec.file);
        if (!text) return std::unexpected(text.error());
        t.result.push_back(ResultLine{l.directive, *text, l.trivia, l.line});
      }
    } else {
      return fail("S07", fmt::format("unknown section '{}' in @ticket", s.name),
                  SourceLoc{rec.file, s.line, 1});
    }
  }
  return t;
}

// --- parse: report / change ---------------------------------------------------------------

Result<Report> parse_report(const Record& rec) {
  if (rec.kind != "report") return fail("S02", "expected @report", SourceLoc{rec.file, rec.line, 1});
  Report r;
  r.ticket = rec.id;
  for (const auto& a : rec.attrs) {
    if (a.key == "from") r.from = a.value;
    else if (a.key == "role") r.role = a.value;
    else return fail("S06", fmt::format("unknown header attribute '{}' for @report", a.key), SourceLoc{rec.file, rec.line, 1});
  }
  DirectiveCounter counter;
  for (const auto& l : rec.lines) {
    if (auto c = counter.check(rec, l, "report"); !c) return std::unexpected(c.error());
    const auto& d = l.directive;
    if (d == "status") {
      auto s = scalar_word(rec, l, {"fixed", "done", "failed", "needs_contract_change", "needs_info", "not_here"});
      if (!s) return std::unexpected(s.error());
      r.status = s->value;
      continue;
    }
    auto text = free_text(l, rec.file);
    if (!text) return std::unexpected(text.error());
    if (d == "cause") r.cause = *text;
    if (d == "fix") r.fix = *text;
    if (d == "files") r.files = *text;
    if (d == "tests") r.tests = *text;
    if (d == "api") r.api = *text;
  }
  for (const auto& s : rec.sections) {
    if (s.name != "diff") {
      return fail("S07", fmt::format("unknown section '{}' in @report", s.name), SourceLoc{rec.file, s.line, 1});
    }
    for (const auto& l : s.lines) r.proposal.push_back(l.directive + " " + l.text);
  }
  return r;
}

Result<Change> parse_change(const Record& rec) {
  if (rec.kind != "change") return fail("S02", "expected @change", SourceLoc{rec.file, rec.line, 1});
  Change c;
  c.id = rec.id;
  c.file = rec.file;
  c.line = rec.line;
  c.header = rec.trivia;
  c.tail = rec.tail;
  DirectiveCounter counter;
  for (const auto& l : rec.lines) {
    if (auto r = counter.check(rec, l, "change"); !r) return std::unexpected(r.error());
    VoidResult r;
    if (l.directive == "from") r = assign(scalar_text(rec, l), c.from);
    if (l.directive == "reason") r = assign(scalar_text(rec, l), c.reason);
    if (l.directive == "status") r = assign(scalar_word(rec, l, {"pending", "approved", "rejected"}), c.status);
    if (!r) return std::unexpected(r.error());
  }
  for (const auto& s : rec.sections) {
    if (s.name != "diff") {
      return fail("S07", fmt::format("unknown section '{}' in @change", s.name), SourceLoc{rec.file, s.line, 1});
    }
    for (const auto& l : s.lines) {
      if (l.directive != "+" && l.directive != "-" && l.directive != "~") {
        return fail("S04", "diff lines start with '+', '-' or '~'", SourceLoc{rec.file, l.line, l.col});
      }
      c.diff.push_back(DiffLine{l.directive[0], l.text, l.line});
    }
  }
  return c;
}

// --- whole-file helpers -----------------------------------------------------------------------

namespace {
template <class T, class F>
Result<T> parse_single(std::string_view text, std::string_view file, std::string_view kind, F&& f) {
  auto recs = parse_records(text, file);
  if (!recs) return std::unexpected(recs.error());
  if (recs->empty()) {
    return fail("S01", fmt::format("file has no @{} record", kind), SourceLoc{std::string(file), 1, 1});
  }
  if (recs->size() > 1) {
    return fail("S01", fmt::format("file must hold exactly one @{} record", kind),
                SourceLoc{std::string(file), (*recs)[1].line, 1});
  }
  if ((*recs)[0].kind != kind) {
    return fail("S02", fmt::format("expected @{}, found @{}", kind, (*recs)[0].kind),
                SourceLoc{std::string(file), (*recs)[0].line, 1});
  }
  return f((*recs)[0]);
}
}  // namespace

Result<Block> parse_block_text(std::string_view text, std::string_view file) {
  return parse_single<Block>(text, file, "block", parse_block);
}
Result<Project> parse_project_text(std::string_view text, std::string_view file) {
  return parse_single<Project>(text, file, "project", parse_project);
}
Result<Ticket> parse_ticket_text(std::string_view text, std::string_view file) {
  return parse_single<Ticket>(text, file, "ticket", parse_ticket);
}
Result<Report> parse_report_text(std::string_view text, std::string_view file) {
  return parse_single<Report>(text, file, "report", parse_report);
}
Result<Change> parse_change_text(std::string_view text, std::string_view file) {
  return parse_single<Change>(text, file, "change", parse_change);
}

// --- writers ------------------------------------------------------------------------------------

namespace {

void add_attr(std::string& s, std::string_view key, const std::string& value) {
  if (!value.empty()) s += " " + format_attr(key, value);
}

void emit_scalar(std::string& out, std::string_view directive, const Scalar& s, bool free) {
  if (!s.set) return;
  emit(out, s.trivia, format_line(directive, free ? format_free_text(s.value) : s.value));
}

void emit_tail(std::string& out, const std::vector<std::string>& tail) {
  for (const auto& c : tail) out += (c.empty() ? "#" : "# " + c) + "\n";
}

}  // namespace

std::string format_port(const Port& p) {
  std::string args;
  if (p.dir == PortDir::Fn) {
    args = p.signature();
  } else {
    args = p.name + " " + p.type;
    if (p.from) args += " from=" + p.from->str();
    if (p.dir == PortDir::In && (p.link_written || p.link != LinkKind::Data)) {
      args += " link=" + std::string(to_string(p.link));
    }
    add_attr(args, "via", p.via);
    add_attr(args, "sync", p.sync);
    add_attr(args, "rate", p.rate);
  }
  return format_line(to_string(p.dir), args);
}

std::string write_block(const Block& b) {
  std::string out;
  emit(out, b.header, "@block " + b.id);
  emit_scalar(out, "purpose", b.purpose, true);
  emit_scalar(out, "kind", b.kind_text, false);
  emit_scalar(out, "thread", b.thread, false);
  emit_scalar(out, "est", b.est, false);
  for (const auto& f : b.files) emit(out, f.trivia, format_line("files", join(f.paths, " ")));
  for (auto dir : {PortDir::In, PortDir::Out, PortDir::Fn}) {
    for (const auto& p : b.ports) {
      if (p.dir == dir) emit(out, p.trivia, format_port(p));
    }
  }
  for (const auto& c : b.calls) {
    std::string args = c.target.str();
    add_attr(args, "via", c.via);
    add_attr(args, "sync", c.sync);
    add_attr(args, "rate", c.rate);
    emit(out, c.trivia, format_line("call", args));
  }
  for (const auto& u : b.uses) emit(out, u.trivia, format_line("use", u.target));
  for (const auto& s : b.states) {
    std::string args = s.name + " " + s.type + " with=" + s.with;
    add_attr(args, "via", s.via);
    add_attr(args, "sync", s.sync);
    emit(out, s.trivia, format_line("state", args));
  }
  for (const auto& i : b.invs) emit(out, i.trivia, format_line("inv", format_free_text(i.text)));
  for (const auto& n : b.notes) emit(out, n.trivia, format_line("note", format_free_text(n.text)));
  for (const auto& h : b.hist) {
    emit(out, h.trivia, format_line("hist", h.ticket + (h.text.empty() ? "" : " " + format_free_text(h.text))));
  }
  emit_tail(out, b.tail);
  return out;
}

std::string write_project(const Project& p) {
  std::string out;
  emit(out, p.header, "@project " + p.name);
  emit_scalar(out, "idea", p.idea, true);
  emit_scalar(out, "lang", p.lang, false);
  emit_scalar(out, "build", p.build, false);
  emit_scalar(out, "std", p.std, false);
  emit_scalar(out, "test", p.test, false);
  if (p.dirs.written) {
    Dirs def;
    std::string args;
    auto add = [&](std::string_view k, const std::string& v, const std::string& dv) {
      if (v != dv) args += (args.empty() ? "" : " ") + format_attr(k, v);
    };
    add("blocks", p.dirs.blocks, def.blocks);
    add("api", p.dirs.api, def.api);
    add("impl", p.dirs.impl, def.impl);
    add("tests", p.dirs.tests, def.tests);
    add("mocks", p.dirs.mocks, def.mocks);
    emit(out, p.dirs.trivia, format_line("dirs", args));
  }
  for (const auto& r : p.rules) emit(out, r.trivia, format_line("rule", format_free_text(r.text)));
  for (const auto& n : p.notes) emit(out, n.trivia, format_line("note", format_free_text(n.text)));
  emit_tail(out, p.tail);
  return out;
}

std::string format_trail_entry(const TrailEntry& e) {
  std::string attrs;
  for (const auto& a : e.attrs) attrs += (attrs.empty() ? "" : " ") + format_attr(a.key, a.value);
  switch (e.type) {
    case TrailEntry::Type::Level: {
      std::string s = fmt::format("L{} ", e.level);
      if (attrs.empty()) return s + e.role;
      return s + pad_right(e.role, 13) + attrs;
    }
    case TrailEntry::Type::Fact: return format_line("fact", format_free_text(e.text));
    case TrailEntry::Type::Note: return format_line("note", format_free_text(e.text));
    case TrailEntry::Type::Answer: {
      std::string args = attrs;
      if (!e.text.empty()) args += (args.empty() ? "" : " ") + quote(e.text);
      return format_line("answer", args);
    }
  }
  return {};
}

std::string write_ticket(const Ticket& t) {
  std::string out;
  std::string header = "@ticket " + t.id;
  for (const auto& a : t.header_attrs) header += " " + format_attr(a.key, a.value);
  emit(out, t.header, header);
  emit_scalar(out, "kind", t.kind, false);
  emit_scalar(out, "origin", t.origin, false);
  emit_scalar(out, "status", t.status, false);
  emit_scalar(out, "symptom", t.symptom, true);
  emit_scalar(out, "task", t.task, true);
  for (const auto& f : t.facts) emit(out, f.trivia, format_line("fact", format_free_text(f.text)));
  if (!t.trail.empty() || !t.trail_trivia.empty()) {
    emit(out, t.trail_trivia, "--- trail");
    for (const auto& e : t.trail) emit(out, e.trivia, format_trail_entry(e));
  }
  if (!t.result.empty() || !t.result_trivia.empty()) {
    emit(out, t.result_trivia, "--- result");
    for (const auto& r : t.result) {
      emit(out, r.trivia, format_line(r.key, r.key == "status" ? r.text : format_free_text(r.text)));
    }
  }
  emit_tail(out, t.tail);
  return out;
}

std::string write_report(const Report& r) {
  std::string out = "@report " + r.ticket;
  if (!r.from.empty()) out += " from=" + r.from;
  if (!r.role.empty()) out += " role=" + r.role;
  out += "\n";
  auto line = [&](std::string_view d, const std::string& v, bool free) {
    if (!v.empty()) out += format_line(d, free ? format_free_text(v) : v) + "\n";
  };
  line("status", r.status, false);
  line("cause", r.cause, true);
  line("fix", r.fix, true);
  line("files", r.files, true);
  line("tests", r.tests, true);
  line("api", r.api, true);
  if (!r.proposal.empty()) {
    out += "--- diff\n";
    for (const auto& p : r.proposal) out += p + "\n";
  }
  return out;
}

std::string write_change(const Change& c) {
  std::string out;
  emit(out, c.header, "@change " + c.id);
  emit_scalar(out, "from", c.from, true);
  emit_scalar(out, "reason", c.reason, true);
  emit_scalar(out, "status", c.status, false);
  if (!c.diff.empty()) {
    out += "--- diff\n";
    for (const auto& d : c.diff) out += std::string(1, d.op) + (d.text.empty() ? "" : " " + d.text) + "\n";
  }
  emit_tail(out, c.tail);
  return out;
}

}  // namespace kk::core
