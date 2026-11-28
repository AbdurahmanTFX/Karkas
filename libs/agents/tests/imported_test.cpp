// Agents inside imported blocks (ADR 0027): the public part of a header as the contract, the
// sandbox over the files of a card, the tools of a worker there.
#include <doctest/doctest.h>

#include <filesystem>
#include <nlohmann/json.hpp>

#include "kk/agents/changes.hpp"
#include "kk/agents/engine.hpp"
#include "kk/agents/sandbox.hpp"
#include "kk/agents/tools.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/test/support.hpp"
#include "scripted.hpp"

using namespace kk;
using json = nlohmann::json;

namespace {

// The legacy fixture with hand-written cards over its existing files (as in code/tests/imported_test).
struct Legacy : test::FixtureCopy {
  Legacy() : FixtureCopy("legacy") {
    card("game", "purpose game objects\nkind group\n");
    card("game.main", "purpose entry point\nkind entry\nthread main\nfiles src/game/main.cpp\n"
                      "call game.world:update\ncall game.world:draw\ncall game.player:update\n"
                      "call game.player:position\ncall game.player:bag\ncall util.camera:follow\n");
    card("game.world", "purpose the island\nkind leaf\nthread main\nfiles src/game/world.cpp src/game/hpp/world.hpp\n"
                       "fn update(float) -> void\nfn is_walkable(Vec2) -> bool\nfn draw() -> void\ncall util.math:clampf\n");
    card("game.player", "purpose the player\nkind leaf\nthread main\nfiles src/game/player.cpp src/game/hpp/player.hpp\n"
                        "fn update(float, World) -> void\nfn position() -> Vec2\nfn bag() -> Inventory\n"
                        "call game.world:is_walkable\nuse game.inventory\nuse util.math\n");
    card("game.inventory", "purpose items\nkind shared\nfiles src/game/hpp/inventory.hpp\n");
    card("util", "purpose helpers\nkind group\n");
    card("util.camera", "purpose camera\nkind leaf\nthread main\nfiles src/util/camera.cpp src/util/hpp/camera.hpp\n"
                        "fn follow(Vec2, float) -> void\nfn screen_to_world(Vec2) -> Vec2\nfn zoom() -> float\n"
                        "call util.math:clampf\n");
    card("util.math", "purpose math\nkind leaf\nthread main\nfiles src/util/math.c src/util/math.h\n"
                      "fn clampf(float, float, float) -> float\n");
  }
  void card(const std::string& id, const std::string& body) {
    test::write(path() / "blocks" / core::id_to_rel_path(id) / "block.ksl", "@block " + id + "\n" + body);
  }
  core::Schema schema() {
    auto s = core::load_schema(path());
    REQUIRE_MESSAGE(s, (s ? "" : s.error().str()));
    return *s;
  }
};

const std::string kPlayer = R"(#pragma once
#include <string>

// The player.
class Player {
public:
    Player() : score_(0), name_{"p"} {}
    void Update(float dt);
    int Score() const { return score_; }
    Player &operator=(const Player &) = default;
    static const std::string NAME;
    static constexpr int kMax = 5;

private:
    int score_ = 0;
    std::string name_;
};

inline int Twice(int x) { return 2 * x; }
)";

bool kept(const std::string& before, const std::string& after, std::string* why = nullptr) {
  return agents::header_contract_kept(before, after, why);
}

std::string with(std::string text, const std::string& old, const std::string& now) {
  auto at = text.find(old);
  REQUIRE(at != std::string::npos);
  return text.replace(at, old.size(), now);
}

}  // namespace

// --- the public part of a header is the contract -------------------------------------------------

TEST_CASE("header contract: private members, bodies, initial values, comments, includes and constants are free") {
  std::string after = with(kPlayer, "#include <string>\n", "#include <string>\n#include <optional>\n");
  after = with(after, "// The player.", "// The player, who walks.");
  after = with(after, "int Score() const { return score_; }", "int Score() const\n    {\n        return score_ + bonus_;\n    }");
  after = with(after, "Player() : score_(0), name_{\"p\"} {}", "Player() : score_(1), name_{\"q\"}, bonus_(2) {}");
  after = with(after, "static constexpr int kMax = 5;", "static constexpr int kMax = 7;\n    static const std::string NICKNAME;");
  after = with(after, "    int score_ = 0;\n", "    void Helper();\n    int score_ = 3;\n    int bonus_ = 0;\n    struct Cache { int a; };\n");
  after = with(after, "inline int Twice(int x) { return 2 * x; }", "inline int Twice(int x) { return x + x; }");
  std::string why;
  CHECK_MESSAGE(kept(kPlayer, after, &why), why);
  // The same header is kept, of course.
  CHECK(kept(kPlayer, kPlayer));
  // A class's default access is private: a field before any label is free; a struct's is public.
  CHECK(kept("class A {\n  int x;\npublic:\n  void f();\n};\n", "class A {\n  int x;\n  int y;\npublic:\n  void f();\n};\n"));
}

TEST_CASE("header contract: a new, changed or removed public declaration is a contract change") {
  std::string why;
  CHECK_FALSE(kept(kPlayer, with(kPlayer, "public:\n", "public:\n    void Reset();\n"), &why));
  CHECK(core::contains(why, "`void Reset()` (in Player) is a new public declaration"));
  CHECK_FALSE(kept(kPlayer, with(kPlayer, "void Update(float dt);", "void Update(float dt, bool fast);"), &why));
  CHECK(core::contains(why, "`void Update(float dt)` (in Player) was removed or changed"));
  CHECK_FALSE(kept(kPlayer, with(kPlayer, "    int Score() const { return score_; }\n", ""), &why));
  CHECK_FALSE(kept(kPlayer, with(kPlayer, "static const std::string NAME;", "static const std::string NAMES;")));
  CHECK_FALSE(kept(kPlayer, with(kPlayer, "class Player {", "class Player final {")));
  CHECK_FALSE(kept(kPlayer, with(kPlayer, "inline int Twice(int x)", "inline int Twice(long x)")));
  CHECK_FALSE(kept(kPlayer, kPlayer + "int Thrice(int x);\n", &why));
  CHECK(core::contains(why, "`int Thrice(int x)` is a new public declaration"));
  // A public field of a struct, an enumerator, a public nested type.
  CHECK_FALSE(kept("struct V {\n  float x;\n};\n", "struct V {\n  float x;\n  float y;\n};\n"));
  CHECK_FALSE(kept("enum class Kind { A, B };\n", "enum class Kind { A, B, C };\n"));
  CHECK_FALSE(kept(kPlayer, with(kPlayer, "public:\n", "public:\n    struct Stats { int hits; };\n")));
  // A removed #include may break a user that relied on it.
  CHECK_FALSE(kept(kPlayer, with(kPlayer, "#include <string>\n", "")));
}

TEST_CASE("header contract: what an imported game's headers look like (Endless Sky)") {
  const std::string prefs = R"(class Preferences {
public:
	enum class VSync : int_fast8_t {
		off = 0,
		on,
		adaptive,
	};
	static void Load();
	static bool Has(const std::string &name);
	static const std::string FRUGAL_ESCORTS;
	static const std::string LANDING_ZOOM;

private:
	static std::map<std::string, Setting> settings;
};
)";
  std::string why;
  CHECK_MESSAGE(kept(prefs, with(prefs, "\tstatic const std::string LANDING_ZOOM;\n",
                                 "\tstatic const std::string LANDING_ZOOM;\n\tstatic const std::string LOW_FUEL_WARNING;\n"),
                     &why),
                why);
  const std::string engine = R"(class Engine {
public:
	explicit Engine(PlayerInfo &player);
	void Step(bool isActive);

private:
	bool wasActive = false;
	bool hadHostiles = false;
	std::vector<int> emptySoundsTimer;
};
)";
  CHECK_MESSAGE(kept(engine, with(engine, "\tbool hadHostiles = false;\n",
                                  "\tbool hadHostiles = false;\n\t// Whether the flagship had too little fuel.\n\tbool hadLowFuel = false;\n"),
                     &why),
                why);
}

// --- the sandbox of an imported block ------------------------------------------------------------

TEST_CASE("imported sandbox: a worker reads its files, the headers of linked blocks and unowned data") {
  Legacy p;
  test::write(p / "data/levels.txt", "level 1\n");
  test::write(p / ".env", "SECRET=1\n");
  agents::Sandbox sb(p.schema(), "game.player", agents::Role::Worker);
  CHECK(sb.imported());
  CHECK(sb.own_files() == std::vector<std::string>{"src/game/player.cpp", "src/game/hpp/player.hpp"});
  CHECK(sb.resolve_read("src/game/player.cpp"));
  CHECK(sb.resolve_read("src/game/hpp/player.hpp"));
  CHECK(sb.resolve_read("src/game/hpp/world.hpp"));      // call game.world
  CHECK(sb.resolve_read("src/game/hpp/inventory.hpp"));  // shared
  CHECK(sb.resolve_read("src/util/math.h"));             // use util.math
  CHECK(sb.resolve_read("blocks/game/player/block.ksl"));
  CHECK(sb.resolve_read("data/levels.txt"));  // a file of no block that is not code
  CHECK(sb.resolve_read("."));
  CHECK(sb.display(*sb.resolve_read("src/game/hpp/world.hpp")) == "src/game/hpp/world.hpp");
  auto neighbors = sb.neighbor_blocks();
  for (const char* n : {"game.world", "game.inventory", "util.math"}) {
    CHECK(std::find(neighbors.begin(), neighbors.end(), n) != neighbors.end());
  }
  auto src = sb.resolve_read("src/game/world.cpp");  // a linked block's source
  REQUIRE_FALSE(src);
  CHECK(core::contains(src.error().message, "belongs to block game.world"));
  auto cam = sb.resolve_read("src/util/hpp/camera.hpp");
  REQUIRE_FALSE(cam);
  CHECK(core::contains(cam.error().message, "belongs to block util.camera, which is not linked to game.player"));
  CHECK_FALSE(sb.resolve_read("third_party/tiny/tiny.cpp"));  // code of no block
  CHECK_FALSE(sb.resolve_read("blocks/game/world/block.ksl"));
  CHECK_FALSE(sb.resolve_read(".env"));
  CHECK_FALSE(sb.resolve_read("../legacy/src/game/player.cpp"));
  CHECK_FALSE(sb.resolve_read("/etc/passwd"));
}

TEST_CASE("imported sandbox: only the files of the card are written, headers by the access of the run") {
  Legacy p;
  test::write(p / "data/levels.txt", "level 1\n");
  agents::Sandbox sb(p.schema(), "game.player", agents::Role::Worker);
  CHECK(sb.resolve_write("src/game/player.cpp"));
  auto header = sb.resolve_write("src/game/hpp/player.hpp");  // no access given: read-only
  REQUIRE_FALSE(header);
  CHECK(core::contains(header.error().message, "read-only"));
  sb.set_api_access(agents::ApiAccess::Imported);
  REQUIRE(sb.resolve_write("src/game/hpp/player.hpp"));
  CHECK(sb.is_own_api(*sb.resolve_write("src/game/hpp/player.hpp")));
  CHECK_FALSE(sb.is_own_api(*sb.resolve_write("src/game/player.cpp")));
  auto other = sb.resolve_write("src/game/world.cpp");
  REQUIRE_FALSE(other);
  CHECK(core::contains(other.error().message, "src/game/world.cpp belongs to block game.world"));
  auto data = sb.resolve_write("data/levels.txt");
  REQUIRE_FALSE(data);
  CHECK(core::contains(data.error().message, "'~ @block game.player' and '+ files data/levels.txt'"));
  // The tester of an imported block is the behaviour tester (ADR 0029): its prompt says to read the code
  // the workers changed, so it reads every file of its card (a block of one .cpp has no header to read,
  // bugs.txt 24), and writes none of them.
  agents::Sandbox tester(p.schema(), "game.player", agents::Role::Tester);
  CHECK(tester.resolve_read("src/game/hpp/player.hpp"));
  CHECK(tester.resolve_read("src/game/player.cpp"));
  CHECK(tester.own_files() == std::vector<std::string>{"src/game/player.cpp", "src/game/hpp/player.hpp"});
  CHECK_FALSE(tester.resolve_write("src/game/hpp/player.hpp"));
  CHECK_FALSE(tester.resolve_write("src/game/player.cpp"));
  auto world = tester.resolve_read("src/game/world.cpp");  // another block's code stays closed
  REQUIRE_FALSE(world);
  CHECK(core::contains(world.error().message, "belongs to block game.world"));
}

// --- the tools of a worker there -----------------------------------------------------------------

TEST_CASE("imported tools: a header keeps its public part, list_dir and grep show what is readable") {
  Legacy p;
  test::FakeTests tests;
  agents::Sandbox sb(p.schema(), "game.player", agents::Role::Worker);
  sb.set_api_access(agents::ApiAccess::Imported);
  agents::LeafToolHost host(std::move(sb), agents::Role::Worker, tests, {"game.player"});
  auto edit = [&](const std::string& id, const std::string& path, const std::string& old, const std::string& now) {
    return host.call({id, "edit_file", json{{"path", path}, {"old", old}, {"new", now}}.dump()});
  };
  auto priv = edit("1", "src/game/hpp/player.hpp", "    Inventory bag_;\n", "    Inventory bag_;\n    float speed_ = 1.0f;\n");
  CHECK_MESSAGE(!priv.is_error, priv.content);
  auto pub = edit("2", "src/game/hpp/player.hpp", "public:\n", "public:\n    void Reset();\n");
  CHECK(pub.is_error);
  CHECK(core::contains(pub.content, "`void Reset()` (in Player) is a new public declaration"));
  CHECK(core::contains(pub.content, "needs_contract_change"));
  CHECK_FALSE(core::contains(test::read(p / "src/game/hpp/player.hpp"), "Reset"));
  CHECK(core::contains(test::read(p / "src/game/hpp/player.hpp"), "speed_"));
  auto src = edit("3", "src/game/player.cpp", "pos_.x + dt", "pos_.x + dt * speed_");
  CHECK_MESSAGE(!src.is_error, src.content);
  CHECK(host.api_written());
  auto ls = host.call({"4", "list_dir", R"({"path":"src/game"})"});
  CHECK(core::contains(ls.content, "player.cpp"));
  CHECK(core::contains(ls.content, "hpp/"));
  CHECK_FALSE(core::contains(ls.content, "world.cpp"));
  CHECK_FALSE(core::contains(ls.content, "main.cpp"));
  CHECK(core::contains(ls.content, "more entries belong to other blocks"));
  auto g = host.call({"5", "grep", R"({"pattern":"IsWalkable"})"});
  CHECK(core::contains(g.content, "src/game/hpp/world.hpp:"));
  CHECK(core::contains(g.content, "src/game/player.cpp:"));
  CHECK_FALSE(core::contains(g.content, "src/game/world.cpp"));
}

TEST_CASE("imported tools: warnings of the own files are shown, a broken own build names the project's CMakeLists") {
  Legacy p;
  test::FakeTests tests;
  tests.fn = [](const std::vector<std::string>&) {
    agents::TestRun r{true, true, "", {}, "build ok; tests (true): ok\n"};
    r.build_warnings = "src/game/player.cpp:6:10: warning: unused variable 'v' [-Wunused-variable]\n"
                       "src/game/world.cpp:3:1: warning: something else\n";
    return r;
  };
  agents::LeafToolHost host(agents::Sandbox(p.schema(), "game.player", agents::Role::Worker), agents::Role::Worker, tests, {"game.player"});
  auto t = host.call({"1", "test", "{}"});
  CHECK(core::contains(t.content, "warnings in your files"));
  CHECK(core::contains(t.content, "src/game/player.cpp:6:10: warning: unused variable"));
  CHECK_FALSE(core::contains(t.content, "src/game/world.cpp:3:1"));
  std::string summary = "CMake Error at CMakeLists.txt:4 (add_executable): Cannot find source file: src/game/speed.cpp\n";
  tests.fn = [&](const std::vector<std::string>&) { return agents::TestRun{false, false, summary, {}, "build failed\n" + summary}; };
  auto b = host.call({"2", "test", "{}"});
  CHECK(core::contains(b.content, "CMakeLists.txt is the project's own build"));
}

TEST_CASE("imported tools: a data file comes with a card change, and the sandbox follows the approved card") {
  Legacy p;
  test::write(p / "data/levels.txt", "level 1\n");
  test::FakeTests tests;
  agents::LeafToolHost host(agents::Sandbox(p.schema(), "game.player", agents::Role::Worker), agents::Role::Worker, tests, {"game.player"});
  auto refused = host.call({"1", "write_file", json{{"path", "data/levels.txt"}, {"content", "level 1\nlevel 2\n"}}.dump()});
  CHECK(refused.is_error);
  std::string diff;
  host.set_approve_now([&](const std::string&, const std::string& d) -> kk::Result<std::string> {
    diff = d;
    auto card = p / "blocks/game/player/block.ksl";
    test::write(card, test::read(card) + "files data/levels.txt\n");  // what the approval writes
    return std::string("C-0001 approved: 1 card(s) written");
  });
  auto rep = host.call({"2", "report", json{{"status", "needs_contract_change"}, {"cause", "levels are data"},
                                            {"proposal", "~ @block game.player\n+ files data/levels.txt"}}
                                               .dump()});
  CHECK_MESSAGE(!rep.is_error, rep.content);
  CHECK_FALSE(rep.terminal);  // the worker goes on in the same run
  CHECK(core::contains(diff, "+ files data/levels.txt"));
  auto now = host.call({"3", "write_file", json{{"path", "data/levels.txt"}, {"content", "level 1\nlevel 2\n"}}.dump()});
  CHECK_MESSAGE(!now.is_error, now.content);
  CHECK(test::read(p / "data/levels.txt") == "level 1\nlevel 2\n");
  // The card with a data file is a valid card: no E09, the file is no code of the index.
  auto issues = core::check_schema(p.schema());
  for (const auto& i : issues) CHECK_MESSAGE(i.code != "E09", i.str());
}

// --- the context of a worker there ---------------------------------------------------------------

TEST_CASE("imported context: a big neighbor class shows its head, labels and the used public members only") {
  Legacy p;
  std::string world = "/* Copyright (c) 2026 by somebody\n   Licensed under the terms of a license (GPL).\n*/\n"
                      "#pragma once\n#include \"../../util/math.h\"\n\n// The island.\nclass World {\npublic:\n"
                      "    void Update(float dt);\n    // Whether a player may stand at p.\n    bool IsWalkable(Vec2 p) const;\n"
                      "    void Draw() const;\n";
  for (int i = 0; i < 80; ++i) world += "    int Unused" + std::to_string(i) + "(int a, int b) const;\n";
  world += "\nprivate:\n    void Helper();\n    float time_ = 0;\n    int secretCounter_ = 0;\n};\n";
  test::write(p / "src/game/hpp/world.hpp", world);
  test::ScriptedModel model;
  test::FakeTests tests;
  agents::EngineOptions o;
  o.providers = model.factory();
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), test::test_config(), o);
  REQUIRE(e);
  auto ctx = (*e)->context_for(agents::Role::Worker, "game.player");
  CHECK(core::contains(ctx, "=== src/game/hpp/player.hpp\n"));  // its own header, whole
  CHECK(core::contains(ctx, "=== src/game/hpp/world.hpp ("));    // the neighbor, by its used declarations
  CHECK(core::contains(ctx, "class World {"));
  CHECK(core::contains(ctx, "    // Whether a player may stand at p.\n    bool IsWalkable(Vec2 p) const;"));
  CHECK_FALSE(core::contains(ctx, "Unused7"));
  CHECK_FALSE(core::contains(ctx, "secretCounter_"));
  CHECK_FALSE(core::contains(ctx, "Copyright"));
  CHECK(core::contains(ctx, "the project's own build and tests"));
}

// --- a ticket over imported blocks (ADR 0028) ------------------------------------------------------

namespace {

struct LegacyRig {
  Legacy p;
  test::ScriptedModel model;
  test::FakeTests tests;
  std::unique_ptr<agents::Engine> engine;
  LegacyRig() {
    agents::EngineOptions o;
    o.providers = model.factory();
    o.tests = &tests;
    auto e = agents::Engine::create(p.path(), test::test_config(), o);
    REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));
    engine = std::move(*e);
  }
  void plan_world_then_player() {
    model.tool("dispatch", {{"block", "game.world"}, {"difficulty", "S"}, {"why", "walkable tiles"},
                            {"next", json::array({{{"block", "game.player"}, {"difficulty", "S"}, {"why", "the player's speed"}}})}});
  }
  void worker_done(const std::string& what) {
    model.tool("report", {{"status", "done"}, {"fix", what}, {"files", json::array()}, {"api", "unchanged"}});
  }
  std::string ticket() { return test::read(p / ".karkas/tickets/T-0001.ksl"); }
};

}  // namespace

TEST_CASE("imported ticket: the orchestrator gets the likely blocks, one full build runs after the last block") {
  LegacyRig rig;
  rig.plan_world_then_player();
  rig.worker_done("tiles slow the player down");
  rig.worker_done("the player walks at the tile's speed");
  auto r = rig.engine->task("Make the player walk slower on mud tiles of the world");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto t = rig.ticket();
  CHECK_MESSAGE(core::contains(t, "status  closed"), t);
  REQUIRE(rig.model.calls.size() == 3);
  auto orch = test::ScriptedModel::request_text(rig.model.calls[0].req);
  CHECK(core::contains(orch, "--- blocks the code index finds for the words of the ticket"));
  CHECK(core::contains(orch, "game.player"));
  CHECK(core::contains(orch, "src/game/player.cpp"));
  // Each worker's files were compiled; the project's full build ran once, after the last block.
  CHECK(rig.tests.compile_calls == std::vector<std::vector<std::string>>{{"src/game/world.cpp"}, {"src/game/player.cpp"}});
  CHECK(rig.tests.calls.size() == 1);
  CHECK(core::contains(t, "compile check ok; the full build and tests run after the last block"));
}

TEST_CASE("imported ticket: a full build failing in a block's file goes back to that block's worker") {
  LegacyRig rig;
  int builds = 0;
  rig.tests.fn = [&](const std::vector<std::string>&) {
    std::string summary = "src/game/world.cpp:9:3: error: 'mud_' was not declared in this scope";
    if (++builds == 1) return agents::TestRun{false, false, summary, {}, "build failed\n" + summary + "\n"};
    return agents::TestRun{true, true, "", {}, "build ok; tests ok\n"};
  };
  rig.plan_world_then_player();
  rig.worker_done("tiles slow the player down");
  rig.worker_done("the player walks at the tile's speed");
  rig.worker_done("declared the mud factor");  // the world's worker again, with the build errors
  auto r = rig.engine->task("Make the player walk slower on mud tiles of the world");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto t = rig.ticket();
  CHECK_MESSAGE(core::contains(t, "status  closed"), t);
  CHECK(core::contains(t, "the full build fails in a file of game.world"));
  REQUIRE(rig.model.calls.size() == 4);
  auto again = test::ScriptedModel::request_text(rig.model.calls[3].req);
  CHECK(core::contains(again, "The project's full build fails after the changes of this ticket"));
  CHECK(core::contains(again, "'mud_' was not declared"));
  CHECK(builds == 2);
}

TEST_CASE("imported context: a task that names functions gets the linked declarations their bodies use (ADR 0029)") {
  Legacy p;
  test::write(p / "src/game/player.cpp",
              "#include \"hpp/player.hpp\"\n\n#include \"hpp/world.hpp\"\n\n"
              "void Player::Update(float dt, const World& world) {\n    Vec2 next{pos_.x + dt, pos_.y};\n"
              "    if (world.IsWalkable(next)) pos_ = next;\n}\n\n"
              "Vec2 Player::Position() const {\n    static World shown;\n    shown.Draw();\n    return pos_;\n}\n");
  std::string world = "#pragma once\n\nclass World {\npublic:\n    void Update(float dt);\n    bool IsWalkable(Vec2 p) const;\n"
                      "    void Draw() const;\n";
  for (int i = 0; i < 80; ++i) world += "    int Unused" + std::to_string(i) + "(int a, int b) const;\n";
  world += "};\n";
  test::write(p / "src/game/hpp/world.hpp", world);
  test::ScriptedModel model;
  test::FakeTests tests;
  agents::EngineOptions o;
  o.providers = model.factory();
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), test::test_config(), o);
  REQUIRE(e);
  // The task names update(): only what Player::Update uses comes from world.hpp.
  auto focused = (*e)->context_for(agents::Role::Worker, "game.player", "the player must not walk into water in update");
  CHECK(core::contains(focused, "the declarations that Update of this task use"));
  CHECK(core::contains(focused, "bool IsWalkable(Vec2 p) const;"));
  CHECK_FALSE(core::contains(focused, "void Draw() const;"));
  // No function named: the declarations the whole block uses, as before.
  auto whole = (*e)->context_for(agents::Role::Worker, "game.player", "make it nicer");
  CHECK(core::contains(whole, "the declarations this block uses"));
  CHECK(core::contains(whole, "bool IsWalkable(Vec2 p) const;"));
  CHECK(core::contains(whole, "void Draw() const;"));
}


// --- the behaviour test of an imported feature (ADR 0029) ----------------------------------------------

namespace {

struct BehaviourRig {
  Legacy p;
  test::ScriptedModel model;
  test::FakeTests tests;
  std::unique_ptr<agents::Engine> engine;
  BehaviourRig() {
    test::write(p / "tests/HOWTO.md", "A test is a text file with lines `given`, `do` and `expect`.\n");
    test::write(p / "tests/behaviour/walk_test.txt", "given a player on grass\ndo walk 1 s\nexpect x == 1\n");
    auto cfg = test::test_config();
    cfg.test_dir = "tests/behaviour";
    cfg.test_guide = "tests/HOWTO.md";
    cfg.test_command = "true";
    agents::EngineOptions o;
    o.providers = model.factory();
    o.tests = &tests;
    auto e = agents::Engine::create(p.path(), cfg, o);
    REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));
    engine = std::move(*e);
  }
  void script_until_the_test() {
    model.tool("dispatch", {{"block", "game.player"}, {"difficulty", "S"}, {"why", "walk slower on mud"}});
    model.tool("report", {{"status", "done"}, {"fix", "the player walks at half speed on mud"}, {"files", json::array()}, {"api", "unchanged"}});
    model.tool("write_file", {{"path", "tests/behaviour/mud_test.txt"}, {"content", "given a player on mud\ndo walk 1 s\nexpect x == 0.5\n"}});
    model.tool("report", {{"status", "done"}, {"files", "tests/behaviour/mud_test.txt"}, {"fix", "half speed on mud"}});
  }
  std::string ticket() { return test::read(p / ".karkas/tickets/T-0001.ksl"); }
};

}  // namespace

TEST_CASE("imported ticket: with [test] dir, guide and command a feature ends with a behaviour test in the project's framework") {
  BehaviourRig rig;
  rig.script_until_the_test();
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("Make the player walk slower on mud");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 5);
  auto tester = test::ScriptedModel::request_text(rig.model.calls[2].req);
  CHECK(core::contains(tester, "TESTER of a feature of an imported project"));
  CHECK(core::contains(tester, "the project's test folder: tests/behaviour"));
  CHECK(core::contains(tester, "an existing test: tests/behaviour/walk_test.txt"));
  CHECK(core::contains(tester, "A test is a text file with lines `given`"));
  CHECK(core::contains(tester, "the player walks at half speed on mud"));  // what the worker did
  CHECK(test::read(rig.p / "tests/behaviour/mud_test.txt") == "given a player on mud\ndo walk 1 s\nexpect x == 0.5\n");
  auto t = rig.ticket();
  CHECK(core::contains(t, "behaviour test tests/behaviour/mud_test.txt passes: half speed on mud"));
  CHECK_FALSE(std::filesystem::exists(rig.p / ".karkas/cache/ticket_tests.txt"));  // the list goes with the ticket
}

TEST_CASE("imported ticket: a behaviour test that fails on a building project goes back to the orchestrator with its output") {
  BehaviourRig rig;
  int runs = 0;
  rig.tests.fn = [&](const std::vector<std::string>&) {
    if (++runs == 1) return agents::TestRun{true, true, "", {}, "build ok; tests ok\n"};
    return agents::TestRun{true, false, "", {}, "build ok; mud_test: expected x == 0.5, got x == 1\n"};
  };
  rig.script_until_the_test();
  rig.model.tool("escalate", {{"note", "later"}});
  auto r = rig.engine->task("Make the player walk slower on mud");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 5);
  auto back = test::ScriptedModel::request_text(rig.model.calls[4].req);
  CHECK(core::contains(back, "The behaviour test tests/behaviour/mud_test.txt written for this ticket fails"));
  CHECK(core::contains(back, "expected x == 0.5, got x == 1"));
  CHECK(core::contains(rig.ticket(), "the behaviour test tests/behaviour/mud_test.txt fails: back to the orchestrator"));
}

TEST_CASE("imported ticket: after a failing behaviour test a block done in the run is sent once more, with the test's output") {
  BehaviourRig rig;
  int runs = 0;
  rig.tests.fn = [&](const std::vector<std::string>&) {
    ++runs;
    if (runs == 2) return agents::TestRun{true, false, "", {}, "build ok; mud_test: expected x == 0.5, got x == 1\n"};
    return agents::TestRun{true, true, "", {}, "build ok; tests ok\n"};
  };
  rig.script_until_the_test();
  rig.model.tool("dispatch", {{"block", "game.player"}, {"difficulty", "S"}, {"why", "the speed on mud is not applied"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "apply the mud factor in Update"}, {"files", json::array()}, {"api", "unchanged"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("Make the player walk slower on mud");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 7);
  // The orchestrator was not told that game.player answered already: it may send it again.
  auto back = test::ScriptedModel::request_text(rig.model.calls[4].req);
  CHECK(core::contains(back, "a block done in this run may get it once more"));
  CHECK_FALSE(core::contains(back, "- game.player: done"));
  // After the fix passed, the orchestrator hears it, and nothing says the test still fails.
  auto closing = test::ScriptedModel::request_text(rig.model.calls[6].req);
  CHECK(core::contains(closing, "The behaviour test passes now, after the fix of game.player."));
  CHECK_FALSE(core::contains(closing, "still fails"));
  auto again = test::ScriptedModel::request_text(rig.model.calls[5].req);
  CHECK(rig.model.calls[5].tier == "medium");  // a fix of a failing behaviour test goes a tier up
  CHECK(core::contains(again, "The behaviour test tests/behaviour/mud_test.txt of this ticket fails after the blocks were done"));
  CHECK(core::contains(again, "expected x == 0.5, got x == 1"));
  CHECK_MESSAGE(core::contains(rig.ticket(), "status  closed"), rig.ticket());
}

TEST_CASE("imported ticket: a fix of a behaviour test that answers not_here goes to the orchestrator, and the next send is strong") {
  BehaviourRig rig;
  int runs = 0;
  rig.tests.fn = [&](const std::vector<std::string>&) {
    ++runs;
    if (runs == 2) return agents::TestRun{true, false, "", {}, "build ok; mud_test: expected x == 0.5, got x == 1\n"};
    return agents::TestRun{true, true, "", {}, "build ok; tests ok\n"};
  };
  rig.script_until_the_test();
  rig.model.tool("dispatch", {{"block", "game.player"}, {"difficulty", "S"}, {"why", "the speed on mud is not applied"}});
  rig.model.tool("report", {{"status", "not_here"}, {"cause", "Update looks right to me"}});
  rig.model.tool("dispatch", {{"block", "game.player"}, {"difficulty", "S"}, {"why", "Update never reads the tile: multiply dt by tile.speed"}});
  rig.model.tool("report", {{"status", "done"}, {"fix", "Update multiplies dt by the tile's speed"}, {"files", json::array()}, {"api", "unchanged"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("Make the player walk slower on mud");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 9);
  CHECK(rig.model.calls[5].tier == "medium");
  auto orch = test::ScriptedModel::request_text(rig.model.calls[6].req);  // straight back, no navigator in between
  CHECK(core::contains(orch, "The behaviour test still fails: 1 more send(s)"));
  CHECK(rig.model.calls[7].tier == "strong");
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[7].req), "multiply dt by tile.speed"));
  auto t = rig.ticket();
  CHECK(core::contains(t, "game.player could not fix the behaviour test"));
  CHECK_MESSAGE(core::contains(t, "status  closed"), t);
}

TEST_CASE("imported ticket: a fix worker that finds the flaw in the behaviour test sends it to the behaviour tester") {
  BehaviourRig rig;
  int runs = 0;
  rig.tests.fn = [&](const std::vector<std::string>&) {
    ++runs;
    if (runs == 2) return agents::TestRun{true, false, "", {}, "build ok; mud_test: expected x == 0.5, got x == 0.6\n"};
    return agents::TestRun{true, true, "", {}, "build ok; tests ok\n"};
  };
  rig.script_until_the_test();
  rig.model.tool("dispatch", {{"block", "game.player"}, {"difficulty", "S"}, {"why", "the speed on mud is not applied"}});
  rig.model.tool("report", {{"status", "needs_tester"}, {"cause", "the mud starts at x = 0.2: the player walks 0.2 s on grass first"},
                            {"fix", "expect x == 0.6"}});
  rig.model.tool("write_file", {{"path", "tests/behaviour/mud_test.txt"}, {"content", "given a player on mud\ndo walk 1 s\nexpect x == 0.6\n"}});
  rig.model.tool("report", {{"status", "done"}, {"files", "tests/behaviour/mud_test.txt"}, {"fix", "the walk starts on grass: expect 0.6"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("Make the player walk slower on mud");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 9);
  // Not the contract tester (an imported project has none): the behaviour tester, with the worker's finding.
  auto fix = test::ScriptedModel::request_text(rig.model.calls[6].req);
  CHECK(core::contains(fix, "found that the behaviour test of this ticket is wrong, not the code"));
  CHECK(core::contains(fix, "the mud starts at x = 0.2"));
  CHECK(core::contains(fix, "The test: tests/behaviour/mud_test.txt"));
  CHECK_FALSE(core::contains(fix, "tests/contract"));
  auto closing = test::ScriptedModel::request_text(rig.model.calls[8].req);
  CHECK(core::contains(closing, "The behaviour test passes now: game.player found a flaw in it and the tester corrected it"));
  CHECK_FALSE(core::contains(closing, "still fails"));
  auto t = rig.ticket();
  CHECK(core::contains(t, "the tester corrected the behaviour test (game.player found the flaw); it passes"));
  CHECK_MESSAGE(core::contains(t, "status  closed"), t);
}

TEST_CASE("imported ticket: a behaviour test the tester cannot correct leaves the orchestrator to decide by the facts") {
  BehaviourRig rig;
  int runs = 0;
  rig.tests.fn = [&](const std::vector<std::string>&) {
    ++runs;
    if (runs >= 2) return agents::TestRun{true, false, "", {}, "build ok; mud_test: expected x == 0.5, got x == 0.6\n"};
    return agents::TestRun{true, true, "", {}, "build ok; tests ok\n"};
  };
  rig.script_until_the_test();
  rig.model.tool("dispatch", {{"block", "game.player"}, {"difficulty", "S"}, {"why", "the speed on mud is not applied"}});
  rig.model.tool("report", {{"status", "needs_tester"}, {"cause", "the mud starts at x = 0.2"}});
  rig.model.tool("report", {{"status", "failed"}, {"cause", "I cannot place the player on mud"}});
  rig.model.tool("escalate", {{"note", "a human checks the test"}});
  auto r = rig.engine->task("Make the player walk slower on mud");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 8);
  auto orch = test::ScriptedModel::request_text(rig.model.calls[7].req);
  CHECK(core::contains(orch, "The behaviour test is in doubt: game.player found a flaw in it"));
  CHECK(core::contains(orch, "Close with verdict done when the facts show the request is in place"));
  CHECK_FALSE(core::contains(orch, "still fails"));
  CHECK(core::contains(rig.ticket(), "the tester could not correct the behaviour test: back to the orchestrator"));
}

TEST_CASE("imported ticket: a new source file of a block reaches the project's build by Karkas (ADR 0030)") {
  LegacyRig rig;
  rig.p.card("game.player", "purpose the player\nkind leaf\nthread main\nfiles src/game/player.cpp src/game/hpp/player.hpp\n"
                            "files src/game/hud.cpp\nfn update(float, World) -> void\nfn position() -> Vec2\nfn bag() -> Inventory\n"
                            "call game.world:is_walkable\nuse game.inventory\nuse util.math\n");
  test::write(rig.p / "src/game/hud.cpp", "// the player's hud\n");
  rig.plan_world_then_player();
  rig.worker_done("tiles slow the player down");
  rig.worker_done("the player shows a hud");
  auto r = rig.engine->task("Make the player walk slower on mud tiles of the world");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto cmake = test::read(rig.p / "CMakeLists.txt");
  CHECK_MESSAGE(core::contains(cmake, "src/game/hud.cpp src/game/main.cpp"), cmake);
  auto t = rig.ticket();
  CHECK(core::contains(t, "Karkas added the new file(s) of game.player to the project's build: CMakeLists.txt: src/game/hud.cpp"));
  CHECK_MESSAGE(core::contains(t, "status  closed"), t);
}

TEST_CASE("imported ticket: a linker error goes to the block of its symbol (ADR 0030)") {
  LegacyRig rig;
  int builds = 0;
  rig.tests.fn = [&](const std::vector<std::string>&) {
    std::string summary = "/usr/bin/ld: <artificial>:(.text+0x1f): undefined reference to `World::IsWalkable(Vec2)'\n"
                          "collect2: error: ld returned 1 exit status";
    if (++builds == 1) return agents::TestRun{false, false, summary, {}, "build failed\n" + summary + "\n"};
    return agents::TestRun{true, true, "", {}, "build ok; tests ok\n"};
  };
  rig.plan_world_then_player();
  rig.worker_done("tiles slow the player down");
  rig.worker_done("the player walks at the tile's speed");
  rig.worker_done("defined World::IsWalkable");
  auto r = rig.engine->task("Make the player walk slower on mud tiles of the world");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto t = rig.ticket();
  CHECK_MESSAGE(core::contains(t, "status  closed"), t);
  CHECK(core::contains(t, "the full build fails in a file of game.world"));
  REQUIRE(rig.model.calls.size() == 4);
  CHECK(core::contains(test::ScriptedModel::request_text(rig.model.calls[3].req), "undefined reference to `World::IsWalkable"));
}

TEST_CASE("imported ticket: a full build no error of which names a block goes to the orchestrator, not the human (ADR 0030)") {
  LegacyRig rig;
  int builds = 0;
  rig.tests.fn = [&](const std::vector<std::string>&) {
    std::string summary = "/usr/bin/ld: cannot find -lgamepad\ncollect2: error: ld returned 1 exit status";
    if (++builds == 1) return agents::TestRun{false, false, summary, {}, "build failed\n" + summary + "\n"};
    return agents::TestRun{true, true, "", {}, "build ok; tests ok\n"};
  };
  rig.plan_world_then_player();
  rig.worker_done("tiles slow the player down");
  rig.worker_done("the player reads the gamepad");
  rig.model.tool("dispatch", {{"block", "game.player"}, {"difficulty", "S"}, {"why", "drop the gamepad library the build cannot find"}});
  rig.worker_done("the player reads the keyboard again");
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("Make the player walk slower on mud tiles of the world");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto t = rig.ticket();
  CHECK_MESSAGE(core::contains(t, "status  closed"), t);
  CHECK(core::contains(t, "the full build fails and no error names a block's file: back to the orchestrator"));
  REQUIRE(rig.model.calls.size() >= 5);
  auto orch = test::ScriptedModel::request_text(rig.model.calls[3].req);
  CHECK(core::contains(orch, "no error names a file of a block"));
  CHECK(core::contains(orch, "cannot find -lgamepad"));
  CHECK_FALSE(core::contains(orch, "Already answered in this run (do not send them the ticket again):\n- game.player"));
  auto again = test::ScriptedModel::request_text(rig.model.calls[4].req);
  CHECK(core::contains(again, "The project's full build fails after the changes of this ticket"));
  CHECK(builds == 2);
}

TEST_CASE("imported ticket: a fix worker's not_here that names the behaviour test goes to the behaviour tester (ADR 0030)") {
  BehaviourRig rig;
  int runs = 0;
  rig.tests.fn = [&](const std::vector<std::string>&) {
    ++runs;
    if (runs == 2) return agents::TestRun{true, false, "", {}, "build ok; mud_test: expected x == 0.5, got x == 0.6\n"};
    return agents::TestRun{true, true, "", {}, "build ok; tests ok\n"};
  };
  rig.script_until_the_test();
  rig.model.tool("dispatch", {{"block", "game.player"}, {"difficulty", "S"}, {"why", "the speed on mud is not applied"}});
  rig.model.tool("report", {{"status", "not_here"},
                            {"cause", "the code is right; tests/behaviour/mud_test.txt starts the walk on grass, so x is 0.6"}});
  rig.model.tool("write_file", {{"path", "tests/behaviour/mud_test.txt"}, {"content", "given a player on mud\ndo walk 1 s\nexpect x == 0.6\n"}});
  rig.model.tool("report", {{"status", "done"}, {"files", "tests/behaviour/mud_test.txt"}, {"fix", "the walk starts on grass: expect 0.6"}});
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("Make the player walk slower on mud");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  REQUIRE(rig.model.calls.size() == 9);
  auto fix = test::ScriptedModel::request_text(rig.model.calls[6].req);
  CHECK(core::contains(fix, "found that the behaviour test of this ticket is wrong, not the code"));
  CHECK(core::contains(fix, "starts the walk on grass"));
  auto t = rig.ticket();
  CHECK(core::contains(t, "game.player blames the behaviour test tests/behaviour/mud_test.txt (not_here): it goes to the behaviour tester"));
  CHECK(core::contains(t, "the tester corrected the behaviour test (game.player found the flaw); it passes"));
  CHECK_MESSAGE(core::contains(t, "status  closed"), t);
}

TEST_CASE("imported change: a new block with code and no files gets them as the project names its files (ADR 0030)") {
  Legacy p;
  // Lower-case files, sources in src/game/, headers in src/game/hpp/ (the siblings of game.*).
  auto named = agents::name_new_block_files(p.path(), "+ @block game.hud\n+ purpose the hud\n+ kind leaf\n+ thread main\n"
                                                      "+ note types: Hud\n+ fn draw() -> void\n~ @block game.main\n+ call game.hud:draw\n");
  CHECK_MESSAGE(core::contains(named, "+ @block game.hud\n+ files   src/game/hud.cpp src/game/hpp/hud.hpp\n"), named);
  // A header-only shared block gets a header only; a block with files keeps them; a group gets none.
  named = agents::name_new_block_files(p.path(), "+ @block game.items\n+ purpose item list\n+ kind shared\n"
                                                 "+ @block game.save\n+ purpose saving\n+ kind leaf\n+ files src/game/save.cpp\n"
                                                 "+ @block game.parts\n+ purpose parts\n+ kind group\n");
  CHECK_MESSAGE(core::contains(named, "+ @block game.items\n+ files   src/game/hpp/items.hpp\n"), named);
  CHECK(core::contains(named, "+ files src/game/save.cpp\n"));
  CHECK_FALSE(core::contains(named, "+ @block game.parts\n+ files"));
  // Endless Sky's naming: CamelCase files, the type of the note.
  p.card("ui", "purpose screens\nkind group\n");
  p.card("ui.info", "purpose the player info panel\nkind leaf\nthread main\nfiles src/ui/PlayerInfoPanel.cpp src/ui/PlayerInfoPanel.h\n");
  named = agents::name_new_block_files(p.path(), "+ @block ui.pilot_statistics\n+ purpose the statistics panel\n+ kind leaf\n"
                                                 "+ thread main\n+ note types: PilotStatisticsPanel (a Panel)\n");
  CHECK_MESSAGE(core::contains(named, "+ files   src/ui/PilotStatisticsPanel.cpp src/ui/PilotStatisticsPanel.h\n"), named);
  // Through propose_change the card of the approved change names the files.
  auto c = agents::propose_change(p.path(), "orchestrator T-0001", "a hud",
                                  "+ @block game.hud\n+ purpose the hud\n+ kind leaf\n+ thread main\n+ fn draw() -> void\n");
  REQUIRE_MESSAGE(c, (c ? "" : c.error().str()));
  CHECK(std::any_of(c->diff.begin(), c->diff.end(), [](const core::DiffLine& d) {
    return d.text.starts_with("files") && core::contains(d.text, "src/game/hud.cpp src/game/hpp/hud.hpp");
  }));
}

TEST_CASE("imported change: a project of Karkas's own keeps a new block without files (ADR 0030)") {
  test::FixtureCopy p("wavkit");
  const std::string diff = "+ @block dsp.sparkle\n+ purpose short visual sparkles\n+ kind leaf\n+ thread main\n+ fn spark(int) -> void\n";
  CHECK(agents::name_new_block_files(p.path(), diff) == diff);
}

TEST_CASE("imported ticket: a block of the plan that answers not_here goes back to the orchestrator, not up the tree (ADR 0031)") {
  LegacyRig rig;
  rig.plan_world_then_player();
  rig.model.tool("report", {{"status", "needs_info"}, {"cause", "the walkable tiles need a speed the player does not give"}});
  rig.model.tool("dispatch", {{"block", "game.player"}, {"difficulty", "S"}, {"why", "the player gives the tile speed"}});
  rig.worker_done("the player gives the tile speed");
  rig.model.tool("close", {{"verdict", "done"}});
  auto r = rig.engine->task("Make the player walk slower on mud tiles of the world");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto t = rig.ticket();
  CHECK(core::contains(t, "game.world of the plan answered needs_info: back to the orchestrator"));
  REQUIRE(rig.model.calls.size() >= 3);
  auto orch = test::ScriptedModel::request_text(rig.model.calls[2].req);
  CHECK_MESSAGE(core::contains(orch, "game.world: needs_info: the walkable tiles need a speed"), orch);
  CHECK(core::contains(orch, "Planned and not sent yet: game.player"));
  CHECK_FALSE(core::contains(t, "navigator"));
}

TEST_CASE("imported ticket: a worker's done with no file of its block changed is no part done (ADR 0031)") {
  Legacy p;
  test::ScriptedModel model;
  test::FakeTests tests;
  auto cfg = test::test_config();
  cfg.done_needs_change = true;
  agents::EngineOptions o;
  o.providers = model.factory();
  o.tests = &tests;
  auto e = agents::Engine::create(p.path(), cfg, o);
  REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));
  model.tool("dispatch", {{"block", "game.world"}, {"difficulty", "S"}, {"why", "walkable tiles"},
                          {"next", json::array({{{"block", "game.player"}, {"difficulty", "S"}, {"why", "the player's speed"}}})}});
  // The world's worker restates the plan and changes nothing.
  model.tool("report", {{"status", "done"}, {"fix", "the tiles belong to game.player"}, {"files", json::array()}, {"api", "unchanged"}});
  model.tool("dispatch", {{"block", "game.player"}, {"difficulty", "S"}, {"why", "the player walks at the tile's speed"}});
  // The player's worker writes its file.
  model.tool("write_file", {{"path", "src/game/player.cpp"}, {"content", "// the player walks at the tile's speed\n"}});
  model.tool("report", {{"status", "done"}, {"fix", "the player walks at the tile's speed"}, {"files", "src/game/player.cpp"},
                        {"api", "unchanged"}});
  model.tool("close", {{"verdict", "done"}});
  auto r = (*e)->task("Make the player walk slower on mud tiles of the world");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  auto t = test::read(p / ".karkas/tickets/T-0001.ksl");
  CHECK(core::contains(t, "game.world reported done but changed no file of its block: back to the orchestrator"));
  auto orch = test::ScriptedModel::request_text(model.calls[2].req);
  CHECK_MESSAGE(core::contains(orch, "game.world: reported done without changing a file: the tiles belong to game.player"), orch);
  // The player's part counts: its file changed.
  CHECK(core::contains(t, "fix     game.player: the player walks at the tile's speed"));
  CHECK_FALSE(core::contains(t, "fix     game.world:"));
  CHECK_MESSAGE(core::contains(t, "status  closed"), t);
}

TEST_CASE("imported blocks: the entry block adds a public name without a card change (bugs.txt 28)") {
  // T-0007 of ~/.config/hypr: the worker of the entry (hyprland.lua) could not add a global function — the
  // file refused it as a new public name, and the card change '+ fn' was refused by E08.
  Legacy p;
  p.card("game.main", "purpose entry point\nkind entry\nthread main\nfiles src/game/main.cpp src/game/setup.lua\n"
                    "call game.world:update\ncall game.world:draw\ncall game.player:update\n"
                    "call game.player:position\ncall game.player:bag\ncall util.camera:follow\n");
  test::write(p / "src/game/setup.lua", "function setup()\n  return 1\nend\n");
  test::write(p / "src/game/world.lua", "function tiles()\n  return 2\nend\n");
  p.card("game.world", "purpose the island\nkind leaf\nthread main\nfiles src/game/world.cpp src/game/hpp/world.hpp src/game/world.lua\n"
                     "fn update(float) -> void\nfn is_walkable(Vec2) -> bool\nfn draw() -> void\ncall util.math:clampf\n");
  test::FakeTests tests;
  auto add_fn = [&](const std::string& block, const std::string& path, const std::string& end) {
    agents::Sandbox sb(p.schema(), block, agents::Role::Worker);
    sb.set_api_access(agents::ApiAccess::Imported);
    agents::LeafToolHost host(std::move(sb), agents::Role::Worker, tests, {block});
    return host.call({"1", "edit_file", json{{"path", path}, {"old", end}, {"new", end + "\nfunction ws_anim(style)\n  return style\nend\n"}}.dump()});
  };
  auto entry = add_fn("game.main", "src/game/setup.lua", "  return 1\nend\n");
  CHECK_MESSAGE(!entry.is_error, entry.content);
  CHECK(core::contains(test::read(p / "src/game/setup.lua"), "function ws_anim(style)"));
  // A leaf keeps its contract: other blocks build on its public names.
  auto leaf = add_fn("game.world", "src/game/world.lua", "  return 2\nend\n");
  CHECK(leaf.is_error);
  CHECK(core::contains(leaf.content, "new public"));
  CHECK_FALSE(core::contains(test::read(p / "src/game/world.lua"), "ws_anim"));
}

TEST_CASE("imported blocks: a file of a language without headers keeps its public names (ADR 0051)") {
  using kk::agents::contract_kept;
  const std::string py = "def push(frame):\n    return _wrap(frame)\n\n\ndef _wrap(frame):\n    return frame\n";
  std::string why;
  // Bodies and private names are free.
  CHECK(contract_kept("app/ring.py", py, "def push(frame):\n    return frame\n", &why));
  // A public function gone, a changed parameter count or a new public function is a card change.
  CHECK_FALSE(contract_kept("app/ring.py", py, "def _wrap(frame):\n    return frame\n", &why));
  CHECK(kk::core::contains(why, "push"));
  CHECK_FALSE(contract_kept("app/ring.py", py, "def push(frame, n):\n    return frame\n", &why));
  CHECK_FALSE(contract_kept("app/ring.py", py, py + "\ndef reset():\n    pass\n", &why));
  CHECK(kk::core::contains(why, "new public"));
  // A shell script's subcommands are its contract; data files have none.
  const std::string sh = "#!/bin/bash\ncase \"$1\" in\n  start) echo s ;;\n  stop) echo t ;;\nesac\n";
  CHECK(contract_kept("run.sh", sh, core::replace_all(sh, "echo s", "echo started"), &why));
  CHECK_FALSE(contract_kept("run.sh", sh, core::replace_all(sh, "  stop) echo t ;;\n", ""), &why));
  CHECK(contract_kept("conf/idle.conf", "a = 1\n", "b = 2\n", &why));
}
