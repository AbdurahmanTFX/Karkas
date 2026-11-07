// karkas cost [--by block|role|model|ticket] [--since <date>] (16.3) and
// karkas llm ping|tokens — a smoke call / token count against the real API (stage 4).
#include <fmt/format.h>

#include <chrono>
#include <iostream>

#include "app.hpp"
#include "kk/core/config.hpp"
#include "kk/core/paths.hpp"
#include "kk/index/index.hpp"
#include "kk/llm/provider.hpp"
#include "kk/metrics/metrics.hpp"

namespace kk::cli {
namespace {

AutoRegister reg_cost([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("cost", "token and money report from the runs table");
  static std::string by = "model";
  static std::string since;
  cmd->add_option("--by", by, "block | role | model | ticket")->check(CLI::IsMember({"block", "role", "model", "ticket"}));
  cmd->add_option("--since", since, "YYYY-MM-DD or unix time");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto cfg = core::load_config(root);
    if (!cfg) die(cfg.error().str());
    auto idx = open_index(root);
    auto ts = metrics::parse_since(since);
    if (!ts) die(ts.error().message);
    auto rep = metrics::cost_report(*cfg, idx->runs(std::to_string(*ts)), by);
    if (!rep) die(rep.error().message);
    std::cout << *rep;
  });
});

AutoRegister reg_llm([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("llm", "talk to a configured model directly (smoke tests, token counts)");
  static std::string tier = "cheap";
  static std::string text = "Reply with the single word: pong";
  static std::string file;
  auto* ping = cmd->add_subcommand("ping", "one small request; records tokens and cost in the runs table");
  auto* tokens = cmd->add_subcommand("tokens", "count tokens of a text or file with the API counter");
  for (auto* c : {ping, tokens}) {
    c->add_option("--tier", tier, "strong | medium | cheap | local");
    c->add_option("--text", text, "prompt text");
  }
  tokens->add_option("--file", file, "count the tokens of this file");
  cmd->require_subcommand(1);
  auto run = [&app](bool count) {
    auto root = app.root();
    setup_logging(app, root, false);
    auto cfg = core::load_config(root);
    if (!cfg) die(cfg.error().str());
    const auto* t = cfg->tier(tier);
    if (t == nullptr) die("no [models." + tier + "] in the config");
    auto http = llm::make_transport_from_env();
    auto provider = llm::make_provider(*t, *cfg, *http);
    if (!provider) die(provider.error().message);
    llm::Request req;
    req.model = t->model;
    req.max_tokens = 64;
    std::string prompt = text;
    if (!file.empty()) {
      auto content = core::read_file(file);
      if (!content) die(content.error().message);
      prompt = *content;
    }
    req.messages = {{"user", {llm::ContentBlock::make_text(prompt)}}};
    if (count) {
      auto n = (*provider)->count_tokens(req);
      if (!n) die(n.error().message);
      std::cout << fmt::format("tokens {} model={}\n", *n, t->model);
      return;
    }
    if (auto p = metrics::check_price(*cfg, t->provider, t->model); !p) die(p.error().message);
    auto idx = open_index(root);
    auto journal = metrics::RunJournal::open(root, {"", "ping", "", t->provider, t->model, tier, 1});
    if (!journal) die(journal.error().message);
    auto t0 = std::chrono::steady_clock::now();
    auto r = (*provider)->send(req);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (!r) die(r.error().message);
    metrics::Tokens tk{r->usage.input_tokens, r->usage.output_tokens,
                       r->usage.cache_write_tokens - r->usage.cache_write_1h_tokens, r->usage.cache_write_1h_tokens,
                       r->usage.cache_read_tokens};
    auto cost = metrics::cost_usd(*cfg, t->provider, t->model, tk);
    if (!cost) die(cost.error().message);
    auto row = journal->finish(tk, *cost, ms, 1, r->stop_reason, idx.get());
    if (!row) die(row.error().message);
    std::cout << fmt::format("{}\nmodel={} in={} out={} cache_write={} cache_read={} usd={:.6f} ms={}\n", r->text(),
                             r->model, tk.input, tk.output, tk.cache_write(), tk.cache_read, *cost, ms);
  };
  ping->callback([run] { run(false); });
  tokens->callback([run] { run(true); });
});

}  // namespace
}  // namespace kk::cli
