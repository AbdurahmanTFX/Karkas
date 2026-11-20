// karkas mcp — MCP server over stdio (15). stdout carries protocol messages only.
#include <fcntl.h>
#include <fmt/format.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>
#include <map>
#include <memory>
#include <mutex>

#include "app.hpp"
#include "kk/agents/brief.hpp"
#include "kk/agents/changes.hpp"
#include "kk/agents/mcp_turns.hpp"
#include "kk/agents/prototype.hpp"
#include "kk/agents/router.hpp"
#include "kk/core/config.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/index/index.hpp"
#include "kk/core/tickets.hpp"
#include "kk/mcp/server.hpp"
#include "kk/templates.hpp"

extern char** environ;

namespace kk::cli {

// Provided by cmd_test.cpp when the test pipeline is available.
std::string run_tests_text(const fs::path& root, const std::string& block);
bool have_test_pipeline();
// Provided by cmd_check.cpp: the text `karkas check` prints.
std::string check_text(const fs::path& root, bool drift, int* errors);
std::string check_summary(const fs::path& root);

namespace {

// The engine of a ticket, started in the background by kk_task / kk_debug / kk_ticket_run: the
// same `karkas ticket run` the GUI and a terminal use (ADR 0024). Its output goes to a log file —
// never to this server's stdout, which carries the protocol.
struct Started {
  pid_t pid = 0;
  std::string log;
  bool done = false;
  int exit_code = 0;
};

kk::Result<pid_t> spawn_engine(const fs::path& root, const std::vector<std::string>& args, const fs::path& log) {
  std::error_code ec;
  fs::create_directories(log.parent_path(), ec);
  std::string exe = core::self_executable().string();  // also after a rebuild (bugs.txt 9)
  if (exe.empty()) return kk::fail("mcp", "cannot find the karkas executable");
  std::vector<std::string> argv_s = {exe, "--project", root.string()};
  argv_s.insert(argv_s.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& a : argv_s) argv.push_back(a.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);  // its own session: a stop of the client does not kill it
  pid_t pid = 0;
  int rc = posix_spawn(&pid, exe.c_str(), &fa, &at, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  if (rc != 0) return kk::fail("mcp", fmt::format("cannot start karkas: {}", std::strerror(rc)));
  return pid;
}

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("mcp", "MCP server over stdio for external agents (Claude Code and others)");
  static bool only_agent = false;
  cmd->add_flag("--only-agent", only_agent,
                "only kk_agent_next and kk_agent_act: the server of a Claude Code that Karkas starts for one run (ADR 0029)");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, true);
    auto idx = open_index(root);
    mcp::ServerOptions o;
    o.version = KARKAS_VERSION;
    o.analyzer = analyzer();
    if (have_test_pipeline()) {
      o.run_tests = [root](const std::string& block) { return run_tests_text(root, block); };
    }
    o.check = [root] { return check_summary(root); };
    o.propose_change = [root](const std::string& reason, const std::string& diff) -> kk::Result<std::string> {
      auto c = agents::propose_change(root, "mcp", reason, diff);
      if (!c) return std::unexpected(c.error());
      return fmt::format("proposed {}. Stop here: ask the user to review and approve it (karkas-gui tab Changes, or "
                         "`karkas approve {}`). Cards it would write:\n{}",
                         c->id, c->id, agents::preview_diff(root, c->diff));
    };
    o.changes = [root](const std::string& id) -> kk::Result<std::string> {
      agents::ChangeStore store(root);
      if (id.empty()) {
        auto all = store.list();
        if (!all) return std::unexpected(all.error());
        std::string text;
        for (const auto& c : *all) text += fmt::format("{}  {:<9} {}  ({})\n", c.id, c.status.value, c.reason.value, c.from.value);
        return text.empty() ? std::string("(no changes)\n") : text;
      }
      auto c = store.load(id);
      if (!c) return std::unexpected(c.error());
      return core::write_change(*c) + "\n" + agents::preview_diff(root, c->diff);
    };
    // Read karkas.toml on every call: the user may change the models while the session runs.
    o.model_for = [root, ix = idx.get()](const std::string& block, const std::string& role) -> kk::Result<std::string> {
      auto cfg = core::load_config(root);
      if (!cfg) return std::unexpected(cfg.error());
      auto schema = core::load_schema(root);
      if (!schema) return std::unexpected(schema.error());
      auto c = agents::model_for(*cfg, *schema, ix->snapshot(), ix->runs(), block, role);
      if (!c) return std::unexpected(c.error());
      return agents::model_choice_text(*c);
    };
    o.model_hint = [root, ix = idx.get()](const std::string& block) -> std::string {
      auto cfg = core::load_config(root);
      auto schema = core::load_schema(root);
      if (!cfg || !schema) return {};
      auto c = agents::model_for(*cfg, *schema, ix->snapshot(), ix->runs(), block);
      return c ? agents::model_choice_line(*c) : std::string();
    };
    o.brief = [root, ix = idx.get()](const std::string& block, const std::string& task) -> kk::Result<std::string> {
      auto cfg = core::load_config(root);
      if (!cfg) return std::unexpected(cfg.error());
      auto schema = core::load_schema(root);
      if (!schema) return std::unexpected(schema.error());
      return agents::make_brief(root, *schema, *cfg, ix->snapshot(), ix->runs(), block, task);
    };
    o.review = [root, ix = idx.get()](const std::string& block) -> kk::Result<std::string> {
      auto schema = core::load_schema(root);
      if (!schema) return std::unexpected(schema.error());
      return agents::make_review(root, *schema, ix->snapshot(), block);
    };
    // Agents of the program (ADR 0024): tickets started here and the turns of their agents.
    auto started = std::make_shared<std::map<std::string, Started>>();
    auto started_mu = std::make_shared<std::mutex>();
    auto turns = std::make_shared<agents::McpTurns>(root);
    auto mode_of = [root] {
      auto cfg = core::load_config(root);
      return cfg && cfg->mcp() ? std::string("mcp") : std::string("api");
    };
    auto launch = [root, started, started_mu](const std::string& id, std::vector<std::string> args) -> kk::Result<std::string> {
      auto log = root / ".karkas" / "mcp" / (id + ".log");
      auto pid = spawn_engine(root, args, log);
      if (!pid) return std::unexpected(pid.error());
      std::lock_guard lock(*started_mu);
      (*started)[id] = Started{*pid, log.string(), false, 0};
      return std::string();
    };
    auto next_steps = [root, mode_of](const std::string& id) {
      if (auto cfg = core::load_config(root); cfg && cfg->mcp() && cfg->mcp_launch == "claude") {
        return fmt::format("Karkas serves its agents' turns itself ([agents] launch = \"claude\": a headless Claude Code per run "
                           "on the claude.ai login), so do not serve them. Wait for the end with kk_agent_next(list=true, "
                           "wait_s=300) until {} has ended, then report its result.",
                           id);
      }
      if (mode_of() == "mcp") {
        return fmt::format("Its agents start now and wait for their model (mode mcp: you). Serve them: kk_agent_next(list=true) shows "
                           "who waits and the subagent of each; go on until no turn waits and {} ended.",
                           id);
      }
      return fmt::format("Its agents work through the API (mode api). Follow it with kk_ticket_view(id=\"{}\") or kk_agent_next(list=true).", id);
    };
    o.start_ticket = [root, launch, next_steps](const std::string& kind, const std::string& text, const std::vector<std::string>& facts,
                                                const std::string& block) -> kk::Result<std::string> {
      if (core::trim(text).empty()) return kk::fail("mcp", "text is required");
      // A request limited to a block (docs/adr/0034): the block must exist now, not fail in the background.
      std::string scope = block == "root" ? std::string() : block;
      if (!scope.empty()) {
        auto schema = core::load_schema(root);
        if (!schema) return std::unexpected(schema.error());
        if (schema->find(scope) == nullptr) return kk::fail("mcp", "unknown block " + scope);
      }
      core::TicketStore store(root);
      auto t = store.create(kind == "bug" ? core::TicketKind::Bug : core::TicketKind::Feature, "human", text, facts, scope);
      if (!t) return std::unexpected(t.error());
      if (auto r = launch(t->id, {"ticket", "run", t->id}); !r) return std::unexpected(r.error());
      return fmt::format("created {} ({}{}). {}", t->id, kind, scope.empty() ? "" : ", limited to " + scope, next_steps(t->id));
    };
    o.run_ticket = [root, launch, next_steps](const std::string& id, bool force, const std::string& note) -> kk::Result<std::string> {
      bool message = !core::trim(note).empty();
      // Said now, not after a start that ends at once with nobody told why.
      if (auto t = core::TicketStore(root).load(id); !t) {
        return std::unexpected(t.error());
      } else if (t->status.value == "closed" && !force && !message) {
        return kk::fail("mcp", id + " is closed; force=true reopens it (or give the user's message in note)");
      }
      std::vector<std::string> args = {"ticket", "run", id};
      if (force) args.emplace_back("--force");
      if (message) args.insert(args.end(), {"--note", note});  // the user's message: a fact of the ticket, the run forced
      if (auto r = launch(id, args); !r) return std::unexpected(r.error());
      return fmt::format("{} runs again{}. {}", id, message ? " with the user's message as its fact" : "", next_steps(id));
    };
    // The user's prototype (docs/adr/0035): view, set from the user's words, send to the architect.
    o.prototype = [root, launch, mode_of](const std::string& action, const std::string& sketch_json) -> kk::Result<std::string> {
      auto schema = core::load_schema(root);
      if (!schema) return std::unexpected(schema.error());
      auto shape = agents::shape_of(*schema);
      if (action == "view") {
        auto s = agents::load_sketch(root);
        if (!s) return std::unexpected(s.error());
        if (s->empty()) return std::string("no prototype yet: the user draws one in karkas-gui (schema toolbar: Prototype), or write one with action=set\n");
        return agents::sketch_report(*s, shape);
      }
      if (action == "set") {
        auto parsed = agents::sketch_from_json(sketch_json);
        if (!parsed) return std::unexpected(parsed.error());
        auto s = std::move(*parsed);
        if (!s.scope.empty() && !shape.groups.contains(s.scope)) return kk::fail("mcp", "level " + s.scope + " is not a group of the schema");
        if (auto why = agents::sketch_problems(s, shape.blocks); !why.empty()) return kk::fail("mcp", why);
        agents::lay_out_sketch(s);
        bool replaced = false;
        if (auto old = agents::load_sketch(root); old && !old->empty()) replaced = true;
        if (auto w = agents::save_sketch(root, s); !w) return std::unexpected(w.error());
        return fmt::format("prototype saved ({} blocks, {} dependencies{}). The user sees it in karkas-gui (schema toolbar: "
                           "Prototype), can move and edit it, and approves it there; call action=send only when the user asked "
                           "to build it now.\n{}",
                           s.blocks.size(), s.links.size(), replaced ? "; it replaced the previous prototype" : "", agents::sketch_text(s));
      }
      if (action == "send") {
        auto s = agents::load_sketch(root);
        if (!s) return std::unexpected(s.error());
        if (s->empty()) return kk::fail("mcp", "no prototype: write one with action=set or let the user draw it in karkas-gui");
        auto plan = agents::plan_prototype_send(*s, shape, !schema->project.idea.value.empty());
        if (!plan.error.empty()) return kk::fail("mcp", plan.error);
        bool again = !s->sent.empty();
        if (auto r = launch("prototype", plan.args); !r) return std::unexpected(r.error());
        if (!again && plan.sent == "task") s->sent = plan.sent;  // design / expand mark it themselves when they end
        s->sent_at = core::unix_now();
        (void)agents::save_sketch(root, *s);
        std::string what = plan.sent == "design"   ? "the architect designs the top level from the idea and the prototype (karkas design --sketch)"
                           : plan.sent == "expand" ? "the architect expands " + s->scope + " from the prototype (karkas expand --sketch)"
                                                   : "the orchestrator adds what the prototype shows as a change (a ticket" +
                                                         (s->scope.empty() ? std::string(")") : ", limited to " + s->scope + ")");
        std::string serve = mode_of() == "mcp" ? " Its agents wait for their model (mode mcp: you): kk_agent_next(list=true) shows who waits." : "";
        return fmt::format("started in the background: {}.{} The result names what is kept, renamed or left out and the "
                           "architect's variants (C-NNNN) for the user to approve or reject; kk_prototype(action=view) shows the "
                           "check afterwards.",
                           what, serve);
      }
      return kk::fail("mcp", "action is view, set or send");
    };
    o.implement = [launch, mode_of](const std::string& target) -> kk::Result<std::string> {
      auto id = "implement-" + target;
      if (auto r = launch(id, {"implement", target}); !r) return std::unexpected(r.error());
      if (mode_of() == "mcp") {
        return fmt::format("implement {} started in the background. Its testers and workers wait for their model (mode mcp: you): "
                           "kk_agent_next(list=true) shows who waits and the subagent of each; go on until no turn waits and {} ended.",
                           target, id);
      }
      return fmt::format("implement {} started in the background (mode api); kk_agent_next(list=true) says when it ended.", target);
    };
    // Notes which started processes have exited (caller holds started_mu).
    auto reap = [started] {
      for (auto& [id, s] : *started) {
        if (s.done) continue;
        int status = 0;
        if (waitpid(s.pid, &status, WNOHANG) == s.pid) {
          s.done = true;
          s.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
      }
    };
    turns->tickets_running = [started, started_mu, reap] {
      std::lock_guard lock(*started_mu);
      if (started->empty()) return -1;
      reap();
      int running = 0;
      for (const auto& [id, s] : *started) running += s.done ? 0 : 1;
      return running;
    };
    turns->tickets_status = [root, started, started_mu, reap] {
      std::lock_guard lock(*started_mu);
      reap();
      std::string out;
      for (auto& [id, s] : *started) {
        if (!s.done) {
          out += fmt::format("{}: its agents are working (log {}).\n", id, s.log);
          continue;
        }
        if (!id.starts_with("T-")) {  // a command, not a ticket (kk_implement)
          out += fmt::format("{}: ended with exit code {}; its output is in {}.\n", id, s.exit_code, s.log);
          continue;
        }
        // The result an ended ticket's client reports from (ADR 0029): what each block's agent did, the
        // checks, the api and the notes, so no kk_ticket_view is needed for the report.
        core::TicketStore store(root);
        auto t = store.load(id);
        if (!t) {
          out += fmt::format("{}: ended (exit code {}); its log is {}.\n", id, s.exit_code, s.log);
          continue;
        }
        auto cut = [](std::string text) {
          constexpr std::size_t kMax = 240;
          if (text.size() > kMax) text = text.substr(0, kMax - 3) + "...";
          return text;
        };
        std::string status = t->result_value("status");
        out += fmt::format("{}: ended — {}{}.\n", id, t->status.value, status.empty() ? "" : " (status " + status + ")");
        int facts = 0;
        for (const auto& e : t->trail) {
          if (e.type != core::TrailEntry::Type::Fact || ++facts > 8) continue;
          out += "  fact   " + cut(e.text) + "\n";
        }
        for (const auto& r : t->result) {
          if (r.key == "verify" || r.key == "api" || r.key == "note" || r.key == "fix" || r.key == "files") {
            out += fmt::format("  {:<6} {}\n", r.key, cut(r.text));
          }
        }
        out += fmt::format("  (kk_ticket_view(id=\"{}\") shows the whole ticket.)\n", id);
      }
      return out;
    };
    o.agent_next = [turns](const std::string& run, int wait_s, bool list, const std::string& after, const std::vector<std::string>& seen) {
      return turns->next(run, wait_s, list, after, seen);
    };
    o.agent_act = [turns](const std::string& turn, const std::string& text, const std::string& calls, int wait_s) {
      return turns->act(turn, text, calls, wait_s);
    };
    o.instructions = [mode_of]() -> std::string {
      std::string_view file = mode_of() == "mcp" ? "prompts/mcp_instructions_agents.txt" : "prompts/mcp_instructions.txt";
      return std::string(core::trim(templates::get(file).value_or("")));
    };
    if (only_agent) {
      o.only_agent_tools = true;
      o.instructions = [] {
        return std::string(
            "This server hands you the turns of one run of Karkas's agents: kk_agent_next(run=...) takes the turn, "
            "kk_agent_act answers it with calls of the agent's tools and returns the next turn, until the run ends.");
      };
    }
    mcp::Server server(*idx, o);
    std::ios::sync_with_stdio(false);
    std::exit(server.serve(std::cin, std::cout));
  });
});

}  // namespace
}  // namespace kk::cli
