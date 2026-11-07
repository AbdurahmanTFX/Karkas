#include "kk/mcp/server.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <iostream>
#include <nlohmann/json.hpp>

#include "kk/core/tickets.hpp"
#include "kk/index/index.hpp"

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

std::vector<Tool> make_tools(bool with_run_tests) {
  json block = str_prop("block id, e.g. stream.ring");
  std::vector<Tool> t = {
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
  if (with_run_tests) {
    t.push_back({"kk_run_tests", "Build and run the tests of a block subtree (no model involved); returns a compact summary.",
                 {{"type", "object"}, {"properties", {{"block", str_prop("block id; empty = all")}}}}});
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
  for (const auto& t : make_tools(static_cast<bool>(opts_.run_tests))) {
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
          {"instructions",
           "Karkas keeps this project as a hierarchical schema of blocks. Start with kk_overview, then kk_view(block) "
           "to descend; answers are compact KSL text. Blocks map to folders blocks/<path>/ with api/, impl/, tests/."}};
      return response(id, result);
    }
    if (method == "ping") return response(id, json::object());
    if (method == "tools/list") return response(id, json::parse(tools_json()));
    if (method == "tools/call") {
      std::string name = params.value("name", "");
      json args = params.value("arguments", json::object());
      auto tools = make_tools(static_cast<bool>(opts_.run_tests));
      if (std::none_of(tools.begin(), tools.end(), [&](const Tool& t) { return t.name == name; })) {
        return error(id, kInvalidParams, "unknown tool: " + name);
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
        return out(idx_.view(b, level, vo));
      }
      if (name == "kk_children") return out(idx_.children(sarg(args, "block")));
      if (name == "kk_neighbors") {
        std::string dir = sarg(args, "dir");
        return out(idx_.neighbors(sarg(args, "block"), sarg(args, "kind"), dir.empty() ? "both" : dir));
      }
      if (name == "kk_dependents") return out(idx_.dependents(sarg(args, "block")));
      if (name == "kk_path") return out(idx_.path(sarg(args, "from"), sarg(args, "to")));
      if (name == "kk_search") return out(idx_.search(sarg(args, "text")));
      if (name == "kk_history") return out(idx_.history(sarg(args, "block"), args.value("n", 10)));
      if (name == "kk_status") return out(idx_.status(sarg(args, "block")));
      if (name == "kk_probes") return out(idx_.probes(sarg(args, "target")));
      if (name == "kk_drift") return out(idx_.drift(sarg(args, "block")));
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
    }
  } catch (const std::exception& e) {
    return error(id, kInternalError, e.what());
  }
  return error(id, kMethodNotFound, "method not found: " + method);
}

int Server::serve(std::istream& in, std::ostream& out) {
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line == "\r") continue;
    if (auto r = handle(line)) {
      out << *r << '\n';
      out.flush();
    }
  }
  return 0;
}

}  // namespace kk::mcp
