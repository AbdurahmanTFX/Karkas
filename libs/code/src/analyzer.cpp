#include "kk/code/analyzer.hpp"

#include "kk/core/notes.hpp"

#include <fmt/format.h>
#include <tree_sitter/api.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <regex>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <set>

#include "kk/code/gen.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace kk::code {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

// --- tree helpers ------------------------------------------------------------------------

std::string_view type_of(TSNode n) { return ts_node_type(n); }
int line_of(TSNode n) { return static_cast<int>(ts_node_start_point(n).row) + 1; }

std::string text_of(TSNode n, std::string_view src) {
  auto b = ts_node_start_byte(n);
  auto e = ts_node_end_byte(n);
  if (e > src.size() || b > e) return {};
  return std::string(src.substr(b, e - b));
}

TSNode field(TSNode n, const char* name) { return ts_node_child_by_field_name(n, name, static_cast<uint32_t>(std::strlen(name))); }

template <class F>
void each_named(TSNode n, F&& f) {
  uint32_t c = ts_node_named_child_count(n);
  for (uint32_t i = 0; i < c; ++i) f(ts_node_named_child(n, i));
}

// Finds the function_declarator inside a declarator chain (pointer/reference wrappers).
TSNode find_fn_declarator(TSNode n) {
  for (int depth = 0; depth < 8 && !ts_node_is_null(n); ++depth) {
    auto t = type_of(n);
    if (t == "function_declarator") return n;
    if (t == "pointer_declarator" || t == "reference_declarator" || t == "parenthesized_declarator" ||
        t == "attributed_declarator") {
      TSNode inner = field(n, "declarator");
      if (ts_node_is_null(inner) && ts_node_named_child_count(n) > 0) {
        inner = ts_node_named_child(n, ts_node_named_child_count(n) - 1);
      }
      n = inner;
      continue;
    }
    break;
  }
  return TSNode{};
}

// Name of a declarator: identifier, field_identifier, qualified name (last part).
std::string declarator_name(TSNode d, std::string_view src, std::string* scope = nullptr) {
  if (ts_node_is_null(d)) return {};
  auto t = type_of(d);
  if (t == "identifier" || t == "field_identifier" || t == "type_identifier" || t == "operator_name") {
    return text_of(d, src);
  }
  if (t == "destructor_name") return "~" + text_of(d, src).substr(1);
  if (t == "qualified_identifier") {
    TSNode name = field(d, "name");
    TSNode sc = field(d, "scope");
    std::string inner;
    auto n = declarator_name(name, src, &inner);
    if (scope != nullptr) {
      std::string outer = ts_node_is_null(sc) ? std::string() : text_of(sc, src);
      *scope = outer.empty() ? inner : (inner.empty() ? outer : outer + "::" + inner);
    }
    return n;
  }
  // template_function: f<int>; template_type: the name of a class template specialization,
  // struct serialize<T, true> { ... }.
  if (t == "template_function" || t == "template_type") return declarator_name(field(d, "name"), src, scope);
  return {};
}

std::string strip_specifiers(std::string s) {
  for (const char* kw : {"virtual ", "static ", "inline ", "explicit ", "constexpr ", "friend ", "extern "}) {
    std::size_t p = 0;
    while ((p = s.find(kw, p)) != std::string::npos) {
      if (p == 0 || s[p - 1] == ' ' || s[p - 1] == '\n') {
        s.erase(p, std::strlen(kw));
      } else {
        p += 1;
      }
    }
  }
  // [[attributes]]
  for (std::size_t a = s.find("[["); a != std::string::npos; a = s.find("[[")) {
    auto e = s.find("]]", a);
    if (e == std::string::npos) break;
    s.erase(a, e + 2 - a);
  }
  return std::string(core::trim(s));
}

// Parameters: (type, name) pairs; type is the declaration text with the name removed.
std::vector<std::pair<std::string, std::string>> params_of(TSNode plist, std::string_view src) {
  std::vector<std::pair<std::string, std::string>> out;
  each_named(plist, [&](TSNode p) {
    auto t = type_of(p);
    if (t != "parameter_declaration" && t != "optional_parameter_declaration" && t != "variadic_parameter_declaration") {
      return;
    }
    std::string full = text_of(p, src);
    TSNode def = field(p, "default_value");
    if (!ts_node_is_null(def)) {
      auto cut = ts_node_start_byte(def) - ts_node_start_byte(p);
      full = full.substr(0, cut);
      auto eq = full.rfind('=');
      if (eq != std::string::npos) full = full.substr(0, eq);
    }
    // Locate the parameter name: the innermost identifier of the declarator.
    std::string name;
    TSNode d = field(p, "declarator");
    for (int i = 0; i < 6 && !ts_node_is_null(d); ++i) {
      if (type_of(d) == "identifier") {
        name = text_of(d, src);
        auto off = ts_node_start_byte(d) - ts_node_start_byte(p);
        if (off < full.size()) full.erase(off, name.size());
        break;
      }
      TSNode inner = field(d, "declarator");
      if (ts_node_is_null(inner) && ts_node_named_child_count(d) > 0) inner = ts_node_named_child(d, ts_node_named_child_count(d) - 1);
      d = inner;
    }
    out.emplace_back(std::string(core::trim(full)), name);
  });
  if (out.size() == 1 && out[0].first == "void" && out[0].second.empty()) out.clear();
  return out;
}

struct Walker {
  std::string_view src;
  bool api = false;
  index::FileFacts facts;
  std::vector<std::string> ns;      // namespace / class scope stack
  std::vector<std::string> access;  // per scope: "" for namespaces, current access in classes

  std::string scope() const { return core::join(ns, "::"); }

  void add(std::string name, std::string kind, int line, int params = -1, bool pure = false, std::string sig = {}) {
    if (name.empty()) return;
    facts.symbols.push_back(index::SymbolFact{std::move(name), std::move(kind), line, api, params, scope(), pure, std::move(sig),
                                              access.empty() ? std::string() : access.back()});
  }

  void function_like(TSNode decl, TSNode fn, bool in_class, bool definition) {
    std::string qual;
    std::string name = declarator_name(field(fn, "declarator"), src, &qual);
    if (name.empty() || name.starts_with("~") || name.starts_with("operator")) return;
    if (in_class && !ns.empty()) {  // constructor
      auto parts = core::split(ns.back(), ':');
      if (!parts.empty() && name == parts.back()) return;
    }
    TSNode plist = field(fn, "parameters");
    auto params = ts_node_is_null(plist) ? std::vector<std::pair<std::string, std::string>>{} : params_of(plist, src);
    bool pure = false;
    TSNode def = field(decl, "default_value");
    if (!ts_node_is_null(def) && core::trim(text_of(def, src)) == "0") pure = true;
    // Return type: declaration text up to the function declarator.
    auto pre_len = ts_node_start_byte(fn) - ts_node_start_byte(decl);
    std::string ret = strip_specifiers(text_of(decl, src).substr(0, pre_len));
    // Qualifiers after the parameter list.
    std::string after;
    if (!ts_node_is_null(plist)) after = std::string(std::string_view(src).substr(ts_node_end_byte(plist), ts_node_end_byte(fn) - ts_node_end_byte(plist)));
    json sig = {{"ret", ret},
                {"params", json::array()},
                {"const", core::contains(after, "const")},
                {"noexcept", core::contains(after, "noexcept")}};
    for (const auto& [t, n] : params) sig["params"].push_back(json::array({t, n}));
    std::string kind = (in_class || !qual.empty()) ? "method" : "function";
    if (!qual.empty()) {
      ns.push_back(qual);
      add(name, kind, line_of(decl), static_cast<int>(params.size()), pure, sig.dump());
      ns.pop_back();
    } else {
      add(name, kind, line_of(decl), static_cast<int>(params.size()), pure, sig.dump());
    }
    (void)definition;
  }

  void calls_in(TSNode n) {
    if (type_of(n) == "call_expression") {
      TSNode f = field(n, "function");
      std::string name;
      if (!ts_node_is_null(f)) {
        auto t = type_of(f);
        if (t == "identifier") name = text_of(f, src);
        if (t == "field_expression") name = text_of(field(f, "field"), src);
        if (t == "qualified_identifier" || t == "template_function") name = declarator_name(f, src);
      }
      if (!name.empty()) facts.refs.push_back(index::RefFact{line_of(n), "call", name, false});
    }
    each_named(n, [&](TSNode c) { calls_in(c); });
  }

  void walk(TSNode n, bool in_class) {
    auto t = type_of(n);
    if (t == "preproc_include") {
      TSNode p = field(n, "path");
      std::string path = text_of(p, src);
      bool sys = !path.empty() && path[0] == '<';
      if (path.size() >= 2) path = path.substr(1, path.size() - 2);
      facts.refs.push_back(index::RefFact{line_of(n), "include", path, sys});
      return;
    }
    if (t == "namespace_definition") {
      TSNode name = field(n, "name");
      ns.push_back(ts_node_is_null(name) ? std::string() : text_of(name, src));
      access.emplace_back();
      TSNode body = field(n, "body");
      if (!ts_node_is_null(body)) each_named(body, [&](TSNode c) { walk(c, false); });
      access.pop_back();
      ns.pop_back();
      return;
    }
    if (t == "class_specifier" || t == "struct_specifier" || t == "union_specifier") {
      TSNode name = field(n, "name");
      TSNode body = field(n, "body");
      if (!ts_node_is_null(name) && !ts_node_is_null(body)) {
        std::string nm = declarator_name(name, src);
        add(nm, t == "class_specifier" ? "class" : "struct", line_of(n));
        ns.push_back(nm);
        access.emplace_back(t == "class_specifier" ? "private" : "public");
        each_named(body, [&](TSNode c) { walk(c, true); });
        access.pop_back();
        ns.pop_back();
      }
      return;
    }
    if (t == "access_specifier") {
      if (!access.empty()) access.back() = std::string(core::trim(text_of(n, src)));
      return;
    }
    if (t == "enum_specifier") {
      TSNode name = field(n, "name");
      if (!ts_node_is_null(name)) add(text_of(name, src), "enum", line_of(n));
      return;
    }
    if (t == "alias_declaration") {
      add(text_of(field(n, "name"), src), "alias", line_of(n));
      return;
    }
    if (t == "type_definition") {
      TSNode d = field(n, "declarator");
      add(declarator_name(d, src), "alias", line_of(n));
      // typedef struct {...} X;
      TSNode ty = field(n, "type");
      if (!ts_node_is_null(ty)) walk(ty, in_class);
      return;
    }
    if (t == "field_declaration" || t == "declaration") {
      TSNode ty = field(n, "type");
      if (!ts_node_is_null(ty)) {
        auto tt = type_of(ty);
        if (tt == "class_specifier" || tt == "struct_specifier" || tt == "enum_specifier" || tt == "union_specifier") walk(ty, in_class);
      }
      // A declaration may hold several declarators; take every function declarator.
      uint32_t cnt = ts_node_child_count(n);
      for (uint32_t i = 0; i < cnt; ++i) {
        if (std::string_view(ts_node_field_name_for_child(n, i) ? ts_node_field_name_for_child(n, i) : "") != "declarator") continue;
        TSNode fn = find_fn_declarator(ts_node_child(n, i));
        if (!ts_node_is_null(fn)) function_like(n, fn, in_class, false);
      }
      return;
    }
    if (t == "function_definition") {
      TSNode fn = find_fn_declarator(field(n, "declarator"));
      if (!ts_node_is_null(fn)) function_like(n, fn, in_class, true);
      TSNode body = field(n, "body");
      if (!ts_node_is_null(body)) calls_in(body);
      return;
    }
    if (t == "template_declaration" || t == "linkage_specification" || t == "declaration_list" ||
        t == "preproc_ifdef" || t == "preproc_if" || t == "preproc_else" || t == "preproc_elif" ||
        t == "translation_unit" || t == "field_declaration_list") {
      each_named(n, [&](TSNode c) { walk(c, in_class); });
      return;
    }
    if (t == "expression_statement" || t == "compound_statement") calls_in(n);
  }
};

// --- check helpers ------------------------------------------------------------------------

const std::set<std::string>& builtin_types() {
  static const std::set<std::string> s = {
      "void", "bool", "char", "int", "long", "short", "float", "double", "unsigned", "signed", "const", "auto",
      "size_t", "ssize_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
      "std", "string", "string_view", "vector", "span", "array", "map", "optional", "function", "byte", "path",
      "nullptr_t", "ptrdiff_t", "intptr_t", "uintptr_t", "char8_t", "char16_t", "char32_t", "wchar_t"};
  return s;
}

std::vector<std::string> type_identifiers(std::string_view type) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty() && !builtin_types().contains(cur) && !(cur[0] >= '0' && cur[0] <= '9')) out.push_back(cur);
    cur.clear();
  };
  for (char c : type) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
      cur.push_back(c);
    } else {
      flush();
    }
  }
  flush();
  return out;
}

bool is_contract_test(std::string_view path) { return core::contains(path, "/tests/contract/"); }

}  // namespace

// --- public ---------------------------------------------------------------------------------

index::FileFacts CppAnalyzer::analyze(const index::CodeFile& file, std::string_view content) {
  TSParser* parser = ts_parser_new();
  ts_parser_set_language(parser, tree_sitter_cpp());
  TSTree* tree = ts_parser_parse_string(parser, nullptr, content.data(), static_cast<uint32_t>(content.size()));
  Walker w;
  w.src = content;
  w.api = file.role == "api";
  if (tree != nullptr) {
    w.walk(ts_tree_root_node(tree), false);
    ts_tree_delete(tree);
  }
  ts_parser_delete(parser);
  return std::move(w.facts);
}

std::string card_type(std::string t) {
  if (auto eq = t.find('='); eq != std::string::npos) t = t.substr(0, eq);
  for (const char* w : {"const", "volatile", "static", "inline", "virtual", "explicit", "constexpr", "typename", "struct",
                        "class", "enum", "extern", "mutable", "friend"}) {
    t = std::regex_replace(t, std::regex(std::string("\\b") + w + "\\b"), "");
  }
  t = core::replace_all(t, "&", "");
  t = core::replace_all(t, "\"", "");
  t = core::replace_all(t, "#", "");
  t = std::regex_replace(t, std::regex("\\s+"), " ");
  t = std::regex_replace(t, std::regex(" ?\\* ?"), "*");
  t = std::regex_replace(t, std::regex(" ?([<>,]) ?"), "$1");
  return std::string(core::trim(t));
}

std::string CppAnalyzer::version() const { return "cpp " __DATE__ " " __TIME__; }

Signature decode_signature(std::string_view json_text) {
  Signature s;
  try {
    auto j = json::parse(json_text);
    s.ret = j.value("ret", "");
    s.is_const = j.value("const", false);
    s.is_noexcept = j.value("noexcept", false);
    for (const auto& p : j.value("params", json::array())) s.params.emplace_back(p.at(0).get<std::string>(), p.at(1).get<std::string>());
  } catch (const json::exception&) {
  }
  return s;
}

const std::set<std::string>& common_call_names() {
  static const std::set<std::string> names = {
      "size", "push_back", "emplace_back", "begin", "end", "get", "set", "run", "process", "open", "close", "read",
      "write", "start", "stop", "init", "reset", "clear", "data", "value", "push", "pop", "find", "insert", "erase",
      "empty", "front", "back", "load", "store", "count", "make", "create", "update", "apply", "main", "CHECK",
      "REQUIRE", "log", "error", "ok", "has_value", "fill", "capacity", "min", "max", "abs", "move", "swap",
      "is_null", "str", "text", "name", "path", "type", "id", "parse", "dump", "load", "save", "list", "add",
      // logging levels (spdlog::debug, logger.info ...): never a call of a block (ADR 0029)
      "trace", "debug", "info", "warn", "warning", "critical"};
  return names;
}

std::vector<std::string> allowed_api_blocks(const core::Schema& schema, std::string_view block) {
  return allowed_api_blocks(schema, block, schema.links());
}

std::vector<std::string> allowed_api_blocks(const core::Schema& schema, std::string_view block,
                                            const std::vector<core::LinkInfo>& links) {
  std::set<std::string> out;
  const auto* b = schema.find(block);
  if (b == nullptr) return {};
  if (b->kind == core::BlockKind::Entry) {
    for (const auto& x : schema.blocks) {
      if (x.has_code() && x.id != b->id) out.insert(x.id);
    }
  } else {
    for (const auto& l : links) {
      if (l.kind == core::LinkKind::State) {
        if (l.src_block == block) out.insert(l.dst_block);
        if (l.dst_block == block) out.insert(l.src_block);
        continue;
      }
      // Consumers include the api of their suppliers, callers of callees, users of shared.
      if ((l.kind == core::LinkKind::Data || l.kind == core::LinkKind::Event) && l.dst_block == block) out.insert(l.src_block);
      if ((l.kind == core::LinkKind::Call || l.kind == core::LinkKind::Use) && l.src_block == block) out.insert(l.dst_block);
    }
  }
  // Integration tests of a group: every block under it and the shared blocks it uses.
  if (b->kind == core::BlockKind::Group) {
    for (const auto* l : schema.leaves_under(block)) {
      out.insert(l->id);
      for (const auto& a : allowed_api_blocks(schema, l->id, links)) out.insert(a);
    }
  }
  out.erase(std::string(block));
  return {out.begin(), out.end()};
}

index::CheckOutput CppAnalyzer::check(const core::Schema& schema, const index::IndexFacts& facts) {
  index::CheckOutput out;
  if (core::is_notes_project(schema.project)) return out;  // notes hold no code to drift from (docs/adr/0038)
  const auto& dirs = schema.project.dirs;
  auto issue = [&](std::string code, char sev, std::string block, std::string msg, std::string file, int line) {
    out.issues.push_back(core::Issue{std::move(code), sev, std::move(block), std::move(msg), {std::move(file), line, 0}});
  };
  // Roots: "<block dir>/<api>" and "<block dir>/<impl>".
  std::map<std::string, std::string> api_root, impl_root;
  for (const auto& b : schema.blocks) {
    if (!b.has_code()) continue;
    api_root[b.id] = b.dir + "/" + dirs.api;
    impl_root[b.id] = b.dir + "/" + dirs.impl;
  }
  std::set<std::string> known;
  std::map<std::string, const index::CodeFile*> by_path;
  for (const auto& f : facts.files) {
    known.insert(f.path);
    by_path[f.path] = &f;
  }
  // Files reachable as "<root>/<include>" for every api/impl root.
  struct Target {
    std::string block, role, path;
  };
  // Only the roots that hold files are tried: an imported project's blocks have none, and trying
  // 2 × 318 roots for every include took seconds (ADR 0028).
  auto holds_files = [&](const std::string& root) {
    auto it = known.lower_bound(root + "/");
    return it != known.end() && it->starts_with(root + "/");
  };
  std::vector<std::pair<std::string, std::string>> live_api_roots, live_impl_roots;
  for (const auto& [blk, root] : api_root) {
    if (holds_files(root)) live_api_roots.emplace_back(blk, root);
  }
  for (const auto& [blk, root] : impl_root) {
    if (holds_files(root)) live_impl_roots.emplace_back(blk, root);
  }
  // Files of imported blocks by file name, for the suffix match below.
  std::unordered_map<std::string, std::vector<const index::CodeFile*>> imported_by_name;
  for (const auto& f : facts.files) {
    const auto* ob = f.block.empty() ? nullptr : schema.find(f.block);
    if (ob != nullptr && ob->external()) imported_by_name[fs::path(f.path).filename().string()].push_back(&f);
  }
  auto resolve = [&](const index::CodeFile& from, const std::string& inc, bool system) -> std::vector<Target> {
    std::vector<Target> cands;
    auto rel = (fs::path(from.path).parent_path() / inc).lexically_normal().generic_string();
    if (known.contains(rel)) {
      const auto* f = by_path[rel];
      return {Target{f->block, f->role, rel}};
    }
    for (const auto& [blk, root] : live_api_roots) {
      auto p = (fs::path(root) / inc).lexically_normal().generic_string();
      if (known.contains(p)) cands.push_back({blk, "api", p});
    }
    for (const auto& [blk, root] : live_impl_roots) {
      auto p = (fs::path(root) / inc).lexically_normal().generic_string();
      if (known.contains(p)) cands.push_back({blk, "impl", p});
    }
    // Imported code: include paths come from the project's own build; match a unique
    // file of an imported block by its path suffix.
    if (cands.empty() && !system) {
      const index::CodeFile* hit = nullptr;
      int n = 0;
      auto same_name = imported_by_name.find(fs::path(inc).filename().string());
      if (same_name != imported_by_name.end()) {
        for (const auto* f : same_name->second) {
          if (f->path == inc || f->path.ends_with("/" + inc)) {
            hit = f;
            ++n;
          }
        }
      }
      if (n == 1) cands.push_back({hit->block, hit->role, hit->path});
    }
    return cands;
  };

  std::map<std::string, std::set<std::string>> allowed;
  const auto links = schema.links();
  for (const auto& b : schema.blocks) {
    auto a = allowed_api_blocks(schema, b.id, links);
    allowed[b.id] = {a.begin(), a.end()};
  }

  // Exported symbols per block (api files).
  std::map<std::string, std::vector<const index::SymbolFact*>> exported;
  std::map<std::string, std::set<std::string>> own_names;  // every name declared/defined in a block's files
  for (const auto& f : facts.files) {
    auto it = facts.facts.find(f.path);
    if (it == facts.facts.end()) continue;
    for (const auto& s : it->second.symbols) {
      own_names[f.block].insert(s.name);
      if (f.role == "api") exported[f.block].push_back(&s);
    }
  }
  // The blocks that export a function or method of a name, in the order of `exported` (by block id):
  // a call is looked up by its name, not against every exported symbol of the project (ADR 0028).
  std::unordered_map<std::string, std::vector<std::string>> fn_owners;
  for (const auto& [blk, syms] : exported) {
    for (const auto* s : syms) {
      if (s->kind != "method" && s->kind != "function") continue;
      auto& v = fn_owners[s->name];
      if (v.empty() || v.back() != blk) v.push_back(blk);
    }
  }

  // D01 / D02 + resolved include refs; D09 call heuristics.
  const auto& kCommon = common_call_names();
  for (const auto& f : facts.files) {
    auto it = facts.facts.find(f.path);
    if (it == facts.facts.end() || f.block.empty()) continue;
    const auto* owner = schema.find(f.block);
    bool entry = owner != nullptr && owner->kind == core::BlockKind::Entry;
    for (const auto& r : it->second.refs) {
      if (r.kind == "include") {
        auto cands = resolve(f, r.target, r.system);
        if (cands.empty()) continue;
        // Prefer the own block, then allowed blocks.
        std::stable_sort(cands.begin(), cands.end(), [&](const Target& a, const Target& b) {
          auto rank = [&](const Target& t) { return t.block == f.block ? 0 : (allowed[f.block].contains(t.block) ? 1 : 2); };
          return rank(a) < rank(b);
        });
        const auto& t = cands.front();
        out.refs.push_back(index::ResolvedRef{f.block, f.path, r.line, t.block, t.role, "include"});
        if (t.block == f.block) continue;
        if (t.role == "impl") {
          issue("D01", 'E', f.block, fmt::format("includes {} impl/ ({}): impl/ of another block is never visible", t.block, r.target),
                f.path, r.line);
        } else if (t.role == "api" && !entry && !allowed[f.block].contains(t.block)) {
          issue("D02", 'E', f.block, fmt::format("includes {} api without link", t.block), f.path, r.line);
        }
      } else if (r.kind == "call" && f.role == "impl") {
        if (kCommon.contains(r.target) || own_names[f.block].contains(r.target)) continue;
        std::vector<std::string> owners;
        bool allowed_owner = false;
        static const std::vector<std::string> kNoOwner;
        auto named = fn_owners.find(r.target);
        const auto& candidates = named == fn_owners.end() ? kNoOwner : named->second;
        for (const auto& blk : candidates) {
          if (blk == f.block) continue;
          const auto* ob = schema.find(blk);
          if (allowed[f.block].contains(blk) || (ob != nullptr && ob->kind == core::BlockKind::Shared)) allowed_owner = true;
          owners.push_back(blk);
        }
        if (!owners.empty()) out.refs.push_back(index::ResolvedRef{f.block, f.path, r.line, owners.front(), "api", "call"});
        if (!entry && !allowed_owner && owners.size() == 1) {
          issue("D09", 'W', f.block, fmt::format("calls {}() of {} without a call link (heuristic)", r.target, owners[0]),
                f.path, r.line);
        }
      }
    }
  }

  // D03 / D04 / D05 / D08 per leaf.
  auto has_api = [&](const std::string& blk) { return exported.contains(blk); };
  auto declares_type = [&](const std::string& blk, const std::string& name) {
    auto it = exported.find(blk);
    if (it == exported.end()) return false;
    return std::any_of(it->second.begin(), it->second.end(), [&](const index::SymbolFact* s) {
      return s->name == name && (s->kind == "class" || s->kind == "struct" || s->kind == "enum" || s->kind == "alias");
    });
  };
  for (const auto& b : schema.blocks) {
    if (!b.has_code()) continue;
    // D05: api changed after freeze.
    if (facts.state != nullptr) {
      auto fz = facts.state->frozen_api.find(b.id);
      auto cur = facts.api_hash.find(b.id);
      if (fz != facts.state->frozen_api.end() && cur != facts.api_hash.end() && fz->second != cur->second) {
        issue("D05", 'E', b.id, "api/ changed after it was frozen: contract change, needs approval", b.dir + "/" + dirs.api, 0);
      }
    }
    if (!has_api(b.id)) continue;  // api not written yet (design)
    // D03: port types.
    std::vector<std::string> sources = {b.id};
    for (const auto& u : b.uses) sources.push_back(u.target);
    for (const auto& x : schema.blocks) {
      if (x.kind == core::BlockKind::Shared && core::split(x.id, '.').back() == "types") sources.push_back(x.id);
    }
    auto check_type = [&](const std::string& type, std::vector<std::string> extra, const std::string& what, int line) {
      auto src = sources;
      src.insert(src.end(), extra.begin(), extra.end());
      for (const auto& id : type_identifiers(type)) {
        bool found = std::any_of(src.begin(), src.end(), [&](const std::string& s) { return declares_type(s, id); });
        if (!found) {
          issue("D03", 'E', b.id, fmt::format("type '{}' of {} not found in api/ of {}", id, what, core::join(src, ", ")),
                b.file, line);
        }
      }
    };
    for (const auto& p : b.ports) {
      if (p.dir == core::PortDir::Fn) continue;
      std::vector<std::string> extra;
      if (p.from) extra.push_back(p.from->block);
      check_type(p.type, extra, fmt::format("{} {}", core::to_string(p.dir), p.name), p.line);
    }
    // D04: fn in the card must exist in api/ (by name; parameter count is a soft check).
    for (const auto& p : b.ports) {
      if (p.dir != core::PortDir::Fn) continue;
      std::vector<const index::SymbolFact*> overloads;
      // Names are compared as KSL identifiers: imported C++ code keeps "ResolveCollision"
      // while the card says "resolve_collision".
      for (const auto* s : exported[b.id]) {
        if ((s->name == p.name || core::to_ident(s->name) == p.name) && (s->kind == "method" || s->kind == "function")) {
          overloads.push_back(s);
        }
      }
      if (overloads.empty()) {
        issue("D04", 'E', b.id, fmt::format("fn {} of the card is not declared in api/", p.signature()), b.file, p.line);
      } else if (std::none_of(overloads.begin(), overloads.end(),
                              [&](const index::SymbolFact* s) { return s->params == static_cast<int>(p.params.size()); })) {
        issue("D04", 'W', b.id,
              fmt::format("fn {} takes {} parameter(s) in the card but {} in api/", p.name, p.params.size(), overloads[0]->params),
              b.file, p.line);
      } else if (!p.ret.empty()) {
        // The return type, in the card's form: a card saying "-> int" over a function returning int64_t
        // misleads every block that reads the card (ADR 0029). Only overloads whose signature is known count.
        // "std::" is left out on both sides: a card's size_t is the code's std::size_t.
        auto same_form = [](const std::string& t) { return core::replace_all(card_type(t), "std::", ""); };
        std::vector<std::string> code_rets;
        std::vector<std::string> forms;
        for (const auto* s : overloads) {
          if (s->signature.empty()) continue;
          auto ret = card_type(decode_signature(s->signature).ret);
          code_rets.push_back(ret.empty() ? std::string("void") : ret);
          forms.push_back(same_form(code_rets.back()));
        }
        if (!code_rets.empty() && std::find(forms.begin(), forms.end(), same_form(p.ret)) == forms.end()) {
          issue("D04", 'W', b.id, fmt::format("fn {} returns {} in the card but {} in the code", p.name, p.ret, code_rets[0]), b.file,
                p.line);
        }
      }
    }
  }
  for (const auto& b : schema.blocks) {
    if (b.kind != core::BlockKind::Leaf || b.external()) continue;  // imported code has its own tests (or none)
    bool contract = std::any_of(facts.files.begin(), facts.files.end(),
                                [&](const index::CodeFile& f) { return f.block == b.id && is_contract_test(f.path); });
    if (!contract) issue("D08", 'W', b.id, "leaf has no contract tests (tests/contract/)", b.dir, 0);
  }

  // D06: code files outside block folders.
  for (const auto& f : facts.files) {
    if (f.role == "other" && !f.block.empty() && schema.find(f.block) != nullptr && schema.find(f.block)->has_code()) {
      issue("D06", 'W', f.block, "code file outside api/, impl/, tests/, mocks/ of its block", f.path, 0);
    }
  }
  {
    std::error_code ec;
    auto root = schema.root;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      const auto& p = it->path();
      auto name = p.filename().string();
      if (it->is_directory(ec)) {
        auto rel = core::rel_str(root, p);
        bool other_project = p != root && fs::exists(p / "project.ksl");
        if (name.starts_with(".") || name.starts_with("build") || name == "_deps" || name == "third_party" ||
            name == "external" || name == "node_modules" || rel == "cmake" || rel == "tests/integration" || other_project) {
          it.disable_recursion_pending();
          continue;
        }
        // Inside a leaf folder everything is covered by the file list.
        bool leaf_dir = false;
        for (const auto& b : schema.blocks) leaf_dir = leaf_dir || (b.has_code() && b.dir == rel);
        if (leaf_dir) it.disable_recursion_pending();
        continue;
      }
      if (!it->is_regular_file(ec) || !core::is_code_file(p)) continue;
      auto rel = core::rel_str(root, p);
      if (known.contains(rel)) continue;
      issue("D06", 'W', "", "code file outside the block folders (orphan)", rel, 0);
    }
  }

  // D07: generated CMakeLists.txt edited by hand, or stale.
  auto cm = schema.root / "CMakeLists.txt";
  if (fs::exists(cm)) {
    auto text = core::read_file(cm);
    if (text) {
      auto h = cmake_hashes(*text);
      if (h.generated && h.declared != h.actual) {
        issue("D07", 'E', "", "generated CMakeLists.txt was edited by hand (put additions into cmake/user.cmake)",
              "CMakeLists.txt", 0);
      } else if (h.generated && *text != gen_cmake(schema)) {
        issue("D07", 'W', "", "CMakeLists.txt is out of date with the schema: run karkas gen cmake", "CMakeLists.txt", 0);
      }
    }
  }
  std::stable_sort(out.issues.begin(), out.issues.end(), [](const core::Issue& a, const core::Issue& b) {
    return std::tie(a.loc.file, a.loc.line, a.code) < std::tie(b.loc.file, b.loc.line, b.code);
  });
  return out;
}

}  // namespace kk::code
