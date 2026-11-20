// Notes projects (docs/adr/0038): a folder of .txt notes becomes a notes project by itself; its files stay
// where they are, folders become topics.
#include <doctest/doctest.h>

#include "kk/code/drift.hpp"
#include "kk/code/init.hpp"
#include "kk/code/notes.hpp"
#include "kk/core/notes.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/test/support.hpp"

using namespace kk;

TEST_CASE("notes: init finds the notes of a folder, makes topics and notes for them, keeps the files") {
  test::TempDir d{"my_notes"};
  test::write(d / "Покупки.txt", "молоко\n");
  test::write(d / "Проекты/Игра про кота.txt", "См. [[Покупки]].\n");
  test::write(d / "Проекты/Сюжет/Глава 1.txt", "[[Игра про кота]]\n");
  test::write(d / ".git/HEAD.txt", "hidden\n");
  test::write(d / "build/out.txt", "build tree\n");
  CHECK(code::has_notes(d.path()));
  CHECK_FALSE(code::has_code(d.path()));
  auto r = code::init_project(d.path());
  REQUIRE_MESSAGE(r, (r ? "" : r.error().message));
  CHECK(r->notes);
  CHECK(test::read(d / "project.ksl").find("lang    notes") != std::string::npos);
  CHECK_FALSE(std::filesystem::exists(d / "CMakeLists.txt"));
  CHECK_FALSE(std::filesystem::exists(d / "CLAUDE.md"));
  CHECK(test::read(d / "karkas.toml").find("notes = \"medium\"") != std::string::npos);  // the assistant's models
  CHECK(test::read(d / "Покупки.txt") == "молоко\n");  // untouched
  auto s = core::load_schema(d.path());
  REQUIRE(s);
  CHECK(core::is_notes_project(s->project));
  const auto* buy = s->find("pokupki");
  REQUIRE(buy != nullptr);
  CHECK(buy->purpose.value == "Покупки");
  CHECK(core::note_file(*buy) == "Покупки.txt");
  const auto* topic = s->find("proekty");
  REQUIRE(topic != nullptr);
  CHECK(topic->kind == core::BlockKind::Group);
  CHECK(topic->purpose.value == "Проекты");
  const auto* game = s->find("proekty.igra_pro_kota");
  REQUIRE(game != nullptr);
  CHECK(core::note_file(*game) == "Проекты/Игра про кота.txt");
  REQUIRE(s->find("proekty.syuzhet.glava_1") != nullptr);
  CHECK(s->find("git") == nullptr);
  CHECK(s->find("build") == nullptr);
  int refs = 0;
  for (const auto& l : s->links()) refs += l.kind == core::LinkKind::Ref ? 1 : 0;
  CHECK(refs == 2);
  for (const auto& i : core::check_schema(*s)) CHECK_MESSAGE(i.severity != 'E', i.str());
  CHECK(r->imported.size() == 5);  // 2 topics, 3 notes

  // A new file later: only it gets a card; the topic is the same.
  test::write(d / "Проекты/Идеи.txt", "...\n");
  auto again = code::import_notes(d.path());
  REQUIRE(again);
  CHECK(again->topics.empty());
  CHECK(again->notes == std::vector<std::string>{"proekty.idei"});
}

TEST_CASE("notes: a folder with code stays a code project, an empty one too unless notes are asked for") {
  test::TempDir code_dir{"with_code"};
  test::write(code_dir / "main.cpp", "int main() {}\n");
  test::write(code_dir / "README.txt", "readme\n");
  auto c = code::init_project(code_dir.path());
  REQUIRE(c);
  CHECK_FALSE(c->notes);
  CHECK(test::read(code_dir / "project.ksl").find("lang    cpp") != std::string::npos);

  test::TempDir empty{"empty_one"};
  auto e = code::init_project(empty.path());
  REQUIRE(e);
  CHECK_FALSE(e->notes);

  test::TempDir asked{"asked_notes"};
  auto n = code::init_project(asked.path(), "my thoughts", code::ProjectKind::Notes);
  REQUIRE(n);
  CHECK(n->notes);
  CHECK(n->imported.empty());
  auto s = core::load_schema(asked.path());
  REQUIRE(s);
  CHECK(core::is_notes_project(s->project));
  CHECK(s->project.idea.value == "my thoughts");
}

TEST_CASE("notes: links added and removed from the schema, titles changed with the links that named them") {
  test::TempDir d{"vault_edit"};
  test::write(d / "project.ksl", "@project vault\nlang notes\n");
  test::write(d / "blocks/game/block.ksl", "@block game\npurpose Игра про кота\nkind leaf\n");
  test::write(d / "blocks/game/note.txt", "Кот. См. [[Сюжет|историю]] и [[story]].");
  test::write(d / "blocks/story/block.ksl", "@block story\npurpose Сюжет\nkind leaf\n");
  test::write(d / "blocks/music/block.ksl", "@block music\npurpose Музыка\nkind leaf\n");  // no note.txt yet
  auto load = [&] {
    auto s = core::load_schema(d.path());
    REQUIRE(s);
    return std::move(*s);
  };
  // A link from a note without a file yet: the file is made.
  REQUIRE(code::add_note_link(load(), "music", "game"));
  CHECK(test::read(d / "blocks/music/note.txt") == "[[Игра про кота]]\n");
  REQUIRE(code::add_note_link(load(), "game", "music"));
  CHECK(test::read(d / "blocks/game/note.txt") == "Кот. См. [[Сюжет|историю]] и [[story]].\n[[Музыка]]\n");

  // The title changes: [[Сюжет|историю]] follows (its shown text kept), [[story]] (by id) stays.
  auto changed = code::retitle_note(load(), "story", "Сюжет игры");
  REQUIRE(changed);
  CHECK(*changed == std::vector<std::string>{"game"});
  CHECK(test::read(d / "blocks/game/note.txt") == "Кот. См. [[Сюжет игры|историю]] и [[story]].\n[[Музыка]]\n");
  CHECK(load().find("story")->purpose.value == "Сюжет игры");
  CHECK_FALSE(code::retitle_note(load(), "story", "  "));

  // Every link to story goes: plain text stays.
  auto n = code::remove_note_link(load(), "game", "story");
  REQUIRE(n);
  CHECK(*n == 2);
  CHECK(test::read(d / "blocks/game/note.txt") == "Кот. См. историю и story.\n[[Музыка]]\n");
  int refs = 0;
  for (const auto& l : load().links()) refs += l.kind == core::LinkKind::Ref && l.dst_block == "story" ? 1 : 0;
  CHECK(refs == 0);
  CHECK_FALSE(code::add_note_link(load(), "game", "nothing"));
}

TEST_CASE("notes: a note made in Karkas is no leaf of code — no drift checks (contract tests, api)") {
  test::TempDir d{"vault_drift"};
  test::write(d / "project.ksl", "@project vault\nlang notes\n");
  test::write(d / "blocks/idea/block.ksl", "@block idea\npurpose Идея\nkind leaf\n");
  test::write(d / "blocks/idea/note.txt", "текст\n");
  auto s = core::load_schema(d.path());
  REQUIRE(s);
  CHECK(code::check_drift(*s).empty());
}

TEST_CASE("notes: plain words that name another note are links to suggest, one click makes them links") {
  test::TempDir d{"vault_mentions"};
  test::write(d / "project.ksl", "@project vault\nlang notes\n");
  test::write(d / "blocks/game/block.ksl", "@block game\npurpose Игра про кота\nkind leaf\n");
  test::write(d / "blocks/game/note.txt", "Главное — музыка.\nЕщё в игре про кота нужен СЮЖЕТ, см. [[Сюжет]].\nИгра про котан\n");
  test::write(d / "blocks/story/block.ksl", "@block story\npurpose Сюжет\nkind leaf\n");
  test::write(d / "blocks/story/note.txt", "Кот идёт домой. Игра про кота — это квест.\n");
  test::write(d / "blocks/music/block.ksl", "@block music\npurpose Музыка\nkind leaf\n");
  test::write(d / "blocks/ab/block.ksl", "@block ab\npurpose Ки\nkind leaf\n");  // too short to look for
  auto load = [&] {
    auto s = core::load_schema(d.path());
    REQUIRE(s);
    return std::move(*s);
  };
  CHECK(core::find_mentions("Музыкальная музыка, [[Музыка]] (МУЗЫКА)", "Музыка") == std::vector<std::size_t>{23, 55});
  CHECK(core::lower_text("ЁЖ Hello") == "ёж hello");
  CHECK(core::lower_text("ЁЖ Hello").size() == std::string("ЁЖ Hello").size());

  // game names music (no link yet); story it links already; "Ки" is too short.
  auto in_game = code::unlinked_mentions(load(), "game");
  REQUIRE(in_game.size() == 1);
  CHECK(in_game[0].dst == "music");
  CHECK(in_game[0].line == 1);
  CHECK(in_game[0].text == "Главное — музыка.");
  // story names the game without a link; game names itself ("в игре про кота" is another word form, "котан" no word).
  auto of_game = code::mentions_of(load(), "game");
  REQUIRE(of_game.size() == 1);
  CHECK(of_game[0].src == "story");

  REQUIRE(code::link_mention(load(), "story", "game"));
  CHECK(test::read(d / "blocks/story/note.txt") == "Кот идёт домой. [[Игра про кота]] — это квест.\n");
  REQUIRE(code::link_mention(load(), "game", "music"));
  CHECK(test::read(d / "blocks/game/note.txt") == "Главное — [[музыка]].\nЕщё в игре про кота нужен СЮЖЕТ, см. [[Сюжет]].\nИгра про котан\n");
  CHECK(code::mentions_of(load(), "game").empty());
  auto again = code::link_mention(load(), "story", "game");
  REQUIRE(again);
  CHECK_FALSE(*again);  // nothing left to link
}
