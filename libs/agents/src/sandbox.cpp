#include "kk/agents/sandbox.hpp"
#include "kk/core/lang.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <regex>
#include <set>

#include "kk/code/analyzer.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::agents {

namespace fs = std::filesystem;

std::string_view to_string(Role r) {
  switch (r) {
    case Role::Architect: return "architect";
    case Role::Orchestrator: return "orchestrator";
    case Role::Navigator: return "navigator";
    case Role::Worker: return "worker";
    case Role::Tester: return "tester";
  }
  return "?";
}

namespace {

// What other blocks read of an imported block: its headers, or its files in a language without headers,
// whose public part is the contract (ADR 0051).
bool is_header(const fs::path& p) { return core::is_contract_file(p); }
// A file that only declares (a C/C++ header, .pyi, .d.ts): read-only when the run may not touch the api.
bool declares_only(const fs::path& p) { return core::is_interface_file(p); }

}  // namespace

Sandbox::Sandbox(const core::Schema& schema, std::string block, Role role)
    : block_(std::move(block)), role_(role), project_(core::normalize(schema.root)) {
  const auto& dirs = schema.project.dirs;
  api_dir_ = dirs.api;
  const auto* b = schema.find(block_);
  bool group = b != nullptr && b->kind == core::BlockKind::Group;
  for (const auto& x : schema.blocks) known_.push_back(x.id);
  own_ = core::normalize(group ? schema.root / "tests" / "integration" / block_ : schema.block_dir(block_));
  // An imported block works on the files of its card where they are (ADR 0027): paths from the
  // project root, which is its first root (so display() shows them as the card and the compiler do).
  imported_ = b != nullptr && !group && b->external();
  read_roots_.push_back({"", imported_ ? project_ : own_});
  if (imported_) {
    for (const auto& x : schema.blocks) {
      for (const auto& f : x.file_list()) listed_owner_[core::normalize(project_ / f)] = x.id;
    }
    for (const auto& f : b->file_list()) {
      auto abs = core::normalize(project_ / f);
      // The tester checks promises: of an imported block it reads the headers only.
      if (role == Role::Tester && !is_header(abs)) continue;
      own_files_.push_back(f);
      own_listed_.insert(abs);
    }
    internal_dirs_ = {core::normalize(project_ / ".karkas"), core::normalize(project_ / dirs.blocks), core::normalize(project_ / "build")};
  }
  // The headers an imported block reads of another imported block.
  auto add_headers = [&](const core::Block& x) {
    if (!imported_ || !x.external()) return;
    for (const auto& f : x.file_list()) {
      if (is_header(f)) read_listed_.insert(core::normalize(project_ / f));
    }
  };
  auto add_api = [&](const std::string& id) {
    const auto* x = schema.find(id);
    if (x == nullptr || !x->has_code()) return;
    read_roots_.push_back({id, core::normalize(schema.block_dir(id) / dirs.api)});
    neighbors_.push_back(id);
    add_headers(*x);
  };
  const auto links = schema.links();  // once: the users' loop below asks for every block of the schema
  std::vector<std::string> neighbors = code::allowed_api_blocks(schema, block_, links);
  for (const auto& x : schema.blocks) {
    if (x.kind == core::BlockKind::Shared) neighbors.push_back(x.id);
  }
  std::sort(neighbors.begin(), neighbors.end());
  neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
  for (const auto& n : neighbors) {
    if (n != block_) add_api(n);
  }
  // A worker also reads the api/ of the blocks that use its own: when its contract changes it
  // must see how the users call it (read-only; not put into its context).
  if (role == Role::Worker && !group) {
    for (const auto& x : schema.blocks) {
      if (x.id == block_ || !x.has_code()) continue;
      if (std::find(neighbors.begin(), neighbors.end(), x.id) != neighbors.end()) continue;
      auto sup = code::allowed_api_blocks(schema, x.id, links);
      if (std::find(sup.begin(), sup.end(), block_) == sup.end()) continue;
      read_roots_.push_back({x.id, core::normalize(schema.block_dir(x.id) / dirs.api), true});
      users_.push_back(x.id);
      add_headers(x);
    }
  }
  if (imported_) {
    // Writing: the files of the card only (resolve_imported); no folder of its own.
  } else if (group) {
    write_roots_.push_back(own_);
  } else if (role == Role::Worker) {
    write_roots_.push_back(core::normalize(own_ / dirs.impl));
    write_roots_.push_back(core::normalize(own_ / dirs.tests / "unit"));
  } else if (role == Role::Tester) {
    write_roots_.push_back(core::normalize(own_ / dirs.tests / "contract"));
    write_roots_.push_back(core::normalize(own_ / dirs.mocks));
    deny_read_.push_back(core::normalize(own_ / dirs.impl));
  }
  if (role == Role::Tester) {
    // The tester never reads implementations, not even the neighbors' (12.1).
    for (const auto& n : neighbors) {
      const auto* x = schema.find(n);
      if (x != nullptr) deny_read_.push_back(core::normalize(schema.block_dir(n) / dirs.impl));
    }
  }
}

std::vector<std::string> Sandbox::neighbor_blocks() const { return neighbors_; }

// Karkas's own files, the build folder, other blocks' folders and hidden paths: never read.
bool Sandbox::internal(const fs::path& abs) const {
  for (const auto& d : internal_dirs_) {
    if (core::is_within(d, abs)) return true;
  }
  for (const auto& part : abs.lexically_relative(project_)) {
    auto s = part.string();
    if (s.size() > 1 && s.front() == '.' && s != "..") return true;
  }
  return false;
}

void Sandbox::allow_write_dir(const fs::path& dir) { extra_write_.push_back(core::normalize(dir)); }

kk::Result<fs::path> Sandbox::resolve_imported(const std::string& p, bool write) const {
  for (const auto& part : fs::path(p)) {
    if (part == "..") return kk::fail("sandbox", fmt::format("'{}': '..' is not allowed", p));
  }
  auto abs = core::normalize(project_ / p);  // resolves symlinks of existing parts
  if (!core::is_within(project_, abs)) return kk::fail("sandbox", fmt::format("'{}' resolves outside the sandbox", p));
  // The project's own test folder of an imported project's tester (ADR 0029): read and written.
  for (const auto& d : extra_write_) {
    if (!core::is_within(d, abs)) continue;
    std::error_code ec;
    if (write && fs::is_symlink(project_ / p, ec)) return kk::fail("sandbox", fmt::format("'{}' is a symlink", p));
    return abs;
  }
  const bool own = own_listed_.contains(abs);
  auto owner = listed_owner_.find(abs);
  if (write) {
    if (role_ != Role::Worker || !own) {
      std::string whose = owner != listed_owner_.end() && owner->second != block_ ? fmt::format(" ({} belongs to block {})", p, owner->second)
                                                                                  : std::string();
      return kk::fail("sandbox", fmt::format("'{}': you may change only the files of your card: {}{}. A new file, or a file of no "
                                             "block (data, configuration), comes with a card change: report "
                                             "needs_contract_change with '~ @block {}' and '+ files {}'",
                                             p, core::join(own_files_, " "), whose, block_, p));
    }
    if (declares_only(abs) && api_access_ == ApiAccess::None) {
      return kk::fail("sandbox", fmt::format("'{}': the headers of {} are read-only in this run", p, block_));
    }
    std::error_code ec;
    if (fs::is_symlink(project_ / p, ec)) return kk::fail("sandbox", fmt::format("'{}' is a symlink", p));
    return abs;
  }
  if (own || read_listed_.contains(abs) || core::is_within(own_, abs)) return abs;
  // A native neighbor's api/ (a mixed project) under its path as well as under its label.
  for (const auto& r : read_roots_) {
    if (!r.label.empty() && core::is_within(r.path, abs)) return abs;
  }
  if (abs == project_) return abs;
  if (internal(abs)) {
    return kk::fail("sandbox", fmt::format("'{}' is not readable: Karkas's own files, the build folder, the cards of other blocks "
                                           "and hidden paths are outside the sandbox",
                                           p));
  }
  if (owner != listed_owner_.end()) {
    return kk::fail("sandbox", fmt::format("'{}' belongs to block {}, which is not linked to {}: of other blocks only the headers "
                                           "of the linked ones are readable",
                                           p, owner->second, block_));
  }
  std::error_code ec;
  if (fs::is_directory(abs, ec)) return abs;  // a folder: list_dir shows what in it is readable
  if (core::is_code_file(abs)) {
    return kk::fail("sandbox", fmt::format("'{}' is code of no block you may read; readable are your files ({}) and the headers "
                                           "of the blocks linked to {}",
                                           p, core::join(own_files_, " "), block_));
  }
  return abs;  // a project file that is not code and that no card lists (data, configuration)
}

std::vector<fs::path> Sandbox::search_files(const fs::path& dir) const {
  std::vector<fs::path> out;
  if (!imported_ || dir != project_) return out;
  for (const auto* set : {&own_listed_, &read_listed_}) {
    for (const auto& f : *set) {
      std::error_code ec;
      if (fs::is_regular_file(f, ec)) out.push_back(f);
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

kk::Result<fs::path> Sandbox::resolve(std::string_view path, bool write) const {
  std::string p(core::trim(path));
  if (p.empty()) p = ".";
  if (p.front() == '/' || p.front() == '~' || core::contains(p, "\\")) {
    return kk::fail("sandbox", fmt::format("'{}': absolute paths are not allowed; use paths relative to the {}", p,
                                           imported_ ? "project root" : "block"));
  }
  const Root* root = &read_roots_.front();
  auto colon = p.find(':');
  if (imported_ && colon == std::string::npos) return resolve_imported(p, write);
  if (colon != std::string::npos) {
    std::string label = p.substr(0, colon);
    std::string rest = p.substr(colon + 1);
    auto it = std::find_if(read_roots_.begin(), read_roots_.end(), [&](const Root& r) { return r.label == label; });
    if (it == read_roots_.end()) {
      // Name what is readable: an agent that only hears "no" guesses block names (DeepSeek and
      // OpenCode runs tried "game:", "core:", "common:" for a header of shared.types).
      std::vector<std::string> readable, under;
      for (const auto& r : read_roots_) {
        if (r.label.empty()) continue;
        readable.push_back(r.label);
        if (r.label.starts_with(label + ".")) under.push_back(r.label);
      }
      std::string what;
      if (std::find(known_.begin(), known_.end(), label) == known_.end()) {
        what = fmt::format("there is no block {}", label);
      } else if (!under.empty()) {
        what = fmt::format("{} is a group, which has no api/; its blocks readable here: {}", label, core::join(under, ", "));
      } else {
        what = fmt::format("block {} is not linked to {}", label, block_);
      }
      constexpr std::size_t kMaxNames = 40;
      std::string names = core::join(std::vector<std::string>(readable.begin(), readable.begin() + static_cast<std::ptrdiff_t>(
                                                                                   std::min(readable.size(), kMaxNames))),
                                     ", ");
      if (readable.size() > kMaxNames) names += fmt::format(" and {} more", readable.size() - kMaxNames);
      return kk::fail("sandbox", fmt::format("'{}': {}; readable are your own folder and the api/ of: {} (as '<block>:{}/<file>')", p,
                                             what, readable.empty() ? std::string("none") : names, api_dir_));
    }
    if (!(rest == api_dir_ || rest.starts_with(api_dir_ + "/"))) {
      return kk::fail("sandbox", fmt::format("'{}': only api/ of other blocks is readable", p));
    }
    root = &*it;
    p = rest == api_dir_ ? "." : rest.substr(api_dir_.size() + 1);
  }
  for (const auto& part : fs::path(p)) {
    if (part == "..") return kk::fail("sandbox", fmt::format("'{}': '..' is not allowed", path));
  }
  auto abs = core::normalize(root->path / p);  // resolves symlinks of existing parts
  if (!core::is_within(root->path, abs)) {
    return kk::fail("sandbox", fmt::format("'{}' resolves outside the sandbox", path));
  }
  if (write) {
    if (!root->label.empty()) return kk::fail("sandbox", fmt::format("'{}': other blocks are read-only", path));
    auto api = core::normalize(own_ / api_dir_);
    bool api_ok = role_ == Role::Worker && api_access_ != ApiAccess::None && core::is_within(api, abs);
    bool ok = api_ok || std::any_of(write_roots_.begin(), write_roots_.end(), [&](const fs::path& w) { return core::is_within(w, abs); });
    if (!ok) {
      if (core::is_within(api, abs)) {
        return kk::fail("sandbox", fmt::format("'{}': api/ is the frozen contract and cannot be written; if the fix needs a "
                                               "contract change, report status=needs_contract_change with a KSL proposal",
                                               path));
      }
      std::vector<std::string> allowed;
      for (const auto& w : write_roots_) allowed.push_back(core::rel_str(own_, w) + "/");
      return kk::fail("sandbox", fmt::format("'{}': writing is allowed only in {}", path, core::join(allowed, ", ")));
    }
    // Refuse writing through a symlink (even one pointing inside).
    std::error_code ec;
    if (fs::is_symlink(root->path / p, ec)) return kk::fail("sandbox", fmt::format("'{}' is a symlink", path));
  } else {
    for (const auto& d : deny_read_) {
      if (core::is_within(d, abs)) {
        return kk::fail("sandbox", fmt::format("'{}': the tester checks promises, not code: impl/ is not readable", path));
      }
    }
  }
  return abs;
}

bool Sandbox::is_own_api(const fs::path& abs) const {
  if (imported_) return own_listed_.contains(abs) && is_header(abs);
  return core::is_within(core::normalize(own_ / api_dir_), abs);
}

std::string api_shape(std::string_view text) {
  std::string s;
  s.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    if (text.compare(i, 2, "//") == 0) {
      while (i < text.size() && text[i] != '\n') ++i;
      continue;
    }
    if (text.compare(i, 2, "/*") == 0) {
      auto e = text.find("*/", i + 2);
      i = e == std::string_view::npos ? text.size() : e + 2;
      continue;
    }
    s += text[i++];
  }
  // `constexpr T name = value;` and `constexpr T name{a, b};` → `constexpr T name = _;`.
  // A constexpr function keeps its body (a ';' inside braces does not match).
  static const std::regex init(R"((\bconstexpr\b[^;{=]*?)\s*(=[^;]*|\{[^;{}]*\})\s*;)");
  s = std::regex_replace(s, init, "$1 = _;");
  std::string out;
  bool space = false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

bool api_values_or_new_constants(std::string_view before, std::string_view after) {
  auto a = api_shape(before);
  auto b = api_shape(after);
  if (a == b) return true;
  // Declarations as pieces between ';', '{' and '}': every old piece stays, a new one may only
  // be a constexpr constant (its value is a placeholder in the shape, so no parentheses).
  auto pieces = [](const std::string& text) {
    std::multiset<std::string> out;
    std::string cur;
    for (char c : text) {
      if (c == ';' || c == '{' || c == '}') {
        auto t = std::string(core::trim(cur));
        if (!t.empty()) out.insert(t);
        cur.clear();
      } else {
        cur += c;
      }
    }
    auto t = std::string(core::trim(cur));
    if (!t.empty()) out.insert(t);
    return out;
  };
  auto old_p = pieces(a);
  auto new_p = pieces(b);
  static const std::regex constant(R"(^(?:(?:inline|static) )*constexpr [^()]+ = _$)");
  for (const auto& p : old_p) {
    auto it = new_p.find(p);
    if (it == new_p.end()) return false;  // a declaration was removed or changed
    new_p.erase(it);
  }
  return std::all_of(new_p.begin(), new_p.end(), [](const std::string& p) { return std::regex_match(p, constant); });
}

kk::Result<fs::path> Sandbox::resolve_read(std::string_view path) const { return resolve(path, false); }
kk::Result<fs::path> Sandbox::resolve_write(std::string_view path) const { return resolve(path, true); }

std::string Sandbox::display(const fs::path& abs) const {
  for (const auto& r : read_roots_) {
    if (!core::is_within(r.path, abs)) continue;
    auto rel = core::rel_str(r.path, abs);
    if (r.label.empty()) return rel;
    return r.label + ":" + api_dir_ + (rel == "." ? "" : "/" + rel);
  }
  return abs.string();
}

}  // namespace kk::agents
