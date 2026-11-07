#include "kk/code/gen.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <set>

#include "kk/code/analyzer.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/index/hash.hpp"
#include "kk/index/index.hpp"
#include "kk/templates.hpp"

namespace kk::code {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kHashPrefix = "# karkas-hash: ";

const std::set<std::string>& reserved_targets() {
  static const std::set<std::string> s = {"all", "clean", "install", "test", "help", "package", "edit_cache",
                                          "rebuild_cache", "list_install_components", "kk_doctest", "kk_doctest_main"};
  return s;
}

std::string tname(std::string_view id) {
  auto t = core::target_name(id);
  return reserved_targets().contains(t) ? "blk_" + t : t;
}

bool has_files(const fs::path& dir, std::string_view ext_set) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
  for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec) && core::contains(ext_set, it->path().extension().string() + ";")) return true;
  }
  return false;
}

bool has_cpp(const fs::path& dir) { return has_files(dir, ".cpp;.cc;.cxx;"); }

std::string labels(const core::Schema& s, const std::string& id) {
  std::vector<std::string> l = {id};
  for (const auto& a : s.ancestors(id)) l.push_back(a);
  return core::join(l, ";");
}

// Targets a block links to: one per link of the consumer card (5.5), deduplicated.
std::vector<std::string> deps_of(const core::Schema& s, const core::Block& b) {
  std::set<std::string> out;
  for (const auto& id : allowed_api_blocks(s, b.id)) {
    const auto* x = s.find(id);
    if (x != nullptr && x->has_code()) out.insert(tname(id));
  }
  return {out.begin(), out.end()};
}

kk::Result<bool> write_if_changed(const fs::path& p, const std::string& content) {
  if (fs::exists(p)) {
    auto old = core::read_file(p);
    if (old && *old == content) return false;
  }
  if (auto r = core::write_file(p, content); !r) return std::unexpected(r.error());
  return true;
}

void record(GenResult& g, const core::Schema& s, const fs::path& p, bool changed) {
  (changed ? g.written : g.unchanged).push_back(core::rel_str(s.root, p));
}

}  // namespace

kk::Result<std::string> target_template(std::string_view name, const std::vector<std::pair<std::string, std::string>>& vars) {
  auto t = templates::get("target/" + std::string(name));
  if (!t) return kk::fail("templates", fmt::format("missing template target/{}", name));
  return core::render_template(*t, vars);
}

// --- CMake (5.5) --------------------------------------------------------------------------

std::string gen_cmake(const core::Schema& s) {
  std::string std_ver = "20";
  if (s.project.std.set) {
    auto v = s.project.std.value;
    auto pos = v.find_first_of("0123456789");
    if (pos != std::string::npos) std_ver = v.substr(pos);
  }
  std::string body;
  body += target_template("cmake_preamble.cmake", {{"project", s.project.name}, {"std", std_ver}}).value_or("");
  const auto& dirs = s.project.dirs;
  auto rel = [&](const core::Block& b, const std::string& sub) { return "${CMAKE_CURRENT_SOURCE_DIR}/" + b.dir + "/" + sub; };
  std::vector<std::string> leaf_targets;
  for (const auto& b : s.blocks) {
    if (b.kind != core::BlockKind::Leaf && b.kind != core::BlockKind::Shared) continue;
    if (b.external()) continue;  // imported code: the project builds it itself (docs/adr/0009)
    auto t = tname(b.id);
    leaf_targets.push_back(t);
    auto deps = deps_of(s, b);
    bool impl = has_cpp(s.root / b.dir / dirs.impl);
    body += fmt::format("\n# --- {} ({}): {}\n", b.id, b.kind_name(), b.purpose.value);
    if (impl) {
      body += fmt::format("file(GLOB_RECURSE _src CONFIGURE_DEPENDS \"{}/*.cpp\" \"{}/*.cc\")\n", rel(b, dirs.impl), rel(b, dirs.impl));
      body += fmt::format("add_library({} STATIC ${{_src}})\n", t);
      body += fmt::format("target_include_directories({} PUBLIC \"{}\" PRIVATE \"{}\")\n", t, rel(b, dirs.api), rel(b, dirs.impl));
      if (!deps.empty()) body += fmt::format("target_link_libraries({} PRIVATE {})\n", t, core::join(deps, " "));
      body += fmt::format("kk_probes({})\n", t);
    } else {
      body += fmt::format("add_library({} INTERFACE)\n", t);
      body += fmt::format("target_include_directories({} INTERFACE \"{}\")\n", t, rel(b, dirs.api));
      if (!deps.empty()) body += fmt::format("target_link_libraries({} INTERFACE {})\n", t, core::join(deps, " "));
    }
    auto tests_dir = s.root / b.dir / dirs.tests;
    if (has_cpp(tests_dir)) {
      body += fmt::format("file(GLOB_RECURSE _tests CONFIGURE_DEPENDS \"{}/*.cpp\")\n", rel(b, dirs.tests));
      body += fmt::format("add_executable({}_tests ${{_tests}})\n", t);
      body += fmt::format("target_include_directories({}_tests PRIVATE \"{}\" \"{}\")\n", t, rel(b, dirs.mocks), rel(b, dirs.impl));
      std::vector<std::string> tdeps = {t};
      tdeps.insert(tdeps.end(), deps.begin(), deps.end());
      tdeps.push_back("kk_doctest_main");
      body += fmt::format("target_link_libraries({}_tests PRIVATE {})\n", t, core::join(tdeps, " "));
      body += fmt::format("kk_probes({}_tests)\n", t);
      for (const char* kind : {"contract", "unit"}) {
        if (!has_cpp(tests_dir / kind)) continue;
        body += fmt::format("add_test(NAME {}/{} COMMAND {}_tests \"-sf=*/{}/{}/*\")\n", b.id, kind, t, dirs.tests, kind);
        body += fmt::format("set_tests_properties({}/{} PROPERTIES LABELS \"{}\")\n", b.id, kind, labels(s, b.id));
      }
    }
  }
  for (const auto& b : s.blocks) {
    if (b.kind != core::BlockKind::Group) continue;
    auto t = tname(b.id);
    std::vector<std::string> kids;
    for (const auto& c : b.children) {
      const auto* cb = s.find(c);
      if (cb != nullptr && cb->kind != core::BlockKind::Entry) kids.push_back(tname(c));
    }
    body += fmt::format("\n# --- {} (group): {}\nadd_library({} INTERFACE)\n", b.id, b.purpose.value, t);
    if (!kids.empty()) body += fmt::format("target_link_libraries({} INTERFACE {})\n", t, core::join(kids, " "));
    auto integ = s.root / "tests" / "integration" / b.id;
    if (has_cpp(integ)) {
      std::set<std::string> deps = {t};
      for (const auto& a : allowed_api_blocks(s, b.id)) {
        const auto* x = s.find(a);
        if (x != nullptr && x->has_code()) deps.insert(tname(a));
      }
      body += fmt::format("file(GLOB_RECURSE _itests CONFIGURE_DEPENDS \"${{CMAKE_CURRENT_SOURCE_DIR}}/tests/integration/{}/*.cpp\")\n", b.id);
      body += fmt::format("add_executable({}_integration ${{_itests}})\n", t);
      body += fmt::format("target_link_libraries({}_integration PRIVATE {} kk_doctest_main)\n", t,
                          core::join({deps.begin(), deps.end()}, " "));
      body += fmt::format("add_test(NAME {}/integration COMMAND {}_integration)\n", b.id, t);
      body += fmt::format("set_tests_properties({}/integration PROPERTIES LABELS \"{}\")\n", b.id, labels(s, b.id));
    }
  }
  for (const auto& b : s.blocks) {
    if (b.kind != core::BlockKind::Entry) continue;
    if (b.external()) continue;  // imported code: the project builds it itself (docs/adr/0009)
    auto t = tname(b.id);
    body += fmt::format("\n# --- {} (entry): {}\n", b.id, b.purpose.value);
    if (has_cpp(s.root / b.dir / dirs.impl)) {
      body += fmt::format("file(GLOB_RECURSE _app CONFIGURE_DEPENDS \"{}/*.cpp\")\n", rel(b, dirs.impl));
      body += fmt::format("add_executable({} ${{_app}})\n", t);
      body += fmt::format("set_target_properties({} PROPERTIES OUTPUT_NAME {})\n", t, s.project.name);
      body += fmt::format("target_include_directories({} PRIVATE \"{}\" \"{}\")\n", t, rel(b, dirs.api), rel(b, dirs.impl));
      if (!leaf_targets.empty()) body += fmt::format("target_link_libraries({} PRIVATE {})\n", t, core::join(leaf_targets, " "));
      body += fmt::format("kk_probes({})\n", t);
    }
  }
  body += "\ninclude(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/user.cmake\" OPTIONAL)\n";
  std::string head =
      "# Generated by `karkas gen cmake` from the schema (5.5). Do not edit: manual changes are\n"
      "# reported as drift D07. Put additions into cmake/user.cmake.\n";
  return head + std::string(kHashPrefix) + index::xxh3_hex(body) + "\n" + body;
}

CmakeHashes cmake_hashes(std::string_view text) {
  CmakeHashes h;
  auto pos = text.find(kHashPrefix);
  if (pos == std::string_view::npos) return h;
  auto eol = text.find('\n', pos);
  if (eol == std::string_view::npos) return h;
  h.generated = true;
  h.declared = std::string(core::trim(text.substr(pos + kHashPrefix.size(), eol - pos - kHashPrefix.size())));
  h.actual = index::xxh3_hex(text.substr(eol + 1));
  return h;
}

kk::Result<GenResult> write_cmake(const core::Schema& s) {
  GenResult g;
  auto p = s.root / "CMakeLists.txt";
  if (fs::exists(p)) {
    auto old = core::read_file(p);
    if (old && !cmake_hashes(*old).generated) {
      return kk::fail("gen", "CMakeLists.txt exists and was not generated by karkas; move it to cmake/user.cmake first");
    }
  }
  auto ch = write_if_changed(p, gen_cmake(s));
  if (!ch) return std::unexpected(ch.error());
  record(g, s, p, *ch);
  auto user = s.root / "cmake" / "user.cmake";
  if (!fs::exists(user)) {
    auto t = target_template("user.cmake");
    if (!t) return std::unexpected(t.error());
    if (auto r = core::write_file(user, *t); !r) return std::unexpected(r.error());
    g.written.push_back("cmake/user.cmake");
  }
  return g;
}

// --- mocks (8.3) ---------------------------------------------------------------------------

namespace {

struct Interface {
  std::string qualified;  // wk::IReader
  std::string name;       // IReader
  std::string header;     // include path relative to the api root
  std::vector<index::SymbolFact> methods;
};

std::vector<Interface> interfaces_of(const core::Schema& s, const core::Block& b, CppAnalyzer& an) {
  std::vector<Interface> out;
  const auto& dirs = s.project.dirs;
  auto api = s.root / b.dir / dirs.api;
  std::error_code ec;
  if (!fs::is_directory(api, ec)) return out;
  std::vector<fs::path> headers;
  for (const auto& e : fs::recursive_directory_iterator(api, ec)) {
    if (e.is_regular_file(ec) && core::is_code_file(e.path())) headers.push_back(e.path());
  }
  std::sort(headers.begin(), headers.end());
  for (const auto& h : headers) {
    auto text = core::read_file(h);
    if (!text) continue;
    index::CodeFile f{core::rel_str(s.root, h), b.id, "api", "", 0, false};
    auto facts = an.analyze(f, *text);
    std::map<std::string, Interface> by_scope;
    for (const auto& sym : facts.symbols) {
      if (sym.kind != "method" || !sym.pure_virtual) continue;
      auto& itf = by_scope[sym.scope];
      itf.qualified = sym.scope;
      itf.name = core::split(sym.scope, ':').back();
      itf.header = core::rel_str(api, h);
      itf.methods.push_back(sym);
    }
    for (auto& [_, itf] : by_scope) out.push_back(std::move(itf));
  }
  return out;
}

std::string mock_text(const core::Block& owner, const std::vector<Interface>& itfs) {
  std::set<std::string> headers;
  for (const auto& i : itfs) headers.insert(i.header);
  std::string out = fmt::format(
      "// {} — mocks of the {} api/, generated by `karkas gen mocks`.\n"
      "// Regenerated on every run; remove the marker line above to keep manual edits.\n"
      "#pragma once\n#include <functional>\n#include <string>\n#include <type_traits>\n#include <vector>\n\n",
      kGeneratedMarker, owner.id);
  for (const auto& h : headers) out += fmt::format("#include \"{}\"\n", h);
  std::set<std::string> namespaces;
  for (const auto& i : itfs) {
    auto pos = i.qualified.rfind("::");
    if (pos != std::string::npos && pos > 0) namespaces.insert(i.qualified.substr(0, pos));
  }
  out += "\nnamespace kkmock {\n\n";
  for (const auto& ns : namespaces) out += fmt::format("using namespace ::{};  // types of the mocked api\n", ns);
  if (!namespaces.empty()) out += "\n";
  out +=
      "#ifndef KKMOCK_DEFAULT\n#define KKMOCK_DEFAULT\n"
      "// Value returned by a mocked call without a configured handler.\n"
      "template <class T>\nT kk_default() {\n"
      "  if constexpr (std::is_void_v<T>) {\n    return;\n"
      "  } else if constexpr (std::is_reference_v<T>) {\n    static std::remove_cvref_t<T> v{};\n    return v;\n"
      "  } else {\n    return T{};\n  }\n}\n#endif\n";
  for (const auto& itf : itfs) {
    out += fmt::format("\n// Mock of {} ({}): records calls, answers through on_<method>.\n", itf.qualified, owner.id);
    out += fmt::format("class Mock{} : public {} {{\n public:\n  mutable std::vector<std::string> calls;\n", itf.name,
                       itf.qualified.starts_with("::") ? itf.qualified : "::" + itf.qualified);
    std::map<std::string, int> seen;
    std::string handlers;
    std::string methods;
    for (const auto& m : itf.methods) {
      auto sig = decode_signature(m.signature);
      int n = ++seen[m.name];
      std::string handler = n == 1 ? "on_" + m.name : fmt::format("on_{}_{}", m.name, n);
      std::vector<std::string> types, decl, args;
      for (std::size_t i = 0; i < sig.params.size(); ++i) {
        auto pname = sig.params[i].second.empty() ? fmt::format("a{}", i) : sig.params[i].second;
        types.push_back(sig.params[i].first);
        decl.push_back(sig.params[i].first + " " + pname);
        args.push_back(pname);
      }
      std::string ret = sig.ret.empty() ? "void" : sig.ret;
      handlers += fmt::format("  std::function<{}({})> {};\n", ret, core::join(types, ", "), handler);
      std::string quals = std::string(sig.is_const ? " const" : "") + (sig.is_noexcept ? " noexcept" : "");
      methods += fmt::format("  {} {}({}){} override {{\n    calls.push_back(\"{}\");\n", ret, m.name, core::join(decl, ", "),
                             quals, m.name);
      if (ret == "void") {
        methods += fmt::format("    if ({}) {}({});\n  }}\n", handler, handler, core::join(args, ", "));
      } else {
        methods += fmt::format("    if ({}) return {}({});\n    return kk_default<{}>();\n  }}\n", handler, handler,
                               core::join(args, ", "), ret);
      }
    }
    out += handlers + methods + "};\n";
  }
  out += "\n}  // namespace kkmock\n";
  return out;
}

}  // namespace

kk::Result<GenResult> write_mocks(const core::Schema& s, std::string_view block) {
  GenResult g;
  CppAnalyzer an;
  for (const auto& b : s.blocks) {
    if (b.kind != core::BlockKind::Leaf) continue;
    if (!block.empty() && block != "all" && !core::Schema::is_under(block, b.id)) continue;
    if (b.external()) continue;  // imported code: the project builds it itself (docs/adr/0009)
    for (const auto& n : allowed_api_blocks(s, b.id)) {
      const auto* nb = s.find(n);
      if (nb == nullptr || nb->kind == core::BlockKind::Shared || !nb->has_code()) continue;
      auto itfs = interfaces_of(s, *nb, an);
      if (itfs.empty()) continue;
      auto path = s.root / b.dir / s.project.dirs.mocks / (tname(n) + "_mock.hpp");
      if (fs::exists(path)) {
        auto old = core::read_file(path);
        if (old && !core::contains(*old, kGeneratedMarker)) {
          g.skipped.push_back(core::rel_str(s.root, path));
          continue;
        }
      }
      auto ch = write_if_changed(path, mock_text(*nb, itfs));
      if (!ch) return std::unexpected(ch.error());
      record(g, s, path, *ch);
    }
  }
  return g;
}

// --- probe (12.5) ------------------------------------------------------------------------

kk::Result<GenResult> write_probe(const core::Schema& s) {
  GenResult g;
  auto text = target_template("karkas_probe.hpp");
  if (!text) return std::unexpected(text.error());
  const auto* probe = s.find("shared.probe");
  fs::path dir = probe != nullptr ? s.root / probe->dir : s.blocks_root() / "shared" / "probe";
  if (probe == nullptr) {
    auto group = s.blocks_root() / "shared" / "block.ksl";
    if (!fs::exists(group)) {
      if (auto r = core::write_file(group, "@block shared\npurpose common code used by many blocks\nkind    group\n"); !r) {
        return std::unexpected(r.error());
      }
      g.written.push_back(core::rel_str(s.root, group));
    }
    auto card = dir / "block.ksl";
    if (auto r = core::write_file(card, "@block shared.probe\npurpose Karkas probes (generated)\nkind    shared\nthread  any\n"
                                        "inv     KK_PROBE never blocks the caller\n");
        !r) {
      return std::unexpected(r.error());
    }
    g.written.push_back(core::rel_str(s.root, card));
  }
  auto path = dir / s.project.dirs.api / "karkas_probe.hpp";
  auto ch = write_if_changed(path, *text);
  if (!ch) return std::unexpected(ch.error());
  record(g, s, path, *ch);
  return g;
}

// --- skeletons --------------------------------------------------------------------------------

kk::Result<GenResult> write_skel(const core::Schema& s, std::string_view block) {
  GenResult g;
  const auto& dirs = s.project.dirs;
  auto touch = [&](const fs::path& p) -> kk::VoidResult {
    if (fs::exists(p)) return {};
    if (auto r = core::write_file(p, ""); !r) return r;
    g.written.push_back(core::rel_str(s.root, p));
    return {};
  };
  for (const auto& b : s.blocks) {
    if (!b.has_code() || b.kind == core::BlockKind::Shared) continue;
    if (b.external()) continue;  // imported code: the project builds it itself (docs/adr/0009)
    if (!block.empty() && block != "all" && !core::Schema::is_under(block, b.id)) continue;
    auto root = s.root / b.dir;
    std::string includes;
    std::error_code ec;
    std::vector<std::string> hs;
    for (const auto& e : fs::recursive_directory_iterator(root / dirs.api, ec)) {
      if (e.is_regular_file(ec) && core::is_code_file(e.path())) hs.push_back(core::rel_str(root / dirs.api, e.path()));
    }
    std::sort(hs.begin(), hs.end());
    if (b.kind == core::BlockKind::Entry) {
      std::vector<std::string> all;
      for (const auto& x : s.blocks) {
        if (!x.has_code() || x.id == b.id) continue;
        for (const auto& e : fs::recursive_directory_iterator(s.root / x.dir / dirs.api, ec)) {
          if (e.is_regular_file(ec) && core::is_code_file(e.path())) all.push_back(core::rel_str(s.root / x.dir / dirs.api, e.path()));
        }
      }
      std::sort(all.begin(), all.end());
      for (const auto& h : all) includes += fmt::format("#include \"{}\"\n", h);
      if (!has_cpp(root / dirs.impl)) {
        auto m = target_template("skel_main.cpp", {{"block", b.id}});
        auto c = target_template("skel_compose.cpp", {{"project", s.project.name}, {"includes", includes}});
        if (!m || !c) return kk::fail("templates", "missing entry skeleton templates");
        if (auto r = core::write_file(root / dirs.impl / "main.cpp", *m); !r) return std::unexpected(r.error());
        if (auto r = core::write_file(root / dirs.impl / "compose.cpp", *c); !r) return std::unexpected(r.error());
        g.written.push_back(core::rel_str(s.root, root / dirs.impl / "main.cpp"));
        g.written.push_back(core::rel_str(s.root, root / dirs.impl / "compose.cpp"));
      }
      continue;
    }
    for (const auto& h : hs) includes += fmt::format("#include \"{}\"\n", h);
    if (!has_cpp(root / dirs.impl)) {
      auto t = target_template("skel_impl.cpp", {{"block", b.id}, {"includes", includes}});
      if (!t) return std::unexpected(t.error());
      auto p = root / dirs.impl / (core::last_segment(b.id) + ".cpp");
      if (auto r = core::write_file(p, *t); !r) return std::unexpected(r.error());
      g.written.push_back(core::rel_str(s.root, p));
    }
    for (const auto& sub : {fs::path(dirs.tests) / "contract" / ".gitkeep", fs::path(dirs.tests) / "unit" / ".gitkeep",
                            fs::path(dirs.mocks) / ".gitkeep"}) {
      if (auto r = touch(root / sub); !r) return std::unexpected(r.error());
    }
  }
  return g;
}

}  // namespace kk::code
