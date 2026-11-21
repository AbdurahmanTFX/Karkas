#include "worker.hpp"

#include "kk/core/tickets.hpp"

#include <fcntl.h>
#include <fmt/format.h>
#include <poll.h>
#include <spawn.h>
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <ctime>
#include <set>
#include <tuple>

#include "kk/agents/changes.hpp"
#include "kk/agents/engine.hpp"
#include "kk/agents/run.hpp"
#include "kk/code/notes.hpp"
#include "kk/core/markdown.hpp"
#include "kk/core/notes.hpp"
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

void Worker::refresh_run() {
  post([this] {
    RunInfo info;
    info.known = true;
    auto cfg = core::load_config(root_);
    auto schema = core::load_schema(root_);
    if (!cfg || !schema) {
      info.error = !cfg ? cfg.error().str() : schema.error().str();
    } else {
      auto t = agents::run_target(*schema, *cfg);
      info.shown = t.shown;
      info.run_command = cfg->run_command;
      info.generated = t.generated;
      info.error = t.error;
      info.candidates = agents::program_candidates(root_, *cfg);
    }
    run_info_ = std::move(info);
  });
}

void Worker::load_notes() {
  auto s = core::load_schema(root_);
  notes_ = s && core::is_notes_project(s->project);
  if (!notes_) {
    note_texts_.reset();
    return;
  }
  auto texts = std::make_shared<std::map<std::string, NoteText>>();
  for (const auto& b : s->blocks) {
    auto file = core::note_file(b);
    if (file.empty()) continue;
    NoteText n;
    n.file = file;
    n.text = core::read_file(root_ / file).value_or("");
    n.markdown = core::md::is_markdown_file(file);
    constexpr std::size_t kPreviewLines = 12;
    if (n.markdown) {
      n.preview = core::md::plain_lines(n.text, kPreviewLines);
      n.doc = std::make_shared<const std::vector<core::md::Block>>(core::md::parse(n.text));
    } else {
      for (const auto& l : core::split(n.text, '\n')) {
        if (n.preview.size() >= kPreviewLines) break;
        if (!core::trim(l).empty()) n.preview.emplace_back(core::trim(l));
      }
    }
    (*texts)[b.id] = std::move(n);
  }
  note_texts_ = std::move(texts);
}

void Worker::apply_now(MakeEdit make) {
  post([this, make = std::move(make)] {
    auto s = core::load_schema(root_);
    if (!s) {
      message_ = s.error().str();
      return;
    }
    auto op = make(*s);
    if (!op) {
      message_ = op.error().message;
      return;
    }
    auto r = agents::apply_edit(root_, *op);
    if (!r) {
      message_ = r.error().message;
      return;
    }
    message_ = tr("applied: ") + r->title;
    undo_.push_back(std::move(*r));
    files_changed_ = true;
  });
}

void Worker::save_note(const std::string& rel_file, std::string text) {
  post([this, rel_file, text = std::move(text)] {
    auto r = core::write_file(root_ / rel_file, text);
    message_ = r ? tr("saved: ") + rel_file : r.error().message;
    files_changed_ = true;
  });
}

void Worker::link_notes(const std::string& src, const std::string& dst) {
  post([this, src, dst] {
    auto s = core::load_schema(root_);
    auto r = s ? code::add_note_link(*s, src, dst) : kk::VoidResult(std::unexpected(s.error()));
    message_ = r ? fmt::format(fmt::runtime(tr("link: {} -> {}")), src, dst) : r.error().message;
    files_changed_ = true;
  });
}

void Worker::unlink_notes(const std::string& src, const std::string& dst) {
  post([this, src, dst] {
    auto s = core::load_schema(root_);
    if (!s) {
      message_ = s.error().str();
      return;
    }
    auto r = code::remove_note_link(*s, src, dst);
    message_ = r ? fmt::format(fmt::runtime(tr("links removed: {}")), *r) : r.error().message;
    files_changed_ = true;
  });
}

void Worker::retitle_note(const std::string& id, const std::string& title) {
  post([this, id, title] {
    auto s = core::load_schema(root_);
    if (!s) {
      message_ = s.error().str();
      return;
    }
    auto r = code::retitle_note(*s, id, title);
    message_ = r ? fmt::format(fmt::runtime(tr("renamed: {} (links followed in {} notes)")), title, r->size()) : r.error().message;
    files_changed_ = true;
  });
}

void Worker::approve_proposal(const std::string& id) {
  post([this, id] {
    auto r = agents::approve_notes_proposal(root_, id);
    message_ = r ? std::string(core::trim(*r)) : r.error().message;
    files_changed_ = true;
  });
}

void Worker::reject_proposal(const std::string& id) {
  post([this, id] {
    auto r = agents::reject_notes_proposal(root_, id);
    message_ = r ? std::string(core::trim(*r)) : r.error().message;
    files_changed_ = true;
  });
}

void Worker::link_mention(const std::string& src, const std::string& dst) {
  post([this, src, dst] {
    auto s = core::load_schema(root_);
    if (!s) {
      message_ = s.error().str();
      return;
    }
    auto r = code::link_mention(*s, src, dst);
    message_ = !r ? r.error().message : *r ? fmt::format(fmt::runtime(tr("link: {} -> {}")), src, dst) : tr("nothing to link any more");
    files_changed_ = true;
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

void Worker::save_ticket(const std::string& id, const std::string& text) {
  post([this, id, text] {
    auto r = core::TicketStore(root_).edit_by_human(id, text);
    if (r) {
      ticket_edit_error_.clear();
      ++ticket_edit_saved_;
      message_ = id + " " + tr("saved");
    } else {
      ticket_edit_error_ = r.error().message;
    }
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

namespace {

// What a program printed: run without a shell, stderr dropped, killed after `timeout_ms`.
struct Captured {
  bool started = false;  // the program was found
  bool ok = false;       // and ended with 0 in time
  std::string out;
};
Captured capture(const std::vector<std::string>& args, int timeout_ms) {
  Captured r;
  int fds[2];
  if (pipe(fds) != 0) return r;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  std::vector<char*> argv;
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    return r;
  }
  r.started = true;
  constexpr std::size_t kMaxBytes = 64ull * 1024 * 1024;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  bool cut = false;
  std::vector<char> buf(1 << 16);
  while (true) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) {
      cut = true;
      break;
    }
    pollfd pfd{fds[0], POLLIN, 0};
    int pr = poll(&pfd, 1, static_cast<int>(left));
    if (pr < 0 && errno == EINTR) continue;
    if (pr <= 0) {
      cut = pr == 0;
      break;
    }
    auto n = read(fds[0], buf.data(), buf.size());
    if (n <= 0) break;
    r.out.append(buf.data(), static_cast<std::size_t>(n));
    if (r.out.size() > kMaxBytes) {
      cut = true;
      break;
    }
  }
  close(fds[0]);
  if (cut) kill(pid, SIGKILL);
  int status = 0;
  waitpid(pid, &status, 0);
  r.ok = !cut && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  return r;
}

// A file name not taken in `dir`: "name.ext", else "name-2.ext", "name-3.ext" ...
fs::path free_name(const fs::path& dir, const std::string& stem, const std::string& ext) {
  std::error_code ec;
  fs::path p = dir / (stem + ext);
  for (int n = 2; fs::exists(p, ec); ++n) p = dir / (stem + "-" + std::to_string(n) + ext);
  return p;
}

bool same_bytes(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) return false;
  auto x = core::read_file(a);
  auto y = core::read_file(b);
  return x && y && *x == *y;
}

// A Markdown link to `rel` (from the note's folder): ![stem](rel) for an image, [name](rel) for another
// file; a path with spaces or brackets goes between < and >.
std::string md_link(const fs::path& file, const std::string& rel) {
  bool image = core::md::is_image_file(file.filename().string());
  std::string text = image ? file.stem().string() : file.filename().string();
  std::erase_if(text, [](char c) { return c == '[' || c == ']' || c == '|'; });
  bool plain = rel.find_first_of(" ()<>") == std::string::npos;
  return std::string(image ? "!" : "") + "[" + text + "](" + (plain ? rel : "<" + rel + ">") + ")";
}

}  // namespace

void Worker::push_insert(const std::string& note_file, std::string text) {
  inserts_.push_back(NoteInsert{++insert_id_, note_file, std::move(text)});
  if (inserts_.size() > 16) inserts_.erase(inserts_.begin());
  files_changed_ = true;  // a new file, and the model carries the insert
}

void Worker::say(std::string message) {
  post([this, message = std::move(message)] { message_ = message; });
}

void Worker::attach_files(const std::string& note_file, std::vector<std::string> paths) {
  post([this, note_file, paths = std::move(paths)] {
    const fs::path note_dir = (root_ / note_file).parent_path();
    std::vector<std::string> links;
    for (const auto& p : paths) {
      std::error_code ec;
      fs::path src = fs::path(p).lexically_normal();
      if (!fs::is_regular_file(src, ec)) {
        message_ = tr("not a file: ") + p;
        continue;
      }
      fs::path target = src;
      if (!core::is_within(root_, src)) {  // from outside the project: a copy next to the note
        bool image = core::md::is_image_file(src.filename().string());
        fs::path dir = note_dir / (image ? "images" : "files");
        fs::create_directories(dir, ec);
        target = dir / src.filename();
        if (!(fs::exists(target, ec) && same_bytes(src, target))) {
          target = free_name(dir, src.stem().string(), src.extension().string());
          fs::copy_file(src, target, ec);
          if (ec) {
            message_ = tr("cannot copy: ") + p + ": " + ec.message();
            continue;
          }
        }
      }
      links.push_back(md_link(src, fs::relative(target, note_dir, ec).generic_string()));
    }
    if (links.empty()) return;
    message_ = fmt::format(fmt::runtime(tr("added to the note: {}")), links.size());
    push_insert(note_file, core::join(links, "\n"));
  });
}

void Worker::paste_image(const std::string& note_file) {
  post([this, note_file] {
    const char* wl = std::getenv("WAYLAND_DISPLAY");
    const bool wayland = wl != nullptr && *wl != '\0';
    auto types = capture(wayland ? std::vector<std::string>{"wl-paste", "--list-types"}
                                 : std::vector<std::string>{"xclip", "-selection", "clipboard", "-t", "TARGETS", "-o"},
                         3000);
    if (!types.started) {
      message_ = wayland ? tr("cannot read the clipboard: wl-paste (wl-clipboard) is not installed")
                         : tr("cannot read the clipboard: xclip is not installed");
      return;
    }
    auto lines = core::split(types.out, '\n');
    static const std::pair<const char*, const char*> kTypes[] = {{"image/png", ".png"}, {"image/jpeg", ".jpg"}, {"image/gif", ".gif"}, {"image/bmp", ".bmp"}};
    for (const auto& [mime, ext] : kTypes) {
      if (std::none_of(lines.begin(), lines.end(), [&](const std::string& l) { return core::trim(l) == mime; })) continue;
      auto data = capture(wayland ? std::vector<std::string>{"wl-paste", "--no-newline", "--type", mime}
                                  : std::vector<std::string>{"xclip", "-selection", "clipboard", "-t", mime, "-o"},
                          5000);
      if (!data.ok || data.out.empty()) continue;
      std::time_t now = std::time(nullptr);
      std::tm tm{};
      localtime_r(&now, &tm);
      char stamp[32];
      std::strftime(stamp, sizeof stamp, "paste-%Y%m%d-%H%M%S", &tm);
      const fs::path note_dir = (root_ / note_file).parent_path();
      std::error_code ec;
      fs::create_directories(note_dir / "images", ec);
      auto target = free_name(note_dir / "images", stamp, ext);
      if (auto w = core::write_file(target, data.out); !w) {
        message_ = w.error().message;
        return;
      }
      message_ = tr("image pasted: ") + core::rel_str(root_, target);
      push_insert(note_file, md_link(target, fs::relative(target, note_dir, ec).generic_string()));
      return;
    }
    message_ = tr("no image in the clipboard");
  });
}

void Worker::open_link(const std::string& from_file, const std::string& href) {
  post([this, from_file, href] {
    std::string target;
    if (core::md::is_url(href)) {
      auto l = core::to_lower(href);
      if (!l.starts_with("http://") && !l.starts_with("https://") && !l.starts_with("mailto:") && !l.starts_with("www.")) {
        message_ = tr("not opened (only web and mail addresses): ") + href;
        return;
      }
      target = l.starts_with("www.") ? "https://" + href : href;
    } else {
      auto dest = core::md::percent_decode(href.substr(0, href.find_first_of("#?")));
      if (dest.empty()) return;
      fs::path p = dest.starts_with('/') ? root_ / dest.substr(1) : root_ / fs::path(from_file).parent_path() / dest;
      target = p.lexically_normal().string();
      std::error_code ec;
      if (!fs::exists(target, ec)) {
        message_ = tr("no such file: ") + target;
        return;
      }
    }
    message_ = spawn_detached({"xdg-open", target}) ? tr("opened: ") + target : tr("cannot open: ") + target;
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
    if (reindex || !model_) load_notes();  // notes projects: the texts as they are now
    auto m = std::make_shared<Model>();
    m->req = req;
    m->engine = engine->name();
    m->notes = notes_;
    m->note_texts = note_texts_;
    m->note_inserts = inserts_;
    if (notes_) {
      // The assistant's proposals; the links to suggest for the selected note, worked out again when the
      // files or the selection change.
      if (auto list = agents::NotesProposalStore(root_).list()) m->proposals = std::move(*list);
      if (req.change.starts_with("N-")) {
        for (const auto& p : m->proposals) {
          if (p.id == req.change) m->proposal_preview = agents::notes_proposal_preview(root_, p);
        }
      }
      if (reindex || req.selected != mentions_for_) {
        mentions_.clear();
        mentions_for_ = req.selected;
        if (!req.selected.empty()) {
          if (auto s = core::load_schema(root_)) {
            mentions_ = code::unlinked_mentions(*s, req.selected);
            auto of = code::mentions_of(*s, req.selected);
            mentions_.insert(mentions_.end(), of.begin(), of.end());
          }
        }
      }
      m->mentions = mentions_;
    }
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
    if (!req.open_file.empty()) {
      // A note not written yet has no file: its text is empty (ADR 0038).
      std::error_code ec;
      bool missing_note = notes_ && !fs::exists(root_ / req.open_file, ec);
      m->code_text = missing_note ? std::string() : core::read_file(root_ / req.open_file).value_or("(cannot read)");
    }
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
    m->ticket_edit_error = ticket_edit_error_;
    m->ticket_edit_saved = ticket_edit_saved_;
    m->run = run_info_;
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
