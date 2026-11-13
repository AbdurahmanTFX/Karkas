#include "kk/agents/mcp_turns.hpp"

#include <fmt/format.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <optional>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::agents {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr auto kReplace = json::error_handler_t::replace;

std::string str(const json& j, const char* k) {
  if (!j.is_object() || !j.contains(k) || !j[k].is_string()) return {};
  return j[k].get<std::string>();
}

// "name(path: string*, limit: integer)" from a JSON schema of the input (* = required).
std::string signature(const json& tool) {
  std::string out = str(tool, "name") + "(";
  const json& schema = tool.contains("input_schema") ? tool["input_schema"] : json::object();
  std::set<std::string> required;
  if (schema.contains("required") && schema["required"].is_array()) {
    for (const auto& r : schema["required"]) {
      if (r.is_string()) required.insert(r.get<std::string>());
    }
  }
  bool first = true;
  if (schema.contains("properties") && schema["properties"].is_object()) {
    for (const auto& [name, p] : schema["properties"].items()) {
      if (!first) out += ", ";
      first = false;
      std::string type = str(p, "type");
      if (type == "array" && p.contains("items")) type = "array of " + str(p["items"], "type");
      if (p.contains("enum") && p["enum"].is_array()) {
        std::vector<std::string> vals;
        for (const auto& v : p["enum"]) vals.push_back(v.is_string() ? v.get<std::string>() : v.dump());
        type = core::join(vals, "|");
      }
      out += name + (required.contains(name) ? "*" : "") + ": " + (type.empty() ? "any" : type);
    }
  }
  return out + ")";
}

std::string who(const llm::McpTurn& t) {
  std::string role = t.role.empty() ? "agent" : t.role;
  std::string at = t.block.empty() || t.block == "root" ? "the project" : t.block;
  std::string s = fmt::format("{} at {}", role, at);
  if (!t.ticket.empty()) s = t.ticket + " · " + s;
  return s;
}

std::string model_line(const llm::McpTurn& t) {
  return fmt::format("model: tier {} → {} (Claude Code subagent {})", t.tier.empty() ? "?" : t.tier, t.model.empty() ? "?" : t.model,
                     t.subagent.empty() ? "?" : t.subagent);
}

// The text of one message block for the model.
std::string block_text(const json& b, const std::map<std::string, std::string>& tool_names) {
  auto type = str(b, "type");
  if (type == "text") return str(b, "text");
  if (type == "tool_use") {
    return fmt::format("(call {} {})", str(b, "name"), b.contains("input") ? b["input"].dump(-1, ' ', false, kReplace) : "{}");
  }
  if (type == "tool_result") {
    auto it = tool_names.find(str(b, "tool_use_id"));
    std::string name = it == tool_names.end() ? "tool" : it->second;
    bool err = b.value("is_error", false);
    return fmt::format("--- result of {}{} ---\n{}", name, err ? " (error)" : "", str(b, "content"));
  }
  return {};
}

// A run of the same ticket that waits while `run` has no turn waiting: an agent of `run` asked
// another agent (the `ask` tool) and its run waits on that one. Served by the same client, or
// both would wait for ever.
const llm::McpTurn* nested_turn(const std::vector<llm::McpTurn>& waiting, const llm::McpTurn& own) {
  if (own.ticket.empty()) return nullptr;
  for (const auto& w : waiting) {
    if (w.run == own.run) return nullptr;  // the own run goes on
  }
  for (const auto& w : waiting) {
    if (w.ticket == own.ticket && w.run != own.run && !w.taken) return &w;
  }
  return nullptr;
}

std::string nested_note(const llm::McpTurn& nested, const std::string& own_run, const std::string& own_turn) {
  return fmt::format("Your run {} waits for this one: a run of the same ticket ({}). Serve it to its end with kk_agent_act; then "
                     "call kk_agent_next(run=\"{}\"{}) to go on with yours.\n\n",
                     own_run, who(nested), own_run, own_turn.empty() ? "" : fmt::format(", after=\"{}\"", own_turn));
}

// The turn the client answered last comes right before `t` in the same run: the client has seen
// everything up to its answer, so the turn shows only what is new (free of the system prompt,
// tools and every earlier message — the bulk of a whole turn).
bool follows(const llm::McpTurn& t, const std::string& after) {
  if (after.empty() || t.seq <= 1) return false;
  auto dash = after.rfind('-');
  if (dash == std::string::npos) return false;
  auto stem = t.run.ends_with(".jsonl") ? t.run.substr(0, t.run.size() - 6) : t.run;
  if (after.substr(0, dash) != stem) return false;
  int seq = 0;
  for (char c : after.substr(dash + 1)) {
    if (c < '0' || c > '9') return false;
    seq = seq * 10 + (c - '0');
  }
  return seq + 1 == t.seq;
}

// Sections of the first message (the agent's context and task): a line "--- x", "=== x" or "@x"
// starts one. Header → text.
std::vector<std::pair<std::string, std::string>> sections(const std::string& text) {
  std::vector<std::pair<std::string, std::string>> out = {{"(start)", ""}};
  std::size_t start = 0;
  while (start < text.size()) {
    auto end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    auto line = text.substr(start, end - start);
    if (line.starts_with("--- ") || line.starts_with("=== ") || line.starts_with("@")) out.emplace_back(line, "");
    out.back().second += line + "\n";
    start = end + 1;
  }
  return out;
}

std::string first_message_text(const json& req) {
  const json& messages = req.contains("messages") && req["messages"].is_array() ? req["messages"] : json::array();
  if (messages.empty()) return {};
  std::string out;
  for (const auto& b : messages[0].value("content", json::array())) {
    if (str(b, "type") == "text") out += str(b, "text") + "\n";
  }
  return out;
}

std::string hex(std::uint64_t v) { return fmt::format("{:016x}", v); }

std::string tools_text(const json& req) {
  std::string out;
  for (const auto& tool : req.value("tools", json::array())) out += signature(tool) + "\n" + str(tool, "description") + "\n";
  return out;
}

// What went to the client and back, per turn (.karkas/mcp/traffic/<run>.jsonl): the engine turns it
// into an estimate of the client's tokens for the journal and the ticket's cost (ADR 0026).
void record_traffic(const fs::path& root, const llm::McpTurn& t, const json& line) {
  std::error_code ec;
  auto dir = root / ".karkas" / "mcp" / "traffic";
  fs::create_directories(dir, ec);
  auto stem = t.run.ends_with(".jsonl") ? t.run.substr(0, t.run.size() - 6) : t.run;
  if (auto f = std::fopen((dir / (stem + ".jsonl")).c_str(), "a")) {
    auto text = line.dump() + "\n";
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
  }
}

// What a client was shown of a run (.karkas/mcp/shown/<run>.json): the role, block and the hashes of
// its system prompt, tools and first-message sections, so a later run of the same role can leave out
// what that client has already (ADR 0026).
json shown_record(const llm::McpTurn& t, const json& req) {
  json sec = json::object();
  for (const auto& [h, text] : sections(first_message_text(req))) sec[h] = hex(core::fnv1a64(text));
  return {{"run", t.run},
          {"role", t.role},
          {"block", t.block},
          {"system", hex(core::fnv1a64(str(req, "system")))},
          {"tools", hex(core::fnv1a64(tools_text(req)))},
          {"sections", sec}};
}

}  // namespace

std::string format_turn(const llm::McpTurn& t, const std::string& request_json, bool whole) {
  return format_turn(t, request_json, whole, std::string());
}

std::string format_turn(const llm::McpTurn& t, const std::string& request_json, bool whole, const std::string& seen_run_json) {
  auto seen_json = seen_run_json.empty() ? json() : json::parse(seen_run_json, nullptr, false);
  const json* seen_run = seen_json.is_object() ? &seen_json : nullptr;
  auto req = json::parse(request_json, nullptr, false);
  if (!req.is_object()) return "turn " + t.id + ": the request cannot be read";
  std::string out = fmt::format("TURN {} · {} · turn {} of this run\n{}\n", t.id, who(t), t.seq, model_line(t));
  const json& messages = req.contains("messages") && req["messages"].is_array() ? req["messages"] : json::array();
  // Tool names by call id (results name the tool they answer).
  std::map<std::string, std::string> tool_names;
  for (const auto& m : messages) {
    for (const auto& b : m.value("content", json::array())) {
      if (str(b, "type") == "tool_use") tool_names[str(b, "id")] = str(b, "name");
    }
  }
  std::size_t from = 0;
  // A run of the same role the client served already (seen): what it has is not sent again.
  bool same_prefix = seen_run != nullptr && seen_run->is_object() &&
                     seen_run->value("system", "") == hex(core::fnv1a64(str(req, "system"))) &&
                     seen_run->value("tools", "") == hex(core::fnv1a64(tools_text(req)));
  std::string ref = same_prefix ? seen_run->value("run", "") : std::string();
  if (whole && same_prefix) {
    out += fmt::format("\nYou are the model of this agent, as in run {}: the same system prompt and tools (not repeated). Answer "
                       "with kk_agent_act(turn=\"{}\", calls=[...]) using only its tools.\n",
                       ref, t.id);
    out += "\n=== MESSAGES (sections unchanged since that run are left out) ===\n";
  } else if (whole) {
    out += fmt::format(
        "\nYou are the model of this Karkas agent. Answer only with kk_agent_act(turn=\"{}\", calls=[{{\"name\": \"<tool>\", "
        "\"input\": {{...}}}}, ...], text=\"<optional thoughts>\"). Each call is one of the agent's tools below: Karkas runs it in "
        "the agent's sandbox (the files of its block) and shows the results in the next turn. Use no other tool on the project "
        "— the agent's tools are its only way in. The run ends when you call the tool its instructions name as the end. If your "
        "session runs another model than the one named above, hand the run to that subagent: it serves the run with "
        "kk_agent_next(run=\"{}\") and kk_agent_act until the run ends.\n",
        t.id, t.run);
    out += "\n=== SYSTEM ===\n" + str(req, "system") + "\n";
    out += "\n=== TOOLS (input fields, * = required) ===\n";
    for (const auto& tool : req.value("tools", json::array())) {
      out += "• " + signature(tool) + "\n  " + core::replace_all(str(tool, "description"), "\n", "\n  ") + "\n";
    }
    out += "\n=== MESSAGES ===\n";
  } else {
    // Only what came after the model's own last answer.
    for (std::size_t i = 0; i < messages.size(); ++i) {
      if (str(messages[i], "role") == "assistant") from = i + 1;
    }
    out += "\n=== NEW SINCE YOUR LAST ANSWER ===\n";
  }
  for (std::size_t i = from; i < messages.size(); ++i) {
    const auto& m = messages[i];
    std::string role = str(m, "role");
    if (whole) out += "\n[" + (role == "assistant" ? std::string("your earlier answer") : role) + "]\n";
    if (whole && same_prefix && i == 0) {
      // The first message: its sections that did not change since the seen run collapse to one line.
      const json& prev = seen_run->contains("sections") ? (*seen_run)["sections"] : json::object();
      for (const auto& [h, text] : sections(first_message_text(req))) {
        if (prev.contains(h) && prev[h].is_string() && prev[h].get<std::string>() == hex(core::fnv1a64(text))) {
          if (h != "(start)") out += h + "  (unchanged since run " + ref + ")\n";
          continue;
        }
        out += text;
      }
      continue;
    }
    for (const auto& b : m.value("content", json::array())) {
      auto text = block_text(b, tool_names);
      if (!text.empty()) out += text + "\n";
    }
  }
  out += fmt::format("\nAnswer with kk_agent_act(turn=\"{}\", calls=[...], text=\"...\").\n", t.id);
  return out;
}

McpTurns::McpTurns(fs::path project_root) : root_(std::move(project_root)), queue_(root_ / ".karkas" / "mcp") {}

std::string McpTurns::ticket_of_run(const std::string& run) const {
  auto text = core::read_file(root_ / ".karkas" / "runs" / run);
  if (!text) return {};
  auto nl = text->find('\n');
  auto j = json::parse(text->substr(0, nl), nullptr, false);
  return str(j, "ticket");
}

std::string McpTurns::run_end(const std::string& run) const {
  if (run.empty()) return {};
  auto text = core::read_file(root_ / ".karkas" / "runs" / run);
  if (!text) return {};
  std::string outcome;
  std::string terminal;
  for (const auto& line : core::split(*text, '\n', false)) {
    if (line.find("\"run_end\"") == std::string::npos && line.find("\"kind\":\"end\"") == std::string::npos) continue;
    auto j = json::parse(line, nullptr, false);
    if (!j.is_object()) continue;
    if (str(j, "type") == "run_end") {
      if (outcome.empty()) outcome = str(j, "outcome");
      return fmt::format("{}{}", outcome.empty() ? "ended" : outcome, terminal.empty() ? "" : " (" + terminal + ")");
    }
    if (j.contains("data")) {
      outcome = str(j["data"], "outcome");
      terminal = str(j["data"], "terminal");
    }
  }
  return {};
}

// The newest run among `seen` with the same role (and preferably block) as `t`, from what was shown.
std::optional<std::string> McpTurns::seen_like(const llm::McpTurn& t, const std::vector<std::string>& seen) const {
  std::optional<json> best;
  for (const auto& r : seen) {
    if (r == t.run) continue;
    auto text = core::read_file(root_ / ".karkas" / "mcp" / "shown" / (r + ".json"));
    if (!text) continue;
    auto j = json::parse(*text, nullptr, false);
    if (!j.is_object() || j.value("role", "") != t.role) continue;
    bool same_block = j.value("block", "") == t.block;
    if (!best || same_block || j.value("run", "") > best->value("run", "")) {
      if (best && best->value("block", "") == t.block && !same_block) continue;
      best = j;
    }
  }
  if (!best) return std::nullopt;
  return best->dump();
}

std::string McpTurns::show(const llm::McpTurn& t, const std::string& req_text, bool whole, const std::vector<std::string>& seen) {
  auto req = json::parse(req_text, nullptr, false);
  if (whole && req.is_object()) {
    std::error_code ec;
    fs::create_directories(root_ / ".karkas" / "mcp" / "shown", ec);
    (void)core::write_file(root_ / ".karkas" / "mcp" / "shown" / (t.run + ".json"), shown_record(t, req).dump() + "\n");
  }
  auto ref = whole ? seen_like(t, seen) : std::nullopt;
  auto text = format_turn(t, req_text, whole, ref ? *ref : std::string());
  record_traffic(root_, t, {{"turn", t.id}, {"shown", text.size()}, {"whole", whole}, {"seen", ref.has_value()}});
  return text;
}

std::string McpTurns::next(const std::string& run, int wait_s, bool list, const std::string& after, const std::vector<std::string>& seen) {
  auto t0 = std::chrono::steady_clock::now();
  auto now = core::unix_now();
  auto status_text = [&] {
    std::string s = tickets_status ? tickets_status() : std::string();
    return s.empty() ? std::string() : "\n" + s;
  };
  while (true) {
    auto turns = queue_.waiting();
    if (list) {
      if (!turns.empty() || std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(wait_s)) {
        if (turns.empty()) return "No agent turn waits." + status_text();
        std::string out = "Turns waiting for their model:\n";
        for (const auto& t : turns) {
          out += fmt::format("- run {} (turn {}): {} · tier {} → {}, subagent {} · waiting {} s{}\n", t.run, t.seq, who(t), t.tier, t.model,
                             t.subagent, std::max<std::int64_t>(0, now - t.ts), t.taken ? " · taken" : "");
        }
        out += "Serve a run with kk_agent_next(run=\"<run>\") and kk_agent_act until it ends — yourself when you run its model, "
               "otherwise through its subagent.";
        return out + status_text();
      }
    } else {
      const llm::McpTurn* pick = nullptr;
      for (const auto& t : turns) {
        if (!run.empty() && t.run != run) continue;
        // A turn another client took is left to it, unless it was abandoned (2 min) or asked by run.
        if (run.empty() && t.taken && now - t.taken_ts < 120) continue;
        pick = &t;
        break;
      }
      if (pick != nullptr) {
        queue_.take(pick->id);
        auto req = queue_.request(pick->id);
        if (!req) return "turn " + pick->id + " is gone (its run ended)";
        return show(*pick, *req, !follows(*pick, after), seen);
      }
      if (!run.empty() && run_end(run).empty()) {
        // The run (still going) waits on a nested run of its ticket: that one comes first.
        llm::McpTurn own;
        own.run = run;
        if (auto ticket = ticket_of_run(run); !ticket.empty()) {
          own.ticket = ticket;
          if (const auto* n = nested_turn(turns, own)) {
            queue_.take(n->id);
            auto req = queue_.request(n->id);
            if (req) return nested_note(*n, run, after) + show(*n, *req, true, seen);
          }
        }
      }
      if (std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(wait_s)) {
        if (!run.empty()) {
          if (auto end = run_end(run); !end.empty()) return fmt::format("Run {} ended: {}.", run, end) + status_text();
          return fmt::format("No turn of run {} waits yet (its agent is busy: tool calls, a build or tests). Call kk_agent_next(run=\"{}\"{}) "
                             "again.",
                             run, run, after.empty() ? "" : fmt::format(", after=\"{}\"", after));
        }
        return "No agent turn waits." + status_text();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    now = core::unix_now();
  }
}

kk::Result<std::string> McpTurns::act(const std::string& turn, const std::string& text, const std::string& calls_json, int wait_s) {
  auto t = queue_.turn(turn);
  if (!t) return kk::fail("mcp", "no turn " + turn + " waits (answered already, or its run ended); kk_agent_next shows what waits");
  auto req = queue_.request(turn);
  if (!req) return std::unexpected(req.error());
  auto calls = json::parse(calls_json.empty() ? "[]" : calls_json, nullptr, false);
  if (calls.is_object()) calls = json::array({calls});
  if (!calls.is_array()) return kk::fail("mcp", "calls must be a JSON array of {\"name\": \"<tool>\", \"input\": {...}}");
  // Only the agent's own tools: a wrong name is sent back now, not to the engine.
  std::set<std::string> names;
  for (const auto& tool : json::parse(*req, nullptr, false).value("tools", json::array())) names.insert(str(tool, "name"));
  for (const auto& c : calls) {
    if (!c.is_object() || !names.contains(str(c, "name"))) {
      return kk::fail("mcp", fmt::format("unknown tool {} — this agent has: {}", c.is_object() ? str(c, "name") : c.dump(),
                                         core::join(std::vector<std::string>(names.begin(), names.end()), ", ")));
    }
  }
  if (calls.empty() && core::trim(text).empty()) return kk::fail("mcp", "an answer needs a call of the agent's tools (or text)");
  json answer = {{"text", text}, {"calls", calls}};
  auto answer_text = answer.dump(-1, ' ', false, kReplace);
  record_traffic(root_, *t, {{"turn", turn}, {"answer", answer_text.size()}});
  if (auto r = queue_.answer(turn, answer_text); !r) return std::unexpected(r.error());
  // The engine runs the calls and asks again — or the run ends.
  auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(wait_s)) {
    auto waiting = queue_.waiting();
    for (const auto& w : waiting) {
      if (w.run != t->run || w.id == turn) continue;
      queue_.take(w.id);
      auto next_req = queue_.request(w.id);
      if (!next_req) break;
      return show(w, *next_req, false, {});  // recorded like every turn shown (traffic)
    }
    // The run ended (the engine writes that before it starts the next agent of the ticket)...
    if (auto end = run_end(t->run); !end.empty()) {
      return fmt::format("Run {} ended: {}. Call kk_agent_next: the ticket may go on with another agent (another role or block, maybe "
                         "another subagent).",
                         t->run, end);
    }
    // ... or it waits on a nested run of the same ticket (an `ask`): serve that one now.
    if (const auto* n = nested_turn(waiting, *t)) {
      queue_.take(n->id);
      if (auto nreq = queue_.request(n->id)) return nested_note(*n, t->run, turn) + show(*n, *nreq, true, {});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
  }
  return fmt::format("The agent of run {} is still working on your calls (a build or tests can take a while). Call "
                     "kk_agent_next(run=\"{}\", after=\"{}\") to get its next turn (only what is new).",
                     t->run, t->run, turn);
}

}  // namespace kk::agents
