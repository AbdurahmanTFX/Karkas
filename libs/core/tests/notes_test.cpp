// Notes projects (docs/adr/0038): notes as text files, [[links]] read from the text, their checks.
#include <doctest/doctest.h>

#include "kk/core/notes.hpp"
#include "kk/core/schema.hpp"
#include "kk/core/text.hpp"
#include "kk/core/validate.hpp"
#include "kk/test/support.hpp"

using namespace kk;
using namespace kk::core;

namespace {

void notes_project(const test::TempDir& d) {
  test::write(d / "project.ksl", "@project vault\nlang notes\n");
  test::write(d / "blocks/ideas/block.ksl", "@block ideas\npurpose Идеи\nkind group\n");
  test::write(d / "blocks/ideas/game/block.ksl", "@block ideas.game\npurpose Игра про кота\nkind leaf\n");
  test::write(d / "blocks/ideas/game/note.txt", "Кот ловит мышей.\nСм. [[Музыка]] и [[ideas/story|сюжет]], ещё [[нет такой]].\n");
  test::write(d / "blocks/ideas/story/block.ksl", "@block ideas.story\npurpose Сюжет\nkind leaf\n");
  test::write(d / "blocks/ideas/story/note.txt", "[[игра про КОТА]] [[игра  про кота]]\n");
  // A note that was in the folder before Karkas: its card lists the file, spaces quoted.
  test::write(d / "blocks/music/block.ksl", "@block music\npurpose Музыка\nkind leaf\nfiles \"old notes/Музыка к игре.txt\"\n");
  test::write(d / "old notes/Музыка к игре.txt", "Ссылка на [[ideas]] — тему.\n");
}

}  // namespace

TEST_CASE("notes: [[links]] of a text, with lines, labels after '|', nothing across lines") {
  auto r = parse_note_refs("a [[One]] b\n[[ two | Two shown ]] [[]] [[open\nend]] [[x]]");
  REQUIRE(r.size() == 3);
  CHECK(r[0].name == "One");
  CHECK(r[0].line == 1);
  CHECK(r[1].name == "two");
  CHECK(r[1].line == 2);
  CHECK(r[2].name == "x");
  CHECK(r[2].line == 3);
}

TEST_CASE("notes: titles fold case and spaces, ids come from titles") {
  CHECK(fold_title("  Игра  про КОТА ") == "игра про кота");
  CHECK(fold_title("Ёлка Hello") == "ёлка hello");
  CHECK(ident_from_title("Идеи для игры") == "idei_dlya_igry");
  CHECK(ident_from_title("Щука и Ёж!") == "shchuka_i_yozh");
  CHECK(ident_from_title("2024 план") == "n_2024_plan");
  CHECK(ident_from_title("My Notes") == "my_notes");
  CHECK(ident_from_title("…") == "note");
  CHECK(is_ident(ident_from_title("Ўсё і ўсё")));
}

TEST_CASE("notes: the loader reads the links of each note, the schema draws them as ref lines") {
  test::TempDir d{"notes_schema"};
  notes_project(d);
  auto s = load_schema(d.path());
  REQUIRE(s);
  CHECK(is_notes_project(s->project));
  const auto* game = s->find("ideas.game");
  REQUIRE(game != nullptr);
  CHECK(note_file(*game) == "blocks/ideas/game/note.txt");
  CHECK(note_file(*s->find("music")) == "old notes/Музыка к игре.txt");
  CHECK(note_file(*s->find("ideas")).empty());
  REQUIRE(game->refs.size() == 3);
  CHECK(game->refs[0].target == "music");       // by title
  CHECK(game->refs[1].target == "ideas.story");  // by id with '/', label after '|'
  CHECK(game->refs[2].target.empty());
  CHECK(game->refs[2].matches == 0);
  std::vector<std::string> pairs;
  for (const auto& l : s->links()) {
    if (l.kind == LinkKind::Ref) pairs.push_back(l.src_block + "->" + l.dst_block);
  }
  // story names the game twice: one line; music links to a topic.
  CHECK(pairs == std::vector<std::string>{"ideas.game->music", "ideas.game->ideas.story", "ideas.story->ideas.game", "music->ideas"});
  CHECK(to_string(LinkKind::Ref) == "ref");
  CHECK(note_link_name(*s, "ideas.game") == "Игра про кота");
  CHECK(note_link_name(*s, "music") == "Музыка");
}

TEST_CASE("notes: checks of a notes project — kinds, broken and ambiguous links, no checks of code") {
  test::TempDir d{"notes_check"};
  notes_project(d);
  test::write(d / "blocks/ideas/twin/block.ksl", "@block ideas.twin\npurpose Сюжет\nkind leaf\n");  // a second "Сюжет"
  test::write(d / "blocks/ideas/game/note.txt", "[[Сюжет]] [[нет такой]]\n");
  test::write(d / "blocks/app/block.ksl", "@block app\npurpose runs\nkind entry\n");
  test::write(d / "blocks/empty/block.ksl", "@block empty\npurpose Пустая тема\nkind group\n");
  auto s = load_schema(d.path());
  REQUIRE(s);
  auto issues = check_schema(*s);
  std::vector<std::string> got;
  for (const auto& i : issues) got.push_back(i.code + " " + i.block + " " + i.message);
  CHECK(std::find(got.begin(), got.end(), "E10 app a notes project holds topics (group) and notes (leaf), not entry") != got.end());
  CHECK(std::find(got.begin(), got.end(), "W08 ideas.game [[нет такой]] names no note") != got.end());
  CHECK(std::find(got.begin(), got.end(), "W08 ideas.game [[Сюжет]] names 2 notes: write the id (topic.note)") != got.end());
  for (const auto& g : got) {
    CHECK_FALSE(g.starts_with("W06"));  // no guarantees asked of notes
    CHECK_FALSE(g.starts_with("E06 empty"));  // an empty topic is fine
    CHECK_FALSE(core::contains(g, "exactly one entry"));
  }
  for (const auto& i : issues) {
    if (i.code == "W08") CHECK(i.loc.file == "blocks/ideas/game/note.txt");
  }
}

TEST_CASE("notes: a listed path with spaces is written quoted and read back") {
  auto b = parse_block_text("@block music\npurpose Музыка\nkind leaf\nfiles \"old notes/a b.txt\" plain.txt\n", "block.ksl");
  REQUIRE(b);
  CHECK(b->file_list() == std::vector<std::string>{"old notes/a b.txt", "plain.txt"});
  auto text = write_block(*b);
  CHECK(core::contains(text, "files   \"old notes/a b.txt\" plain.txt"));
  auto again = parse_block_text(text, "block.ksl");
  REQUIRE(again);
  CHECK(again->file_list() == b->file_list());
}

// --- Markdown notes (docs/adr/0040) ---------------------------------------------------------------------

TEST_CASE("notes: a note's own text is note.md, a note written before keeps its note.txt") {
  test::TempDir d{"notes_md_file"};
  test::write(d / "project.ksl", "@project vault\nlang notes\n");
  test::write(d / "blocks/old/block.ksl", "@block old\npurpose Старая\nkind leaf\n");
  test::write(d / "blocks/old/note.txt", "текст\n");
  test::write(d / "blocks/new/block.ksl", "@block new\npurpose Новая\nkind leaf\n");  // nothing written yet
  test::write(d / "blocks/both/block.ksl", "@block both\npurpose Обе\nkind leaf\n");
  test::write(d / "blocks/both/note.txt", "old\n");
  test::write(d / "blocks/both/note.md", "new\n");
  auto s = load_schema(d.path());
  REQUIRE(s);
  CHECK(note_file(*s->find("old")) == "blocks/old/note.txt");
  CHECK(note_file(*s->find("new")) == "blocks/new/note.md");
  CHECK(note_file(*s->find("both")) == "blocks/both/note.md");
}

TEST_CASE("notes: the note file a Markdown link names") {
  CHECK(note_link_file("blocks/a/note.md", "../b/note.md") == "blocks/b/note.md");
  CHECK(note_link_file("vault/Index.md", "My%20Note.md#part") == "vault/My Note.md");
  CHECK(note_link_file("vault/Index.md", "sub/x.txt") == "vault/sub/x.txt");
  CHECK(note_link_file("vault/Index.md", "/top.markdown") == "top.markdown");
  CHECK_FALSE(note_link_file("vault/Index.md", "https://a.org/x.md"));
  CHECK_FALSE(note_link_file("vault/Index.md", "#part"));
  CHECK_FALSE(note_link_file("vault/Index.md", "pic.png"));
  CHECK_FALSE(note_link_file("vault/Index.md", "Other Note"));  // no extension: not a note's file
}

TEST_CASE("notes: in a Markdown note [text](file.md) links a note like [[name]], code and other files are no links") {
  test::TempDir d{"notes_md_links"};
  test::write(d / "project.ksl", "@project vault\nlang notes\n");
  test::write(d / "blocks/a/block.ksl", "@block a\npurpose План\nkind leaf\n");
  test::write(d / "blocks/a/note.md",
              "# План\nСм. [бюджет](../b/note.md), [[Идеи]] и [нет](nope.md).\n[сайт](https://x.org) ![](pic.png) [файл](doc.pdf)\n"
              "```\n[код](../b/note.md)\n```\n[старая](<../../vault/My Note.md>)\n");
  test::write(d / "blocks/b/block.ksl", "@block b\npurpose Бюджет\nkind leaf\n");
  test::write(d / "blocks/c/block.ksl", "@block c\npurpose Идеи\nkind leaf\nfiles \"vault/My Note.md\"\n");
  test::write(d / "vault/My Note.md", "[назад](/blocks/a/note.md)\n");
  test::write(d / "blocks/t/block.ksl", "@block t\npurpose Текст\nkind leaf\n");
  test::write(d / "blocks/t/note.txt", "[не ссылка](../b/note.md)\n");  // a .txt note: no Markdown links
  auto s = load_schema(d.path());
  REQUIRE(s);
  const auto* a = s->find("a");
  REQUIRE(a != nullptr);
  REQUIRE(a->refs.size() == 4);
  // By line; on one line the [[links]] first.
  CHECK(a->refs[0].target == "c");  // [[Идеи]]
  CHECK(a->refs[0].path.empty());
  CHECK(a->refs[1].target == "b");
  CHECK(a->refs[1].path == "blocks/b/note.md");
  CHECK(a->refs[1].line == 2);
  CHECK(a->refs[2].matches == 0);
  CHECK(a->refs[2].path == "blocks/a/nope.md");
  CHECK(a->refs[3].target == "c");
  CHECK(a->refs[3].line == 7);
  CHECK(s->find("c")->refs.size() == 1);
  CHECK(s->find("c")->refs[0].target == "a");
  CHECK(s->find("t")->refs.empty());
  std::vector<std::string> pairs;
  for (const auto& l : s->links()) {
    if (l.kind == LinkKind::Ref) pairs.push_back(l.src_block + "->" + l.dst_block);
  }
  CHECK(pairs == std::vector<std::string>{"a->c", "a->b", "c->a"});
  std::vector<std::string> w08;
  for (const auto& i : check_schema(*s)) {
    if (i.code == "W08") w08.push_back(i.message + " @" + i.loc.file + ":" + std::to_string(i.loc.line));
  }
  CHECK(w08 == std::vector<std::string>{"the link (nope.md) names no note: blocks/a/nope.md is not a note's file @blocks/a/note.md:2"});
}

TEST_CASE("notes: words inside a Markdown link are no mention") {
  auto at = find_mentions("см. [Бюджет](money.md) и Бюджет", "Бюджет");
  REQUIRE(at.size() == 1);
  CHECK(at[0] == std::string("см. [Бюджет](money.md) и ").size());
}
