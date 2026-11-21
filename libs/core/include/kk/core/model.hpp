#pragma once
// Typed KSL records: project, block card, ticket, report, change (3, 4.3–4.4, 10).
// Each record can be parsed from a generic Record and written back in the
// canonical form (4.8): same input → same bytes.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/ksl.hpp"

namespace kk::core {

// --- enums -----------------------------------------------------------------------
enum class BlockKind { Group, Leaf, Entry, Shared };
enum class LinkKind { Data, Event, Call, State, Use, Ref };  // Ref: a [[link]] of a note (ADR 0038)
enum class PortDir { In, Out, Fn };
// Stored only in the index (3.2).
enum class BlockStatus {
  Design, ApiFrozen, TestsReady, Implemented, Passing, Failing, Drift, NeedsReview
};
enum class TicketKind { Bug, Feature, Change, Question };
enum class TicketStatus { Open, Descending, Fixing, Verifying, Closed, Escalated };
enum class ReportStatus { Fixed, Done, Failed, NeedsContractChange, NeedsInfo, NotHere };

std::string_view to_string(BlockKind k);
std::string_view to_string(LinkKind k);
std::string_view to_string(PortDir d);
std::string_view to_string(BlockStatus s);
std::string_view to_string(TicketKind k);
std::string_view to_string(TicketStatus s);
std::string_view to_string(ReportStatus s);
std::optional<BlockKind> parse_block_kind(std::string_view s);
std::optional<LinkKind> parse_link_kind(std::string_view s);
std::optional<BlockStatus> parse_block_status(std::string_view s);
std::optional<TicketKind> parse_ticket_kind(std::string_view s);
std::optional<TicketStatus> parse_ticket_status(std::string_view s);
std::optional<ReportStatus> parse_report_status(std::string_view s);

// "Worse" ranks higher; a group's status is the worst of its descendants (3.6).
int badness(BlockStatus s);

// --- common pieces -----------------------------------------------------------------
struct Scalar {
  std::string value;
  Trivia trivia;
  int line = 0;
  int col = 0;
  bool set = false;
};

struct TextItem {
  std::string text;
  Trivia trivia;
  int line = 0;
  int col = 0;
};

struct PortRef {
  std::string block;
  std::string port;
  [[nodiscard]] std::string str() const { return block + ":" + port; }
  bool operator==(const PortRef&) const = default;
};
std::optional<PortRef> parse_port_ref(std::string_view s);

// --- block card (4.4) --------------------------------------------------------------
struct Port {
  PortDir dir = PortDir::In;
  std::string name;
  std::string type;                 // in/out
  std::vector<std::string> params;  // fn
  std::string ret;                  // fn; empty when no "->"
  std::optional<PortRef> from;      // in
  LinkKind link = LinkKind::Data;   // in: data | event
  bool link_written = false;        // "link=" was present in the source
  std::string via, sync, rate;
  Trivia trivia;
  int line = 0;
  int col = 0;

  // fn: "process(Frame[]) -> void"; in/out: "Frame[]"
  [[nodiscard]] std::string signature() const;
};

struct CallDecl {
  PortRef target;
  std::string via, sync, rate;
  Trivia trivia;
  int line = 0;
  int col = 0;
};

struct UseDecl {
  std::string target;
  Trivia trivia;
  int line = 0;
  int col = 0;
};

struct StateDecl {
  std::string name, type, with, via, sync;
  Trivia trivia;
  int line = 0;
  int col = 0;
};

// "files <path>..." — the block's code lives in existing files of the project, not in
// its own api/ impl/ folders (imported projects, docs/adr/0009). Paths are relative to
// the project root; headers are the block's api, other code files its impl.
struct FilesDecl {
  std::vector<std::string> paths;
  Trivia trivia;
  int line = 0;
  int col = 0;
};

// A link in the text of a note (notes projects, docs/adr/0038): read from the note's file by the schema
// loader, never written into the card. A [[link]] names a note; in a Markdown note a [text](file.md) link
// names the note whose file it is (docs/adr/0040).
struct NoteRef {
  std::string name;    // as written between [[ and ]], or the destination of a Markdown link
  int line = 0;        // in the note's file
  std::string target;  // the note or topic it names ("" when it names none or several)
  int matches = 0;     // how many blocks the name fits
  std::string path;    // a Markdown link: the file it names, from the project root ("" for a [[link]])
};

struct HistEntry {
  std::string ticket;  // "T-0042" or "summary" for the folded line (3.10)
  std::string text;
  Trivia trivia;
  int line = 0;
};

inline constexpr std::size_t kMaxHistEntries = 10;

struct Block {
  std::string id;
  Scalar purpose, kind_text, thread, est;
  std::optional<BlockKind> kind;
  std::vector<Port> ports;  // in, out and fn in source order
  std::vector<CallDecl> calls;
  std::vector<UseDecl> uses;
  std::vector<StateDecl> states;
  std::vector<TextItem> invs;
  std::vector<TextItem> notes;
  std::vector<HistEntry> hist;
  std::vector<FilesDecl> files;
  std::vector<NoteRef> refs;  // notes projects: the [[links]] of the note's text (filled by the loader)
  Trivia header;
  std::vector<std::string> tail;
  std::string file;
  int line = 0;

  // Filled by the schema loader.
  std::string dir;     // folder relative to the project root, e.g. "blocks/stream/ring"
  std::string parent;  // "" for top-level blocks
  std::vector<std::string> children;
  int depth = 0;          // top-level = 1
  bool note_txt = false;  // notes: its own text is note.txt, written before notes were Markdown (ADR 0040)

  [[nodiscard]] const Port* port(std::string_view name) const;
  [[nodiscard]] bool has_code() const;  // leaf, entry or shared
  [[nodiscard]] std::string_view kind_name() const;
  // Code in existing files ("files" directive) instead of the block's own folders.
  [[nodiscard]] bool external() const { return !files.empty(); }
  [[nodiscard]] std::vector<std::string> file_list() const;

  // Appends a hist entry and folds entries beyond kMaxHistEntries (3.10).
  void add_hist(std::string ticket, std::string text);
};

// --- project (4.3) -------------------------------------------------------------------
struct Dirs {  // folder conventions; "dirs" directive (extension, see docs/adr/0003)
  std::string blocks = "blocks";
  std::string api = "api";
  std::string impl = "impl";
  std::string tests = "tests";
  std::string mocks = "mocks";
  bool written = false;
  Trivia trivia;
  int line = 0;
};

struct Project {
  std::string name;
  Scalar idea, lang, build, std, test;
  Dirs dirs;
  std::vector<TextItem> rules, notes;
  Trivia header;
  std::vector<std::string> tail;
  std::string file;
  int line = 0;
};

// --- ticket (10.2) ----------------------------------------------------------------------
struct TrailEntry {
  enum class Type { Level, Fact, Answer, Note };
  Type type = Type::Level;
  int level = 0;     // Level: n of "L<n>"
  std::string role;  // Level: orchestrator | navigator | worker | tester | architect
  std::vector<Attr> attrs;
  std::string text;  // Fact/Note text, Answer text
  Trivia trivia;
  int line = 0;

  [[nodiscard]] std::string get(std::string_view key) const;
  [[nodiscard]] std::vector<std::string> list(std::string_view key) const;  // comma list
  void set(std::string_view key, std::string value);
  void erase(std::string_view key);
};

struct ResultLine {
  std::string key;  // status | cause | fix | files | tests | api | verify | cost | note | state
  std::string text;
  Trivia trivia;
  int line = 0;
};

struct Ticket {
  std::string id;
  std::vector<Attr> header_attrs;  // e.g. view=path at=stream (path views)
  Scalar kind, origin, status, symptom, task;
  Scalar scope;  // the block (and its subtree) the human limited the ticket to; unset = the whole project (docs/adr/0034)
  std::vector<TextItem> facts;  // original facts (immutable, 10.1)
  std::vector<TrailEntry> trail;
  std::vector<ResultLine> result;
  Trivia header, trail_trivia, result_trivia;
  std::vector<std::string> tail;
  std::string file;
  int line = 0;

  [[nodiscard]] std::optional<TicketKind> kind_enum() const;
  [[nodiscard]] std::optional<TicketStatus> status_enum() const;
  [[nodiscard]] std::string result_value(std::string_view key) const;
};

// --- report (10.6) -----------------------------------------------------------------------
struct Report {
  std::string ticket;  // T-NNNN
  std::string from;    // block id
  std::string role;    // worker | tester | navigator | ...
  std::string status;  // fixed | done | failed | needs_contract_change | needs_info | not_here
  std::string cause, fix, files, tests, api;
  std::vector<std::string> proposal;  // KSL diff lines ("+ ...") for a contract change
};

// --- change (10.7) -------------------------------------------------------------------------
struct DiffLine {
  char op = '+';     // '+', '-', '~'
  std::string text;  // raw KSL line
  int line = 0;
};

struct Change {
  std::string id;  // C-NNNN
  Scalar from, reason, status;
  std::vector<DiffLine> diff;
  Trivia header;
  std::vector<std::string> tail;
  std::string file;
  int line = 0;
};

// --- directive table (4.4, 4.5) ----------------------------------------------------------------
struct DirectiveSpec {
  std::string_view record;  // "block", "project", "ticket", "report", "change"
  std::string_view name;
  int max;                  // -1 = unlimited
  std::string_view args;    // human-readable argument shape (for the KSL reference)
  std::string_view kinds;   // block kinds where allowed ("" = all), for E08
};
const std::vector<DirectiveSpec>& directive_table();
const DirectiveSpec* find_directive(std::string_view record, std::string_view name);

// --- parse / write -------------------------------------------------------------------------------
Result<Block> parse_block(const Record& rec);
Result<Project> parse_project(const Record& rec);
Result<Ticket> parse_ticket(const Record& rec);
Result<Report> parse_report(const Record& rec);
Result<Change> parse_change(const Record& rec);

// Whole-file helpers: the file must hold exactly one record of that kind.
Result<Block> parse_block_text(std::string_view text, std::string_view file);
Result<Project> parse_project_text(std::string_view text, std::string_view file);
Result<Ticket> parse_ticket_text(std::string_view text, std::string_view file);
Result<Report> parse_report_text(std::string_view text, std::string_view file);
Result<Change> parse_change_text(std::string_view text, std::string_view file);

// Canonical writers (4.8). Output ends with '\n'.
std::string write_block(const Block& b);
std::string write_project(const Project& p);
std::string write_ticket(const Ticket& t);
std::string write_report(const Report& r);
std::string write_change(const Change& c);

// Single canonical lines, used by writers, views and diffs.
std::string format_port(const Port& p);  // "in      push Frame[] from=..."
std::string format_trail_entry(const TrailEntry& e);

}  // namespace kk::core
