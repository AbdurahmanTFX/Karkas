#include "kk/mcp/server.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

#include "kk/core/text.hpp"
#include "kk/core/tickets.hpp"
#include "kk/index/index.hpp"
#include "kk/templates.hpp"

namespace kk::mcp {

using json = nlohmann::json;

namespace {

// JSON-RPC 2.0 error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

struct Tool {
  std::string name;
  std::string description;
  json schema;
};

json str_prop(const char* desc) { return json{{"type", "string"}, {"description", desc}}; }

std::string template_text(std::string_view path) { return std::string(templates::get(path).value_or("")); }

std::vector<Tool> make_tools(const ServerOptions& o) {
  json block = str_prop("block id, e.g. stream.ring");
  json none = {{"type", "object"}, {"properties", json::object()}};
  std::vector<Tool> t = {
      {"kk_guide", "The full Karkas method (order of work, schema changes, delegation). Read it once before a feature or a "
                   "schema change; a bug or a small fix needs only kk_locate.",
       none},
      {"kk_ksl_reference", "Syntax of cards (block.ksl), links and change diffs, with examples.", none},
      {"kk_overview", "Project overview (L0 view): top-level blocks with status and lifted links. Start here.",
       {{"type", "object"},
        {"properties", {{"use", {{"type", "boolean"}, {"description", "include shared blocks and use links"}}}}}}},
      {"kk_view", "View of one block: L1 for a group (children, internal/external links, probes), L2 for a leaf (card + neighbor ports).",
       {{"type", "object"},
        {"properties",
         {{"block", block},
          {"level", {{"type", "integer"}, {"enum", {1, 2}}, {"description", "1 group view, 2 leaf view; default by kind"}}},
          {"use", {{"type", "boolean"}}}}},
        {"required", {"block"}}}},
      {"kk_children", "Children of a block (or of the project) with kind, purpose, thread and status.",
       {{"type", "object"}, {"properties", {{"block", str_prop("block id; empty = top level")}}}}},
      {"kk_neighbors", "Links crossing the boundary of a block.",
       {{"type", "object"},
        {"properties",
         {{"block", block},
          {"kind", {{"type", "string"}, {"enum", {"data", "event", "call", "state", "use"}}}},
          {"dir", {{"type", "string"}, {"enum", {"in", "out", "both"}}}}}},
        {"required", {"block"}}}},
      {"kk_dependents", "Blocks that depend on the interface of a block (who breaks if it changes).",
       {{"type", "object"}, {"properties", {{"block", block}}}, {"required", {"block"}}}},
      {"kk_path", "Chain of links from one block to another.",
       {{"type", "object"},
        {"properties", {{"from", str_prop("start block")}, {"to", str_prop("end block")}}},
        {"required", {"from", "to"}}}},
      {"kk_locate",
       "Where is it? Give the words of a symptom or a feature (\"ball passes through the net top\"): the blocks that "
       "probably hold it, ranked, with their files and the matching functions at file:line. Start a bug or a small change "
       "here instead of walking the levels.",
       {{"type", "object"}, {"properties", {{"text", str_prop("words of the symptom or the feature")}}}, {"required", {"text"}}}},
      {"kk_search", "Full-text search over block purposes, invariants and history.",
       {{"type", "object"}, {"properties", {{"text", str_prop("words to search")}}}, {"required", {"text"}}}},
      {"kk_history", "Recent bug/fix history of a block (and its descendants).",
       {{"type", "object"},
        {"properties", {{"block", block}, {"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}}}}},
        {"required", {"block"}}}},
      {"kk_status", "Statuses, failing tests and drift; whole project when block is empty.",
       {{"type", "object"}, {"properties", {{"block", str_prop("block id; empty = project")}}}}},
      {"kk_probes", "Probe summaries (numbers, not logs) for a block or a link 'a->b'.",
       {{"type", "object"}, {"properties", {{"target", str_prop("block id or link a->b; empty = all")}}}}},
      {"kk_drift", "Schema/code drift report (KSL errors and D01-D09).",
       {{"type", "object"}, {"properties", {{"block", str_prop("block id; empty = project")}}}}},
      {"kk_note_bug",
       "Record a bug you noticed that is not your task — do not fix it. It becomes an open ticket for the user (karkas ticket "
       "list); name it in your report and go on with your own task.",
       {{"type", "object"},
        {"properties",
         {{"summary", str_prop("one line: what is wrong, as a user would see it or as the code shows it")},
          {"where", str_prop("block, or file:line")},
          {"evidence", str_prop("why you think so: the line of code, a value, a test")}}},
        {"required", {"summary"}}}},
      {"kk_ticket_view", "A ticket. With 'at' it returns the path view an agent at that block would get.",
       {{"type", "object"},
        {"properties", {{"id", str_prop("ticket id, e.g. T-0042")}, {"at", str_prop("block id or root for the path view")}}},
        {"required", {"id"}}}},
      {"kk_ticket_append", "Append one line to the trail of a ticket (tickets are append-only).",
       {{"type", "object"},
        {"properties",
         {{"id", str_prop("ticket id")},
          {"line", str_prop("KSL trail line, e.g. 'fact probe x min=0' or 'L1 navigator at=stream pick=stream.ring why=\"...\"'")}}},
        {"required", {"id", "line"}}}},
  };
  if (o.check) {
    t.push_back({"kk_check", "Check the schema and the code against it now: KSL errors/warnings (E, W) and drift (D01-D09).", none});
  }
  if (o.propose_change) {
    t.push_back({"kk_propose_change",
                 "Propose a schema change (new blocks, links, ports, functions, inv lines) as a KSL diff. It is checked first; "
                 "if it passes it is stored as C-NNNN for the user to approve (never applied by itself). Returns the card diff, "
                 "or the problems to fix.",
                 {{"type", "object"},
                  {"properties",
                   {{"reason", str_prop("one line: why the schema changes")},
                    {"diff", str_prop("lines: '+ @block <id>' starts a new card from the following '+' lines; '~ @block <id>' "
                                      "selects a card, then '+ <ksl line>' adds and '- <ksl line>' removes a line")}}},
                  {"required", {"reason", "diff"}}}});
  }
  if (o.changes) {
    t.push_back({"kk_changes", "Schema changes: all of them with status (no id), or one with the card diff it makes.",
                 {{"type", "object"}, {"properties", {{"id", str_prop("change id, e.g. C-0003; empty = list")}}}}});
  }
  if (o.model_for) {
    t.push_back({"kk_model_for",
                 "Which model works on a block, as set in karkas.toml ([blocks], [levels], [roles], routing by size), and "
                 "the Claude Code subagent to delegate that work to. Call it before you implement, test or design a block.",
                 {{"type", "object"},
                  {"properties",
                   {{"block", str_prop("block id; empty = the project")},
                    {"role", {{"type", "string"},
                              {"enum", {"worker", "tester", "integration_tester", "architect", "orchestrator", "navigator",
                                        "composer", "ask"}},
                              {"description", "default: worker for a leaf, architect for a group or the project"}}}}}}});
  }
  if (o.brief) {
    t.push_back({"kk_brief",
                 "The brief for the subagent of a block: everything Karkas knows (card, files, guarantees, the api it may use, "
                 "rules, how to test, what not to touch) and [fill] lines for the task, the spec with concrete numbers and the "
                 "checks. Fill them, add what you found, then hand it over.",
                 {{"type", "object"},
                  {"properties", {{"block", str_prop("leaf to hand over")}, {"task", str_prop("optional: the task, as a start")}}},
                  {"required", {"block"}}}});
  }
  if (o.review) {
    t.push_back({"kk_review",
                 "Before accepting a subagent's work on a block: its guarantees as a checklist, the changed files (outside the "
                 "block flagged), the diff of its files, its issues and test status.",
                 {{"type", "object"}, {"properties", {{"block", str_prop("block whose work you review")}}}, {"required", {"block"}}}});
  }
  if (o.run_tests) {
    t.push_back({"kk_run_tests", "Build and run the tests of a block subtree (no model involved); returns a compact summary.",
                 {{"type", "object"}, {"properties", {{"block", str_prop("block id; empty = all")}}}}});
  }
  // The program's own agents (ADR 0024): the same protocol as `karkas task`; in mode "mcp" this
  // client is their model.
  if (o.start_ticket) {
    t.push_back({"kk_task",
                 "Give a feature or change to Karkas's agents, exactly like `karkas task`: a ticket is created and the "
                 "orchestrator, the navigators, a worker and a tester take it down the schema to the block. In mode mcp you are "
                 "their model: serve their turns with kk_agent_next / kk_agent_act until the ticket ends.",
                 {{"type", "object"},
                  {"properties",
                   {{"text", str_prop("what to do, in the user's words")},
                    {"block", str_prop("only inside this block and its subtree (the user chose it): the descent starts there; "
                                       "empty = the whole project, the orchestrator decides")}}},
                  {"required", {"text"}}}});
    t.push_back({"kk_debug",
                 "Give a bug to Karkas's agents, exactly like `karkas debug`: a bug ticket goes down the schema to the block "
                 "with the cause; its worker adds a test that reproduces it and fixes it. In mode mcp serve the turns.",
                 {{"type", "object"},
                  {"properties",
                   {{"text", str_prop("the symptom")},
                    {"facts", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "facts you already have"}}},
                    {"block", str_prop("only inside this block and its subtree (the user chose it); empty = the whole project")}}},
                  {"required", {"text"}}}});
  }
  if (o.run_ticket) {
    t.push_back({"kk_ticket_run",
                 "Run a ticket again or continue it (like `karkas ticket run`), with the same agents. With note: the user's "
                 "message about this ticket becomes its fact \"human: ...\" and the ticket runs (also a closed one).",
                 {{"type", "object"},
                  {"properties",
                   {{"id", str_prop("ticket id, e.g. T-0012")},
                    {"note", str_prop("the user's message about the ticket (what to add or correct)")},
                    {"force", {{"type", "boolean"}, {"description", "run even if nothing changed since its last result"}}}}},
                  {"required", {"id"}}}});
  }
  if (o.prototype) {
    json block = {{"type", "object"},
                  {"properties",
                   {{"name", str_prop("identifier: a-z, 0-9, _")},
                    {"purpose", str_prop("optional, the user's words")},
                    {"kind", str_prop("optional: group | leaf | entry | shared (else the architect decides)")}}},
                  {"required", {"name"}}};
    json dep = {{"type", "object"},
                {"properties", {{"from", str_prop("block id")}, {"to", str_prop("block id")}, {"note", str_prop("optional")}}},
                {"required", {"from", "to"}}};
    t.push_back({"kk_prototype",
                 "The user's prototype: rough blocks and dependencies of a level that the architect builds the schema from, "
                 "keeping its main logic (karkas-gui draws it as ghosts). view: the sketch and what the schema keeps of it. "
                 "set: write a sketch from the user's description (replaces the one there; the user sees and edits it in "
                 "karkas-gui). send: give it to the architect now — only when the user asked for it; in mode mcp serve the turns.",
                 {{"type", "object"},
                  {"properties",
                   {{"action", {{"type", "string"}, {"enum", {"view", "set", "send"}}}},
                    {"level", str_prop("set: the group it is drawn in; empty = the top level")},
                    {"idea", str_prop("set: what the project (or the group) does, in the user's words")},
                    {"blocks", {{"type", "array"}, {"items", block}}},
                    {"dependencies", {{"type", "array"}, {"items", dep}, {"description", "from uses / sends to / calls to"}}}}},
                  {"required", {"action"}}}});
  }
  if (o.implement) {
    t.push_back({"kk_implement",
                 "Implement the leaves and then the composition root, like `karkas implement <target>`, started in the "
                 "background: testers write the contract tests, workers the code. In mode mcp serve their turns.",
                 {{"type", "object"},
                  {"properties", {{"target", str_prop("block id, or \"all\" (default)")}}}}});
  }
  if (o.agent_next) {
    t.push_back({"kk_agent_next",
                 "The turns Karkas's agents wait for (mode mcp: you are their model). With list=true: who waits — ticket, role, "
                 "block, tier and the subagent of that tier — nothing is taken. Otherwise the oldest waiting turn (of `run` when "
                 "given) is taken and shown whole: the agent's system prompt, its tools and its messages — or, with `after` = "
                 "the turn of that run you answered last, only what is new since your answer. Answer it with kk_agent_act.",
                 {{"type", "object"},
                  {"properties",
                   {{"run", str_prop("serve this run only (its id from the list)")},
                    {"after", str_prop("with run: the turn of this run you answered last; you get only what is new since then")},
                    {"seen", {{"type", "array"}, {"items", {{"type", "string"}}},
                              {"description", "runs you served already in this session: a new run of the same role comes without "
                                              "the system prompt, tools and unchanged context you have"}}},
                    {"list", {{"type", "boolean"}, {"description", "only list what waits"}}},
                    {"wait_s", {{"type", "integer"}, {"description", "wait up to this many seconds for a turn (default 20, max 300)"}}}}}}});
  }
  if (o.agent_act) {
    t.push_back({"kk_agent_act",
                 "Your answer to one turn of an agent: calls of the agent's own tools (Karkas runs them in its sandbox) and "
                 "optional text. Returns the next turn of the same run (the results of your calls) or how the run ended.",
                 {{"type", "object"},
                  {"properties",
                   {{"turn", str_prop("the turn id from kk_agent_next")},
                    {"calls",
                     {{"type", "array"},
                      {"items",
                       {{"type", "object"},
                        {"properties", {{"name", {{"type", "string"}}}, {"input", {{"type", "object"}}}}},
                        {"required", {"name"}}}},
                      {"description", "tool calls of the agent, in order: [{\"name\": \"read_file\", \"input\": {\"path\": \"...\"}}]"}}},
                    {"text", str_prop("optional: your thoughts, as the model's text")},
                    {"wait_s", {{"type", "integer"}, {"description", "wait up to this many seconds for the next turn (default 60, max 300)"}}}}},
                  {"required", {"turn"}}}});
  }
  if (o.only_agent_tools) {
    std::erase_if(t, [](const Tool& x) { return x.name != "kk_agent_next" && x.name != "kk_agent_act"; });
  }
  return t;
}

std::string response(const json& id, const json& result) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump();
}

std::string error(const json& id, int code, const std::string& message) {
  return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}}.dump();
}

json text_result(const std::string& text, bool is_error) {
  return json{{"content", json::array({json{{"type", "text"}, {"text", text}}})}, {"isError", is_error}};
}

std::string sarg(const json& args, const char* key) {
  if (!args.contains(key) || args[key].is_null()) return {};
  if (args[key].is_string()) return args[key].get<std::string>();
  return args[key].dump();
}

}  // namespace

const std::vector<std::string>& Server::supported_versions() {
  static const std::vector<std::string> v = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
  return v;
}

Server::Server(index::Index& idx, ServerOptions opts) : idx_(idx), opts_(std::move(opts)) {}

std::string Server::tools_json() const {
  json tools = json::array();
  for (const auto& t : make_tools(opts_)) {
    tools.push_back(json{{"name", t.name}, {"description", t.description}, {"inputSchema", t.schema}});
  }
  return json{{"tools", tools}}.dump();
}

std::optional<std::string> Server::handle(std::string_view line) {
  json msg;
  try {
    msg = json::parse(line);
  } catch (const json::exception& e) {
    return error(nullptr, kParseError, std::string("parse error: ") + e.what());
  }
  if (msg.is_array()) {
    json out = json::array();
    for (const auto& m : msg) {
      if (auto r = handle(m.dump())) out.push_back(json::parse(*r));
    }
    if (out.empty()) return std::nullopt;
    return out.dump();
  }
  if (!msg.is_object() || msg.value("jsonrpc", "") != "2.0" || !msg.contains("method") || !msg["method"].is_string()) {
    return error(msg.is_object() && msg.contains("id") ? msg["id"] : json(nullptr), kInvalidRequest, "invalid request");
  }
  const bool is_notification = !msg.contains("id");
  const json id = is_notification ? json(nullptr) : msg["id"];
  const std::string method = msg["method"];
  const json params = msg.value("params", json::object());
  spdlog::debug("mcp: {}", method);

  if (method == "notifications/initialized") {
    initialized_ = true;
    return std::nullopt;
  }
  if (method.starts_with("notifications/")) return std::nullopt;
  if (is_notification) return std::nullopt;

  try {
    if (method == "initialize") {
      std::string requested = params.value("protocolVersion", "");
      const auto& sv = supported_versions();
      std::string version = std::find(sv.begin(), sv.end(), requested) != sv.end() ? requested : sv.front();
      json result = {
          {"protocolVersion", version},
          {"capabilities", {{"tools", {{"listChanged", false}}}}},
          {"serverInfo", {{"name", "karkas"}, {"version", opts_.version}}},
          {"instructions", opts_.instructions ? opts_.instructions() : std::string(core::trim(template_text("prompts/mcp_instructions.txt")))}};
      return response(id, result);
    }
    if (method == "ping") return response(id, json::object());
    if (method == "tools/list") return response(id, json::parse(tools_json()));
    if (method == "tools/call") {
      std::string name = params.value("name", "");
      json args = params.value("arguments", json::object());
      auto call_tool = [&]() -> std::string {
        auto tools = make_tools(opts_);
        if (std::none_of(tools.begin(), tools.end(), [&](const Tool& t) { return t.name == name; })) {
          return error(id, kInvalidParams, "unknown tool: " + name);
        }
        if (name == "kk_guide") return response(id, text_result(template_text("target/karkas_guide_full.md"), false));
        if (name == "kk_ksl_reference") {
          return response(id, text_result(template_text("prompts/ksl_reference.txt") + "\n" +
                                              template_text("prompts/ksl_change_example.txt"),
                                          false));
        }
        // The program's agents (ADR 0024) need no index; their turns may be answered on other
        // threads, so they never touch it.
        auto result = [&](const kk::Result<std::string>& r) {
          return response(id, r ? text_result(*r, false) : text_result(r.error().message, true));
        };
        if (name == "kk_task" || name == "kk_debug") {
          std::vector<std::string> facts;
          if (args.contains("facts") && args["facts"].is_array()) {
            for (const auto& f : args["facts"]) {
              if (f.is_string()) facts.push_back(f.get<std::string>());
            }
          }
          return result(opts_.start_ticket(name == "kk_task" ? "feature" : "bug", sarg(args, "text"), facts, sarg(args, "block")));
        }
        if (name == "kk_ticket_run") return result(opts_.run_ticket(sarg(args, "id"), args.value("force", false), sarg(args, "note")));
        if (name == "kk_prototype") {
          json sketch = args.is_object() ? args : json::object();
          sketch.erase("action");
          return result(opts_.prototype(sarg(args, "action"), sketch.dump()));
        }
        if (name == "kk_implement") {
          auto target = sarg(args, "target");
          return result(opts_.implement(target.empty() ? std::string("all") : target));
        }
        if (name == "kk_agent_next") {
          int wait = std::clamp(args.value("wait_s", 20), 0, 300);
          std::vector<std::string> seen;
          if (args.contains("seen") && args["seen"].is_array()) {
            for (const auto& r : args["seen"]) {
              if (r.is_string()) seen.push_back(r.get<std::string>());
            }
          }
          return response(id, text_result(opts_.agent_next(sarg(args, "run"), wait, args.value("list", false), sarg(args, "after"), seen), false));
        }
        if (name == "kk_agent_act") {
          int wait = std::clamp(args.value("wait_s", 60), 0, 300);
          std::string calls = args.contains("calls") ? args["calls"].dump() : "[]";
          return result(opts_.agent_act(sarg(args, "turn"), sarg(args, "text"), calls, wait));
        }
        if (auto st = idx_.update({false, opts_.analyzer}); !st) {
          return response(id, text_result("index update failed: " + st.error().str(), true));
        }
        auto out = [&](const kk::Result<std::string>& r) {
          return r ? response(id, text_result(*r, false)) : response(id, text_result(r.error().message, true));
        };
        index::ViewOptions vo{args.value("use", false)};
        if (name == "kk_overview") return out(idx_.overview(vo));
        if (name == "kk_view") {
          std::string b = sarg(args, "block");
          int level = args.value("level", 0);
          if (level == 0) {
            auto snap = idx_.snapshot();
            const auto* row = snap.block(b);
            level = row != nullptr && row->kind == "group" ? 1 : 2;
          }
          auto v = idx_.view(b, level, vo);
          if (v && opts_.model_hint) *v += opts_.model_hint(b);
          return out(v);
        }
        if (name == "kk_children") return out(idx_.children(sarg(args, "block")));
        if (name == "kk_neighbors") {
          std::string dir = sarg(args, "dir");
          return out(idx_.neighbors(sarg(args, "block"), sarg(args, "kind"), dir.empty() ? "both" : dir));
        }
        if (name == "kk_dependents") return out(idx_.dependents(sarg(args, "block")));
        if (name == "kk_path") return out(idx_.path(sarg(args, "from"), sarg(args, "to")));
        if (name == "kk_search") return out(idx_.search(sarg(args, "text")));
        if (name == "kk_locate") return out(idx_.locate(sarg(args, "text")));
        if (name == "kk_history") return out(idx_.history(sarg(args, "block"), args.value("n", 10)));
        if (name == "kk_status") return out(idx_.status(sarg(args, "block")));
        if (name == "kk_probes") return out(idx_.probes(sarg(args, "target")));
        if (name == "kk_drift") return out(idx_.drift(sarg(args, "block")));
        if (name == "kk_note_bug") {
          auto summary = std::string(core::trim(sarg(args, "summary")));
          if (summary.empty()) return response(id, text_result("summary is required", true));
          std::vector<std::string> facts;
          if (auto w = std::string(core::trim(sarg(args, "where"))); !w.empty()) facts.push_back("found at " + w);
          if (auto e = std::string(core::trim(sarg(args, "evidence"))); !e.empty()) facts.push_back("evidence " + e);
          core::TicketStore store(idx_.root());
          auto t = store.create(core::TicketKind::Bug, "agent", summary, facts);
          if (!t) return response(id, text_result(t.error().message, true));
          return response(id, text_result(fmt::format("noted {} (open, for the user). Do not fix it now: name it in your "
                                                      "report and go on with your task.\n", t->id),
                                          false));
        }
        if (name == "kk_ticket_view" || name == "kk_ticket_append") {
          core::TicketStore store(idx_.root());
          std::string tid = sarg(args, "id");
          if (name == "kk_ticket_append") {
            auto r = store.append_line(tid, sarg(args, "line"));
            if (!r) return response(id, text_result(r.error().message, true));
            return response(id, text_result("ok\n", false));
          }
          auto t = store.load(tid);
          if (!t) return response(id, text_result(t.error().message, true));
          std::string at = sarg(args, "at");
          return response(id, text_result(at.empty() ? core::write_ticket(*t) : core::path_view(*t, at), false));
        }
        if (name == "kk_run_tests") return response(id, text_result(opts_.run_tests(sarg(args, "block")), false));
        if (name == "kk_check") return response(id, text_result(opts_.check(), false));
        if (name == "kk_propose_change") return out(opts_.propose_change(sarg(args, "reason"), sarg(args, "diff")));
        if (name == "kk_changes") return out(opts_.changes(sarg(args, "id")));
        if (name == "kk_model_for") return out(opts_.model_for(sarg(args, "block"), sarg(args, "role")));
        if (name == "kk_brief") return out(opts_.brief(sarg(args, "block"), sarg(args, "task")));
        if (name == "kk_review") return out(opts_.review(sarg(args, "block")));
        return error(id, kMethodNotFound, "unknown tool: " + name);
      };
      // Every call is logged (with its time) so a session can be followed in .karkas/karkas.log.
      auto t0 = std::chrono::steady_clock::now();
      std::string resp = call_tool();
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      std::string brief = args.dump();
      if (brief.size() > 160) brief = brief.substr(0, 157) + "...";
      bool failed = core::contains(resp, "\"isError\":true") || core::contains(resp, "\"error\":{");
      spdlog::info("mcp: {} {} -> {} in {} ms", name, brief, failed ? "error" : "ok", ms);
      return resp;
    }
  } catch (const std::exception& e) {
    return error(id, kInternalError, e.what());
  }
  return error(id, kMethodNotFound, "method not found: " + method);
}

int Server::serve(std::istream& in, std::ostream& out) {
  auto mu = std::make_shared<std::mutex>();
  auto write = [&out, mu](const std::string& s) {
    std::lock_guard lock(*mu);
    out << s << '\n';
    out.flush();
  };
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line == "\r") continue;
    // A turn of an agent waits for its model or its engine for up to minutes: it is answered on
    // its own thread, so other calls — another subagent's turns — go on meanwhile (ADR 0024).
    bool slow = line.find("\"tools/call\"") != std::string::npos &&
                (line.find("\"kk_agent_next\"") != std::string::npos || line.find("\"kk_agent_act\"") != std::string::npos);
    if (slow) {
      std::thread([this, line, write] {
        if (auto r = handle(line)) write(*r);
      }).detach();
      continue;
    }
    if (auto r = handle(line)) write(*r);
  }
  return 0;
}

}  // namespace kk::mcp
