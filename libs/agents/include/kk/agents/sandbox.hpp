#pragma once
// Sandbox of a leaf agent (9.5): every path is normalized and checked against the
// allowed roots; "..", absolute paths and symlinks pointing outside are refused.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "kk/agents/types.hpp"
#include "kk/core/schema.hpp"

namespace kk::agents {

class Sandbox {
 public:
  // role Worker: read own block + api/ of linked and shared blocks; write impl/, tests/unit/.
  // role Tester: read own block except impl/ + the same neighbor api/; write tests/contract/, mocks/.
  // Groups (integration tester): read api/ of every block under the group; write tests/integration/<group>/.
  Sandbox(const core::Schema& schema, std::string block, Role role);

  // Paths: "impl/ring.cpp" (own block), "<block id>:api/x.hpp" (neighbor api).
  kk::Result<std::filesystem::path> resolve_read(std::string_view path) const;
  kk::Result<std::filesystem::path> resolve_write(std::string_view path) const;
  // Display form of an absolute path inside the sandbox ("impl/a.cpp" or "dsp.gain:api/gain.hpp").
  [[nodiscard]] std::string display(const std::filesystem::path& abs) const;
  [[nodiscard]] const std::filesystem::path& own_root() const { return own_; }
  [[nodiscard]] std::vector<std::string> neighbor_blocks() const;
  [[nodiscard]] const std::string& block() const { return block_; }

 private:
  struct Root {
    std::string label;  // "" for the own block, block id for neighbors
    std::filesystem::path path;
  };
  kk::Result<std::filesystem::path> resolve(std::string_view path, bool write) const;
  std::string block_;
  Role role_;
  std::filesystem::path project_;
  std::filesystem::path own_;
  std::vector<Root> read_roots_;
  std::vector<std::filesystem::path> write_roots_;
  std::vector<std::filesystem::path> deny_read_;  // impl/ for the tester
  std::string api_dir_;
};

}  // namespace kk::agents
