#pragma once
// Text templates embedded into Karkas at build time (templates/prompts, templates/target).
// Paths are relative to templates/, e.g. "prompts/worker.txt", "target/karkas_probe.hpp"; doctest.h of the
// build is "vendor/doctest/doctest.h" (docs/adr/0047).
#include <optional>
#include <string_view>
#include <vector>

namespace kk::templates {

std::optional<std::string_view> get(std::string_view path);
std::vector<std::string_view> list(std::string_view prefix);

}  // namespace kk::templates
