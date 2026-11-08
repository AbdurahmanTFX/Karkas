#include "kk/agents/edit.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <regex>
#include <set>

#include "kk/agents/changes.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"

namespace kk::agents {

namespace fs = std::filesystem;

namespace {

std::vector<std::string> card_lines(const core::Block& b) {
  auto lines = core::split(core::write_block(b), '\n', false);
  if (!lines.empty() && lines.back().empty()) lines.pop_back();
  if (!lines.empty()) lines.erase(lines.begin());  // header
  return lines;
}

std::string esc(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '.') out += "\\.";
    else out.push_back(c);
  }
  return out;
}

// Rewrites references to block `from` (and, with `prefix`, to its descendants) into `to`.
// With `port_filter`, only port references "from:<port>" whose port is in the set move.
std::string rewrite_refs(const std::string& line, const std::string& from, const std::string& to, bool prefix,
                         const std::set<std::string>* port_filter = nullptr) {
  std::string tail = prefix ? "((?:\\.[a-z0-9_]+)*)" : "()";
  if (port_filter != nullptr) {
    std::string out = line;
    for (const auto& p : *port_filter) {
      out = std::regex_replace(out, std::regex("(from=)" + esc(from) + ":" + p + "(?=\\s|$)"), "$1" + to + ":" + p);
      out = std::regex_replace(out, std::regex("^(call\\s+)" + esc(from) + ":" + p + "(?=\\s|$)"), "$1" + to + ":" + p);
    }
    return out;
  }
  std::string out = line;
  out = std::regex_replace(out, std::regex("(from=|with=)" + esc(from) + tail + "(?=[:\\s]|$)"), "$1" + to + "$2");
  out = std::regex_replace(out, std::regex("^(call\\s+)" + esc(from) + tail + "(?=:)"), "$1" + to + "$2");
  out = std::regex_replace(out, std::regex("^(use\\s+)" + esc(from) + tail + "$"), "$1" + to + "$2");
  return out;
}

bool refers_to(const std::string& line, const std::string& id) { return rewrite_refs(line, id, "\x01", false) != line; }

// Diff sections that rewrite references in every card except `skip` (and its subtree).
void add_ref_rewrites(const core::Schema& s, std::vector<core::DiffLine>& diff, const std::function<std::string(const std::string&)>& fn,
                      const std::set<std::string>& skip) {
  for (const auto& b : s.blocks) {
    bool skipped = std::any_of(skip.begin(), skip.end(), [&](const std::string& k) { return core::Schema::is_under(k, b.id); });
    if (skipped) continue;
    std::vector<core::DiffLine> mine;
    for (const auto& l : card_lines(b)) {
      auto n = fn(l);
      if (n == l) continue;
      mine.push_back({'-', l, 0});
      if (!n.empty()) mine.push_back({'+', n, 0});
    }
    if (mine.empty()) continue;
    diff.push_back({'~', "@block " + b.id, 0});
    diff.insert(diff.end(), mine.begin(), mine.end());
  }
}

bool has_code_files(const core::Schema& s, const std::string& id) {
  std::error_code ec;
  auto dir = s.block_dir(id);
  for (const auto& sub : {s.project.dirs.api, s.project.dirs.impl}) {
    for (auto it = fs::recursive_directory_iterator(dir / sub, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (it->is_regular_file(ec) && core::is_code_file(it->path())) return true;
    }
  }
  return false;
}

kk::Result<const core::Block*> need(const core::Schema& s, const std::string& id) {
  const auto* b = s.find(id);
  if (b == nullptr) return kk::fail("edit", fmt::format("unknown block '{}'", id));
  return b;
}

std::string strip_from(const std::string& line) {
  return std::regex_replace(line, std::regex("\\s+(from=\\S+|link=data)"), "");
}

void remove_empty_dirs(fs::path dir, const fs::path& stop) {
  std::error_code ec;
  while (dir != stop && core::is_within(stop, dir) && fs::is_directory(dir, ec) && fs::is_empty(dir, ec)) {
    fs::remove(dir, ec);
    dir = dir.parent_path();
  }
}

}  // namespace

kk::Result<EditOp> op_add_block(const core::Schema& s, const std::string& parent, const std::string& name, core::BlockKind kind,
                                const std::string& purpose, const std::string& thread) {
  if (!core::is_ident(name)) return kk::fail("edit", fmt::format("'{}' is not an identifier [a-z][a-z0-9_]*", name));
  std::string p = parent == "root" ? "" : parent;
  if (!p.empty()) {
    auto pb = need(s, p);
    if (!pb) return std::unexpected(pb.error());
    if ((*pb)->kind != core::BlockKind::Group) return kk::fail("edit", fmt::format("{} is not a group", p));
  }
  std::string id = p.empty() ? name : p + "." + name;
  if (s.find(id) != nullptr) return kk::fail("edit", fmt::format("{} already exists", id));
  if (core::trim(purpose).empty()) return kk::fail("edit", "purpose is required");
  EditOp op;
  op.title = fmt::format("add {} {}", core::to_string(kind), id);
  op.diff.push_back({'+', "@block " + id, 0});
  op.diff.push_back({'+', core::format_line("purpose", core::format_free_text(purpose)), 0});
  op.diff.push_back({'+', core::format_line("kind", core::to_string(kind)), 0});
  if (kind != core::BlockKind::Group) op.diff.push_back({'+', core::format_line("thread", thread.empty() ? "main" : thread), 0});
  if (kind == core::BlockKind::Group) op.tolerate.push_back("E06");
  return op;
}

kk::Result<EditOp> op_remove_block(const core::Schema& s, const std::string& id) {
  auto b = need(s, id);
  if (!b) return std::unexpected(b.error());
  if (!(*b)->children.empty()) return kk::fail("edit", fmt::format("{} has children; remove them first", id));
  if (has_code_files(s, id)) return kk::fail("edit", fmt::format("{} has code in api/ or impl/; move or delete it first", id));
  EditOp op;
  op.title = "remove " + id;
  op.diff.push_back({'-', "@block " + id, 0});
  add_ref_rewrites(s, op.diff, [&](const std::string& l) -> std::string {
    if (!refers_to(l, id)) return l;
    if (l.starts_with("in ")) return strip_from(l);
    return "";  // call / use / state lines go away
  }, {id});
  return op;
}

kk::Result<EditOp> op_add_port(const core::Schema& s, const std::string& id, core::PortDir dir, const std::string& name,
                               const std::string& spec) {
  auto b = need(s, id);
  if (!b) return std::unexpected(b.error());
  if (!(*b)->has_code()) return kk::fail("edit", "ports belong to leaves (3.3)");
  std::string line = dir == core::PortDir::Fn ? core::format_line("fn", spec)
                                              : core::format_line(core::to_string(dir), name + " " + spec);
  auto check = core::parse_block_text("@block " + id + "\n" + line + "\n", "edit");
  if (!check) return kk::fail("edit", check.error().message);
  auto pname = check->ports.empty() ? name : check->ports[0].name;
  if ((*b)->port(pname) != nullptr) return kk::fail("edit", fmt::format("{} already has a port '{}'", id, pname));
  EditOp op;
  op.title = fmt::format("add {} {} to {}", core::to_string(dir), pname, id);
  op.diff = {{'~', "@block " + id, 0}, {'+', card_lines(*check).front(), 0}};
  return op;
}

kk::Result<EditOp> op_remove_port(const core::Schema& s, const std::string& id, const std::string& port) {
  auto b = need(s, id);
  if (!b) return std::unexpected(b.error());
  const auto* p = (*b)->port(port);
  if (p == nullptr) return kk::fail("edit", fmt::format("{} has no port '{}'", id, port));
  EditOp op;
  op.title = fmt::format("remove port {}:{}", id, port);
  op.diff = {{'~', "@block " + id, 0}, {'-', core::format_port(*p), 0}};
  std::set<std::string> only = {port};
  add_ref_rewrites(s, op.diff, [&](const std::string& l) -> std::string {
    if (rewrite_refs(l, id, "\x01", false, &only) == l) return l;
    return l.starts_with("in ") ? strip_from(l) : "";
  }, {id});
  return op;
}

kk::Result<EditOp> op_connect(const core::Schema& s, const std::string& from_block, const std::string& from_port,
                              const std::string& to_block, const std::string& to_port) {
  auto a = need(s, from_block);
  if (!a) return std::unexpected(a.error());
  auto b = need(s, to_block);
  if (!b) return std::unexpected(b.error());
  if (!(*a)->has_code() || !(*b)->has_code()) return kk::fail("edit", "links connect leaves (3.3); open the group first");
  if (from_block == to_block) return kk::fail("edit", "a block does not link to itself");
  const auto* p = (*a)->port(from_port);
  if (p == nullptr) return kk::fail("edit", fmt::format("{} has no port '{}'", from_block, from_port));
  if (p->dir == core::PortDir::In) {
    if (to_port.empty()) return kk::fail("edit", "drag from an out port or a fn to the consumer");
    return op_connect(s, to_block, to_port, from_block, from_port);
  }
  EditOp op;
  op.diff.push_back({'~', "@block " + to_block, 0});
  if (p->dir == core::PortDir::Fn) {
    for (const auto& c : (*b)->calls) {
      if (c.target.block == from_block && c.target.port == from_port) return kk::fail("edit", "this call exists already");
    }
    op.title = fmt::format("{} calls {}:{}", to_block, from_block, from_port);
    op.diff.push_back({'+', core::format_line("call", from_block + ":" + from_port), 0});
    return op;
  }
  op.title = fmt::format("link {}:{} -> {}", from_block, from_port, to_block);
  const core::Port* target = to_port.empty() ? nullptr : (*b)->port(to_port);
  if (target != nullptr) {
    if (target->dir != core::PortDir::In) return kk::fail("edit", fmt::format("{}:{} is not an in port", to_block, to_port));
    core::Port np = *target;
    np.from = core::PortRef{from_block, from_port};
    np.trivia = {};
    op.diff.push_back({'-', core::format_port(*target), 0});
    op.diff.push_back({'+', core::format_port(np), 0});
    return op;
  }
  std::string name = to_port.empty() ? from_port : to_port;
  for (int i = 2; (*b)->port(name) != nullptr; ++i) name = fmt::format("{}{}", from_port, i);
  core::Port np;
  np.dir = core::PortDir::In;
  np.name = name;
  np.type = p->type;
  np.from = core::PortRef{from_block, from_port};
  op.diff.push_back({'+', core::format_port(np), 0});
  return op;
}

kk::Result<EditOp> op_disconnect(const core::Schema& s, const std::string& kind, const std::string& src_block,
                                 const std::string& src_port, const std::string& dst_block, const std::string& dst_port) {
  EditOp op;
  if (kind == "data" || kind == "event") {
    auto b = need(s, dst_block);
    if (!b) return std::unexpected(b.error());
    const auto* p = (*b)->port(dst_port);
    if (p == nullptr || !p->from) return kk::fail("edit", "no such link");
    core::Port np = *p;
    np.from.reset();
    np.link_written = false;
    np.trivia = {};
    op.title = fmt::format("unlink {}:{} -> {}:{}", src_block, src_port, dst_block, dst_port);
    op.diff = {{'~', "@block " + dst_block, 0}, {'-', core::format_port(*p), 0}, {'+', core::format_port(np), 0}};
    return op;
  }
  auto b = need(s, src_block);
  if (!b) return std::unexpected(b.error());
  std::string line;
  if (kind == "call") line = core::format_line("call", dst_block + ":" + dst_port);
  if (kind == "use") line = core::format_line("use", dst_block);
  if (kind == "state") {
    for (const auto& l : card_lines(**b)) {
      if (l.starts_with("state") && core::contains(l, "with=" + dst_block)) line = l;
    }
  }
  auto lines = card_lines(**b);
  if (line.empty() || std::find(lines.begin(), lines.end(), line) == lines.end()) return kk::fail("edit", "no such link");
  op.title = fmt::format("remove {} link {} -> {}", kind, src_block, dst_block);
  op.diff = {{'~', "@block " + src_block, 0}, {'-', line, 0}};
  return op;
}

kk::Result<EditOp> op_rename(const core::Schema& s, const std::string& id, const std::string& new_name) {
  auto b = need(s, id);
  if (!b) return std::unexpected(b.error());
  if (!core::is_ident(new_name)) return kk::fail("edit", fmt::format("'{}' is not an identifier", new_name));
  auto parent = core::parent_id(id);
  std::string nid = parent.empty() ? new_name : parent + "." + new_name;
  if (s.find(nid) != nullptr) return kk::fail("edit", fmt::format("{} already exists", nid));
  EditOp op;
  op.title = fmt::format("rename {} -> {}", id, nid);
  op.rename_from = id;
  op.rename_to = nid;
  add_ref_rewrites(s, op.diff, [&](const std::string& l) { return rewrite_refs(l, id, nid, true); }, {});
  return op;
}

kk::Result<EditOp> op_move(const core::Schema& s, const std::string& id, const std::string& new_parent_in) {
  auto b = need(s, id);
  if (!b) return std::unexpected(b.error());
  std::string parent = new_parent_in == "root" ? "" : new_parent_in;
  if (!parent.empty()) {
    auto pb = need(s, parent);
    if (!pb) return std::unexpected(pb.error());
    if ((*pb)->kind != core::BlockKind::Group) return kk::fail("edit", fmt::format("{} is not a group", parent));
    if (core::Schema::is_under(id, parent)) return kk::fail("edit", fmt::format("{} cannot move into itself", id));
  }
  if (core::parent_id(id) == parent) return kk::fail("edit", fmt::format("{} is already there", id));
  auto pos = id.rfind('.');
  std::string name = pos == std::string::npos ? id : id.substr(pos + 1);
  std::string nid = parent.empty() ? name : parent + "." + name;
  if (s.find(nid) != nullptr) return kk::fail("edit", fmt::format("{} already exists", nid));
  EditOp op;
  op.title = fmt::format("move {} -> {}", id, nid);
  op.rename_from = id;
  op.rename_to = nid;
  op.tolerate.push_back("E06");  // the old group may be left empty
  add_ref_rewrites(s, op.diff, [&](const std::string& l) { return rewrite_refs(l, id, nid, true); }, {});
  return op;
}

kk::Result<EditOp> op_set_purpose(const core::Schema& s, const std::string& id, const std::string& purpose) {
  auto b = need(s, id);
  if (!b) return std::unexpected(b.error());
  if (core::trim(purpose).empty()) return kk::fail("edit", "purpose is required");
  EditOp op;
  op.title = "purpose of " + id;
  op.diff.push_back({'~', "@block " + id, 0});
  for (const auto& l : card_lines(**b)) {
    if (l.starts_with("purpose")) op.diff.push_back({'-', l, 0});
  }
  op.diff.push_back({'+', core::format_line("purpose", core::format_free_text(purpose)), 0});
  return op;
}

kk::Result<EditOp> op_merge(const core::Schema& s, const std::string& keep, const std::string& absorb) {
  auto k = need(s, keep);
  if (!k) return std::unexpected(k.error());
  auto a = need(s, absorb);
  if (!a) return std::unexpected(a.error());
  if (!(*k)->has_code() || !(*a)->has_code()) return kk::fail("edit", "merge works on leaves");
  if (has_code_files(s, absorb)) return kk::fail("edit", fmt::format("{} has code; move it into {} first", absorb, keep));
  EditOp op;
  op.title = fmt::format("merge {} into {}", absorb, keep);
  op.diff.push_back({'~', "@block " + keep, 0});
  for (const auto& l : card_lines(**k)) {
    if (!refers_to(l, absorb)) continue;
    op.diff.push_back({'-', l, 0});
    if (l.starts_with("in ")) op.diff.push_back({'+', strip_from(l), 0});
  }
  std::set<std::string> keep_uses;
  for (const auto& u : (*k)->uses) keep_uses.insert(u.target);
  for (const auto& p : (*a)->ports) {
    if ((*k)->port(p.name) != nullptr) return kk::fail("edit", fmt::format("both blocks have a port '{}'", p.name));
  }
  for (const auto& l : card_lines(**a)) {
    if (l.starts_with("purpose") || l.starts_with("kind") || l.starts_with("thread") || l.starts_with("est") ||
        l.starts_with("hist")) {
      continue;
    }
    if (l.starts_with("use")) {
      auto target = std::string(core::trim(std::string_view(l).substr(3)));
      if (keep_uses.contains(target)) continue;
    }
    if (refers_to(l, keep)) {
      if (l.starts_with("in ")) op.diff.push_back({'+', strip_from(l), 0});
      continue;  // calls into keep become internal
    }
    op.diff.push_back({'+', l, 0});
  }
  op.diff.push_back({'-', "@block " + absorb, 0});
  add_ref_rewrites(s, op.diff, [&](const std::string& l) { return rewrite_refs(l, absorb, keep, false); }, {keep, absorb});
  return op;
}

kk::Result<EditOp> op_split(const core::Schema& s, const std::string& id, const std::string& new_name,
                            const std::vector<std::string>& ports) {
  auto b = need(s, id);
  if (!b) return std::unexpected(b.error());
  if (!(*b)->has_code()) return kk::fail("edit", "split works on leaves");
  if (!core::is_ident(new_name)) return kk::fail("edit", fmt::format("'{}' is not an identifier", new_name));
  if (ports.empty()) return kk::fail("edit", "choose the ports that move to the new block");
  auto parent = core::parent_id(id);
  std::string nid = parent.empty() ? new_name : parent + "." + new_name;
  if (s.find(nid) != nullptr) return kk::fail("edit", fmt::format("{} already exists", nid));
  EditOp op;
  op.title = fmt::format("split {} out of {}", nid, id);
  op.diff.push_back({'+', "@block " + nid, 0});
  op.diff.push_back({'+', core::format_line("purpose", core::format_free_text("part of " + id + ": " + core::join(ports, ", "))), 0});
  op.diff.push_back({'+', core::format_line("kind", (*b)->kind_name()), 0});
  if ((*b)->thread.set) op.diff.push_back({'+', core::format_line("thread", (*b)->thread.value), 0});
  std::vector<core::DiffLine> removals;
  for (const auto& name : ports) {
    const auto* p = (*b)->port(name);
    if (p == nullptr) return kk::fail("edit", fmt::format("{} has no port '{}'", id, name));
    auto line = core::format_port(*p);
    op.diff.push_back({'+', line, 0});
    removals.push_back({'-', line, 0});
  }
  for (const auto& u : (*b)->uses) op.diff.push_back({'+', core::format_line("use", u.target), 0});
  op.diff.push_back({'~', "@block " + id, 0});
  op.diff.insert(op.diff.end(), removals.begin(), removals.end());
  std::set<std::string> moved(ports.begin(), ports.end());
  add_ref_rewrites(s, op.diff, [&](const std::string& l) { return rewrite_refs(l, id, nid, false, &moved); }, {id});
  return op;
}

// --- apply / undo ------------------------------------------------------------------------------

std::string preview_edit(const fs::path& root, const EditOp& op) {
  std::string out = op.title + "\n";
  if (!op.rename_from.empty()) {
    auto s = core::load_schema(root);
    if (s) {
      out += fmt::format("move {} -> {}\n", core::rel_str(root, s->block_dir(op.rename_from)),
                         core::rel_str(root, s->block_dir(op.rename_to)));
    }
  }
  if (!op.diff.empty()) out += preview_diff(root, op.diff);
  return out;
}

kk::Result<AppliedEdit> apply_edit(const fs::path& root, const EditOp& op) {
  auto s = core::load_schema(root);
  if (!s) return std::unexpected(s.error());
  AppliedEdit a;
  a.title = op.title;
  if (!op.rename_from.empty()) {
    // Snapshot every card, move the folder, rewrite ids and references, check.
    for (const auto& b : s->blocks) {
      auto p = s->block_dir(b.id) / "block.ksl";
      auto moved = core::Schema::is_under(op.rename_from, b.id)
                       ? s->block_dir(op.rename_to + b.id.substr(op.rename_from.size())) / "block.ksl"
                       : p;
      a.before.emplace_back(moved, core::read_file(p).value_or(""));
    }
    a.moved_from = s->block_dir(op.rename_from).string();
    a.moved_to = s->block_dir(op.rename_to).string();
    std::error_code ec;
    if (fs::exists(a.moved_to)) return kk::fail("edit", "target folder exists");
    fs::rename(a.moved_from, a.moved_to, ec);
    if (ec) return kk::fail("edit", "cannot move the folder: " + ec.message());
    for (const auto& [path, _] : a.before) {
      auto text = core::read_file(path);
      if (!text) continue;
      auto rec = core::parse_block_text(*text, path.string());
      if (!rec) continue;
      auto b = *rec;
      if (core::Schema::is_under(op.rename_from, b.id)) b.id = op.rename_to + b.id.substr(op.rename_from.size());
      std::vector<std::string> lines = {"@block " + b.id};
      for (const auto& l : card_lines(b)) lines.push_back(rewrite_refs(l, op.rename_from, op.rename_to, true));
      auto np = core::parse_block_text(core::join(lines, "\n") + "\n", path.string());
      if (!np) continue;
      np->header = b.header;
      auto out = core::write_block(*np);
      if (out != *text) {
        (void)core::write_file(path, out);
        a.files.push_back(core::rel_str(root, path));
      }
    }
    auto after = core::load_schema(root);
    // Errors the edit accepts (a move may leave its old group empty: E06).
    std::string why = after ? "" : after.error().message;
    if (after) {
      for (const auto& i : core::check_schema(*after)) {
        if (i.severity == 'E' && std::ranges::find(op.tolerate, i.code) == op.tolerate.end()) why += i.str() + "\n";
      }
    }
    if (!why.empty()) {
      (void)undo_edit(root, a);
      return kk::fail("edit", "rename breaks the schema:\n" + why);
    }
    return a;
  }
  std::set<std::string> ids;
  for (const auto& d : op.diff) {
    if (d.text.starts_with("@block ")) ids.insert(std::string(core::trim(std::string_view(d.text).substr(7))));
  }
  for (const auto& id : ids) {
    auto p = s->block_dir(id) / "block.ksl";
    if (fs::exists(p)) {
      a.before.emplace_back(p, core::read_file(p).value_or(""));
    } else {
      a.before.emplace_back(p, std::nullopt);
    }
  }
  auto r = apply_diff(root, op.diff, op.tolerate);
  if (!r) return std::unexpected(r.error());
  if (!r->issues.empty()) {
    std::string why;
    for (const auto& i : r->issues) why += i.str() + "\n";
    return kk::fail("edit", "the edit breaks the schema:\n" + why);
  }
  a.files = r->files;
  auto broot = s->blocks_root();
  for (const auto& [p, _] : a.before) {
    if (!fs::exists(p)) remove_empty_dirs(p.parent_path(), broot);
  }
  return a;
}

kk::VoidResult undo_edit(const fs::path& root, const AppliedEdit& e) {
  std::error_code ec;
  if (!e.moved_from.empty()) {
    fs::rename(e.moved_to, e.moved_from, ec);
    if (ec) return kk::fail("edit", "cannot move the folder back: " + ec.message());
    auto s = core::load_schema(root);
    for (const auto& [p, text] : e.before) {
      // Paths were recorded at their new location; map them back.
      auto rel = core::rel_str(e.moved_to, p);
      fs::path back = core::is_within(e.moved_to, p) ? fs::path(e.moved_from) / rel : p;
      if (text) (void)core::write_file(back, *text);
    }
    return {};
  }
  auto s = core::load_schema(root);
  for (const auto& [p, text] : e.before) {
    if (text) {
      if (auto r = core::write_file(p, *text); !r) return r;
    } else {
      fs::remove(p, ec);
      if (s) remove_empty_dirs(p.parent_path(), s->blocks_root());
    }
  }
  return {};
}

}  // namespace kk::agents
