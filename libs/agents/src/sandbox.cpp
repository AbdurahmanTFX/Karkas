#include "kk/agents/sandbox.hpp"

#include <fmt/format.h>

#include <algorithm>

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
    if (!r.label.empty()) out.push_back(r.label);
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
      return kk::fail("sandbox", fmt::format("'{}': block {} is not linked to {}; only linked api/ is readable", p, label, block_));
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
    bool ok = std::any_of(write_roots_.begin(), write_roots_.end(), [&](const fs::path& w) { return core::is_within(w, abs); });
    if (!ok) {
      auto api = core::normalize(own_ / api_dir_);
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
