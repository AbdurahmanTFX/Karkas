#include "worker.hpp"

#include <fmt/format.h>
#include <spawn.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <tuple>
#include <chrono>
#include <set>

#include "kk/agents/changes.hpp"
#include "kk/agents/engine.hpp"
#include "kk/code/analyzer.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/index/watch.hpp"
#include "i18n.hpp"
#include "metrics.hpp"
#include "settings.hpp"

extern char** environ;

namespace kk::gui {

Worker::Worker(fs::path root, core::Config cfg, std::function<void()> wake)
    : root_(std::move(root)), cfg_(std::move(cfg)), wake_(std::move(wake)) {
  thread_ = std::thread([this] { loop(); });
}

Worker::~Worker() {
  {
    std::lock_guard lk(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Worker::set_request(const Request& r) {
  {
    std::lock_guard lk(mu_);
    if (req_ == r) return;
    req_ = r;
    dirty_ = true;
  }
  cv_.notify_all();
}

std::shared_ptr<const Model> Worker::model() const {
  std::lock_guard lk(mu_);
  return model_;
}

void Worker::post(std::function<void()> job) {
  {
    std::lock_guard lk(mu_);
    jobs_.push_back(std::move(job));
  }
  cv_.notify_all();
}

void Worker::reindex() {
  std::lock_guard lk(mu_);
  files_changed_ = true;
  cv_.notify_all();
}

void Worker::propose(MakeEdit make) {
  post([this, make = std::move(make)] {
    pending_.reset();
    pending_error_.clear();
    auto s = core::load_schema(root_);
    if (!s) {
      pending_error_ = s.error().str();
      return;
    }
    auto op = make(*s);
    if (!op) {
      pending_error_ = op.error().message;
      return;
    }
    pending_ = std::move(*op);
  });
}

void Worker::confirm() {
  post([this] {
    if (!pending_) return;
    auto r = agents::apply_edit(root_, *pending_);
    if (r) {
      message_ = tr("applied: ") + r->title;
      undo_.push_back(std::move(*r));
      pending_.reset();
      files_changed_ = true;
    } else {
      pending_error_ = r.error().message;
    }
  });
}

void Worker::cancel() {
  post([this] {
    pending_.reset();
    pending_error_.clear();
  });
}

void Worker::undo() {
  post([this] {
    if (undo_.empty()) return;
    auto e = std::move(undo_.back());
    undo_.pop_back();
    auto r = agents::undo_edit(root_, e);
    message_ = r ? tr("undone: ") + e.title : tr("undo failed: ") + r.error().message;
    files_changed_ = true;
  });
}

void Worker::reload_config() {
  post([this] {
    if (auto c = core::load_config(root_)) cfg_ = std::move(*c);
  });
}

void Worker::stop_run(const fs::path& journal) {
  post([this, journal] {
    auto r = metrics::request_stop(root_, journal);
    message_ = r ? tr("stop requested: ") + journal.filename().string() : r.error().message;
  });
}

void Worker::save_cleared(Cleared c) {
  post([this, c = std::move(c)] {
    if (auto r = gui::save_cleared(root_, c); !r) message_ = r.error().message;
  });
}

void Worker::approve_change(const std::string& id) {
  post([this, id] {
    auto e = agents::Engine::create(root_, cfg_);
    if (!e) {
      message_ = e.error().message;
      return;
    }
    auto r = (*e)->approve(id);
    message_ = r ? core::split(*r, '\n').front() : r.error().message;
    files_changed_ = true;
  });
}

void Worker::reject_change(const std::string& id) {
  post([this, id] {
    agents::ChangeStore store(root_);
    auto r = store.set_status(id, "rejected");
    message_ = r ? id + " " + tr("rejected") : r.error().message;
    files_changed_ = true;
  });
}

void Worker::edit_change(const std::string& id, const std::string& diff_text) {
  post([this, id, diff_text] {
    agents::ChangeStore store(root_);
    auto c = store.load(id);
    if (!c) {
      message_ = c.error().message;
      return;
    }
    if (c->status.value != "pending") {
      message_ = id + ": " + tr("only a pending change can be edited");
      return;
    }
    if (auto ok = agents::check_proposal(root_, diff_text); !ok) {
      message_ = id + ": " + ok.error().message;
      return;
    }
    auto d = agents::parse_diff(diff_text);
    if (!d) {
      message_ = d.error().message;
      return;
    }
    c->diff = std::move(*d);
    auto w = core::write_file(store.file(id), core::write_change(*c));
    message_ = w ? id + " " + tr("saved") : w.error().message;
    files_changed_ = true;
  });
}

void Worker::save_positions(const std::string& view, std::map<std::string, layout::Pos> pos) {
  post([this, view, pos = std::move(pos)] {
    layout::LayoutStore store(root_);
    if (auto r = store.save_manual(view, pos); !r) message_ = r.error().message;
  });
}

void Worker::open_in_editor(const std::string& rel_file, int line) {
  post([this, rel_file, line] {
    auto abs = (root_ / rel_file).string();
    std::string quoted = "'" + core::replace_all(abs, "'", "'\\''") + "'";
    std::string cmd = core::replace_all(core::replace_all(cfg_.editor_command, "{file}", quoted), "{line}",
                                        std::to_string(std::max(line, 1)));
    // Detached: the GUI never waits for the editor (14.7).
    pid_t pid = 0;
    const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
    if (posix_spawn(&pid, "/bin/sh", nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0) {
      message_ = tr("cannot start the editor: ") + cmd;
    } else {
      message_ = tr("editor: ") + cmd;
    }
  });
}

namespace {

// What a change does to the schema, for drawing it before approval (ADR 0021).
ChangeGhost change_ghost(const fs::path& root, const core::Change& c) {
  ChangeGhost g;
  g.id = c.id;
  auto schema = core::load_schema(root);
  if (!schema) {
    g.error = schema.error().message;
    return g;
  }
  if (c.status.value != "pending") {
    // A decided change is in the cards already (or never will be): only its blocks are shown.
    std::set<std::string> ids;
    for (const auto& d : c.diff) {
      if (d.text.starts_with("@block ")) ids.insert(std::string(core::trim(std::string_view(d.text).substr(7))));
    }
    for (const auto& id : ids) {
      if (schema->find(id) != nullptr) g.touched.push_back(id);
    }
    std::string scope;
    bool first = true;
    for (const auto& id : g.touched) {
      std::string parent = core::parent_id(id);
      if (first) {
        scope = parent;
        first = false;
        continue;
      }
      while (!scope.empty() && !(parent == scope || parent.starts_with(scope + "."))) scope = core::parent_id(scope);
    }
    g.scope = scope;
    std::set<std::string> expand;
    for (const auto& id : g.touched) {
      for (std::string x = core::parent_id(id); !x.empty() && x != scope; x = core::parent_id(x)) expand.insert(x);
    }
    g.expand.assign(expand.begin(), expand.end());
    return g;
  }
  auto cards = agents::cards_after(root, c.diff);
  if (!cards) {
    g.error = cards.error().message;
    return g;
  }
  using LinkT = std::tuple<std::string, std::string, std::string, std::string, std::string>;
  auto dir_name = [](core::PortDir d) { return d == core::PortDir::In ? "in" : d == core::PortDir::Out ? "out" : "fn"; };
  auto ports_of = [&](const core::Block& b) {
    std::set<std::pair<std::string, std::string>> out;
    for (const auto& p : b.ports) out.insert({dir_name(p.dir), p.name});
    return out;
  };
  auto links_of = [](const core::Block& b) {
    std::set<LinkT> out;
    for (const auto& p : b.ports) {
      if (p.dir == core::PortDir::In && p.from) {
        out.insert({p.link == core::LinkKind::Event ? "event" : "data", p.from->block, p.from->port, b.id, p.name});
      }
    }
    for (const auto& cl : b.calls) out.insert({"call", b.id, "", cl.target.block, cl.target.port});
    for (const auto& u : b.uses) out.insert({"use", b.id, "", u.target, ""});
    return out;
  };
  std::vector<std::string> involved;
  for (const auto& [id, text] : *cards) {
    const auto* old = schema->find(id);
    if (text.empty()) {  // removed card
      if (old != nullptr) g.touched.push_back(id);
      involved.push_back(id);
      continue;
    }
    auto nb = core::parse_block_text(text, id + "/block.ksl");
    if (!nb) continue;
    involved.push_back(id);
    std::set<std::pair<std::string, std::string>> before;
    std::set<LinkT> links_before;
    if (old == nullptr) {
      GhostBlock gb{id, nb->kind_text.value, nb->purpose.value, {}};
      for (const auto& [d, n] : ports_of(*nb)) gb.ports.push_back({id, n, d, true});
      g.blocks.push_back(std::move(gb));
    } else {
      g.touched.push_back(id);
      before = ports_of(*old);
      links_before = links_of(*old);
      auto after = ports_of(*nb);
      for (const auto& [d, n] : after) {
        if (!before.contains({d, n})) g.ports.push_back({id, n, d, true});
      }
      for (const auto& [d, n] : before) {
        if (!after.contains({d, n})) g.ports.push_back({id, n, d, false});
      }
    }
    auto links_after = links_of(*nb);
    for (const auto& l : links_after) {
      if (!links_before.contains(l)) g.links.push_back({std::get<0>(l), std::get<1>(l), std::get<2>(l), std::get<3>(l), std::get<4>(l), true});
    }
    for (const auto& l : links_before) {
      if (!links_after.contains(l)) g.links.push_back({std::get<0>(l), std::get<1>(l), std::get<2>(l), std::get<3>(l), std::get<4>(l), false});
    }
  }
  for (const auto& l : g.links) {
    involved.push_back(l.src_block);
    involved.push_back(l.dst_block);
  }
  // The level: the deepest group that holds every block involved; groups below it on the way
  // to an involved block open in place, so the blocks themselves are on the canvas.
  std::string scope;
  bool first = true;
  for (const auto& id : involved) {
    std::string parent = core::parent_id(id);
    if (first) {
      scope = parent;
      first = false;
      continue;
    }
    while (!scope.empty() && !(parent == scope || parent.starts_with(scope + "."))) scope = core::parent_id(scope);
  }
  g.scope = scope;
  std::set<std::string> expand;
  for (const auto& id : involved) {
    for (std::string x = core::parent_id(id); !x.empty() && x != scope; x = core::parent_id(x)) {
      if (const auto* b = schema->find(x); b != nullptr && b->kind == core::BlockKind::Group) expand.insert(x);
    }
  }
  g.expand.assign(expand.begin(), expand.end());
  std::sort(g.touched.begin(), g.touched.end());
  g.touched.erase(std::unique(g.touched.begin(), g.touched.end()), g.touched.end());
  return g;
}

}  // namespace

namespace {

// Starts a program detached (the GUI never waits for it); false when it cannot start.
bool spawn_detached(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  return posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) == 0;
}

}  // namespace

void Worker::open_folder(const std::string& rel_dir) {
  post([this, rel_dir] {
    auto dir = rel_dir.empty() ? root_ : root_ / rel_dir;
    message_ = spawn_detached({"xdg-open", dir.string()}) ? tr("folder: ") + dir.string() : tr("cannot open the folder: ") + dir.string();
  });
}

void Worker::open_block_folder(const std::string& block) {
  post([this, block] {
    std::string rel;
    if (auto s = core::load_schema(root_)) {
      if (const auto* b = s->find(block)) {
        rel = b->dir;
        if (b->external()) rel = fs::path(b->file_list().front()).parent_path().generic_string();
      }
    }
    auto dir = root_ / rel;
    message_ = spawn_detached({"xdg-open", dir.string()}) ? tr("folder: ") + dir.string() : tr("cannot open the folder: ") + dir.string();
  });
}

void Worker::loop() {
  code::CppAnalyzer analyzer;
  std::unique_ptr<index::Index> idx;
  auto opened = index::Index::open(root_);
  if (opened) {
    idx = std::move(*opened);
  } else {
    message_ = opened.error().str();
  }
  index::Watcher watcher;
  (void)watcher.start(root_, [this] {
    {
      std::lock_guard lk(mu_);
      files_changed_ = true;
    }
    cv_.notify_all();
  });
  auto engine = layout::default_engine();
  layout::LayoutStore store(root_);
  while (true) {
    Request req;
    bool reindex = false;
    bool woke = false;
    std::deque<std::function<void()>> jobs;
    {
      std::unique_lock lk(mu_);
      woke = cv_.wait_for(lk, std::chrono::seconds(2), [&] { return stop_ || dirty_ || files_changed_ || !jobs_.empty(); });
      if (stop_) break;
      req = req_;
      reindex = files_changed_;
      files_changed_ = false;
      dirty_ = false;
      jobs.swap(jobs_);
    }
    if (!woke && model_) {
      // Timer tick: only the "agent working" markers can change without a file event
      // the watcher sees; rebuild (and wake the GUI) only when they did (14.8).
      auto active = metrics::active_runs(root_);
      bool same = active.size() == model_->active.size() &&
                  std::equal(active.begin(), active.end(), model_->active.begin(),
                             [](const metrics::ActiveRun& a, const metrics::ActiveRun& b) { return a.journal == b.journal; });
      if (same) continue;
    }
    for (auto& j : jobs) j();
    {
      std::lock_guard lk(mu_);
      if (files_changed_) {
        reindex = true;
        files_changed_ = false;
      }
    }
    auto m = std::make_shared<Model>();
    m->req = req;
    m->engine = engine->name();
    if (idx) {
      auto t0 = std::chrono::steady_clock::now();
      if (reindex || !model_) {
        auto st = idx->update({false, &analyzer});
        if (!st) message_ = st.error().str();
      }
      m->index_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      auto snap = std::make_shared<index::Snapshot>(idx->snapshot());
      m->snap = snap;
      float cw = req.char_w;
      float lh = req.line_h;

      // Default size of a block (scaled by the settings); the GUI computes the same base.
      double scale = std::clamp(static_cast<double>(req.block_scale), static_cast<double>(kMinBlockScale),
                                static_cast<double>(kMaxBlockScale));
      layout::Measure measure = [&](const index::BlockRow& b) {
        auto [left, right] = pin_rows(*snap, b);
        auto [w, h] = base_block_size(b.id, b.kind, b.purpose, left, right, cw, lh);
        return std::make_pair(std::max(w * scale, static_cast<double>(kMinBlockW)), std::max(h * scale, static_cast<double>(kMinBlockH)));
      };
      m->graph = layout::level_graph(*snap, req.scope, req.show_use, measure);
      std::string view = req.scope.empty() ? "root" : req.scope;
      // Sizes set by hand on the picture win over the default ones.
      m->manual = store.manual(view);
      for (auto& n : m->graph.nodes) {
        auto it = m->manual.find(n.id);
        if (it == m->manual.end() || it->second.w <= 0 || it->second.h <= 0) continue;
        n.w = std::clamp(it->second.w, static_cast<double>(kMinBlockW), static_cast<double>(kMaxBlockW));
        n.h = std::clamp(it->second.h, static_cast<double>(kMinBlockH), static_cast<double>(kMaxBlockH));
      }
      m->level = store.get(view, m->graph, *engine);
      m->layout_key = view + "|" + layout::graph_hash(m->graph) + (req.show_use ? "|use" : "");
      if (!req.expanded.empty() || req.all_levels) {
        // Groups opened in place (Shift+double-click): a nested layout of this level; the
        // visible blocks are its non-frame boxes. Not saved: a temporary view.
        std::set<std::string> open(req.expanded.begin(), req.expanded.end());
        bool all = req.all_levels;
        m->nested = layout::layout_nested(*snap, req.scope, [&](const std::string& id) { return all || open.contains(id); },
                                          req.show_use, measure, *engine);
        m->graph.nodes.clear();
        m->graph.edges.clear();
        m->level.pos.clear();
        for (const auto& b : m->nested) {
          if (b.group) continue;
          m->graph.nodes.push_back({b.id, b.w, b.h});
          m->level.pos[b.id] = layout::Pos{b.x, b.y};
        }
        m->layout_key = view + (all ? "|all" : "|open:" + core::join(req.expanded, ",")) + "|" + layout::graph_hash(m->graph) +
                        (req.show_use ? "|use" : "");
      }
      m->runs = idx->runs();
      if (!req.selected.empty()) {
        if (const auto* b = snap->block(req.selected)) {
          m->card_text = core::read_file(root_ / b->dir / "block.ksl").value_or("");
          auto v = idx->view(b->id, b->kind == "group" ? 1 : 2);
          m->view_text = v ? *v : v.error().message;
          std::error_code ec;
          if (b->kind != "group") {
            std::vector<std::string> files;
            for (auto it = fs::recursive_directory_iterator(root_ / b->dir, ec); it != fs::recursive_directory_iterator();
                 it.increment(ec)) {
              if (ec) break;
              if (it->is_regular_file(ec) && core::is_code_file(it->path())) files.push_back(core::rel_str(root_, it->path()));
            }
            // Imported blocks: their code lives elsewhere ("files" of the card).
            if (auto card = core::parse_block_text(m->card_text, b->dir + "/block.ksl"); card && card->external()) {
              for (const auto& f : card->file_list()) files.push_back(f);
            }
            std::sort(files.begin(), files.end());
            m->files = std::move(files);
          }
        }
      }
    }
    if (!req.open_file.empty()) m->code_text = core::read_file(root_ / req.open_file).value_or("(cannot read)");
    if (!req.ticket.empty()) m->ticket_text = core::read_file(root_ / ".karkas" / "tickets" / (req.ticket + ".ksl")).value_or("");
    agents::ChangeStore changes(root_);
    if (auto cl = changes.list()) m->changes = std::move(*cl);
    if (!req.change.empty()) {
      for (const auto& c : m->changes) {
        if (c.id != req.change) continue;
        m->change_cards = agents::preview_diff(root_, c.diff);
        m->change_preview = core::write_change(c) + "\n" + m->change_cards;
        m->ghost = change_ghost(root_, c);
      }
    }
    m->active = metrics::active_runs(root_);
    if (pending_) {
      m->has_pending = true;
      m->pending_title = pending_->title;
      m->pending_preview = agents::preview_edit(root_, *pending_);
    }
    m->pending_error = pending_error_;
    m->can_undo = !undo_.empty();
    m->message = message_;
    m->serial = ++serial_;
    {
      std::lock_guard lk(mu_);
      model_ = std::move(m);
    }
    if (wake_) wake_();
  }
  watcher.stop();
}

}  // namespace kk::gui
