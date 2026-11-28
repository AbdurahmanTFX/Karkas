#include "kk/code/import.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>

#include "kk/code/analyzer.hpp"
#include "kk/code/lexical.hpp"
#include "kk/core/lang.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/index/index.hpp"

namespace kk::code {

namespace fs = std::filesystem;

namespace {

// Folder names that only separate headers from sources: "src/active/hpp/cat.hpp" and
// "src/active/cat.cpp" are the same module of the area "src/active".
bool is_layout_dir(const std::string& name) {
  static const std::set<std::string> names = {"hpp",    "h",      "hh",     "include", "inc",     "headers",
                                              "header", "src",    "source", "sources", "impl",    "api",
                                              "public", "private", "detail", "internal"};
  return names.contains(core::to_lower(name));
}

// Folders that are not modules of the program.
bool is_foreign_dir(const std::string& name) {
  static const std::set<std::string> names = {"third_party", "thirdparty", "3rdparty", "external", "extern",
                                              "vendor",      "deps",       "_deps",    "node_modules", "test",
                                              "tests",       "testing"};
  return names.contains(core::to_lower(name));
}

// The class or namespace path of a symbol without template arguments: "leveldb::SkipList<Key, Cmp>" →
// "leveldb::SkipList". A definition out of its class (SkipList<Key, Cmp>::Insert) then matches its declaration.
std::string plain_scope(std::string_view scope) {
  std::string out;
  int depth = 0;
  for (char c : scope) {
    if (c == '<') ++depth;
    if (depth == 0 && c != ' ') out += c;
    if (c == '>' && depth > 0) --depth;
  }
  return out;
}

std::string area_of(const std::string& rel) {
  std::vector<std::string> parts;
  for (const auto& c : fs::path(rel).parent_path()) parts.push_back(c.string());
  while (!parts.empty() && is_layout_dir(parts.back())) parts.pop_back();
  return core::join(parts, "/");
}

struct SourceFile {
  std::string path;
  bool header = false;  // declares only: a C/C++ header, a .pyi, a .d.ts (ADR 0051)
  bool split = true;    // its language keeps the interface in headers (C/C++); otherwise the file is its api
  std::string text;
  index::FileFacts facts;
};

struct Module {
  std::string area, stem, name;
  std::vector<std::size_t> files;
  bool merged = false;  // folded into another module
  // Filled later.
  std::string id;
  core::BlockKind kind = core::BlockKind::Leaf;
  std::map<std::string, std::string> fns;                 // C++ name → port name (public api)
  std::vector<std::string> port_order;                     // port names in header order
  std::map<std::string, const index::SymbolFact*> port_of;  // port name → first declaration
  std::vector<std::string> types;                          // public type names
  std::set<std::string> own;                               // every name declared or defined here
  std::set<std::size_t> includes;                          // modules included
  std::set<std::pair<std::size_t, std::string>> calls;     // (module, port)
};

bool public_symbol(const index::SymbolFact& s) { return s.access.empty() || s.access == "public"; }

// "const std::string&" → "std::string"; drops qualifiers that say nothing about the data.
bool type_ok(std::string_view t) {
  if (t.empty()) return false;
  char c = t[0];
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
}

// The first comment block at the top of a file, before the first declaration; its first
// sentence becomes the purpose. License headers and decoration lines are skipped.
std::string comment_purpose(const std::string& text) {
  std::vector<std::string> buf;
  bool collecting = false;
  bool in_block = false;
  auto flush_if_license = [&] {
    std::string all = core::join(buf, " ");
    if (core::contains(all, "Copyright") || core::contains(all, "SPDX") || core::contains(all, "License") ||
        core::contains(all, "(c)")) {
      buf.clear();
      collecting = false;
    }
  };
  for (const auto& raw : core::split(text, '\n', false)) {
    std::string l(core::trim(raw));
    if (in_block) {
      auto end = l.find("*/");
      std::string part = end == std::string::npos ? l : l.substr(0, end);
      while (!part.empty() && part[0] == '*') part.erase(0, 1);
      if (!core::trim(part).empty()) buf.emplace_back(core::trim(part));
      if (end != std::string::npos) {
        in_block = false;
        flush_if_license();
      }
      continue;
    }
    if (l.empty()) {
      if (collecting) {
        flush_if_license();
        if (!buf.empty()) break;
      }
      continue;
    }
    if (l.starts_with("//")) {
      std::string c = l.substr(2);
      while (!c.empty() && (c[0] == '/' || c[0] == '!')) c.erase(0, 1);
      c = std::string(core::trim(c));
      bool decoration = !c.empty() && std::all_of(c.begin(), c.end(), [](char ch) { return ch == '=' || ch == '-' || ch == '*' || ch == '/'; });
      if (!c.empty() && !decoration) buf.push_back(c);
      collecting = true;
      continue;
    }
    if (l.starts_with("/*")) {
      std::string c = l.substr(2);
      while (!c.empty() && (c[0] == '*' || c[0] == '!')) c.erase(0, 1);
      auto end = c.find("*/");
      if (end != std::string::npos) {
        c = c.substr(0, end);
        if (!core::trim(c).empty()) buf.emplace_back(core::trim(c));
        flush_if_license();
      } else {
        if (!core::trim(c).empty()) buf.emplace_back(core::trim(c));
        in_block = true;
      }
      collecting = true;
      continue;
    }
    if (l[0] == '#' || (l.ends_with(";") && (l.starts_with("class ") || l.starts_with("struct ") || l.starts_with("using ")))) {
      if (collecting && !buf.empty()) break;
      continue;
    }
    break;  // the first declaration
  }
  if (buf.empty()) return {};
  std::string all = core::join(buf, " ");
  if (auto dot = all.find(". "); dot != std::string::npos) all = all.substr(0, dot);
  while (!all.empty() && (all.back() == '.' || all.back() == ':' || all.back() == ' ')) all.pop_back();
  if (all.size() > 100) {  // W04: a purpose is one short line
    auto cut = all.rfind(' ', 97);
    all = all.substr(0, cut == std::string::npos ? 97 : cut);
    while (!all.empty() && (all.back() == ',' || all.back() == ' ' || all.back() == ':')) all.pop_back();
    all += "…";
  }
  return all;
}

// The leading comment of a file in a language other than C/C++, as "//" lines for comment_purpose:
// "#" or "--" lines after a shebang, a Python module docstring, a leading block comment (ADR 0051).
std::string lead_comment(const core::Language& lang, const std::string& text) {
  std::string out;
  auto lines = core::split(text, '\n', false);
  std::size_t i = 0;
  while (i < lines.size()) {
    std::string l(core::trim(lines[i]));
    if (l.empty() || l.starts_with("#!") || l.starts_with("<?php") || l.starts_with("# -*-") || l.starts_with("\"use strict\"") ||
        l.starts_with("'use strict'") || l.starts_with("package ")) {
      ++i;
      continue;
    }
    break;
  }
  if (i >= lines.size()) return out;
  std::string first(core::trim(lines[i]));
  if (lang.triple_quotes && (first.starts_with("\"\"\"") || first.starts_with("'''"))) {
    const std::string q = first.substr(0, 3);
    std::string body = first.substr(3);
    for (std::size_t k = i;; ++k) {
      auto end = body.find(q);
      if (end != std::string::npos) body = body.substr(0, end);
      if (!core::trim(body).empty()) out += "// " + std::string(core::trim(body)) + "\n";
      if (end != std::string::npos || k + 1 >= lines.size()) break;
      body = lines[k + 1];
    }
    return out;
  }
  for (const auto& [open, close] : lang.block_comments) {
    if (!first.starts_with(open) || open.front() == '=') continue;
    std::string body = first.substr(open.size());
    for (std::size_t k = i;; ++k) {
      auto end = body.find(close);
      if (end != std::string::npos) body = body.substr(0, end);
      std::string b(core::trim(body));
      while (!b.empty() && (b.front() == '*' || b.front() == '-')) b.erase(0, 1);
      if (!core::trim(b).empty()) out += "// " + std::string(core::trim(b)) + "\n";
      if (end != std::string::npos || k + 1 >= lines.size()) break;
      body = lines[k + 1];
    }
    return out;
  }
  for (; i < lines.size(); ++i) {
    std::string l(core::trim(lines[i]));
    bool comment = false;
    for (const auto& lc : lang.line_comments) {
      if (!l.starts_with(lc)) continue;
      std::string c = l.substr(lc.size());
      while (!c.empty() && c.front() == lc.back()) c.erase(0, 1);  // "##", "---", "///"
      out += "// " + std::string(core::trim(c)) + "\n";
      comment = true;
      break;
    }
    if (!comment) break;
  }
  return out;
}

struct Node {
  std::map<std::string, Node> groups;
  std::vector<std::size_t> modules;
};

void collapse(Node& n) {
  for (auto& [k, g] : n.groups) {
    collapse(g);
    while (g.modules.empty() && g.groups.size() == 1) {
      Node inner = std::move(g.groups.begin()->second);
      g = std::move(inner);
    }
  }
}

std::string cmake_std(const fs::path& root) {
  auto text = core::read_file(root / "CMakeLists.txt");
  if (!text) return {};
  std::smatch m;
  if (std::regex_search(*text, m, std::regex(R"(CMAKE_CXX_STANDARD\s+(\d+))"))) return m[1].str();
  if (std::regex_search(*text, m, std::regex(R"(cxx_std_(\d+))"))) return m[1].str();
  return {};
}

}  // namespace

bool import_skips_dir(std::string_view name) { return is_foreign_dir(std::string(name)); }

bool is_test_source(const fs::path& p) {
  if (!core::is_code_file(p) || core::is_interface_file(p)) return false;
  const auto stem = p.stem().string();
  const auto low = core::to_lower(stem);
  if (low == "test" || low == "tests" || low == "unittest" || low == "unittests" || low == "conftest") return true;
  // Other languages (ADR 0051): foo_test.go, foo.spec.ts, foo_spec.rb, test-foo.sh.
  for (std::string_view s : {"_test", "_tests", "_unittest", "_unittests", "-test", "-tests", ".test", ".tests", ".spec", "_spec",
                             "-spec"}) {
    if (low.ends_with(s)) return true;
  }
  for (std::string_view s : {"test_", "tests_", "tst_", "test-"}) {
    if (low.starts_with(s)) return true;
  }
  // CamelCase: FooTest, FooTests (not "Latest" or "Contest": the T is a capital after a small letter).
  for (std::string_view s : {"Test", "Tests"}) {
    if (stem.size() > s.size() && stem.ends_with(s)) {
      char before = stem[stem.size() - s.size() - 1];
      if (before >= 'a' && before <= 'z') return true;
    }
  }
  return false;
}

kk::Result<ImportPlan> plan_import(const fs::path& root_in, const ImportOptions& opts) {
  auto root = core::normalize(root_in);
  auto schema = core::load_schema(root);
  if (!schema) return std::unexpected(schema.error());
  ImportPlan plan;

  // 1. Code files not owned by any block yet.
  std::set<std::string> claimed;
  for (const auto& f : index::collect_code_files(*schema)) claimed.insert(f.path);
  std::string blocks_rel = schema->blocks_root() == root ? "" : core::rel_str(root, schema->blocks_root());
  std::vector<SourceFile> files;
  std::error_code ec;
  std::vector<fs::path> paths;
  for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto& p = it->path();
    auto name = p.filename().string();
    auto rel = core::rel_str(root, p);
    if (it->is_directory(ec)) {
      bool user_excluded = std::any_of(opts.exclude.begin(), opts.exclude.end(), [&](const std::string& e) {
        auto x = std::string(core::trim(e));
        while (x.ends_with('/')) x.pop_back();
        return rel == x || rel.starts_with(x + "/");
      });
      bool skip = name.starts_with(".") || core::is_build_dir(p) || rel == "cmake" || rel == blocks_rel ||
                  fs::exists(p / "project.ksl") || user_excluded;
      if (!skip && is_foreign_dir(name)) {
        skip = true;
        bool has_code = false;
        for (auto in = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
             in != fs::recursive_directory_iterator() && !has_code; in.increment(ec)) {
          if (ec) break;
          has_code = in->is_regular_file(ec) && core::is_project_code(schema->project, in->path());
        }
        if (has_code) plan.skipped.push_back(rel);
      } else if (user_excluded) {
        plan.skipped.push_back(rel);
      }
      if (skip) it.disable_recursion_pending();
      continue;
    }
    // Code of the project's languages (`lang` of project.ksl, ADR 0051).
    if (it->is_regular_file(ec) && core::is_project_code(schema->project, p) && !claimed.contains(rel)) paths.push_back(p);
  }
  std::sort(paths.begin(), paths.end());
  // Test programs next to the code stay in the project's own suite, like test folders (docs/adr/0048).
  {
    std::set<std::string> header_stems;
    for (const auto& p : paths) {
      if (core::is_interface_file(p)) header_stems.insert(p.stem().string());
    }
    std::erase_if(paths, [&](const fs::path& p) {
      if (!is_test_source(p) || header_stems.contains(p.stem().string())) return false;
      plan.tests.push_back(core::rel_str(root, p));
      return true;
    });
  }
  CppAnalyzer analyzer;
  for (const auto& p : paths) {
    auto text = core::read_file(p);
    if (!text) continue;
    SourceFile f;
    f.path = core::rel_str(root, p);
    f.header = core::is_interface_file(p);
    const auto* lang = core::language_of(p);
    f.split = lang == nullptr || lang->split_interface;
    f.text = std::move(*text);
    index::CodeFile cf;
    cf.path = f.path;
    cf.role = f.header ? "api" : "impl";
    f.facts = analyzer.analyze(cf, f.text);
    files.push_back(std::move(f));
  }
  if (files.empty()) return plan;

  // 2. Modules: files of one area with one stem; then lone headers and lone sources with a
  // unique common stem; then sources that only define what one module's header declares.
  std::vector<Module> mods;
  std::map<std::pair<std::string, std::string>, std::size_t> by_key;
  for (std::size_t i = 0; i < files.size(); ++i) {
    // The module's name: the file's stem; the folder's name for a Go package and for the files that
    // stand for their folder (index.ts, mod.rs, __init__.py, init.lua).
    fs::path fp(files[i].path);
    std::string stem = fp.stem().string();
    if (stem.ends_with(".d")) stem.resize(stem.size() - 2);  // index.d.ts
    const auto* lang = core::language_of(fp);
    const auto folder = fp.parent_path().filename().string();
    if (!folder.empty() && ((lang != nullptr && lang->package_is_folder) || stem == "index" || stem == "mod" || stem == "__init__" ||
                            (stem == "init" && lang != nullptr && lang->id == "lua"))) {
      stem = folder;
    }
    auto key = std::make_pair(area_of(files[i].path), stem);
    auto [it, fresh] = by_key.emplace(key, mods.size());
    if (fresh) mods.push_back(Module{key.first, key.second, {}, {}});
    mods[it->second].files.push_back(i);
  }
  auto has = [&](const Module& m, bool header) {
    return std::any_of(m.files.begin(), m.files.end(), [&](std::size_t i) { return files[i].header == header; });
  };
  auto merge = [&](std::size_t from, std::size_t into) {
    for (auto i : mods[from].files) mods[into].files.push_back(i);
    mods[from].files.clear();
    mods[from].merged = true;
  };
  {
    std::map<std::string, std::vector<std::size_t>> lone_h, lone_s;
    for (std::size_t m = 0; m < mods.size(); ++m) {
      if (has(mods[m], true) && !has(mods[m], false)) lone_h[mods[m].stem].push_back(m);
      if (has(mods[m], false) && !has(mods[m], true)) lone_s[mods[m].stem].push_back(m);
    }
    for (const auto& [stem, hs] : lone_h) {
      auto it = lone_s.find(stem);
      if (hs.size() == 1 && it != lone_s.end() && it->second.size() == 1) merge(hs[0], it->second[0]);
    }
  }
  auto defines_main = [&](const Module& m) {
    return std::any_of(m.files.begin(), m.files.end(), [&](std::size_t i) {
      return std::any_of(files[i].facts.symbols.begin(), files[i].facts.symbols.end(), [](const index::SymbolFact& s) {
        return s.name == "main" && s.kind == "function" && s.scope.empty();
      });
    });
  };
  for (std::size_t s = 0; s < mods.size(); ++s) {
    if (mods[s].merged || has(mods[s], true) || defines_main(mods[s])) continue;
    // Names with their class or namespace: CacheTest::Insert is not SkipList::Insert.
    auto key = [](const index::SymbolFact& sym) { return plain_scope(sym.scope) + "::" + sym.name; };
    std::set<std::string> defined;
    for (auto i : mods[s].files) {
      for (const auto& sym : files[i].facts.symbols) {
        if (sym.kind == "function" || sym.kind == "method") defined.insert(key(sym));
      }
    }
    std::vector<std::size_t> owners;
    for (std::size_t m = 0; m < mods.size(); ++m) {
      if (m == s || mods[m].merged || !has(mods[m], true)) continue;
      bool declares = std::any_of(mods[m].files.begin(), mods[m].files.end(), [&](std::size_t i) {
        return files[i].header && std::any_of(files[i].facts.symbols.begin(), files[i].facts.symbols.end(),
                                               [&](const index::SymbolFact& sym) { return defined.contains(key(sym)); });
      });
      if (declares) owners.push_back(m);
    }
    if (owners.size() == 1) merge(s, owners[0]);
  }
  std::vector<std::size_t> live;
  std::map<std::size_t, std::size_t> module_of_file;
  for (std::size_t m = 0; m < mods.size(); ++m) {
    if (mods[m].merged) continue;
    std::sort(mods[m].files.begin(), mods[m].files.end());
    // The area of a module is where its sources are.
    for (auto i : mods[m].files) {
      if (!files[i].header) {
        mods[m].area = area_of(files[i].path);
        break;
      }
    }
    mods[m].name = core::to_ident(mods[m].stem);
    if (mods[m].name.empty()) mods[m].name = "module";
    for (auto i : mods[m].files) module_of_file[i] = m;
    live.push_back(m);
  }

  // 3. Public api of each module (its headers) and every name it declares.
  for (auto m : live) {
    auto& mod = mods[m];
    for (auto i : mod.files) {
      for (const auto& s : files[i].facts.symbols) {
        mod.own.insert(s.name);
        // C/C++: the public declarations of headers; other languages: what the language makes public.
        if (files[i].split ? (!files[i].header || !public_symbol(s)) : !s.exported) continue;
        if (s.kind == "function" || s.kind == "method" || s.kind == "command") {
          if (s.name == "main" || s.name.starts_with("operator") || s.name.starts_with("~")) continue;
          auto port = core::to_ident(s.name);
          if (port.empty()) continue;
          mod.fns.emplace(s.name, port);
          if (!mod.port_of.contains(port)) {
            mod.port_of[port] = &s;
            mod.port_order.push_back(port);
          }
        } else if ((s.kind == "class" || s.kind == "struct" || s.kind == "enum" || s.kind == "alias") &&
                   std::find(mod.types.begin(), mod.types.end(), s.name) == mod.types.end() &&
                   (s.scope.empty() || s.access.empty())) {
          mod.types.push_back(s.name);
        }
      }
    }
  }

  // 4. The entry: the module with main() (the shallowest one when there are several).
  std::size_t entry = SIZE_MAX;
  bool schema_has_entry = std::any_of(schema->blocks.begin(), schema->blocks.end(),
                                      [](const core::Block& b) { return b.kind == core::BlockKind::Entry; });
  for (auto m : live) {
    if (!defines_main(mods[m])) continue;
    if (schema_has_entry) {
      plan.warnings.push_back(fmt::format("{} defines main(), but the schema already has an entry block: imported as a leaf",
                                          files[mods[m].files.front()].path));
      continue;
    }
    if (entry == SIZE_MAX || std::count(mods[m].area.begin(), mods[m].area.end(), '/') <
                                 std::count(mods[entry].area.begin(), mods[entry].area.end(), '/')) {
      if (entry != SIZE_MAX) plan.warnings.push_back(fmt::format("several main(): {} imported as a leaf", mods[entry].stem));
      entry = m;
    } else {
      plan.warnings.push_back(fmt::format("several main(): {} imported as a leaf", mods[m].stem));
    }
  }

  // 5. Links: includes resolve relative to the file, otherwise by a unique path suffix
  // (the same rules as the drift check, so the new schema has no D02 right away).
  std::set<std::string> all_paths;
  std::map<std::string, std::size_t> file_index;
  for (std::size_t i = 0; i < files.size(); ++i) {
    all_paths.insert(files[i].path);
    file_index[files[i].path] = i;
  }
  auto resolve = [&](const std::string& from, const index::RefFact& r) -> std::optional<std::size_t> {
    auto rel = (fs::path(from).parent_path() / r.target).lexically_normal().generic_string();
    if (all_paths.contains(rel)) return file_index[rel];
    if (r.system) return std::nullopt;
    std::optional<std::size_t> hit;
    int n = 0;
    for (const auto& p : all_paths) {
      if (p == r.target || p.ends_with("/" + r.target)) {
        hit = file_index[p];
        ++n;
      }
    }
    return n == 1 ? hit : std::nullopt;
  };
  const auto& common = common_call_names();
  const PathIndex path_index(std::vector<std::string>(all_paths.begin(), all_paths.end()));
  for (auto m : live) {
    auto& mod = mods[m];
    for (auto i : mod.files) {
      for (const auto& r : files[i].facts.refs) {
        std::vector<std::size_t> targets;
        if (r.kind == "include") {
          if (auto t = resolve(files[i].path, r)) targets.push_back(*t);
        } else if (r.kind == "import") {
          if (const auto* lang = core::language_of(files[i].path)) {
            for (const auto& p : resolve_import(*lang, files[i].path, r.target, path_index)) targets.push_back(file_index[p]);
          }
        }
        for (auto t : targets) {
          auto tm = module_of_file[t];
          if (tm != m && tm != entry) mod.includes.insert(tm);
        }
      }
    }
  }
  for (auto m : live) {
    auto& mod = mods[m];
    for (auto i : mod.files) {
      for (const auto& r : files[i].facts.refs) {
        if (r.kind != "call" || mod.own.contains(r.target)) continue;
        bool linked = false;
        for (auto t : mod.includes) {
          auto it = mods[t].fns.find(r.target);
          if (it == mods[t].fns.end()) continue;
          mod.calls.emplace(t, it->second);
          linked = true;
        }
        if (linked || common.contains(r.target)) continue;
        std::vector<std::size_t> owners;
        for (auto t : live) {
          if (t != m && t != entry && mods[t].fns.contains(r.target)) owners.push_back(t);
        }
        if (owners.size() == 1) mod.calls.emplace(owners[0], mods[owners[0]].fns[r.target]);
      }
    }
  }
  // Kinds: header-only modules that call nothing are shared (types, constants, inline helpers).
  for (auto m : live) {
    auto& mod = mods[m];
    if (m == entry) {
      mod.kind = core::BlockKind::Entry;
    } else if (!has(mod, false) && mod.calls.empty()) {
      mod.kind = core::BlockKind::Shared;
    }
  }

  // 6. Groups from areas: a leading src/ is dropped, chains of single subfolders fold.
  Node tree;
  for (auto m : live) {
    auto parts = core::split(mods[m].area, '/');
    if (!parts.empty()) {
      auto first = core::to_lower(parts.front());
      if (first == "src" || first == "source" || first == "sources") parts.erase(parts.begin());
    }
    Node* n = &tree;
    for (const auto& p : parts) {
      auto key = core::to_ident(p);
      n = &n->groups[key.empty() ? "group" : key];
    }
    n->modules.push_back(m);
  }
  collapse(tree);
  while (tree.modules.empty() && tree.groups.size() == 1) {
    Node inner = std::move(tree.groups.begin()->second);
    tree = std::move(inner);
  }

  // 7. Ids (existing groups are reused, clashes get a suffix) and cards.
  std::set<std::string> taken;
  for (const auto& b : schema->blocks) taken.insert(b.id);
  auto unique_id = [&](const std::string& base) {
    std::string id = base;
    for (int n = 2; taken.contains(id); ++n) id = fmt::format("{}_{}", base, n);
    taken.insert(id);
    return id;
  };
  auto scalar = [](std::string v) { return core::Scalar{std::move(v), {}, 0, 0, true}; };
  std::vector<std::pair<std::string, std::size_t>> group_cards;  // (id, index in plan.blocks)
  std::function<void(const Node&, const std::string&)> emit = [&](const Node& n, const std::string& prefix) {
    for (const auto& [key, g] : n.groups) {
      std::string want = prefix.empty() ? key : prefix + "." + key;
      const auto* existing = schema->find(want);
      std::string id;
      if (existing != nullptr && existing->kind == core::BlockKind::Group) {
        id = want;
      } else {
        id = unique_id(want);
        core::Block b;
        b.id = id;
        b.kind = core::BlockKind::Group;
        b.kind_text = scalar("group");
        group_cards.emplace_back(id, plan.blocks.size());
        plan.blocks.push_back(std::move(b));
        ++plan.groups;
      }
      emit(g, id);
    }
    for (auto m : n.modules) mods[m].id = unique_id(prefix.empty() ? mods[m].name : prefix + "." + mods[m].name);
  };
  emit(tree, "");

  for (auto m : live) {
    const auto& mod = mods[m];
    core::Block b;
    b.id = mod.id;
    b.kind = mod.kind;
    b.kind_text = scalar(std::string(core::to_string(mod.kind)));
    b.thread = scalar("main");
    core::FilesDecl fd;
    for (auto i : mod.files) {
      fd.paths.push_back(files[i].path);
      plan.files.push_back(files[i].path);
    }
    b.files.push_back(std::move(fd));
    // Purpose: the first comment of the header, then of the source; otherwise a placeholder.
    std::string purpose;
    for (bool header : {true, false}) {
      for (auto i : mod.files) {
        if (purpose.empty() && files[i].header == header) {
          const auto* lang = core::language_of(files[i].path);
          purpose = comment_purpose(files[i].split || lang == nullptr ? files[i].text : lead_comment(*lang, files[i].text));
        }
      }
    }
    bool placeholder = purpose.empty();
    if (placeholder) {
      std::string main_file = files[mod.files.front()].path;
      if (mod.kind == core::BlockKind::Entry) {
        purpose = fmt::format("entry point: main() in {}", main_file);
      } else if (!mod.types.empty()) {
        std::vector<std::string> t(mod.types.begin(), mod.types.begin() + static_cast<long>(std::min<std::size_t>(mod.types.size(), 3)));
        purpose = fmt::format("{} ({})", core::join(t, ", "), main_file);
      } else {
        purpose = fmt::format("code of {}", main_file);
      }
    }
    b.purpose = scalar(purpose);
    // Ports: functions other blocks call come first, then the rest in header order.
    if (mod.kind != core::BlockKind::Entry) {
      std::set<std::string> needed;
      for (auto other : live) {
        for (const auto& [t, port] : mods[other].calls) {
          if (t == m) needed.insert(port);
        }
      }
      std::vector<std::string> chosen;
      for (const auto& p : mod.port_order) {
        if (needed.contains(p)) chosen.push_back(p);
      }
      std::vector<std::string> rest;
      for (const auto& p : mod.port_order) {
        if (needed.contains(p)) continue;
        if (chosen.size() < opts.max_ports) {
          chosen.push_back(p);
        } else {
          rest.push_back(p);
        }
      }
      for (const auto& p : mod.port_order) {
        if (std::find(chosen.begin(), chosen.end(), p) == chosen.end()) continue;
        const auto* s = mod.port_of.at(p);
        auto sig = decode_signature(s->signature);
        core::Port port;
        port.dir = core::PortDir::Fn;
        port.name = p;
        for (const auto& [type, name] : sig.params) {
          auto t = card_type(type);
          port.params.push_back(type_ok(t) ? t : (name.empty() ? std::string("arg") : name));
        }
        // A language without types in the facts: as many "any" as the code has parameters (none when
        // it does not say, like a shell function or subcommand).
        if (s->signature.empty()) {
          for (int k = 0; k < s->params; ++k) port.params.emplace_back("any");
        }
        auto ret = card_type(sig.ret);
        if (type_ok(ret) && ret != "void") port.ret = ret;
        b.ports.push_back(std::move(port));
      }
      if (!rest.empty()) {
        std::vector<std::string> shown(rest.begin(), rest.begin() + static_cast<long>(std::min<std::size_t>(rest.size(), 8)));
        b.notes.push_back(core::TextItem{fmt::format("{} more public functions: {}{}", rest.size(), core::join(shown, ", "),
                                                     rest.size() > shown.size() ? ", …" : ""),
                                         {}, 0, 0});
      }
    } else if (!mod.port_order.empty()) {
      b.notes.push_back(core::TextItem{"public functions of the entry module are not ports: " + core::join(mod.port_order, ", "), {}, 0, 0});
    }
    if (!mod.types.empty()) b.notes.push_back(core::TextItem{"types: " + core::join(mod.types, ", "), {}, 0, 0});
    if (placeholder) b.notes.push_back(core::TextItem{"purpose was not found in comments: describe the block", {}, 0, 0});
    // Links.
    std::set<std::size_t> called;
    for (const auto& [t, port] : mod.calls) {
      if (mod.kind == core::BlockKind::Shared) break;
      b.calls.push_back(core::CallDecl{core::PortRef{mods[t].id, port}, "", "", "", {}, 0, 0});
      called.insert(t);
      ++plan.links;
    }
    for (auto t : mod.includes) {
      if (called.contains(t)) continue;
      b.uses.push_back(core::UseDecl{mods[t].id, {}, 0, 0});
      ++plan.links;
    }
    ++plan.leaves;
    plan.blocks.push_back(std::move(b));
  }
  // Group purposes: the folder and the names inside.
  for (const auto& [gid, idx] : group_cards) {
    std::vector<std::string> kids;
    std::string folder;
    for (auto m : live) {
      if (core::parent_id(mods[m].id) != gid && !core::Schema::is_under(gid, mods[m].id)) continue;
      if (core::parent_id(mods[m].id) == gid) kids.push_back(core::last_segment(mods[m].id));
      if (folder.empty()) {
        folder = mods[m].area;
      } else {
        while (!folder.empty() && !(mods[m].area == folder || mods[m].area.starts_with(folder + "/"))) {
          auto slash = folder.rfind('/');
          folder = slash == std::string::npos ? "" : folder.substr(0, slash);
        }
      }
    }
    for (const auto& [other, oi] : group_cards) {
      if (core::parent_id(other) == gid) kids.insert(kids.begin(), core::last_segment(other));
    }
    std::vector<std::string> shown(kids.begin(), kids.begin() + static_cast<long>(std::min<std::size_t>(kids.size(), 5)));
    plan.blocks[idx].purpose = scalar(fmt::format("{}: {}{}", folder.empty() ? "project root" : folder, core::join(shown, ", "),
                                                  kids.size() > shown.size() ? ", …" : ""));
  }
  if (entry == SIZE_MAX && !schema_has_entry) {
    core::Block b;
    b.id = unique_id("app");
    b.kind = core::BlockKind::Entry;
    b.kind_text = scalar("entry");
    b.thread = scalar("main");
    b.purpose = scalar("composition root: no main() was found in the imported code");
    plan.blocks.push_back(std::move(b));
    plan.warnings.push_back("no main() found: added an empty entry block 'app' (the schema needs exactly one)");
  }
  std::stable_sort(plan.blocks.begin(), plan.blocks.end(), [](const core::Block& a, const core::Block& b) { return a.id < b.id; });
  for (const auto& b : plan.blocks) {
    auto text = core::write_block(b);
    for (const auto& line : core::split(text, '\n')) plan.diff.push_back(core::DiffLine{'+', line, 0});
  }
  std::sort(plan.files.begin(), plan.files.end());

  // 8. Hints about project.ksl written by `karkas init` before the import.
  auto cxx = cmake_std(root);
  if (!cxx.empty() && schema->project.std.set && schema->project.std.value != "c++" + cxx) {
    plan.warnings.push_back(fmt::format("project.ksl says std {}, but CMakeLists.txt sets C++{}: fix project.ksl",
                                        schema->project.std.value, cxx));
  }
  for (const auto& r : schema->project.rules) {
    if (r.text == "blocks talk only through api/ headers" || r.text == "no exceptions across block boundaries, use Result<T>") {
      plan.warnings.push_back("project.ksl rule '" + r.text + "' comes from karkas init and may not hold for the existing code");
    }
  }
  return plan;
}

std::string describe_plan(const ImportPlan& plan) {
  std::string out;
  int entries = 0;
  int shared = 0;
  for (const auto& b : plan.blocks) {
    entries += b.kind == core::BlockKind::Entry ? 1 : 0;
    shared += b.kind == core::BlockKind::Shared ? 1 : 0;
  }
  out += fmt::format("{} groups, {} blocks with code (entry {}, shared {}), {} links, {} files\n\n", plan.groups, plan.leaves,
                     entries, shared, plan.links, plan.files.size());
  for (const auto& b : plan.blocks) {
    std::string indent(static_cast<std::size_t>(2 * (core::id_depth(b.id) - 1)), ' ');
    std::string head = indent + b.id;
    std::string kind(b.kind_name());
    if (b.kind == core::BlockKind::Group) {
      out += fmt::format("{:<34} {:<6} {}\n", head, kind, b.purpose.value);
      continue;
    }
    int fns = 0;
    for (const auto& p : b.ports) fns += p.dir == core::PortDir::Fn ? 1 : 0;
    out += fmt::format("{:<34} {:<6} fn {:<2} call {:<2} use {:<2} {}\n", head, kind, fns, b.calls.size(), b.uses.size(),
                       core::join(b.file_list(), " "));
  }
  if (!plan.skipped.empty()) out += "\nskipped folders: " + core::join(plan.skipped, ", ") + "\n";
  if (!plan.tests.empty()) {
    std::vector<std::string> shown(plan.tests.begin(), plan.tests.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(plan.tests.size(), 4)));
    out += fmt::format("\ntest programs left to the project's own suite: {} ({}{})\n", plan.tests.size(), core::join(shown, ", "),
                       plan.tests.size() > shown.size() ? ", ..." : "");
  }
  if (!plan.warnings.empty()) {
    out += "\nwarnings:\n";
    for (const auto& w : plan.warnings) out += "  " + w + "\n";
  }
  return out;
}

}  // namespace kk::code
