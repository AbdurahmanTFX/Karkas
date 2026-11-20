#include "kk/agents/tools.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>

#include "kk/agents/changes.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/templates.hpp"

namespace kk::agents {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json str(const char* d) { return {{"type", "string"}, {"description", d}}; }
// The plan of a feature: the blocks that follow the first one, in order (orchestrator).
json plan_steps() {
  return {{"type", "array"},
          {"maxItems", 8},
          {"description", "blocks that follow, in order: {block, difficulty S|M|L, why}"},
          {"items",
           {{"type", "object"},
            {"properties", {{"block", {{"type", "string"}}}, {"difficulty", {{"type", "string"}, {"enum", {"S", "M", "L"}}}},
                            {"why", {{"type", "string"}, {"maxLength", 160}}}}},
            {"required", {"block", "difficulty", "why"}}}}};
}
json obj(json props, std::vector<std::string> required = {}) {
  json j = {{"type", "object"}, {"properties", std::move(props)}};
  if (!required.empty()) j["required"] = required;
  return j;
}

const std::map<std::string, std::pair<std::string, json>>& defs() {
  static const std::map<std::string, std::pair<std::string, json>> d = {
      {"view", {"KSL view of a block: L1 for a group, L2 for a leaf (card + neighbor ports).",
                obj({{"block", str("block id")}, {"level", {{"type", "integer"}, {"enum", {1, 2}}}}}, {"block"})}},
      {"neighbors", {"Links crossing the boundary of a block.",
                     obj({{"block", str("block id")},
                          {"kind", {{"type", "string"}, {"enum", {"data", "event", "call", "state", "use"}}}},
                          {"dir", {{"type", "string"}, {"enum", {"in", "out", "both"}}}}},
                         {"block"})}},
      {"dependents", {"Blocks that depend on the interface of a block.", obj({{"block", str("block id")}}, {"block"})}},
      {"path", {"Chain of links between two blocks.", obj({{"from", str("block id")}, {"to", str("block id")}}, {"from", "to"})}},
      {"search", {"Full-text search over purposes, invariants and history.", obj({{"text", str("words")}}, {"text"})}},
      {"history", {"Recent bug/fix history of a block.",
                   obj({{"block", str("block id")}, {"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", 30}}}}, {"block"})}},
      {"status", {"Statuses, failing tests and drift of a block (or of the project).", obj({{"block", str("block id, optional")}})}},
      {"probes", {"Probe summaries (numbers, not logs) for a block or a link a->b.", obj({{"target", str("block id or a->b")}})}},
      {"dispatch",
       {"Send the ticket down to a child block with your trail entry. A feature over several blocks: list the blocks "
        "that follow in next, in order; the program sends them one after another.",
        obj({{"block", str("child block id")},
             {"suspect", {{"type", "array"}, {"items", {{"type", "string"}}}}},
             {"ruled_out", {{"type", "array"}, {"items", {{"type", "string"}}}}},
             {"difficulty", {{"type", "string"}, {"enum", {"S", "M", "L"}}}},
             {"why", {{"type", "string"}, {"maxLength", 160}}},
             {"new_facts", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 2}}},
             {"next", plan_steps()}},
            {"block", "difficulty", "why"})}},
      {"ask", {"Ask one short question to the agent of a child block; you get only its answer (<= 80 tokens).",
               obj({{"block", str("child block id")}, {"question", str("one question")}}, {"block", "question"})}},
      {"run_tests", {"Build and run the tests of a block subtree; returns a compact summary.", obj({{"block", str("block id")}}, {"block"})}},
      {"propose_change",
       {"Propose new blocks, ports, links or constants as a KSL diff (approved by the human, or at once in Auto). next: "
        "the blocks to send after the approval, in order.",
        obj({{"reason", str("one line")},
             {"diff", str("KSL diff: '+ @block x.y' then '+ purpose ...' lines; '~ @block z' then '+ line' / '- line'; "
                          "'+ const <name> <C++ type> <value> # note' adds a constant to the api/ of the selected block")},
             {"next", plan_steps()}},
            {"reason", "diff"})}},
      {"ksl_reference", {"The whole KSL syntax of cards and change diffs, with an example change.", obj(json::object())}},
      {"close", {"Close the ticket: no work needed (duplicate, not a bug, done already) or the work is complete.",
                 obj({{"verdict", {{"type", "string"}, {"enum", {"done", "not_a_bug", "duplicate", "wont_fix"}}}},
                      {"note", {{"type", "string"}, {"maxLength", 200}}}},
                     {"verdict"})}},
      {"escalate", {"Give the ticket to the human when no agent can go on (for example a change of a type in a "
                    "frozen api/ that no card diff expresses). Say exactly what the human must do.",
                    obj({{"note", {{"type", "string"}, {"maxLength", 300}}}}, {"note"})}},
      {"report_leaf",
       {"Finish your work and report to the level above.",
        obj({{"status", {{"type", "string"},
                         {"enum", {"fixed", "done", "failed", "needs_contract_change", "needs_info", "not_here", "needs_tester"}}}},
             {"cause", {{"type", "string"}, {"maxLength", 200}}},
             {"fix", {{"type", "string"}, {"maxLength", 200}}},
             {"files", {{"type", "array"}, {"items", {{"type", "string"}}}}},
             {"tests", {{"type", "string"}, {"maxLength", 120}}},
             {"api", {{"type", "string"}, {"enum", {"unchanged", "changed", "change_requested"}}}},
             {"proposal", str("KSL diff if a contract change is requested")}},
            {"status"})}},
      {"report_navigator", {"The cause is outside your block: send the ticket back up.",
                            obj({{"status", {{"type", "string"}, {"enum", {"not_here"}}}},
                                 {"cause", {{"type", "string"}, {"maxLength", 200}}}},
                                {"status", "cause"})}},
      {"report_architect", {"Finish the design step.",
                            obj({{"status", {{"type", "string"}, {"enum", {"done", "failed"}}}},
                                 {"summary", {{"type", "string"}, {"maxLength", 300}}}},
                                {"status"})}},
      {"answer", {"Answer the question (at most 80 tokens of observed facts).",
                  obj({{"text", {{"type", "string"}, {"maxLength", 400}}}}, {"text"})}},
      {"read_file", {"Read a file of your block (impl/x.cpp) or a neighbor api (<block>:api/x.hpp).",
                     obj({{"path", str("relative path")},
                          {"offset", {{"type", "integer"}, {"description", "first line, 1-based"}}},
                          {"limit", {{"type", "integer"}, {"description", "number of lines"}}}},
                         {"path"})}},
      {"list_dir", {"List a folder of your block or a neighbor api.", obj({{"path", str("relative path, default .")}})}},
      {"grep", {"Search a regular expression in the files you may read.",
                obj({{"pattern", str("ECMAScript regex")}, {"path", str("folder, default .")}}, {"pattern"})}},
      {"write_file", {"Create or replace a file inside the folders you may write.",
                      obj({{"path", str("relative to the block root, e.g. impl/ring.cpp")}, {"content", str("full text")}},
                          {"path", "content"})}},
      {"edit_file", {"Replace one exact, unique string in a file inside your block.",
                     obj({{"path", str("relative to block root, e.g. impl/ring.cpp")}, {"old", {{"type", "string"}}}, {"new", {{"type", "string"}}}},
                         {"path", "old", "new"})}},
      {"build", {"Configure and build the project; returns compressed compiler errors.", obj(json::object())}},
      {"test", {"Build and run the tests of your block; returns pass counts and failing cases.",
                obj({{"filter", str("optional test case name filter")}})}},
      {"write_card", {"Write the KSL card of a block (creates its folder).",
                      obj({{"block", str("block id")}, {"ksl", str("full @block record")}}, {"block", "ksl"})}},
      {"write_api", {"Write a C++ api/ header of a leaf (interfaces and plain types only).",
                     obj({{"block", str("leaf id")}, {"file", str("header name, e.g. ring.hpp")}, {"code", str("C++ code")}},
                         {"block", "file", "code"})}},
  };
  return d;
}

std::string sget(const json& j, const char* k) {
  if (!j.contains(k) || j[k].is_null()) return {};
  if (j[k].is_string()) return j[k].get<std::string>();
  return j[k].dump();
}

ToolOutcome ok(std::string s) { return {std::move(s), false, false}; }
ToolOutcome err(std::string s) { return {std::move(s), true, false}; }
ToolOutcome done(std::string s) { return {std::move(s), false, true}; }

ToolOutcome from(const kk::Result<std::string>& r) { return r ? ok(*r) : err(r.error().message); }

json parse_input(const ToolCall& c, bool& bad) {
  auto j = json::parse(c.input_json.empty() ? "{}" : c.input_json, nullptr, false);
  bad = j.is_discarded() || !j.is_object() || j.contains("_unparsed");
  return bad ? json::object() : j;
}

// Shared query tools of schema-level agents.
std::optional<ToolOutcome> query_tool(index::Index& idx, const std::string& name, const json& in, const std::string& scope) {
  if (name == "view") {
    std::string b = sget(in, "block");
    if (b.empty()) b = scope;
    int level = in.value("level", 0);
    if (b.empty() || b == "root") return ok(idx.overview());
    if (level == 0) {
      auto snap = idx.snapshot();
      const auto* row = snap.block(b);
      level = row != nullptr && row->kind == "group" ? 1 : 2;
    }
    return from(idx.view(b, level));
  }
  if (name == "neighbors") {
    std::string dir = sget(in, "dir");
    return from(idx.neighbors(sget(in, "block"), sget(in, "kind"), dir.empty() ? "both" : dir));
  }
  if (name == "dependents") return from(idx.dependents(sget(in, "block")));
  if (name == "path") return from(idx.path(sget(in, "from"), sget(in, "to")));
  if (name == "search") return ok(idx.search(sget(in, "text")));
  if (name == "history") return from(idx.history(sget(in, "block"), in.value("n", 10)));
  if (name == "status") return from(idx.status(sget(in, "block")));
  if (name == "probes") return ok(idx.probes(sget(in, "target")));
  return std::nullopt;
}

}  // namespace

Tool tool_def(std::string_view name) {
  std::string key(name);
  auto it = defs().find(key);
  if (it == defs().end()) return {key, "", "{}"};
  std::string public_name = key.starts_with("report_") ? "report" : key;
  return {public_name, it->second.first, it->second.second.dump()};
}

// --- schema tools -----------------------------------------------------------------------

SchemaToolHost::SchemaToolHost(index::Index& idx, Role role, std::string scope, SchemaCallbacks cb)
    : idx_(idx), role_(role), scope_(std::move(scope)), cb_(std::move(cb)) {}

std::vector<Tool> SchemaToolHost::tools() const {
  std::vector<std::string> names = {"view", "neighbors", "dependents", "path", "search", "history", "status", "dispatch"};
  // Probe summaries only when the project has any: an empty tool is a step spent for nothing.
  if (!idx_.snapshot().probes.empty()) names.insert(names.end() - 1, "probes");
  if (cb_.ask) names.push_back("ask");
  if (cb_.run_tests) names.push_back("run_tests");
  if (role_ == Role::Orchestrator) {
    names.push_back("propose_change");
    names.push_back("ksl_reference");
    names.push_back("close");
    names.push_back("escalate");
  } else {
    names.push_back("report_navigator");
  }
  std::vector<Tool> out;
  for (const auto& n : names) out.push_back(tool_def(n));
  return out;
}

// The `next` steps of a plan: known blocks inside the project, each once, none that answered in this
// run, at most 8. Empty when fine.
namespace {
std::string plan_error(index::Index& idx, const SchemaCallbacks& cb, const json& in, const std::string& first) {
  if (!in.contains("next") || in["next"].is_null()) return {};
  if (!in["next"].is_array()) return "next must be a list of {block, difficulty, why}";
  if (!cb.plans) return "next is for a feature planned by the orchestrator; dispatch one block";
  if (in["next"].size() > 8) return "at most 8 blocks in next; plan the rest after them";
  auto snap = idx.snapshot();
  std::set<std::string> seen;
  if (!first.empty()) seen.insert(first);
  for (const auto& step : in["next"]) {
    if (!step.is_object()) return "each step of next is {block, difficulty, why}";
    std::string b = sget(step, "block");
    if (snap.block(b) == nullptr) return fmt::format("next: unknown block '{}'", b);
    if (!cb.limit.empty() && b != cb.limit && !index::Snapshot::under(cb.limit, b)) {
      return fmt::format("next: the human limited this ticket to {}; {} is outside it", cb.limit, b);
    }
    if (!seen.insert(b).second) return fmt::format("next: {} is planned twice", b);
    auto d = sget(step, "difficulty");
    if (d != "S" && d != "M" && d != "L") return fmt::format("next: difficulty of {} must be S, M or L", b);
    if (cb.excluded) {
      if (auto why = cb.excluded(b); !why.empty()) return fmt::format("next: {} already answered in this run ({})", b, why);
    }
  }
  return {};
}
}  // namespace

ToolOutcome SchemaToolHost::call(const ToolCall& c) {
  bool bad = false;
  auto in = parse_input(c, bad);
  if (bad) return err("invalid JSON input");
  if (auto q = query_tool(idx_, c.name, in, scope_)) return *q;
  if (c.name == "ksl_reference" && role_ == Role::Orchestrator) {
    return ok(std::string(templates::get("prompts/ksl_reference.txt").value_or("")) + "\n" +
              std::string(templates::get("prompts/ksl_change_example.txt").value_or("")));
  }
  if (c.name == "dispatch") {
    auto snap = idx_.snapshot();
    std::string b = sget(in, "block");
    if (snap.block(b) == nullptr) return err(fmt::format("unknown block '{}'", b));
    std::string scope = scope_ == "root" ? "" : scope_;
    if (b == scope || !index::Snapshot::under(scope, b)) {
      return err(fmt::format("dispatch must name a block inside {}", scope.empty() ? "the project" : scope));
    }
    if (!cb_.limit.empty() && b != cb_.limit && !index::Snapshot::under(cb_.limit, b)) {
      return err(fmt::format("the human limited this ticket to {} (and the blocks inside it): dispatch {} or a block inside it, "
                             "or escalate when the request needs a block outside it",
                             cb_.limit, cb_.limit));
    }
    if (cb_.excluded) {
      if (auto why = cb_.excluded(b); !why.empty()) {
        return err(fmt::format("{} already answered in this run ({}); sending it the same ticket again changes nothing. "
                               "Pick another block{}.",
                               b, why,
                               role_ == Role::Orchestrator ? ", propose_change, escalate to the human or close"
                                                           : ", or report not_here"));
      }
    }
    auto diff = sget(in, "difficulty");
    if (diff != "S" && diff != "M" && diff != "L") return err("difficulty must be S, M or L");
    if (sget(in, "why").size() > 200) return err("why must be at most 20 words");
    if (in.contains("new_facts") && in["new_facts"].is_array() && in["new_facts"].size() > 2) return err("at most 2 new facts");
    if (auto why = plan_error(idx_, cb_, in, b); !why.empty()) return err(why);
    return done("dispatched to " + b);
  }
  if (c.name == "ask" && cb_.ask) return ok(cb_.ask(sget(in, "block"), sget(in, "question")));
  if (c.name == "run_tests" && cb_.run_tests) return ok(cb_.run_tests(sget(in, "block")));
  if (c.name == "propose_change" && role_ == Role::Orchestrator) {
    auto d = parse_diff(sget(in, "diff"));
    if (!d) return err(d.error().message);
    if (d->empty()) return err("the diff is empty");
    // The same checks as a worker's proposal: a diff that changes no card or breaks the
    // schema is refused here, so the model corrects it now and not the human at approval.
    if (cb_.check_change) {
      if (auto why = cb_.check_change(sget(in, "diff")); !why.empty()) return err("the change cannot be proposed: " + why);
    }
    if (auto why = plan_error(idx_, cb_, in, ""); !why.empty()) return err(why);
    return done("change proposed");
  }
  if (c.name == "close" && role_ == Role::Orchestrator) return done("closed");
  if (c.name == "escalate" && role_ == Role::Orchestrator) {
    if (core::trim(sget(in, "note")).empty()) return err("say what the human must do in note");
    return done("given to the human");
  }
  if (c.name == "report" && role_ == Role::Navigator) {
    if (sget(in, "status") != "not_here") return err("a navigator reports only status=not_here");
    return done("sent back up");
  }
  return err("unknown tool " + c.name);
}

// --- leaf tools ---------------------------------------------------------------------------

namespace {

// "no such file: x; impl/ has: a.cpp, b.cpp" — what is there instead, so the agent does not guess.
std::string missing(const Sandbox& sb, const fs::path& abs, const char* what, const std::string& asked) {
  std::string out = fmt::format("no such {}: {}", what, asked);
  std::error_code ec;
  for (auto dir = abs.parent_path(); !dir.empty() && dir != dir.parent_path(); dir = dir.parent_path()) {
    if (!sb.resolve_read(sb.display(dir)).has_value()) break;  // left the sandbox
    if (!fs::is_directory(dir, ec)) continue;
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
      auto n = e.path().filename().string();
      if (n.starts_with(".")) continue;
      names.push_back(e.is_directory(ec) ? n + "/" : n);
    }
    std::sort(names.begin(), names.end());
    if (names.size() > 30) {
      auto more = names.size() - 30;
      names.resize(30);
      names.push_back(fmt::format("and {} more", more));
    }
    auto where = sb.display(dir);
    out += fmt::format("; {} has: {}", where == "." ? std::string("your folder") : where,
                       names.empty() ? std::string("nothing yet") : core::join(names, ", "));
    break;
  }
  return out;
}

// A failed build whose errors are all in files this agent may not change: say so, or it spends its
// steps on other blocks' errors (free-AI log: testers looping on stale impl/ of other leaves).
std::string build_hint(const Sandbox& sb, const std::string& summary) {
  bool mine = false;
  bool cmake = false;
  std::vector<std::string> elsewhere;
  for (const auto& line : core::split(summary, '\n')) {
    if (line.starts_with("CMake Error at CMakeLists.txt")) {
      cmake = true;
      continue;
    }
    auto colon = line.find(':');
    if (colon == std::string::npos || colon == 0) continue;
    std::string file = line.substr(0, colon);
    if (file.front() == '/' || !core::contains(file, "/")) continue;  // system headers, linker lines
    auto abs = core::normalize(sb.project() / file);
    if (sb.resolve_write(sb.display(abs)).has_value()) {
      mine = true;
      break;
    }
    if (std::find(elsewhere.begin(), elsewhere.end(), file) == elsewhere.end() && elsewhere.size() < 6) elsewhere.push_back(file);
  }
  if (mine) return "";
  std::string out;
  if (!elsewhere.empty()) {
    out += fmt::format("None of these errors is in a file you may change ({}). Do not fix or work around them: do your own part, "
                       "and if it cannot be checked, report it with these files named in cause.\n",
                       core::join(elsewhere, ", "));
  }
  if (cmake) {
    out += sb.imported() ? "CMakeLists.txt is the project's own build and not yours to edit; a new source file of your card is added "
                           "to it by Karkas when you report done.\n"
                         : "CMakeLists.txt is generated by Karkas from the schema; it is not yours to edit.\n";
  }
  return out;
}

// The compiler warnings in files this agent may change: a change must not add warnings, and the
// project's build shows only the files it compiled (ADR 0027).
std::string own_warnings(const Sandbox& sb, const std::string& warnings) {
  std::vector<std::string> mine;
  for (const auto& line : core::split(warnings, '\n')) {
    auto colon = line.find(':');
    if (colon == std::string::npos || colon == 0) continue;
    std::string file = line.substr(0, colon);
    if (file.front() == '/') continue;  // system headers
    auto abs = core::normalize(sb.project() / file);
    if (sb.resolve_write(sb.display(abs)).has_value()) mine.push_back(line);
  }
  if (mine.empty()) return "";
  return "warnings in your files (a change must not add warnings: fix the ones your change made):\n" + core::join(mine, "\n") + "\n";
}

}  // namespace

LeafToolHost::LeafToolHost(Sandbox sandbox, Role role, ITestService& tests, std::vector<std::string> test_labels)
    : sb_(std::move(sandbox)), role_(role), tests_(tests), labels_(std::move(test_labels)) {}

std::vector<Tool> LeafToolHost::tools() const {
  std::vector<Tool> out;
  for (const char* n : {"read_file", "list_dir", "grep", "write_file", "edit_file", "build", "test", "report_leaf"}) {
    out.push_back(tool_def(n));
  }
  return out;
}

ToolOutcome LeafToolHost::call(const ToolCall& c) {
  bool bad = false;
  auto in = parse_input(c, bad);
  if (bad) return err("invalid JSON input");
  if (c.name == "read_file") {
    auto p = sb_.resolve_read(sget(in, "path"));
    if (!p) return err(p.error().message);
    if (fs::is_directory(*p)) return err("this is a folder; use list_dir");
    if (std::error_code ec; !fs::exists(*p, ec)) return err(missing(sb_, *p, "file", sget(in, "path")));
    auto text = core::read_file(*p);
    if (!text) return err("cannot read " + sget(in, "path"));
    reads_.push_back(sb_.display(*p));
    int offset = in.value("offset", 0);
    int limit = in.value("limit", 0);
    if (offset > 0 || limit > 0) {
      // Raw lines: core::split trims, and a model copies these lines into edit_file's `old`, which
      // must match the file with its indentation (flappy T-0014: a tester got its code unindented).
      std::vector<std::string> lines;
      for (std::size_t start = 0; start <= text->size();) {
        auto end = text->find('\n', start);
        if (end == std::string::npos) end = text->size();
        lines.push_back(text->substr(start, end - start));
        start = end + 1;
      }
      std::string out;
      int from_line = std::max(1, offset);
      int to_line = limit > 0 ? from_line + limit - 1 : static_cast<int>(lines.size());
      for (int i = from_line; i <= to_line && i <= static_cast<int>(lines.size()); ++i) out += lines[static_cast<std::size_t>(i - 1)] + "\n";
      return ok(out);
    }
    constexpr std::size_t kMax = 64 * 1024;
    if (text->size() > kMax) return ok(core::truncate_utf8(*text, kMax) + "\n... (truncated; use offset/limit)");
    return ok(*text);
  }
  if (c.name == "list_dir") {
    std::string path = sget(in, "path");
    auto p = sb_.resolve_read(path.empty() ? "." : path);
    if (!p) return err(p.error().message);
    std::error_code ec;
    if (!fs::exists(*p, ec)) {
      // The own folder of an integration tester exists only after its first file.
      if (sb_.display(*p) == ".") return ok("(empty: nothing is written here yet)\n");
      return err(missing(sb_, *p, "folder", path));
    }
    if (!fs::is_directory(*p, ec)) return err(fmt::format("{} is a file, not a folder: use read_file", path));
    std::vector<std::string> entries;
    std::size_t hidden = 0;
    for (const auto& e : fs::directory_iterator(*p, ec)) {
      auto n = e.path().filename().string();
      if (n.starts_with(".")) continue;
      // Only what can be read is listed (an imported block's folder holds other blocks' files too).
      if (!sb_.resolve_read(sb_.display(e.path())).has_value()) {
        ++hidden;
        continue;
      }
      if (e.is_directory(ec)) {
        entries.push_back(n + "/");
      } else {
        entries.push_back(fmt::format("{} {}", n, e.file_size(ec)));
      }
    }
    std::sort(entries.begin(), entries.end());
    std::string out = entries.empty() ? "(empty)\n" : core::join(entries, "\n") + "\n";
    if (hidden > 0 && sb_.imported()) out += fmt::format("({} more entries belong to other blocks or are not readable)\n", hidden);
    return ok(out);
  }
  if (c.name == "grep") {
    std::regex re;
    try {
      re = std::regex(sget(in, "pattern"));
    } catch (const std::regex_error& e) {
      return err(std::string("bad regex: ") + e.what());
    }
    std::string path = sget(in, "path");
    auto p = sb_.resolve_read(path.empty() ? "." : path);
    if (!p) return err(p.error().message);
    std::vector<std::string> hits;
    std::error_code ec;
    auto scan = [&](const fs::path& f) {
      if (!sb_.resolve_read(sb_.display(f))) return;
      std::error_code size_ec;
      if (fs::file_size(f, size_ec) > (4u << 20)) return;
      auto text = core::read_file(f);
      if (!text) return;
      if (text->find('\0') < 1024) return;  // binary (images, sounds)
      int n = 0;
      for (const auto& line : core::split(*text, '\n', false)) {
        ++n;
        if (hits.size() >= 100) return;
        if (std::regex_search(line, re)) hits.push_back(fmt::format("{}:{}: {}", sb_.display(f), n, core::trim(line)));
      }
      reads_.push_back(sb_.display(f));
    };
    if (fs::is_regular_file(*p, ec)) {
      scan(*p);
    } else if (auto listed = sb_.search_files(*p); !listed.empty()) {
      // An imported block from the project root: its files and the headers it may read.
      for (const auto& f : listed) scan(f);
    } else {
      for (auto it = fs::recursive_directory_iterator(*p, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) {
          // Folders the agent may not read are not walked (an imported project's build tree).
          if (!sb_.resolve_read(sb_.display(it->path())).has_value()) it.disable_recursion_pending();
          continue;
        }
        if (it->is_regular_file(ec)) scan(it->path());
      }
    }
    return ok(hits.empty() ? "(no matches)\n" : core::join(hits, "\n") + "\n");
  }
  if (c.name == "write_file" || c.name == "edit_file") {
    auto p = sb_.resolve_write(sget(in, "path"));
    if (!p) return err(p.error().message);
    std::string content;
    if (c.name == "edit_file") {
      auto text = core::read_file(*p);
      if (!text) return err("cannot read " + sget(in, "path"));
      std::string old = sget(in, "old");
      if (old.empty()) return err("old must not be empty");
      std::size_t first = text->find(old);
      if (first == std::string::npos) return err("old text not found");
      if (text->find(old, first + 1) != std::string::npos) return err("old text is not unique; include more context");
      content = text->substr(0, first) + sget(in, "new") + text->substr(first + old.size());
    } else {
      content = sget(in, "content");
    }
    if (sb_.is_own_api(*p)) {
      if (sb_.api_access() == ApiAccess::ValuesOnly) {
        auto before = core::read_file(*p).value_or("");
        if (!api_values_or_new_constants(before, content)) {
          return err("api/ is the frozen contract: only values of its constants may change and new constexpr constants be "
                     "added (a removed or changed declaration, a new type, field or function is a contract change: "
                     "report needs_contract_change with a KSL proposal)");
        }
      } else if (sb_.api_access() == ApiAccess::Imported) {
        // The public part of an imported header is what other blocks build on (ADR 0027).
        std::string why;
        auto before = core::read_file(*p);
        if (before && !header_contract_kept(*before, content, &why)) {
          return err(fmt::format("{}: {}. The public part of this header is the contract of {}: private members, inline "
                                 "bodies, initial values, comments, new #include lines and new public constants may "
                                 "change; a new or changed public function, type or field is a card change: report "
                                 "needs_contract_change with the card line (e.g. '~ @block {}' then '+ fn reset() -> void')",
                                 sb_.display(*p), why, sb_.block(), sb_.block()));
        }
      }
      api_written_ = true;
    }
    if (auto r = core::write_file(*p, content); !r) return err(r.error().message);
    written_.insert(sb_.display(*p));
    return ok(fmt::format("wrote {} ({} bytes)", sb_.display(*p), content.size()));
  }
  if (c.name == "build" || c.name == "test") {
    // build() of an imported block compiles the sources of its card (seconds); test() is the project's
    // full build and tests (minutes), which the ticket runs after its last block anyway (ADR 0028).
    std::vector<std::string> sources;
    if (c.name == "build" && sb_.imported()) {
      for (const auto& f : sb_.own_files()) {
        fs::path p(f);
        if (core::is_code_file(p) && p.extension().string().starts_with(".c")) sources.push_back(f);
      }
    }
    auto r = sources.empty() ? tests_.build_and_test(labels_, c.name == "build") : tests_.compile_check(sources);
    if (!r) return err(r.error().message);
    auto warn = own_warnings(sb_, r->build_warnings);
    if (c.name == "build") {
      return ok(r->build_ok ? (sources.empty() ? std::string("build ok\n") : r->text) + warn
                            : "build failed\n" + r->build_summary + "\n" + build_hint(sb_, r->build_summary));
    }
    last_test_ = r->text;
    if (!r->build_ok) return ok(r->text + build_hint(sb_, r->build_summary));
    if (r->text.starts_with("tests none found")) {
      return ok(r->text + warn +
                fmt::format("No test of {} exists yet. Karkas compiles every tests/**/*.cpp of the block into one test "
                            "program (doctest: #include <doctest/doctest.h>, TEST_CASE, CHECK; no main(), no CMake "
                            "file) at the next build() or test(): write the test first.\n",
                            sb_.block()));
    }
    return ok(r->text + warn);
  }
  if (c.name == "report") {
    std::string st = sget(in, "status");
    static const std::vector<std::string> worker = {"fixed",      "done",     "failed",      "needs_contract_change",
                                                    "needs_info", "not_here", "needs_tester"};
    if (st.empty()) {
      return err(fmt::format("report needs status, one of: {}", core::join(worker, ", ")));
    }
    if (std::find(worker.begin(), worker.end(), st) == worker.end()) {
      return err(fmt::format("unknown status '{}'; use one of: {}", st, core::join(worker, ", ")));
    }
    if (role_ == Role::Tester && st != "done" && st != "failed") return err("a tester reports status done or failed");
    // A proposal that cannot become a @change (a unified diff of api/, a '~ line' edit, a diff that
    // changes no card or breaks the schema) is refused here so the model fixes it in the same run.
    if (st == "needs_contract_change") {
      auto prop = sget(in, "proposal");
      if (!prop.empty()) {
        if (auto ok = check_proposal(sb_.project(), prop); !ok) {
          return err("proposal cannot become a @change (" + ok.error().message +
                     "). Card diff syntax: '~ @block <id>' selects the block, '+ <card line>' adds a line, "
                     "'- <card line>' removes one; to change a line send '- old line' then '+ new line' "
                     "('~' never goes before a card line). Or leave proposal empty and name the missing "
                     "declaration in cause and fix.");
        }
        // Auto: approved now, and the worker goes on with the new card in this same run (no round
        // through the orchestrator and a second whole run of this worker).
        if (approve_now_ && role_ == Role::Worker) {
          if (auto r = approve_now_(sget(in, "cause"), prop)) {
            // The approved card may give the block new files or links: the sandbox follows it
            // (an imported block that took a data file writes it in this same run).
            if (auto schema = core::load_schema(sb_.project())) sb_ = Sandbox(*schema, sb_.block(), role_);
            sb_.set_api_access(ApiAccess::Full);
            approved_in_run_ = true;
            return ok(*r);
          }
        }
      }
    }
    return done("reported " + st);
  }
  return err("unknown tool " + c.name);
}

// --- architect tools ----------------------------------------------------------------------

ArchitectToolHost::ArchitectToolHost(fs::path root, index::Index& idx, std::string scope, std::set<std::string> frozen)
    : root_(std::move(root)), idx_(idx), scope_(std::move(scope)), frozen_(std::move(frozen)) {}

std::vector<Tool> ArchitectToolHost::tools() const {
  std::vector<Tool> out;
  for (const char* n : {"view", "search", "write_card", "write_api", "propose_change", "report_architect"}) out.push_back(tool_def(n));
  return out;
}

ToolOutcome ArchitectToolHost::call(const ToolCall& c) {
  bool bad = false;
  auto in = parse_input(c, bad);
  if (bad) return err("invalid JSON input");
  if (c.name == "view" || c.name == "search") return *query_tool(idx_, c.name, in, scope_);
  auto schema = core::load_schema(root_);
  if (!schema) return err(schema.error().message);
  auto in_scope = [&](const std::string& id) {
    if (scope_.empty()) return core::id_depth(id) == 1 || id.starts_with("shared.");
    return id == scope_ || core::Schema::is_under(scope_, id);
  };
  if (c.name == "write_card") {
    std::string id = sget(in, "block");
    if (!core::is_block_path(id)) return err(fmt::format("'{}' is not a block id", id));
    if (!in_scope(id)) return err(fmt::format("{} is outside your scope ({})", id, scope_.empty() ? "top level" : scope_));
    auto parent = core::parent_id(id);
    if (!parent.empty() && schema->find(parent) == nullptr) return err(fmt::format("write the card of {} first", parent));
    auto b = core::parse_block_text(sget(in, "ksl"), id + "/block.ksl");
    if (!b) return err(b.error().str());
    if (b->id != id) return err(fmt::format("the card says @block {}, expected {}", b->id, id));
    auto path = schema->block_dir(id) / "block.ksl";
    if (auto r = core::write_file(path, core::write_block(*b)); !r) return err(r.error().message);
    written_.push_back(core::rel_str(root_, path));
    (void)idx_.update();
    return ok("wrote " + core::rel_str(root_, path));
  }
  if (c.name == "write_api") {
    std::string id = sget(in, "block");
    const auto* b = schema->find(id);
    if (b == nullptr) return err(fmt::format("write the card of {} first", id));
    if (!b->has_code()) return err(fmt::format("{} is a group: api/ belongs to leaves", id));
    if (!in_scope(id)) return err(fmt::format("{} is outside your scope", id));
    if (frozen_.contains(id)) return err(fmt::format("api/ of {} is frozen; propose a change instead", id));
    std::string file = sget(in, "file");
    fs::path fp(file);
    bool safe = !file.empty() && fp.is_relative() && std::none_of(fp.begin(), fp.end(), [](const fs::path& p) { return p == ".."; });
    auto ext = fp.extension().string();
    if (!safe || (ext != ".hpp" && ext != ".h")) return err("file must be a relative .hpp or .h name");
    auto path = schema->block_dir(id) / schema->project.dirs.api / fp;
    if (auto r = core::write_file(path, sget(in, "code")); !r) return err(r.error().message);
    written_.push_back(core::rel_str(root_, path));
    return ok("wrote " + core::rel_str(root_, path));
  }
  if (c.name == "propose_change") {
    auto d = parse_diff(sget(in, "diff"));
    if (!d) return err(d.error().message);
    return done("change proposed; it waits for human approval");
  }
  if (c.name == "report") return done("reported " + sget(in, "status"));
  return err("unknown tool " + c.name);
}

// --- ask tools ---------------------------------------------------------------------------------

AskToolHost::AskToolHost(index::Index& idx, const core::Schema& schema, std::string block) : idx_(idx), block_(std::move(block)) {
  const auto* b = schema.find(block_);
  leaf_ = b != nullptr && b->has_code();
  if (leaf_) sb_.emplace(schema, block_, Role::Worker);
}

std::vector<Tool> AskToolHost::tools() const {
  std::vector<Tool> out;
  if (leaf_) {
    for (const char* n : {"read_file", "list_dir", "grep"}) out.push_back(tool_def(n));
  } else {
    for (const char* n : {"view", "neighbors", "status", "probes"}) out.push_back(tool_def(n));
  }
  out.push_back(tool_def("answer"));
  return out;
}

ToolOutcome AskToolHost::call(const ToolCall& c) {
  bool bad = false;
  auto in = parse_input(c, bad);
  if (bad) return err("invalid JSON input");
  if (c.name == "answer") return done("answered");
  if (!leaf_) {
    if (auto q = query_tool(idx_, c.name, in, block_)) return *q;
    return err("unknown tool " + c.name);
  }
  if (c.name == "read_file" || c.name == "list_dir" || c.name == "grep") {
    struct NoTests final : ITestService {
      kk::Result<TestRun> build_and_test(const std::vector<std::string>&, bool) override { return kk::fail("ask", "not available"); }
    } none;
    LeafToolHost h(*sb_, Role::Worker, none, {});
    return h.call(c);
  }
  return err("unknown tool " + c.name);
}

}  // namespace kk::agents
