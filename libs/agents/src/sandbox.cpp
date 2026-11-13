#include "kk/agents/sandbox.hpp"

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

Sandbox::Sandbox(const core::Schema& schema, std::string block, Role role)
    : block_(std::move(block)), role_(role), project_(core::normalize(schema.root)) {
  const auto& dirs = schema.project.dirs;
  api_dir_ = dirs.api;
  const auto* b = schema.find(block_);
  bool group = b != nullptr && b->kind == core::BlockKind::Group;
  for (const auto& x : schema.blocks) known_.push_back(x.id);
  own_ = core::normalize(group ? schema.root / "tests" / "integration" / block_ : schema.block_dir(block_));
  read_roots_.push_back({"", own_});
  auto add_api = [&](const std::string& id) {
    const auto* x = schema.find(id);
    if (x == nullptr || !x->has_code()) return;
    read_roots_.push_back({id, core::normalize(schema.block_dir(id) / dirs.api)});
  };
  std::vector<std::string> neighbors = code::allowed_api_blocks(schema, block_);
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
      auto sup = code::allowed_api_blocks(schema, x.id);
      if (std::find(sup.begin(), sup.end(), block_) == sup.end()) continue;
      read_roots_.push_back({x.id, core::normalize(schema.block_dir(x.id) / dirs.api), true});
      users_.push_back(x.id);
    }
  }
  if (group) {
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

std::vector<std::string> Sandbox::neighbor_blocks() const {
  std::vector<std::string> out;
  for (const auto& r : read_roots_) {
    if (!r.label.empty() && !r.user) out.push_back(r.label);
  }
  return out;
}

kk::Result<fs::path> Sandbox::resolve(std::string_view path, bool write) const {
  std::string p(core::trim(path));
  if (p.empty()) p = ".";
  if (p.front() == '/' || p.front() == '~' || core::contains(p, "\\")) {
    return kk::fail("sandbox", fmt::format("'{}': absolute paths are not allowed; use paths relative to the block", p));
  }
  const Root* root = &read_roots_.front();
  auto colon = p.find(':');
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

bool Sandbox::is_own_api(const fs::path& abs) const { return core::is_within(core::normalize(own_ / api_dir_), abs); }

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
