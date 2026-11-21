#include "kk/agents/brief.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <set>

#include "kk/agents/router.hpp"
#include "kk/code/drift.hpp"
#include "kk/code/gen.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/index/state.hpp"
#include "kk/runner/runner.hpp"

namespace kk::agents {

namespace fs = std::filesystem;

namespace {

bool is_header(std::string_view p) {
  return p.ends_with(".h") || p.ends_with(".hpp") || p.ends_with(".hh") || p.ends_with(".hxx");
}

// Files of a block: the files line of imported blocks, or what is in its own folder.
std::vector<std::string> files_of(const fs::path& root, const core::Block& b) {
  if (b.external()) return b.file_list();
  std::vector<std::string> out;
  std::error_code ec;
  for (const char* sub : {"api", "impl", "tests"}) {
    auto d = root / b.dir / sub;
    if (!fs::is_directory(d, ec)) continue;
    for (auto it = fs::recursive_directory_iterator(d, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec) && it->path().filename() != ".gitkeep") out.push_back(core::rel_str(root, it->path()));
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Where the api of a block is: its headers (imported) or its api/ folder.
std::string api_of(const core::Block& b) {
  if (!b.external()) return b.dir + "/api/";
  std::vector<std::string> h;
  for (const auto& f : b.file_list()) {
    if (is_header(f)) h.push_back(f);
  }
  return h.empty() ? std::string("(no header)") : core::join(h, " ");
}

std::string list_lines(const std::vector<std::string>& items, std::size_t max, std::string_view indent = "  ") {
  std::string out;
  for (std::size_t i = 0; i < items.size() && i < max; ++i) out += fmt::format("{}{}\n", indent, items[i]);
  if (items.size() > max) out += fmt::format("{}... {} more\n", indent, items.size() - max);
  return out;
}

std::string git(const fs::path& root, std::vector<std::string> args, bool* ok = nullptr) {
  std::vector<std::string> argv = {"git", "-C", root.string()};
  argv.insert(argv.end(), args.begin(), args.end());
  auto r = runner::run({argv, root, std::chrono::seconds(30)});
  if (ok != nullptr) *ok = r && r->ok();
  return r ? r->output : std::string();
}

}  // namespace

kk::Result<std::string> make_brief(const fs::path& root, const core::Schema& schema, const core::Config& cfg,
                                   const index::Snapshot& snap, const std::vector<index::RunRow>& runs, std::string_view block,
                                   std::string_view task) {
  const auto* b = schema.find(block);
  if (b == nullptr) return kk::fail("brief", fmt::format("no block {}", block));
  if (!b->has_code()) return kk::fail("brief", fmt::format("{} is a group: brief one of its leaves", block));
  std::string out;
  auto model = model_for(cfg, schema, snap, runs, block, "worker");
  if (model && !model->subagent.empty()) {
    out += fmt::format("# Brief for {} ({}) — block {}\n", model->subagent, model->claude_model, b->id);
  } else {
    out += fmt::format("# Brief — block {}\n", b->id);
  }
  out += "Fill every [fill] line before you hand it over: the subagent knows only this text.\n\n";
  out += fmt::format("Task: {}\n", task.empty() ? std::string("[fill: what to build or fix, and why — one paragraph]") : std::string(task));
  out += "Spec: [fill: concrete numbers and behaviour — sizes, colours, timings, limits, edge cases; what good looks like, "
         "what to avoid; the facts you already found (files, lines, causes). Visual work: sizes in px, colours as RGB, "
         "durations in ms, easing, counts, positions, font size and weight]\n";
  out += "Done when: [fill: observable checks] — and every guarantee below holds, the tests pass.\n\n";

  out += fmt::format("Block: {} ({}, thread {}{}) — {}\n", b->id, b->kind_name(), b->thread.value.empty() ? "main" : b->thread.value,
                     b->est.value.empty() ? "" : ", est " + b->est.value, b->purpose.value);
  auto files = files_of(root, *b);
  out += "Files you change:\n";
  out += files.empty() ? (b->external() ? "  (listed in the card, not written yet)\n" : fmt::format("  {}/api/ impl/ tests/contract/\n", b->dir))
                       : list_lines(files, 12);
  std::vector<std::string> contract;
  for (const auto& p : b->ports) contract.push_back(core::format_port(p));
  if (!contract.empty()) out += "Contract:\n" + list_lines(contract, 24);
  if (!b->invs.empty()) {
    std::vector<std::string> inv;
    for (const auto& i : b->invs) inv.push_back("- " + i.text);
    out += "Guarantees (one test each, written before the code):\n" + list_lines(inv, 12);
  } else {
    out += "Guarantees: none in the card yet — the checks of \"Done when\" are the contract.\n";
  }

  // What it may use: the api of the blocks its card links to.
  std::vector<std::string> order;
  std::map<std::string, std::vector<std::string>> uses;
  auto add = [&](const std::string& id, const std::string& what) {
    if (!uses.contains(id)) order.push_back(id);
    auto& v = uses[id];
    if (!what.empty() && std::find(v.begin(), v.end(), what) == v.end()) v.push_back(what);
  };
  for (const auto& c : b->calls) {
    const auto* t = schema.find(c.target.block);
    const auto* p = t != nullptr ? t->port(c.target.port) : nullptr;
    std::string sig = p != nullptr ? p->signature() : std::string();
    auto paren = sig.find('(');
    add(c.target.block, paren == std::string::npos ? c.target.port : c.target.port + sig.substr(paren));
  }
  for (const auto& p : b->ports) {
    if (p.from) add(p.from->block, fmt::format("out {} {}", p.from->port, p.type));
  }
  for (const auto& u : b->uses) add(u.target, "its types");
  if (!order.empty()) {
    out += "You may use (only through these headers):\n";
    for (const auto& id : order) {
      const auto* t = schema.find(id);
      auto& v = uses[id];
      std::vector<std::string> head(v.begin(), v.begin() + static_cast<long>(std::min<std::size_t>(v.size(), 6)));
      out += fmt::format("  {} [{}]: {}{}\n", id, t != nullptr ? api_of(*t) : "?", core::join(head, "; "),
                         v.size() > 6 ? fmt::format(" +{}", v.size() - 6) : "");
    }
  }

  auto st = index::StateStore(root).load();
  bool frozen = st && st->frozen_api.contains(b->id);
  out += "Do not touch: files of other blocks, the cards (block.ksl)";
  out += frozen ? ", this block's frozen api" : "";
  out += ". A new block, link, port or function is a schema change: stop and report it instead.\n";
  if (!schema.project.rules.empty()) {
    std::vector<std::string> rules;
    for (const auto& r : schema.project.rules) rules.push_back("- " + r.text);
    out += "Project rules:\n" + list_lines(rules, 8);
  }
  std::string tests = code::has_own_cmake(root)
                          ? (cfg.test_command.empty() ? std::string("the project's own build and ctest")
                                                      : "the project's own build, then `" + cfg.test_command + "`")
                          : "the block's contract tests";
  out += fmt::format("Build and test: kk_run_tests {} ({}); kk_check must show 0 errors.\n", b->id, tests);
  out += "Visual result (drawing, effects, fonts, layout): render a preview image (the project's code, or a faithful "
         "mock-up of the same drawing), look at it, improve it, and give its path in the report — the work is accepted by "
         "that image.\n";
  out += "Only this task: another bug you notice is not fixed — record it with kk_note_bug and list it in the report.\n";
  out += "Report: files changed, test results, bugs noted, what you could not do, open questions — a few lines.\n";
  return out;
}

kk::Result<std::string> make_review(const fs::path& root, const core::Schema& schema, const index::Snapshot& snap,
                                    std::string_view block) {
  const auto* b = schema.find(block);
  if (b == nullptr) return kk::fail("review", fmt::format("no block {}", block));
  std::string out = fmt::format("@review {}\n", b->id);
  if (!b->invs.empty()) {
    std::vector<std::string> inv;
    for (const auto& i : b->invs) inv.push_back("[ ] " + i.text);
    out += "Guarantees — find each in the code and the tests:\n" + list_lines(inv, 12);
  }
  auto files = files_of(root, *b);
  std::set<std::string> mine(files.begin(), files.end());
  bool in_git = false;
  git(root, {"rev-parse", "--is-inside-work-tree"}, &in_git);
  if (!in_git) {
    out += "Changes: not a git repository — read the block's files yourself:\n" + list_lines(files, 12);
  } else {
    std::vector<std::string> changed;
    std::vector<std::string> outside;
    // The project may be a folder of a larger repository: paths come relative to its top.
    auto prefix = std::string(core::trim(git(root, {"rev-parse", "--show-prefix"})));
    for (const auto& raw : core::split(git(root, {"status", "--porcelain", "--untracked-files=all", "--", "."}), '\n')) {
      auto l = std::string(core::trim(raw));
      auto sp = l.find(' ');
      if (sp == std::string::npos) continue;
      auto code = l.substr(0, sp);
      auto path = std::string(core::trim(l.substr(sp)));
      if (auto arrow = path.find(" -> "); arrow != std::string::npos) path = path.substr(arrow + 4);
      if (!prefix.empty() && path.starts_with(prefix)) path = path.substr(prefix.size());
      auto slash = path.find('/');
      bool in_build = slash != std::string::npos && core::is_build_dir(root / path.substr(0, slash));
      if (in_build || path.starts_with(".karkas/") || path.ends_with(".tmp")) continue;
      bool own = mine.contains(path) || (!b->external() && path.starts_with(b->dir + "/"));
      changed.push_back(fmt::format("{:<2} {}", code, path));
      if (!own) outside.push_back(path);
    }
    out += changed.empty() ? "Changes: none since the last commit.\n" : "Changes (since the last commit):\n" + list_lines(changed, 20);
    if (!outside.empty()) {
      out += "Outside this block — justify or undo:\n" + list_lines(outside, 12);
    }
    std::vector<std::string> args = {"diff", "HEAD", "--"};
    for (const auto& f : files) args.push_back(f);
    if (files.empty() && !b->external()) args.push_back(b->dir);
    auto diff = git(root, args);
    auto lines = core::split(diff, '\n');
    if (!diff.empty()) {
      const std::size_t max = 200;
      out += "Diff of the block's files:\n";
      for (std::size_t i = 0; i < lines.size() && i < max; ++i) out += lines[i] + "\n";
      if (lines.size() > max) out += fmt::format("... {} more lines (git diff HEAD -- <file>)\n", lines.size() - max);
    }
  }
  auto issues = core::check_schema(schema);
  auto drift = code::check_drift(schema);
  issues.insert(issues.end(), drift.begin(), drift.end());
  std::vector<std::string> own_issues;
  for (const auto& i : issues) {
    if (core::Schema::is_under(b->id, i.block)) own_issues.push_back(i.str());
  }
  out += own_issues.empty() ? "Issues of the block: none.\n" : "Issues of the block:\n" + list_lines(own_issues, 10);
  if (const auto* row = snap.block(b->id)) {
    out += fmt::format("Tests: status {}{} — run kk_run_tests {} if nothing ran since the change.\n", row->status,
                       row->failed_tests > 0 ? fmt::format(", {} failing", row->failed_tests) : "", b->id);
  }
  out += "Visual work: open the preview image named in the report and judge it against the spec yourself.\n";
  out += "Accept only if every guarantee holds, only the block's files changed (or the rest is justified), no frozen api "
         "changed and the tests pass; otherwise send it back with concrete corrections.\n";
  return out;
}

}  // namespace kk::agents
