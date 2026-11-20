#pragma once
// Sandbox of a leaf agent (9.5): every path is normalized and checked against the
// allowed roots; "..", absolute paths and symlinks pointing outside are refused.
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "kk/agents/types.hpp"
#include "kk/core/schema.hpp"

namespace kk::agents {

// Access of a worker to the api/ of its own block (ADR 0020).
enum class ApiAccess {
  None,        // read-only (the tester; the worker outside tickets)
  ValuesOnly,  // frozen contract: declarations stay, values of constants may change
  Full,        // unfrozen by an approved change: the worker brings it in line with the card
  Imported,    // headers of an imported block: the public part stays, the rest is free (ADR 0027)
};

// The contract shape of a header: comments dropped, initializers of constexpr constants
// replaced by a placeholder, whitespace collapsed. Two headers with the same shape declare
// the same contract; only values differ.
std::string api_shape(std::string_view header);
// True when `after` keeps every declaration of `before` and adds at most new constexpr
// constants (plus changed values): nothing a user of the header can break on.
bool api_values_or_new_constants(std::string_view before, std::string_view after);
// The header of an imported block after an edit keeps every declaration other code can see (outside
// private sections; bodies, initial values and comments do not count) and adds at most public
// constants and #include lines (ADR 0027). `why` names the first declaration that breaks it.
bool header_contract_kept(std::string_view before, std::string_view after, std::string* why = nullptr);

class Sandbox {
 public:
  // role Worker: read own block + api/ of linked and shared blocks; write impl/, tests/unit/.
  // role Tester: read own block except impl/ + the same neighbor api/; write tests/contract/, mocks/.
  // Groups (integration tester): read api/ of every block under the group; write tests/integration/<group>/.
  // An imported block (`files`, ADR 0027): paths relative to the project root; read its files, the
  // headers of linked blocks and unowned files that are not code; write only the files of its card.
  Sandbox(const core::Schema& schema, std::string block, Role role);

  // Paths: "impl/ring.cpp" (own block), "<block id>:api/x.hpp" (neighbor api); an imported block:
  // "source/Engine.cpp" (relative to the project root).
  kk::Result<std::filesystem::path> resolve_read(std::string_view path) const;
  kk::Result<std::filesystem::path> resolve_write(std::string_view path) const;
  // Display form of an absolute path inside the sandbox ("impl/a.cpp" or "dsp.gain:api/gain.hpp").
  [[nodiscard]] std::string display(const std::filesystem::path& abs) const;
  [[nodiscard]] const std::filesystem::path& own_root() const { return own_; }
  [[nodiscard]] std::vector<std::string> neighbor_blocks() const;  // blocks whose api/ this one uses
  // Blocks that use this block's api/ (a worker reads their api/ to see how it is called).
  [[nodiscard]] const std::vector<std::string>& user_blocks() const { return users_; }
  [[nodiscard]] const std::string& block() const { return block_; }
  [[nodiscard]] const std::filesystem::path& project() const { return project_; }
  void set_api_access(ApiAccess a) { api_access_ = a; }
  [[nodiscard]] ApiAccess api_access() const { return api_access_; }
  [[nodiscard]] bool is_own_api(const std::filesystem::path& abs) const;
  // An imported block: the files of its card, relative to the project root.
  [[nodiscard]] bool imported() const { return imported_; }
  [[nodiscard]] const std::vector<std::string>& own_files() const { return own_files_; }
  // Files a recursive grep reads under `dir`. An imported block searching from the project root reads
  // its files and the headers it may read, not the whole tree; empty: walk the folder.
  [[nodiscard]] std::vector<std::filesystem::path> search_files(const std::filesystem::path& dir) const;
  // An imported project's tester writes its behaviour test into the project's own test folder
  // ([test] dir, ADR 0029): files under `dir` (absolute) become readable and writable.
  void allow_write_dir(const std::filesystem::path& dir);

 private:
  struct Root {
    std::string label;  // "" for the own block, block id for neighbors
    std::filesystem::path path;
    bool user = false;  // a block that uses this one (readable, not part of the context)
  };
  kk::Result<std::filesystem::path> resolve(std::string_view path, bool write) const;
  kk::Result<std::filesystem::path> resolve_imported(const std::string& path, bool write) const;
  [[nodiscard]] bool internal(const std::filesystem::path& abs) const;
  std::string block_;
  Role role_;
  std::filesystem::path project_;
  std::filesystem::path own_;
  std::vector<Root> read_roots_;
  std::vector<std::filesystem::path> write_roots_;
  std::vector<std::filesystem::path> deny_read_;  // impl/ for the tester
  std::string api_dir_;
  ApiAccess api_access_ = ApiAccess::None;
  std::vector<std::string> users_;
  std::vector<std::string> neighbors_;  // blocks whose api/ (or headers) this one reads
  std::vector<std::string> known_;      // every block id of the schema, for the messages
  // An imported block (ADR 0027): absolute paths of its files and of the headers of other blocks it
  // may read; the owner of every file a card lists; folders no agent reads (.karkas, build, blocks).
  bool imported_ = false;
  std::vector<std::string> own_files_;
  std::set<std::filesystem::path> own_listed_;
  std::set<std::filesystem::path> read_listed_;
  std::map<std::filesystem::path, std::string> listed_owner_;
  std::vector<std::filesystem::path> internal_dirs_;
  std::vector<std::filesystem::path> extra_write_;  // allow_write_dir
};

}  // namespace kk::agents
