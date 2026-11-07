#pragma once
// Small string utilities shared by all modules.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kk::core {

std::string_view trim(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep, bool skip_empty = true);
std::vector<std::string> split_ws(std::string_view s);
std::string join(const std::vector<std::string>& parts, std::string_view sep);
std::string pad_right(std::string_view s, std::size_t width);  // at least one space after s
std::string replace_all(std::string s, std::string_view from, std::string_view to);
bool contains(std::string_view s, std::string_view needle);
std::string to_lower(std::string_view s);

// Identifier rules from KSL 4.2.
bool is_ident(std::string_view s);       // [a-z][a-z0-9_]*
bool is_block_path(std::string_view s);  // ident(.ident)*
bool is_record_id(std::string_view s);   // T-0042 | C-0003

// Quote a value for KSL when needed ("..." with \" and \\ escapes).
std::string quote(std::string_view s);
bool needs_quotes(std::string_view s);

// Cheap local token estimate (only for budgets before sending; real numbers
// come from API usage). Intentionally errs on the high side.
std::size_t estimate_tokens(std::string_view text);

// FNV-1a 64-bit (for in-memory keys; file hashes use xxh3 in kk_index).
std::uint64_t fnv1a64(std::string_view s);
std::string hex64(std::uint64_t v);

// Current UNIX time in seconds.
std::int64_t unix_now();

}  // namespace kk::core
