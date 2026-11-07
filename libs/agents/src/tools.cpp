#include "kk/agents/tools.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>
#include <regex>

#include "kk/agents/changes.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::agents {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

json str(const char* d) { return {{"type", "string"}, {"description", d}}; }
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
       {"Send the ticket down to a child block with your trail entry.",
        obj({{"block", str("child block id")},
             {"suspect", {{"type", "array"}, {"items", {{"type", "string"}}}}},
             {"ruled_out", {{"type", "array"}, {"items", {{"type", "string"}}}}},
             {"difficulty", {{"type", "string"}, {"enum", {"S", "M", "L"}}}},
             {"why", {{"type", "string"}, {"maxLength", 160}}},
             {"new_facts", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 2}}}},
            {"block", "difficulty", "why"})}},
      {"ask", {"Ask one short question to the agent of a child block; you get only its answer (<= 80 tokens).",
               obj({{"block", str("child block id")}, {"question", str("one question")}}, {"block", "question"})}},
      {"run_tests", {"Build and run the tests of a block subtree; returns a compact summary.", obj({{"block", str("block id")}}, {"block"})}},
      {"propose_change",
       {"Propose new blocks, ports or links as a KSL diff (needs human approval).",
        obj({{"reason", str("one line")},
             {"diff", str("KSL diff: '+ @block x.y' then '+ purpose ...' lines; '~ @block z' then '+ line' / '- line'")}},
            {"reason", "diff"})}},
      {"close", {"Close the ticket without work (duplicate, not a bug, done already).",
                 obj({{"verdict", {{"type", "string"}, {"enum", {"done", "not_a_bug", "duplicate", "wont_fix"}}}},
                      {"note", {{"type", "string"}, {"maxLength", 200}}}},
                     {"verdict"})}},
      {"report_leaf",
       {"Finish your work and report to the level above.",
        obj({{"status", {{"type", "string"},
                         {"enum", {"fixed", "done", "failed", "needs_contract_change", "needs_info", "not_here"}}}},
             {"cause", {{"type", "string"}, {"maxLength", 200}}},
             {"fix", {{"type", "string"}, {"maxLength", 200}}},
             {"files", {{"type", "array"}, {"items", {{"type", "string"}}}}},
             {"tests", {{"type", "string"}, {"maxLength", 120}}},
             {"api", {{"type", "string"}, {"enum", {"unchanged", "change_requested"}}}},
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
  std::vector<std::string> names = {"view", "neighbors", "dependents", "path", "search", "history", "status", "probes", "dispatch"};
  if (cb_.ask) names.push_back("ask");
  if (cb_.run_tests) names.push_back("run_tests");
  if (role_ == Role::Orchestrator) {
    names.push_back("propose_change");
    names.push_back("close");
  } else {
    names.push_back("report_navigator");
  }
  std::vector<Tool> out;
  for (const auto& n : names) out.push_back(tool_def(n));
  return out;
}

ToolOutcome SchemaToolHost::call(const ToolCall& c) {
  bool bad = false;
  auto in = parse_input(c, bad);
  if (bad) return err("invalid JSON input");
  if (auto q = query_tool(idx_, c.name, in, scope_)) return *q;
  if (c.name == "dispatch") {
    auto snap = idx_.snapshot();
    std::string b = sget(in, "block");
    if (snap.block(b) == nullptr) return err(fmt::format("unknown block '{}'", b));
    std::string scope = scope_ == "root" ? "" : scope_;
    if (b == scope || !index::Snapshot::under(scope, b)) {
      return err(fmt::format("dispatch must name a block inside {}", scope.empty() ? "the project" : scope));
    }
    auto diff = sget(in, "difficulty");
    if (diff != "S" && diff != "M" && diff != "L") return err("difficulty must be S, M or L");
    if (sget(in, "why").size() > 200) return err("why must be at most 20 words");
    if (in.contains("new_facts") && in["new_facts"].is_array() && in["new_facts"].size() > 2) return err("at most 2 new facts");
    return done("dispatched to " + b);
  }
  if (c.name == "ask" && cb_.ask) return ok(cb_.ask(sget(in, "block"), sget(in, "question")));
  if (c.name == "run_tests" && cb_.run_tests) return ok(cb_.run_tests(sget(in, "block")));
  if (c.name == "propose_change" && role_ == Role::Orchestrator) {
    auto d = parse_diff(sget(in, "diff"));
    if (!d) return err(d.error().message);
    if (d->empty()) return err("the diff is empty");
    return done("change proposed; it waits for human approval");
  }
  if (c.name == "close" && role_ == Role::Orchestrator) return done("closed");
  if (c.name == "report" && role_ == Role::Navigator) {
    if (sget(in, "status") != "not_here") return err("a navigator reports only status=not_here");
    return done("sent back up");
  }
  return err("unknown tool " + c.name);
}

// --- leaf tools ---------------------------------------------------------------------------

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
    auto text = core::read_file(*p);
    if (!text) return err("cannot read " + sget(in, "path"));
    reads_.push_back(sb_.display(*p));
    int offset = in.value("offset", 0);
    int limit = in.value("limit", 0);
    if (offset > 0 || limit > 0) {
      auto lines = core::split(*text, '\n', false);
      std::string out;
      int from_line = std::max(1, offset);
      int to_line = limit > 0 ? from_line + limit - 1 : static_cast<int>(lines.size());
      for (int i = from_line; i <= to_line && i <= static_cast<int>(lines.size()); ++i) out += lines[static_cast<std::size_t>(i - 1)] + "\n";
      return ok(out);
    }
    constexpr std::size_t kMax = 64 * 1024;
    if (text->size() > kMax) return ok(text->substr(0, kMax) + "\n... (truncated; use offset/limit)");
    return ok(*text);
  }
  if (c.name == "list_dir") {
    std::string path = sget(in, "path");
    auto p = sb_.resolve_read(path.empty() ? "." : path);
    if (!p) return err(p.error().message);
    std::error_code ec;
    if (!fs::is_directory(*p, ec)) return err("not a folder");
    std::vector<std::string> entries;
    for (const auto& e : fs::directory_iterator(*p, ec)) {
      auto n = e.path().filename().string();
      if (n.starts_with(".")) continue;
      if (e.is_directory(ec)) {
        if (sb_.resolve_read(sb_.display(e.path())).has_value()) entries.push_back(n + "/");
      } else {
        entries.push_back(fmt::format("{} {}", n, e.file_size(ec)));
      }
    }
    std::sort(entries.begin(), entries.end());
    return ok(entries.empty() ? "(empty)\n" : core::join(entries, "\n") + "\n");
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
      auto text = core::read_file(f);
      if (!text) return;
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
    } else {
      for (auto it = fs::recursive_directory_iterator(*p, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
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
    if (auto r = core::write_file(*p, content); !r) return err(r.error().message);
    written_.insert(sb_.display(*p));
    return ok(fmt::format("wrote {} ({} bytes)", sb_.display(*p), content.size()));
  }
  if (c.name == "build" || c.name == "test") {
    auto r = tests_.build_and_test(labels_, c.name == "build");
    if (!r) return err(r.error().message);
    if (c.name == "build") return ok(r->build_ok ? "build ok\n" : "build failed\n" + r->build_summary + "\n");
    last_test_ = r->text;
    return ok(r->text);
  }
  if (c.name == "report") {
    std::string st = sget(in, "status");
    static const std::vector<std::string> worker = {"fixed", "done", "failed", "needs_contract_change", "needs_info", "not_here"};
    if (std::find(worker.begin(), worker.end(), st) == worker.end()) return err("unknown status " + st);
    if (role_ == Role::Tester && st != "done" && st != "failed") return err("a tester reports status done or failed");
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
