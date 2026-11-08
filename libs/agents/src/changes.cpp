#include "kk/agents/changes.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <map>
#include <set>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"

namespace kk::agents {

namespace fs = std::filesystem;

fs::path ChangeStore::file(std::string_view id) const { return root_ / ".karkas" / "changes" / (std::string(id) + ".ksl"); }

kk::Result<core::Change> ChangeStore::create(std::string from, std::string reason, std::vector<core::DiffLine> diff) const {
  auto dir = root_ / ".karkas" / "changes";
  std::error_code ec;
  fs::create_directories(dir, ec);
  int max = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    auto stem = e.path().stem().string();
    if (stem.size() > 2 && stem[0] == 'C' && stem[1] == '-') {
      int n = 0;
      std::from_chars(stem.data() + 2, stem.data() + stem.size(), n);
      max = std::max(max, n);
    }
  }
  core::Change c;
  c.id = fmt::format("C-{:04d}", max + 1);
  c.from = {std::move(from), {}, 0, 0, true};
  c.reason = {std::move(reason), {}, 0, 0, true};
  c.status = {"pending", {}, 0, 0, true};
  c.diff = std::move(diff);
  if (auto r = core::write_file(file(c.id), core::write_change(c)); !r) return std::unexpected(r.error());
  return c;
}

kk::Result<core::Change> ChangeStore::load(std::string_view id) const {
  auto p = file(id);
  if (!fs::exists(p)) return kk::fail("changes", fmt::format("no change {}", id));
  auto text = core::read_file(p);
  if (!text) return std::unexpected(text.error());
  return core::parse_change_text(*text, core::rel_str(root_, p));
}

kk::Result<std::vector<core::Change>> ChangeStore::list() const {
  std::vector<core::Change> out;
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(root_ / ".karkas" / "changes", ec)) {
    if (e.path().extension() == ".ksl") files.push_back(e.path());
  }
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    auto c = load(f.stem().string());
    if (!c) return std::unexpected(c.error());
    out.push_back(std::move(*c));
  }
  return out;
}

kk::VoidResult ChangeStore::set_status(std::string_view id, std::string_view status) const {
  auto c = load(id);
  if (!c) return std::unexpected(c.error());
  c->status.value = std::string(status);
  return core::write_file(file(id), core::write_change(*c));
}

kk::Result<std::vector<core::DiffLine>> parse_diff(std::string_view text) {
  std::vector<core::DiffLine> out;
  int n = 0;
  for (const auto& raw : core::split(text, '\n', false)) {
    ++n;
    auto line = core::trim(raw);
    if (line.empty()) continue;
    char op = line[0];
    if ((op != '+' && op != '-' && op != '~') || (line.size() > 1 && line[1] != ' ' && line[1] != '\t')) {
      return kk::fail("diff", fmt::format("diff line {} must start with '+ ', '- ' or '~ ': {}", n, line));
    }
    out.push_back(core::DiffLine{op, std::string(core::trim(line.substr(1))), n});
  }
  return out;
}

namespace {

struct Edit {
  bool is_new = false;
  bool remove_block = false;
  std::vector<std::string> add;
  std::vector<std::string> remove;
};

std::vector<std::string> card_lines(const core::Block& b) {
  auto text = core::write_block(b);
  auto lines = core::split(text, '\n', false);
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  return lines;
}

// Canonical form of one card line ("call  a:b" → "call    a:b").
kk::Result<std::string> canonical_line(const std::string& id, const std::string& line) {
  auto b = core::parse_block_text("@block " + id + "\n" + line + "\n", "diff");
  if (!b) return std::unexpected(b.error());
  auto lines = card_lines(*b);
  if (lines.size() != 2) return kk::fail("diff", "expected one card line: " + line);
  return lines[1];
}

kk::Result<std::map<std::string, std::string>> compute(const fs::path& root, const std::vector<core::DiffLine>& diff,
                                                       core::Schema& schema) {
  std::map<std::string, Edit> edits;
  std::vector<std::string> order;
  std::string current;
  for (const auto& d : diff) {
    std::string_view t = d.text;
    bool header = t.starts_with("@block ");
    std::string id = header ? std::string(core::trim(t.substr(7))) : "";
    if (d.op == '~') {
      if (!header) return kk::fail("diff", fmt::format("line {}: '~' selects a record: '~ @block <id>'", d.line));
      if (schema.find(id) == nullptr) return kk::fail("diff", fmt::format("line {}: no block {}", d.line, id));
      current = id;
      if (!edits.contains(id)) order.push_back(id);
      edits[id];
      continue;
    }
    if (header) {
      if (!core::is_block_path(id)) return kk::fail("diff", fmt::format("line {}: bad block id '{}'", d.line, id));
      if (d.op == '+') {
        if (schema.find(id) != nullptr) return kk::fail("diff", fmt::format("line {}: block {} already exists", d.line, id));
        edits[id].is_new = true;
      } else {
        if (schema.find(id) == nullptr) return kk::fail("diff", fmt::format("line {}: no block {}", d.line, id));
        edits[id].remove_block = true;
      }
      if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
      current = id;
      continue;
    }
    if (current.empty()) return kk::fail("diff", fmt::format("line {}: select a block first ('~ @block <id>')", d.line));
    (d.op == '+' ? edits[current].add : edits[current].remove).push_back(d.text);
  }
  std::map<std::string, std::string> out;  // id → new card text ("" = remove)
  for (const auto& id : order) {
    const auto& e = edits[id];
    if (e.remove_block) {
      out[id] = "";
      continue;
    }
    std::vector<std::string> lines;
    if (e.is_new) {
      lines.push_back("@block " + id);
    } else {
      lines = card_lines(*schema.find(id));
    }
    for (const auto& r : e.remove) {
      auto c = canonical_line(id, r);
      if (!c) return std::unexpected(c.error());
      auto it = std::find(lines.begin() + 1, lines.end(), *c);
      if (it == lines.end()) return kk::fail("diff", fmt::format("{}: no line '{}' to remove", id, r));
      lines.erase(it);
    }
    for (const auto& a : e.add) lines.push_back(a);
    auto text = core::join(lines, "\n") + "\n";
    auto parsed = core::parse_block_text(text, id + "/block.ksl");
    if (!parsed) return kk::fail("diff", fmt::format("{}: {}", id, parsed.error().message));
    out[id] = core::write_block(*parsed);
  }
  (void)root;
  return out;
}

}  // namespace

namespace {

kk::Result<ApplyResult> apply_or_check(const fs::path& root, const std::vector<core::DiffLine>& diff,
                                       const std::vector<std::string>& tolerate, bool write) {
  auto schema = core::load_schema(root);
  if (!schema) return std::unexpected(schema.error());
  auto cards = compute(root, diff, *schema);
  if (!cards) return std::unexpected(cards.error());
  // Check the resulting schema in memory before writing anything.
  core::Schema next = *schema;
  for (const auto& [id, text] : *cards) {
    std::erase_if(next.blocks, [&](const core::Block& b) { return b.id == id; });
    if (text.empty()) continue;
    auto b = core::parse_block_text(text, id + "/block.ksl");
    if (!b) return std::unexpected(b.error());
    b->dir = core::rel_str(root, next.block_dir(id));
    b->file = b->dir + "/block.ksl";
    b->parent = core::parent_id(id);
    b->depth = core::id_depth(id);
    next.blocks.push_back(std::move(*b));
  }
  std::sort(next.blocks.begin(), next.blocks.end(), [](const core::Block& a, const core::Block& b) { return a.id < b.id; });
  for (auto& b : next.blocks) b.children.clear();
  for (auto& b : next.blocks) {
    if (b.parent.empty()) continue;
    if (auto* p = next.find_mut(b.parent)) p->children.push_back(b.id);
  }
  ApplyResult out;
  for (const auto& i : core::validate(next)) {
    bool touched = cards->contains(i.block) || i.block.empty();
    bool tolerated = std::find(tolerate.begin(), tolerate.end(), i.code) != tolerate.end();
    if (i.severity == 'E' && touched && !tolerated) out.issues.push_back(i);
  }
  for (const auto& [id, text] : *cards) {
    if (!text.empty() && core::parent_id(id) != "" && next.find(core::parent_id(id)) == nullptr) {
      out.issues.push_back(core::Issue{"E01", 'E', id, "parent block " + core::parent_id(id) + " does not exist", {}});
    }
  }
  if (!out.issues.empty()) return out;
  for (const auto& [id, text] : *cards) {
    auto path = next.block_dir(id) / "block.ksl";
    if (!write) {
      out.files.push_back(core::rel_str(root, path));
      continue;
    }
    if (text.empty()) {
      std::error_code ec;
      fs::remove(path, ec);
    } else if (auto r = core::write_file(path, text); !r) {
      return std::unexpected(r.error());
    }
    out.files.push_back(core::rel_str(root, path));
  }
  return out;
}

}  // namespace

kk::Result<ApplyResult> apply_diff(const fs::path& root, const std::vector<core::DiffLine>& diff,
                                   const std::vector<std::string>& tolerate) {
  return apply_or_check(root, diff, tolerate, true);
}

kk::Result<ApplyResult> check_diff(const fs::path& root, const std::vector<core::DiffLine>& diff) {
  return apply_or_check(root, diff, {}, false);
}

std::string preview_diff(const fs::path& root, const std::vector<core::DiffLine>& diff) {
  auto schema = core::load_schema(root);
  if (!schema) return schema.error().str() + "\n";
  auto cards = compute(root, diff, *schema);
  if (!cards) return cards.error().message + "\n";
  std::string out;
  for (const auto& [id, text] : *cards) {
    std::vector<std::string> before;
    if (const auto* b = schema->find(id)) before = card_lines(*b);
    auto after = core::split(text, '\n', false);
    if (!after.empty() && after.back().empty()) after.pop_back();
    auto rel = core::rel_str(root, schema->block_dir(id) / "block.ksl");
    out += fmt::format("--- {}\n+++ {}\n", before.empty() ? "/dev/null" : "a/" + rel, text.empty() ? "/dev/null" : "b/" + rel);
    // LCS line diff (cards are small).
    std::size_t n = before.size();
    std::size_t m = after.size();
    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (std::size_t i = n; i-- > 0;) {
      for (std::size_t j = m; j-- > 0;) {
        lcs[i][j] = before[i] == after[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
      }
    }
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n || j < m) {
      if (i < n && j < m && before[i] == after[j]) {
        out += " " + before[i] + "\n";
        ++i;
        ++j;
      } else if (j < m && (i == n || lcs[i][j + 1] >= lcs[i + 1][j])) {
        out += "+" + after[j++] + "\n";
      } else {
        out += "-" + before[i++] + "\n";
      }
    }
  }
  return out;
}

}  // namespace kk::agents
