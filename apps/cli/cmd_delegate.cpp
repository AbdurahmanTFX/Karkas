// karkas brief <block> [--task] and karkas review <block>: the brief for a subagent and what to
// check before accepting its work (docs/adr/0018), the same texts as kk_brief / kk_review.
#include <iostream>

#include "app.hpp"
#include "kk/agents/brief.hpp"
#include "kk/core/config.hpp"
#include "kk/core/schema.hpp"
#include "kk/index/index.hpp"

namespace kk::cli {
namespace {

AutoRegister reg([](CLI::App& cli, App& app) {
  static std::string block, task;
  auto* brief = cli.add_subcommand("brief", "the brief for the subagent of a block ([fill] lines for the delegating model)");
  brief->add_option("block", block, "leaf to hand over")->required();
  brief->add_option("--task", task, "the task, as a start");
  brief->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto cfg = core::load_config(root);
    if (!cfg) die(cfg.error().str());
    auto schema = core::load_schema(root);
    if (!schema) die(schema.error().str());
    auto idx = open_index(root);
    auto r = agents::make_brief(root, *schema, *cfg, idx->snapshot(), idx->runs(), block, task);
    if (!r) die(r.error().message);
    std::cout << *r;
  });
  auto* review = cli.add_subcommand("review", "what to check before accepting the work on a block");
  review->add_option("block", block, "block whose work you review")->required();
  review->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto schema = core::load_schema(root);
    if (!schema) die(schema.error().str());
    auto idx = open_index(root);
    auto r = agents::make_review(root, *schema, idx->snapshot(), block);
    if (!r) die(r.error().message);
    std::cout << *r;
  });
});

}  // namespace
}  // namespace kk::cli
