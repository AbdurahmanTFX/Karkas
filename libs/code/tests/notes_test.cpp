// Notes projects (docs/adr/0038): a folder of .txt notes becomes a notes project by itself; its files stay
// where they are, folders become topics.
#include <doctest/doctest.h>

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
