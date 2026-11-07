#pragma once
// Proposed schema changes .karkas/changes/C-NNNN.ksl (10.7) and their application.
#include <filesystem>
#include <string>
#include <vector>

#include "kk/core/model.hpp"
#include "kk/core/schema.hpp"

namespace kk::agents {

class ChangeStore {
 public:
  explicit ChangeStore(std::filesystem::path root) : root_(std::move(root)) {}
  kk::Result<core::Change> create(std::string from, std::string reason, std::vector<core::DiffLine> diff) const;
  kk::Result<core::Change> load(std::string_view id) const;
  kk::Result<std::vector<core::Change>> list() const;
  kk::VoidResult set_status(std::string_view id, std::string_view status) const;
  [[nodiscard]] std::filesystem::path file(std::string_view id) const;

 private:
  std::filesystem::path root_;
};

// Parses "+ ..."/"- ..."/"~ ..." lines (as written by agents) into diff lines.
kk::Result<std::vector<core::DiffLine>> parse_diff(std::string_view text);

// Applies a diff to the cards on disk: "+ @block X" creates a card from the following
// "+" lines, "~ @block X" selects a card, then "+ line" appends and "- line" removes
// a matching line. Writes canonical cards and returns the files written. Nothing is
// written when the result does not parse or breaks the schema checks (E codes).
struct ApplyResult {
  std::vector<std::string> files;
  std::vector<core::Issue> issues;
};
kk::Result<ApplyResult> apply_diff(const std::filesystem::path& root, const std::vector<core::DiffLine>& diff,
                                   const std::vector<std::string>& tolerate = {});

// A unified-diff-like text of the card changes, for showing before confirmation (14.6).
std::string preview_diff(const std::filesystem::path& root, const std::vector<core::DiffLine>& diff);

}  // namespace kk::agents
