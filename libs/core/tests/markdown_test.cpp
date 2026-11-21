// Markdown of notes (docs/adr/0040): blocks, inline marks, links, plain lines.
#include <doctest/doctest.h>

#include "kk/core/markdown.hpp"

using namespace kk::core;
using md::Kind;

namespace {

// The runs as "text" with marks: *italic*, **bold**, ~~strike~~, `code`, <text|href>, [[shown|name]], !alt|src.
std::string show(const std::vector<md::Run>& runs) {
  std::string out;
  for (const auto& r : runs) {
    std::string t = r.text;
    if (r.code) t = "`" + t + "`";
    if (r.italic) t = "*" + t + "*";
    if (r.bold) t = "**" + t + "**";
    if (r.strike) t = "~~" + t + "~~";
    if (r.image) {
      t = "!" + t + "|" + r.href + (r.width > 0 ? "@" + std::to_string(r.width) : "") + (r.wiki ? "~" : "");
    } else if (r.wiki) {
      t = "[[" + t + "|" + r.href + "]]";
    } else if (!r.href.empty()) {
      t = "<" + t + "|" + r.href + ">";
    }
    out += t;
  }
  return out;
}

std::string inl(std::string_view s) { return show(md::parse_inline(s)); }

}  // namespace

TEST_CASE("markdown: emphasis, strikethrough and code spans") {
  CHECK(inl("plain text") == "plain text");
  CHECK(inl("a **bold** b") == "a **bold** b");
  CHECK(inl("a *it* and _it_ b") == "a *it* and *it* b");
  CHECK(inl("***both***") == "***both***");
  CHECK(inl("**bold *it* bold**") == "**bold *****it***** bold**");
  CHECK(inl("*it **bold** it*") == "*it ****bold**** it*");
  CHECK(inl("~~gone~~ ~one~") == "~~gone~~ ~one~");
  CHECK(inl("`a *b* c` d") == "`a *b* c` d");
  CHECK(inl("`` a ` b ``") == "`a ` b`");
  CHECK(inl("Жирный **текст** тут") == "Жирный **текст** тут");
}

TEST_CASE("markdown: what is not emphasis stays text") {
  CHECK(inl("2 * 3 * 4") == "2 * 3 * 4");
  CHECK(inl("snake_case_name") == "snake_case_name");
  CHECK(inl("**open") == "**open");
  CHECK(inl("\\*not\\*") == "*not*");
  CHECK(inl("`open") == "`open");
  CHECK(inl("a ** b") == "a ** b");
  CHECK(inl("foo*bar*baz") == "foo*bar*baz");
}

TEST_CASE("markdown: links, images, [[links]], autolinks and bare URLs") {
  CHECK(inl("see [the plan](plan.md) now") == "see <the plan|plan.md> now");
  CHECK(inl("[**b** c](x.md)") == "<**b**|x.md>< c|x.md>");
  CHECK(inl("[t](<my note.md> \"title\")") == "<t|my note.md>");
  CHECK(inl("[a](b(c).md)") == "<a|b(c).md>");
  CHECK(inl("[x](y.md#part)") == "<x|y.md#part>");
  CHECK(inl("[](z.md)") == "<z.md|z.md>");
  CHECK(inl("![кот](img/cat.png)") == "!кот|img/cat.png");
  CHECK(inl("[[Plan]] and [[ideas.game|игра]]") == "[[Plan|Plan]] and [[игра|ideas.game]]");
  CHECK(inl("<https://a.org/x> <me@mail.org>") == "<https://a.org/x|https://a.org/x> <me@mail.org|mailto:me@mail.org>");
  CHECK(inl("go https://a.org/p?q=1, then") == "go <https://a.org/p?q=1|https://a.org/p?q=1>, then");
  CHECK(inl("(see www.a.org)") == "(see <www.a.org|https://www.a.org>)");
  CHECK(inl("[not a link] (x)") == "[not a link] (x)");
}

TEST_CASE("markdown: a line break in a paragraph stays a line break") {
  auto b = md::parse("one\ntwo\\\nthree");
  REQUIRE(b.size() == 1);
  CHECK(b[0].kind == Kind::Paragraph);
  CHECK(md::plain(b[0].runs) == "one\ntwo\nthree");
}

TEST_CASE("markdown: headings, rules, paragraphs, setext headings") {
  auto b = md::parse("# Title #\n\ntext\nmore\n\n## Sub\n---\nunder\n===\n\n***");
  REQUIRE(b.size() == 6);
  CHECK(b[0].kind == Kind::Heading);
  CHECK(b[0].level == 1);
  CHECK(md::plain(b[0].runs) == "Title");
  CHECK(b[0].line == 1);
  CHECK(b[1].kind == Kind::Paragraph);
  CHECK(b[1].line == 3);
  CHECK(b[2].kind == Kind::Heading);
  CHECK(b[2].level == 2);
  CHECK(b[3].kind == Kind::Rule);  // "---" after a heading is a rule
  CHECK(b[4].kind == Kind::Heading);
  CHECK(b[4].level == 1);
  CHECK(md::plain(b[4].runs) == "under");
  CHECK(b[5].kind == Kind::Rule);
  auto tag = md::parse("#tag");
  REQUIRE(tag.size() == 1);
  CHECK(tag[0].kind == Kind::Paragraph);  // no space after '#': not a heading
  auto h2 = md::parse("text\n---");
  REQUIRE(h2.size() == 1);
  CHECK(h2[0].kind == Kind::Heading);
  CHECK(h2[0].level == 2);
}

TEST_CASE("markdown: lists nest, carry numbers and task boxes, go on over lines") {
  auto b = md::parse("- one\n  - inner\n    more\n- [ ] todo\n- [x] done\n\n1. first\n2) second\n\n  after\n");
  REQUIRE(b.size() == 7);
  CHECK(b[0].kind == Kind::Item);
  CHECK(b[0].marker == "-");
  CHECK(b[0].depth == 0);
  CHECK(b[1].depth == 1);
  CHECK(md::plain(b[1].runs) == "inner\nmore");
  CHECK(b[2].task == 0);
  CHECK(md::plain(b[2].runs) == "todo");
  CHECK(b[3].task == 1);
  CHECK(b[4].marker == "1.");
  CHECK(b[5].marker == "2)");
  CHECK(b[6].kind == Kind::Paragraph);  // inside the item, after a blank line
  CHECK(b[6].depth == 1);
}

TEST_CASE("markdown: a list interrupts a paragraph, a number other than 1 does not") {
  auto b = md::parse("Shopping:\n- milk\n\nIn\n2020. it rained");
  REQUIRE(b.size() == 3);
  CHECK(b[0].kind == Kind::Paragraph);
  CHECK(b[1].kind == Kind::Item);
  CHECK(b[2].kind == Kind::Paragraph);
  CHECK(md::plain(b[2].runs) == "In\n2020. it rained");
}

TEST_CASE("markdown: quotes hold blocks of their own") {
  auto b = md::parse("> quoted\n> - item\n>\n> > deeper\nafter");
  REQUIRE(b.size() == 4);
  CHECK(b[0].kind == Kind::Paragraph);
  CHECK(b[0].quote == 1);
  CHECK(b[1].kind == Kind::Item);
  CHECK(b[1].quote == 1);
  CHECK(b[2].quote == 2);
  CHECK(md::plain(b[2].runs) == "deeper");
  CHECK(b[2].line == 4);
  CHECK(b[3].quote == 0);
}

TEST_CASE("markdown: fenced and indented code keep their text") {
  auto b = md::parse("```cpp main\nint *a = **b;\n  [x](y.md)\n```\n\n    indented\n    code\n\ntext");
  REQUIRE(b.size() == 3);
  CHECK(b[0].kind == Kind::Code);
  CHECK(b[0].lang == "cpp");
  CHECK(b[0].code == "int *a = **b;\n  [x](y.md)");
  CHECK(b[1].kind == Kind::Code);
  CHECK(b[1].code == "indented\ncode");
  CHECK(b[2].kind == Kind::Paragraph);
  auto open = md::parse("~~~\nnever closed");
  REQUIRE(open.size() == 1);
  CHECK(open[0].code == "never closed");
  auto in_list = md::parse("- item\n  ```\n  code\n  ```");
  REQUIRE(in_list.size() == 2);
  CHECK(in_list[1].kind == Kind::Code);
  CHECK(in_list[1].depth == 1);
  CHECK(in_list[1].code == "code");
}

TEST_CASE("markdown: tables with alignment, escaped pipes and short rows") {
  auto b = md::parse("| Name | Sum |\n|:-----|----:|\n| a \\| b | **1** |\n| c |\n\nnext");
  REQUIRE(b.size() == 2);
  REQUIRE(b[0].kind == Kind::Table);
  CHECK(b[0].align == std::vector<char>{'l', 'r'});
  REQUIRE(b[0].rows.size() == 3);
  CHECK(md::plain(b[0].rows[0][1]) == "Sum");
  CHECK(md::plain(b[0].rows[1][0]) == "a | b");
  CHECK(show(b[0].rows[1][1]) == "**1**");
  CHECK(b[0].rows[2].size() == 2);
  CHECK(b[0].rows[2][1].empty());
  auto no = md::parse("a | b\n---");  // no pipe in the second line: a heading, not a table
  REQUIRE(no.size() == 1);
  CHECK(no[0].kind == Kind::Heading);
}

TEST_CASE("markdown: front matter is a block of its own") {
  auto b = md::parse("---\ntags: [a, b]\ntitle: x\n---\n# Head");
  REQUIRE(b.size() == 2);
  CHECK(b[0].kind == Kind::Meta);
  CHECK(b[0].code == "tags: [a, b]\ntitle: x");
  CHECK(b[1].kind == Kind::Heading);
  CHECK(b[1].line == 5);
}

TEST_CASE("markdown: the links of a text, outside code, with lines and offsets") {
  std::string t = "---\nlink: [no](meta.md)\n---\nSee [plan](plan.md) and ![img](a.png).\n`[code](c.md)`\n```\n[fenced](f.md)\n```\n[[wiki]] [two](<my note.md>)";
  auto l = md::links(t);
  REQUIRE(l.size() == 2);
  CHECK(l[0].label == "plan");
  CHECK(l[0].href == "plan.md");
  CHECK(l[0].line == 4);
  CHECK(t.substr(l[0].begin, l[0].end - l[0].begin) == "[plan](plan.md)");
  CHECK(l[1].href == "my note.md");
  CHECK(l[1].line == 9);
  CHECK(t.substr(l[1].begin, l[1].end - l[1].begin) == "[two](<my note.md>)");
}

TEST_CASE("markdown: plain lines for the schema") {
  auto p = md::plain_lines("---\nx: 1\n---\n# План\n\n**Шаг** 1\nдва\n\n- [ ] купить\n  - [x] *молоко*\n1. раз\n\n---\n| a | b |\n|---|---|\n| 1 | 2 |\n");
  CHECK(p == std::vector<std::string>{"План", "Шаг 1", "два", "☐ купить", "  ☑ молоко", "1. раз", "a | b", "1 | 2"});
  CHECK(md::plain_lines("a\nb\nc", 2) == std::vector<std::string>{"a", "b"});
}

TEST_CASE("markdown: file names, URLs, percent escapes") {
  CHECK(md::is_markdown_file("notes/Plan.MD"));
  CHECK(md::is_markdown_file("a.markdown"));
  CHECK_FALSE(md::is_markdown_file("a.txt"));
  CHECK(md::is_url("https://a.org"));
  CHECK(md::is_url("mailto:me@a.org"));
  CHECK(md::is_url("www.a.org"));
  CHECK_FALSE(md::is_url("C:/notes/a.md"));
  CHECK_FALSE(md::is_url("notes/a.md"));
  CHECK_FALSE(md::is_url("#part"));
  CHECK(md::percent_decode("My%20Note.md") == "My Note.md");
  CHECK(md::percent_decode("%D0%BA%D0%BE%D1%82") == "кот");
  CHECK(md::percent_decode("100%") == "100%");
}

// --- images (docs/adr/0041) ---------------------------------------------------------------------------------

TEST_CASE("markdown: images with an Obsidian size, embeds of image files, embeds of notes stay links") {
  CHECK(inl("![диаграмма|300](img/d.png)") == "!диаграмма|img/d.png@300");
  CHECK(inl("![a|b](x.png)") == "!a|b|x.png");  // not a size: the alt text as written
  CHECK(inl("![|120x80](x.png)") == "!|x.png@120");
  CHECK(inl("see ![[cat.png|200]] here") == "see !cat.png|cat.png@200~ here");
  CHECK(inl("![[Photo.JPG]]") == "!Photo.JPG|Photo.JPG~");
  CHECK(inl("![[a.png|подпись]]") == "!a.png|a.png~");
  CHECK(inl("![[Note]]") == "![[Note|Note]]");  // an embed of a note: a [[link]] after '!'
}

TEST_CASE("markdown: the images of a text outside code, with lines, alt text, size and kind") {
  std::string t = "![a](x.png) and ![[y.png|40]]\n`![c](code.png)`\n```\n![f](f.png)\n```\n![[Note]] [l](z.md) ![b|64](<my pic.png>)";
  auto im = md::images(t);
  REQUIRE(im.size() == 3);
  CHECK(im[0].src == "x.png");
  CHECK(im[0].alt == "a");
  CHECK_FALSE(im[0].wiki);
  CHECK(im[0].line == 1);
  CHECK(im[1].src == "y.png");
  CHECK(im[1].wiki);
  CHECK(im[1].width == 40);
  CHECK(im[2].src == "my pic.png");
  CHECK(im[2].alt == "b");
  CHECK(im[2].width == 64);
  CHECK(im[2].line == 6);
  CHECK(md::links(t).size() == 1);  // [l](z.md) only
  CHECK(md::is_image_file("a/B.PNG"));
  CHECK(md::is_image_file("x.jpeg"));
  CHECK(md::is_image_file("x.webp"));
  CHECK_FALSE(md::is_image_file("x.md"));
  CHECK_FALSE(md::is_image_file("png"));
}
