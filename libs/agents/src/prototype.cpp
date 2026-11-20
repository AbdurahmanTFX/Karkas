#include "kk/agents/prototype.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <nlohmann/json.hpp>

#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::agents {

namespace fs = std::filesystem;
using json = nlohmann::json;

const SketchBlock* Sketch::find(std::string_view id) const {
  for (const auto& b : blocks) {
    if (id_of(b) == id) return &b;
  }
  return nullptr;
}

fs::path sketch_path(const fs::path& root) { return root / ".karkas" / "prototype.json"; }

kk::Result<Sketch> load_sketch(const fs::path& root) {
  Sketch s;
  std::error_code ec;
  if (!fs::exists(sketch_path(root), ec)) return s;
  auto text = core::read_file(sketch_path(root));
  if (!text) return std::unexpected(text.error());
  auto j = json::parse(*text, nullptr, false);
  if (!j.is_object()) return kk::fail("prototype", sketch_path(root).string() + " is not a JSON object");
  auto str = [](const json& o, const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); };
  auto num = [](const json& o, const char* k) { return o.contains(k) && o[k].is_number() ? o[k].get<float>() : 0.0f; };
  s.scope = str(j, "scope");
  s.idea = str(j, "idea");
  s.sent = str(j, "sent");
  s.sent_at = j.contains("sent_at") && j["sent_at"].is_number_integer() ? j["sent_at"].get<std::int64_t>() : 0;
  if (j.contains("blocks") && j["blocks"].is_array()) {
    for (const auto& b : j["blocks"]) {
      if (!b.is_object()) continue;
      s.blocks.push_back({str(b, "name"), str(b, "purpose"), str(b, "kind"), num(b, "x"), num(b, "y")});
    }
  }
  if (j.contains("links") && j["links"].is_array()) {
    for (const auto& l : j["links"]) {
      if (l.is_object()) s.links.push_back({str(l, "from"), str(l, "to"), str(l, "note")});
    }
  }
  if (j.contains("map") && j["map"].is_object()) {
    for (const auto& [k, v] : j["map"].items()) {
      if (v.is_string()) s.map[k] = v.get<std::string>();
    }
  }
  return s;
}

kk::VoidResult save_sketch(const fs::path& root, const Sketch& s) {
  json j = {{"scope", s.scope}, {"idea", s.idea}, {"blocks", json::array()}, {"links", json::array()}};
  for (const auto& b : s.blocks) {
    json o = {{"name", b.name}, {"x", b.x}, {"y", b.y}};
    if (!b.purpose.empty()) o["purpose"] = b.purpose;
    if (!b.kind.empty()) o["kind"] = b.kind;
    j["blocks"].push_back(o);
  }
  for (const auto& l : s.links) {
    json o = {{"from", l.from}, {"to", l.to}};
    if (!l.note.empty()) o["note"] = l.note;
    j["links"].push_back(o);
  }
  if (!s.map.empty()) j["map"] = s.map;
  if (!s.sent.empty()) {
    j["sent"] = s.sent;
    j["sent_at"] = s.sent_at;
  }
  return core::write_file(sketch_path(root), j.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
}

kk::VoidResult clear_sketch(const fs::path& root) {
  std::error_code ec;
  fs::remove(sketch_path(root), ec);
  if (ec) return kk::fail("prototype", ec.message());
  return {};
}

std::string sketch_problems(const Sketch& s, const std::set<std::string>& existing) {
  if (s.blocks.empty()) return "the prototype has no blocks: add some on the schema";
  std::set<std::string> ids;
  for (const auto& b : s.blocks) {
    if (!core::is_ident(b.name)) return fmt::format("'{}' is not a block name (a-z, 0-9, _; starts with a letter)", b.name);
    auto id = s.id_of(b);
    if (!ids.insert(id).second) return fmt::format("{} is in the prototype twice", id);
    if (existing.contains(id)) return fmt::format("{} exists already: name the prototype block differently or link to it", id);
    if (!b.kind.empty() && b.kind != "group" && b.kind != "leaf" && b.kind != "entry" && b.kind != "shared") {
      return fmt::format("{}: kind is group, leaf, entry or shared, not '{}'", id, b.kind);
    }
  }
  std::set<std::pair<std::string, std::string>> seen;
  for (const auto& l : s.links) {
    for (const auto& end : {l.from, l.to}) {
      if (!ids.contains(end) && !existing.contains(end)) return fmt::format("the dependency {} -> {} names {}, which is neither drawn nor a block", l.from, l.to, end);
    }
    if (l.from == l.to) return fmt::format("{} depends on itself", l.from);
    if (!seen.insert({l.from, l.to}).second) return fmt::format("the dependency {} -> {} is drawn twice", l.from, l.to);
  }
  return {};
}

std::string sketch_text(const Sketch& s) {
  std::string out = fmt::format("@prototype level={} blocks={} dependencies={}\n", s.scope.empty() ? "top" : s.scope, s.blocks.size(),
                                s.links.size());
  if (!s.idea.empty()) out += "idea  " + s.idea + "\n";
  for (const auto& b : s.blocks) {
    out += fmt::format("block {}{}{}\n", s.id_of(b), b.kind.empty() ? "" : " kind=" + b.kind, b.purpose.empty() ? "" : " \"" + b.purpose + "\"");
  }
  for (const auto& l : s.links) out += fmt::format("dep   {} -> {}{}\n", l.from, l.to, l.note.empty() ? "" : " \"" + l.note + "\"");
  return out;
}

std::string sketch_task(const Sketch& s) {
  std::vector<std::string> blocks;
  for (const auto& b : s.blocks) blocks.push_back(s.id_of(b) + (b.purpose.empty() ? "" : " (" + b.purpose + ")"));
  std::vector<std::string> deps;
  for (const auto& l : s.links) deps.push_back(l.from + " -> " + l.to + (l.note.empty() ? "" : " (" + l.note + ")"));
  std::string out = s.idea.empty() ? std::string("Add what the human's prototype shows") : s.idea;
  out += fmt::format(". The human's prototype of {}: new blocks {}", s.scope.empty() ? "the top level" : s.scope, core::join(blocks, "; "));
  if (!deps.empty()) out += "; dependencies " + core::join(deps, "; ");
  out += ". Keep these blocks and dependencies in the change (rename or add blocks only for a stated reason).";
  return out;
}

SchemaShape shape_of(const core::Schema& s) {
  SchemaShape out;
  for (const auto& b : s.blocks) {
    out.blocks.insert(b.id);
    if (b.kind == core::BlockKind::Group) out.groups.insert(b.id);
    if (b.kind == core::BlockKind::Group && b.children.empty()) out.childless_groups.insert(b.id);
  }
  for (const auto& l : s.links()) out.links.emplace_back(l.src_block, l.dst_block);
  return out;
}

std::size_t SketchCheck::kept_blocks() const {
  return static_cast<std::size_t>(std::count_if(found.begin(), found.end(), [](const auto& f) { return !f.second.empty(); }));
}

std::size_t SketchCheck::kept_links() const { return static_cast<std::size_t>(std::count(links.begin(), links.end(), 1)); }

SketchCheck check_sketch(const Sketch& s, const SchemaShape& shape) {
  SketchCheck c;
  for (const auto& b : s.blocks) {
    auto id = s.id_of(b);
    auto it = s.map.find(id);
    std::string target = it != s.map.end() ? it->second : id;
    c.found[id] = shape.blocks.contains(target) ? target : std::string();
    if (c.found[id].empty()) c.gaps.push_back(fmt::format("block {} is not in the schema", id));
  }
  auto realized = [&](const std::string& end) -> std::string {
    if (auto it = c.found.find(end); it != c.found.end()) return it->second;
    return shape.blocks.contains(end) ? end : std::string();  // an existing block the human linked to
  };
  for (const auto& l : s.links) {
    auto a = realized(l.from);
    auto b = realized(l.to);
    if (a.empty() || b.empty()) {
      c.links.push_back(0);  // its missing block is the gap already
      continue;
    }
    bool kept = std::any_of(shape.links.begin(), shape.links.end(), [&](const auto& x) {
      return (core::Schema::is_under(a, x.first) && core::Schema::is_under(b, x.second)) ||
             (core::Schema::is_under(b, x.first) && core::Schema::is_under(a, x.second));
    });
    if (kept) {
      c.links.push_back(1);
    } else if (shape.childless_groups.contains(a) || shape.childless_groups.contains(b)) {
      c.links.push_back(-1);  // links of a group exist between its children, after it is expanded
    } else {
      c.links.push_back(0);
      c.gaps.push_back(fmt::format("dependency {} -> {} is not kept", l.from, l.to));
    }
  }
  return c;
}

std::vector<std::string> unexplained_gaps(const Sketch& s, const SketchCheck& c, std::string_view deviations) {
  // A name counts as a whole word of the text (dots and underscores belong to it).
  auto names = [&](const std::string& id) {
    for (std::size_t p = deviations.find(id); p != std::string_view::npos; p = deviations.find(id, p + 1)) {
      auto word = [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '.'; };
      bool left = p == 0 || !word(deviations[p - 1]);
      std::size_t e = p + id.size();
      bool right = e >= deviations.size() || !word(deviations[e]) || (deviations[e] == '.' && (e + 1 >= deviations.size() || !word(deviations[e + 1])));
      if (left && right) return true;
    }
    return false;
  };
  std::vector<std::string> out;
  for (const auto& b : s.blocks) {
    auto id = s.id_of(b);
    if (c.found.at(id).empty() && !names(id) && !names(b.name)) out.push_back(fmt::format("block {} is not in the schema", id));
  }
  for (std::size_t i = 0; i < s.links.size() && i < c.links.size(); ++i) {
    const auto& l = s.links[i];
    bool ends_found = (c.found.contains(l.from) ? !c.found.at(l.from).empty() : true) && (c.found.contains(l.to) ? !c.found.at(l.to).empty() : true);
    if (c.links[i] == 0 && ends_found && !(names(l.from) && names(l.to))) out.push_back(fmt::format("dependency {} -> {} is not kept", l.from, l.to));
  }
  return out;
}

PrototypeSend plan_prototype_send(const Sketch& s, const SchemaShape& shape, bool idea_known) {
  PrototypeSend p;
  auto as_task = [&](const Sketch& what) {
    p.text = sketch_task(what);
    p.args = {"task"};
    if (!s.scope.empty()) p.args.insert(p.args.end(), {"--block", s.scope});
    p.args.push_back(p.text);
    p.sent = "task";
  };
  if (!s.scope.empty() && !shape.groups.contains(s.scope)) {
    p.error = shape.blocks.contains(s.scope) ? fmt::format("{} is not a group: a prototype is drawn on the top level or inside a group", s.scope)
                                             : fmt::format("the level {} of the prototype is not a block any more", s.scope);
    return p;
  }
  if (!s.sent.empty()) {
    // Sent before: only what the schema does not have yet.
    auto c = check_sketch(s, shape);
    Sketch rest = s;
    rest.blocks.clear();
    rest.links.clear();
    for (const auto& b : s.blocks) {
      if (c.found[s.id_of(b)].empty()) rest.blocks.push_back(b);
    }
    for (std::size_t i = 0; i < s.links.size() && i < c.links.size(); ++i) {
      if (c.links[i] == 0) rest.links.push_back(s.links[i]);
    }
    if (rest.empty()) {
      p.error = "everything of the prototype is in the schema (dependencies through groups come when they are expanded)";
      return p;
    }
    as_task(rest);
    return p;
  }
  if (auto why = sketch_problems(s, shape.blocks); !why.empty()) {
    p.error = why;
    return p;
  }
  bool empty = std::none_of(shape.blocks.begin(), shape.blocks.end(), [&](const std::string& id) { return core::parent_id(id) == s.scope; });
  if (s.scope.empty() && empty) {
    if (!idea_known && core::trim(s.idea).empty()) {
      p.error = "write the idea first: the architect builds from the idea and the sketch";
      return p;
    }
    p.args = {"design", "--sketch"};
    p.sent = "design";
  } else if (empty) {
    p.args = {"expand", s.scope, "--sketch"};
    p.sent = "expand";
  } else {
    as_task(s);
  }
  return p;
}

std::string sketch_report(const Sketch& s, const SchemaShape& shape) {
  std::string out = sketch_text(s);
  if (s.sent.empty()) return out + "state  a draft: not given to the agents yet\n";
  auto c = check_sketch(s, shape);
  out += fmt::format("state  given to the agents ({}); kept {}/{} blocks, {}/{} dependencies\n", s.sent, c.kept_blocks(), s.blocks.size(),
                     c.kept_links(), s.links.size());
  for (const auto& b : s.blocks) {
    auto id = s.id_of(b);
    const auto& f = c.found[id];
    out += f.empty() ? fmt::format("missing {}\n", id) : f == id ? fmt::format("kept    {}\n", id) : fmt::format("kept    {} as {}\n", id, f);
  }
  for (std::size_t i = 0; i < s.links.size() && i < c.links.size(); ++i) {
    const char* st = c.links[i] == 1 ? "kept   " : c.links[i] == -1 ? "pending" : "missing";
    out += fmt::format("{} {} -> {}{}\n", st, s.links[i].from, s.links[i].to, c.links[i] == -1 ? " (comes with the expansion of its group)" : "");
  }
  return out;
}

kk::Result<Sketch> sketch_from_json(std::string_view text) {
  auto j = json::parse(text, nullptr, false);
  if (!j.is_object()) return kk::fail("prototype", "the sketch is not a JSON object");
  auto str = [](const json& o, const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); };
  Sketch s;
  s.scope = str(j, "level") == "root" ? std::string() : str(j, "level");
  s.idea = str(j, "idea");
  if (j.contains("blocks") && j["blocks"].is_array()) {
    for (const auto& b : j["blocks"]) {
      if (b.is_object()) s.blocks.push_back({str(b, "name"), str(b, "purpose"), str(b, "kind"), 0, 0});
    }
  }
  auto full = [&](const std::string& n) {
    for (const auto& b : s.blocks) {
      if (b.name == n) return s.id_of(b);
    }
    return n;
  };
  if (j.contains("dependencies") && j["dependencies"].is_array()) {
    for (const auto& d : j["dependencies"]) {
      if (d.is_object()) s.links.push_back({full(str(d, "from")), full(str(d, "to")), str(d, "note")});
    }
  }
  return s;
}

void lay_out_sketch(Sketch& s) {
  // Column of a block = the longest chain of dependencies of the sketch that leads to it.
  std::map<std::string, int> col;
  for (const auto& b : s.blocks) col[s.id_of(b)] = 0;
  for (std::size_t round = 0; round < s.blocks.size(); ++round) {
    bool moved = false;
    for (const auto& l : s.links) {
      if (!col.contains(l.from) || !col.contains(l.to)) continue;
      if (col[l.to] < col[l.from] + 1 && col[l.from] + 1 < static_cast<int>(s.blocks.size())) {
        col[l.to] = col[l.from] + 1;
        moved = true;
      }
    }
    if (!moved) break;  // (a cycle stops at the number of blocks)
  }
  std::map<int, int> rows;
  for (auto& b : s.blocks) {
    if (b.x != 0 || b.y != 0) continue;
    int c = col[s.id_of(b)];
    int r = rows[c]++;
    b.x = static_cast<float>(c) * 320.0f;
    b.y = static_cast<float>(r) * 170.0f;
  }
}

std::map<std::string, std::string> parse_sketch_map(std::string_view text) {
  std::map<std::string, std::string> out;
  std::string t(text);
  for (char& ch : t) {
    if (ch == ';' || ch == ',') ch = '\n';
  }
  for (const auto& line : core::split(t, '\n')) {
    auto l = std::string(core::trim(line));
    for (std::string_view sep : {"->", "=>", ":", "="}) {
      auto p = l.find(sep);
      if (p == std::string::npos) continue;
      auto a = std::string(core::trim(std::string_view(l).substr(0, p)));
      auto b = std::string(core::trim(std::string_view(l).substr(p + sep.size())));
      if (core::is_block_path(a) && core::is_block_path(b)) out[a] = b;
      break;
    }
  }
  return out;
}

}  // namespace kk::agents
