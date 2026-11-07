// karkas index | view | query (7.2, 7.3, 4.7).
#include <fmt/format.h>

#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>

#include "app.hpp"
#include "kk/index/index.hpp"
#include "kk/index/watch.hpp"

namespace kk::cli {
namespace {

void print(const kk::Result<std::string>& r) {
  if (!r) die(r.error().message);
  std::cout << *r;
}

std::atomic<bool> g_stop{false};

AutoRegister reg_index([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("index", "build or update the index (.karkas/index.db)");
  static bool watch = false;
  static bool full = false;
  cmd->add_flag("--watch", watch, "keep running and re-index on every change (inotify)");
  cmd->add_flag("--full", full, "ignore hashes and re-analyze every file");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto idx = open_index(root, false);
    auto run = [&](bool f) {
      auto st = idx->update({f, analyzer()});
      if (!st) {
        std::cerr << "karkas: " << st.error().str() << "\n";
        return;
      }
      std::cout << fmt::format("index: {} blocks, {} files, {} analyzed, {} in {:.1f} ms\n", st->blocks, st->files,
                               st->analyzed, st->changed ? "updated" : "no changes", st->ms)
                << std::flush;
    };
    run(full);
    if (!watch) return;
    std::mutex mu;
    std::condition_variable cv;
    bool dirty = false;
    index::Watcher w;
    auto r = w.start(root, [&] {
      std::lock_guard lk(mu);
      dirty = true;
      cv.notify_one();
    });
    if (!r) die(r.error().message);
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::cout << "watching " << root.string() << " (Ctrl+C to stop)\n" << std::flush;
    while (!g_stop) {
      std::unique_lock lk(mu);
      cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return dirty || g_stop.load(); });
      if (!dirty) continue;
      dirty = false;
      lk.unlock();
      run(false);
    }
  });
});

AutoRegister reg_view([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand("view", "print the model view of the project or a block (L0/L1/L2)");
  static std::string block;
  static int level = -1;
  static bool use = false;
  cmd->add_option("block", block, "block id (default: whole project)");
  cmd->add_option("--level,-l", level, "0, 1 or 2 (default: 0 for the project, 1 for groups, 2 for leaves)");
  cmd->add_flag("--use", use, "include use links and shared blocks");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto idx = open_index(root);
    index::ViewOptions o{use};
    if (block.empty() || block == "root") {
      std::cout << idx->overview(o);
      return;
    }
    int lv = level;
    if (lv < 0) {
      auto snap = idx->snapshot();
      const auto* b = snap.block(block);
      if (b == nullptr) die("unknown block '" + block + "'");
      lv = b->kind == "group" ? 1 : 2;
    }
    print(idx->view(block, lv, o));
  });
});

AutoRegister reg_query([](CLI::App& cli, App& app) {
  auto* cmd = cli.add_subcommand(
      "query", "schema queries: children neighbors dependents path writers readers by_thread search history status probes drift");
  static std::string name;
  static std::vector<std::string> args;
  static std::string kind;
  static std::string dir = "both";
  cmd->add_option("query", name, "query name")->required();
  cmd->add_option("args", args, "query arguments");
  cmd->add_option("--kind", kind, "neighbors: data|event|call|state|use");
  cmd->add_option("--dir", dir, "neighbors: in|out|both");
  cmd->callback([&app] {
    auto root = app.root();
    setup_logging(app, root, false);
    auto idx = open_index(root);
    auto arg = [&](std::size_t i) -> std::string {
      if (i >= args.size()) die(fmt::format("query {} needs argument {}", name, i + 1));
      return args[i];
    };
    auto opt = [&](std::size_t i) { return i < args.size() ? args[i] : std::string(); };
    if (name == "children") return print(idx->children(opt(0)));
    if (name == "neighbors") return print(idx->neighbors(arg(0), kind, dir));
    if (name == "dependents") return print(idx->dependents(arg(0)));
    if (name == "path") return print(idx->path(arg(0), arg(1)));
    if (name == "writers") return print(idx->writers(arg(0)));
    if (name == "readers") return print(idx->readers(arg(0)));
    if (name == "by_thread") return print(idx->by_thread(arg(0)));
    if (name == "search") {
      std::string q;
      for (const auto& a : args) q += (q.empty() ? "" : " ") + a;
      std::cout << idx->search(q);
      return;
    }
    if (name == "history") return print(idx->history(arg(0), args.size() > 1 ? std::stoi(args[1]) : 10));
    if (name == "status") return print(idx->status(opt(0)));
    if (name == "probes") {
      std::cout << idx->probes(opt(0));
      return;
    }
    if (name == "drift") {
      std::cout << idx->drift(opt(0));
      return;
    }
    die("unknown query '" + name + "'");
  });
});

}  // namespace
}  // namespace kk::cli
