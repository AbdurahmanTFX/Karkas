// Commands that run agents (appendix G): design, expand, approve, implement, task, debug,
// ask, ticket, change.
#include <fmt/format.h>

#include <iostream>

#include "app.hpp"
#include "kk/agents/changes.hpp"
#include "kk/agents/engine.hpp"
#include "kk/core/config.hpp"
#include "kk/core/text.hpp"
#include "kk/core/tickets.hpp"

namespace kk::cli {
namespace {

std::unique_ptr<agents::Engine> engine(App& app) {
  auto root = app.root();
  setup_logging(app, root, false);
  auto cfg = core::load_config(root);
  if (!cfg) die(cfg.error().str());
  agents::EngineOptions o;
  o.analyzer = analyzer();
  o.progress = [](const std::string& m) { std::cerr << "· " << m << "\n"; };
  auto e = agents::Engine::create(root, *cfg, o);
  if (!e) die(e.error().str());
  return std::move(*e);
}

void out(const kk::Result<std::string>& r) {
  if (!r) die(r.error().str());
  std::cout << *r;
}

AutoRegister reg([](CLI::App& cli, App& app) {
  static std::string idea, block, what, text, origin = "human", id, at, question;
  static bool all = false, path = false;
  static std::vector<std::string> facts;

  auto* design = cli.add_subcommand("design", "architect: design the top level from an idea (stops for approval)");
  design->add_option("idea", idea, "one-line idea (default: idea of project.ksl)");
  design->callback([&app] { out(engine(app)->design(idea)); });

  auto* expand = cli.add_subcommand("expand", "architect: expand a group into children, or write the api/ of a leaf");
  expand->add_option("block", block, "block id");
  expand->add_flag("--all", all, "expand every group without children and every leaf without api/");
  expand->callback([&app] {
    if (!all && block.empty()) die("expand <block> or expand --all");
    out(engine(app)->expand(all ? (block.empty() ? "all" : block) : block, all));
  });

  auto* approve = cli.add_subcommand("approve", "approve L0, the api/ (freeze), or a change C-NNNN");
  approve->add_option("what", what, "L0 | api | C-NNNN")->required();
  approve->add_option("block", block, "with api: only this subtree");
  approve->callback([&app] { out(engine(app)->approve(what, block)); });

  auto* implement = cli.add_subcommand("implement", "testers and workers implement leaves, then the composition root");
  implement->add_option("target", block, "block id or all")->required();
  implement->callback([&app] { out(engine(app)->implement(block)); });

  auto* task = cli.add_subcommand("task", "a feature or change request through the orchestrator");
  task->add_option("description", text, "what to do")->required();
  task->callback([&app] { out(engine(app)->task(text)); });

  auto* debug = cli.add_subcommand("debug", "debug by descending the schema: symptom + facts");
  debug->add_option("symptom", text, "symptom in the user's words")->required();
  debug->add_option("--fact", facts, "observed fact (failing test, probe value); repeatable");
  debug->add_option("--origin", origin, "human | tester | orchestrator");
  debug->callback([&app] { out(engine(app)->debug(text, facts, origin)); });

  auto* ask = cli.add_subcommand("ask", "ask one short question to the agent of a block");
  ask->add_option("block", block, "block id")->required();
  ask->add_option("question", question, "question")->required();
  ask->add_option("--ticket", id, "append the answer to this ticket");
  ask->callback([&app] { out(engine(app)->ask(block, question, id)); });

  auto* ticket = cli.add_subcommand("ticket", "tickets: list | show <id> [--path [--at <block>]] | run <id>");
  ticket->require_subcommand(1);
  auto* list = ticket->add_subcommand("list", "all tickets");
  list->callback([&app] {
    core::TicketStore store(app.root());
    auto all_t = store.list();
    if (!all_t) die(all_t.error().str());
    for (const auto& t : *all_t) {
      std::cout << fmt::format("{}  {:<8} {:<10} {}\n", t.id, t.kind.value, t.status.value,
                               t.symptom.set ? t.symptom.value : t.task.value);
    }
    if (all_t->empty()) std::cout << "(no tickets)\n";
  });
  auto* show = ticket->add_subcommand("show", "a ticket file, or the path view an agent gets");
  show->add_option("id", id, "T-NNNN")->required();
  show->add_flag("--path", path, "path view (10.1)");
  show->add_option("--at", at, "block of the path view (default: the last pick)");
  show->callback([&app] {
    core::TicketStore store(app.root());
    auto t = store.load(id);
    if (!t) die(t.error().str());
    if (!path) {
      std::cout << core::write_ticket(*t);
      return;
    }
    std::string where = at;
    if (where.empty()) {
      where = "root";
      for (const auto& e : t->trail) {
        if (e.type == core::TrailEntry::Type::Level && !e.get("pick").empty()) where = e.get("pick");
      }
    }
    std::cout << core::path_view(*t, where);
  });
  auto* run = ticket->add_subcommand("run", "continue an open ticket");
  run->add_option("id", id, "T-NNNN")->required();
  run->callback([&app] { out(engine(app)->run_ticket(id)); });

  auto* change = cli.add_subcommand("change", "schema changes: list | show <id> | reject <id>");
  change->require_subcommand(1);
  auto* clist = change->add_subcommand("list", "all changes");
  clist->callback([&app] {
    agents::ChangeStore store(app.root());
    auto all_c = store.list();
    if (!all_c) die(all_c.error().str());
    for (const auto& c : *all_c) std::cout << fmt::format("{}  {:<9} {}  ({})\n", c.id, c.status.value, c.reason.value, c.from.value);
    if (all_c->empty()) std::cout << "(no changes)\n";
  });
  auto* cshow = change->add_subcommand("show", "the change and the card diff it would make");
  cshow->add_option("id", id, "C-NNNN")->required();
  cshow->callback([&app] {
    agents::ChangeStore store(app.root());
    auto c = store.load(id);
    if (!c) die(c.error().str());
    std::cout << core::write_change(*c) << "\n" << agents::preview_diff(app.root(), c->diff);
  });
  auto* reject = change->add_subcommand("reject", "reject a pending change");
  reject->add_option("id", id, "C-NNNN")->required();
  reject->callback([&app] { out(engine(app)->reject(id)); });
});

}  // namespace
}  // namespace kk::cli
