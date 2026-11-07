#pragma once
// Schema checks E02–E08 and W01–W06 (4.6). E01 is reported while loading.
#include <vector>

#include "kk/core/schema.hpp"

namespace kk::core {

std::vector<Issue> validate(const Schema& schema);

// load issues + validate(), sorted by file, line, code.
std::vector<Issue> check_schema(const Schema& schema);

}  // namespace kk::core
