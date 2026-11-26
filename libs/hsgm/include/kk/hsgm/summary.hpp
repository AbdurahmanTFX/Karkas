#pragma once
// summary (ADR 0049): one line per HSGM measurement.
#include <string>

#include "kk/hsgm/input.hpp"

namespace kk::hsgm {

std::string summary(const Input& in);

}  // namespace kk::hsgm
