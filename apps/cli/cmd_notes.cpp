// Notes projects (docs/adr/0039): karkas notes "<request>" [--block <id>] — the notes assistant reads the
// notes, answers and proposes edits; karkas proposal list | show | approve | reject — the human reviews them.
#include <fmt/format.h>

#include <csignal>
#include <iostream>

#include "app.hpp"
#include "kk/agents/engine.hpp"
#include "kk/agents/notes_agent.hpp"
#include "kk/core/cancel.hpp"
#include "kk/core/config.hpp"
#include "kk/core/text.hpp"

namespace kk::cli {
namespace {

void on_stop(int sig) {
  if (kk::core::cancel_requested()) {
    std::signal(sig, SIG_DFL);
    std::raise(sig);
    return;
  }
  kk::core::request_cancel();
}

AutoRegister reg([](CLI::App& cli, App& app) {
  auto* notes = cli.add_subcommand("notes", "notes project: ask the assistant; it answers and proposes edits (N-NNNN) for you to approve");
  static std::string request, scope;
  notes->add_option("request", request, "what to do or what to find out")->required();
  notes->add_option("--block", scope, "the note or topic the request is about");
  notes->callback([&app] {
    std::signal(SIGINT, on_stop);
    std::signal(SIGTERM, on_stop);
    auto root = app.root();
    setup_logging(app, root, false);
    auto cfg = core::load_config(root);
    if (!cfg) die(cfg.error().str());
    agents::EngineOptions o;
    o.analyzer = analyzer();
    o.progress = [](const std::string& m) { std::cerr << "· " << m << "\n"; };
    auto e = agents::Engine::create(root, *cfg, o);
    if (!e) die(e.error().str());
    auto r = (*e)->notes(request, scope);
    if (!r && r.error().code == "cancelled") die(r.error().message, 130);
    if (!r) die(r.error().message);
    std::cout << *r;
  });

  auto* prop = cli.add_subcommand("proposal", "the notes assistant's proposals: list | show <id> | approve <id> | reject <id>");
  static std::string action, id;
  prop->add_option("action", action, "list | show | approve | reject")->required();
  prop->add_option("id", id, "N-NNNN");
  prop->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    agents::NotesProposalStore store(root);
    if (action == "list") {
      auto all = store.list();
      if (!all) die(all.error().message);
      if (all->empty()) std::cout << "no proposals\n";
      for (const auto& p : *all) {
        std::cout << fmt::format("{}  {:<9} {}  ({} edits) — {}\n", p.id, p.status, p.summary, p.edits.size(), core::truncate_utf8(p.request, 80));
      }
      return;
    }
    if (id.empty()) die("proposal " + action + " <N-NNNN>");
    if (action == "show") {
      auto p = store.get(id);
      if (!p) die(p.error().message);
      std::cout << fmt::format("{} {} — {}\nrequest: {}\n", p->id, p->status, p->summary, p->request);
      std::cout << agents::notes_proposal_preview(root, *p);
      return;
    }
    kk::Result<std::string> r = action == "approve" ? agents::approve_notes_proposal(root, id)
                               : action == "reject" ? agents::reject_notes_proposal(root, id)
                                                    : kk::Result<std::string>(kk::fail("cli", "list | show | approve | reject"));
    if (!r) die(r.error().message);
    std::cout << *r;
  });
});

}  // namespace
}  // namespace kk::cli
