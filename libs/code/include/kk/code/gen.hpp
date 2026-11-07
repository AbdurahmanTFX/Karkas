#pragma once
// Deterministic generation (8.3): CMakeLists.txt of the target project (5.5), mocks of
// neighbors, the probe header (12.5) and leaf skeletons.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/schema.hpp"

namespace kk::code {

inline constexpr std::string_view kGeneratedMarker = "karkas:generated";
inline constexpr std::string_view kSkeletonMarker = "karkas:skeleton";

// The CMakeLists.txt text for the schema (with its karkas-hash line).
std::string gen_cmake(const core::Schema& schema);
// Hash embedded in a generated CMakeLists.txt, and the hash of its body.
struct CmakeHashes {
  bool generated = false;
  std::string declared;
  std::string actual;
};
CmakeHashes cmake_hashes(std::string_view text);

struct GenResult {
  std::vector<std::string> written;    // files created or changed (relative paths)
  std::vector<std::string> unchanged;  // files that already had this content
  std::vector<std::string> skipped;    // hand-written files left alone
};

// Writes CMakeLists.txt (and cmake/user.cmake when missing).
kk::Result<GenResult> write_cmake(const core::Schema& schema);
// Mocks of the neighbors of `block` ("" or "all" = every leaf) into <leaf>/mocks/.
kk::Result<GenResult> write_mocks(const core::Schema& schema, std::string_view block);
// blocks/shared/probe/api/karkas_probe.hpp (and its card when the block is missing).
kk::Result<GenResult> write_probe(const core::Schema& schema);
// Empty impl/ and tests/ of new leaves (and main/compose of the entry).
kk::Result<GenResult> write_skel(const core::Schema& schema, std::string_view block);

// Text of a template from templates/target (embedded), with {{vars}} applied.
kk::Result<std::string> target_template(std::string_view name,
                                        const std::vector<std::pair<std::string, std::string>>& vars = {});

}  // namespace kk::code
