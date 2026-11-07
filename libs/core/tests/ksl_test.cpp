#include <doctest/doctest.h>

#include "kk/core/ksl.hpp"

using namespace kk::core;

TEST_CASE("ksl: header, lines and attributes") {
  auto r = parse_records("@report T-0042 from=stream.ring role=worker\nstatus  fixed\n", "r.ksl");
  REQUIRE(r);
  REQUIRE(r->size() == 1);
  const auto& rec = (*r)[0];
  CHECK(rec.kind == "report");
  CHECK(rec.id == "T-0042");
  REQUIRE(rec.attrs.size() == 2);
  CHECK(rec.attrs[0].key == "from");
  CHECK(rec.attrs[0].value == "stream.ring");
  REQUIRE(rec.lines.size() == 1);
  CHECK(rec.lines[0].directive == "status");
  CHECK(rec.lines[0].text == "fixed");
  CHECK(rec.lines[0].line == 2);
  CHECK(rec.lines[0].text_col == 9);
}

TEST_CASE("ksl: comments are kept as lead and trailing trivia") {
  auto r = parse_records("# card of the ring\n@block a\n# why\npurpose x  # trailing note\n# end\n", "a.ksl");
  REQUIRE(r);
  const auto& rec = (*r)[0];
  REQUIRE(rec.trivia.lead.size() == 1);
  CHECK(rec.trivia.lead[0] == "card of the ring");
  REQUIRE(rec.lines.size() == 1);
  CHECK(rec.lines[0].text == "x");
  CHECK(rec.lines[0].trivia.lead == std::vector<std::string>{"why"});
  CHECK(rec.lines[0].trivia.has_trailing);
  CHECK(rec.lines[0].trivia.trailing == "trailing note");
  CHECK(rec.tail == std::vector<std::string>{"end"});
}

TEST_CASE("ksl: '#' inside quotes or inside a word is not a comment") {
  auto r = parse_records("@block a\nnote    \"issue #12 fixed\"\npurpose port C# bindings\n", "a.ksl");
  REQUIRE(r);
  const auto& rec = (*r)[0];
  CHECK(rec.lines[0].text == "\"issue #12 fixed\"");
  CHECK_FALSE(rec.lines[0].trivia.has_trailing);
  CHECK(rec.lines[1].text == "port C# bindings");
}

TEST_CASE("ksl: sections and multiple records") {
  auto r = parse_records("@ticket T-0001\nkind bug\n--- trail\nL0 orchestrator at=root\n--- result\nstatus fixed\n"
                         "@ticket T-0002\nkind feature\n",
                         "t.ksl");
  REQUIRE(r);
  REQUIRE(r->size() == 2);
  REQUIRE((*r)[0].sections.size() == 2);
  CHECK((*r)[0].sections[0].name == "trail");
  CHECK((*r)[0].sections[0].lines[0].directive == "L0");
  CHECK((*r)[0].sections[1].lines[0].text == "fixed");
  CHECK((*r)[1].id == "T-0002");
}

TEST_CASE("ksl: diff section lines are raw, including '#'") {
  auto r = parse_records("@change C-0003\n--- diff\n+ @block dsp.reverb\n+ note x # kept\n~ @block dsp.chain\n- call a:b\n",
                         "c.ksl");
  REQUIRE(r);
  const auto& d = (*r)[0].sections[0].lines;
  REQUIRE(d.size() == 4);
  CHECK(d[0].directive == "+");
  CHECK(d[0].text == "@block dsp.reverb");
  CHECK(d[1].text == "note x # kept");
  CHECK(d[2].directive == "~");
  CHECK(d[3].directive == "-");
  CHECK(d[3].text == "call a:b");
}

TEST_CASE("ksl: unknown record kind is S02 with file:line:col") {
  auto r = parse_records("\n  @blok a\n", "x/block.ksl");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "S02");
  CHECK(r.error().str() == "x/block.ksl:2:3: error S02: unknown record kind '@blok'");
}

TEST_CASE("ksl: content before the first header is S01") {
  auto r = parse_records("# c\npurpose x\n@block a\n", "f.ksl");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "S01");
  CHECK(r.error().loc.line == 2);
  CHECK(r.error().loc.col == 1);
}

TEST_CASE("ksl: malformed section header is S01") {
  auto r = parse_records("@ticket T-0001\n--- Trail Two\n", "t.ksl");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "S01");
  CHECK(r.error().loc.line == 2);
}

TEST_CASE("ksl: invalid record id is S06 at the id column") {
  auto r = parse_records("@ticket 42\n", "t.ksl");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "S06");
  CHECK(r.error().str() == "t.ksl:1:9: error S06: invalid id '42' for @ticket");
  auto b = parse_records("@block Stream.Ring\n", "b.ksl");
  REQUIRE_FALSE(b);
  CHECK(b.error().code == "S06");
}

TEST_CASE("ksl: header extras must be attributes") {
  auto r = parse_records("@report T-0001 worker\n", "r.ksl");
  REQUIRE_FALSE(r);
  CHECK(r.error().code == "S06");
  CHECK(r.error().loc.col == 16);
}

TEST_CASE("ksl: tokenizer keeps balanced brackets together") {
  Line l;
  l.text = "open(const Path& p, int n) -> std::map<K, V> via=ring_buffer(64)";
  l.line = 1;
  l.text_col = 9;
  auto t = tokenize(l, "f");
  REQUIRE(t);
  REQUIRE(t->size() == 4);
  CHECK((*t)[0].text == "open(const Path& p, int n)");
  CHECK((*t)[1].text == "->");
  CHECK((*t)[2].text == "std::map<K, V>");
  CHECK((*t)[3].kind == Token::Kind::Attr);
  CHECK((*t)[3].key == "via");
  CHECK((*t)[3].value == "ring_buffer(64)");
  CHECK((*t)[3].col == 9 + 45);
}

TEST_CASE("ksl: quoted attribute values and escapes") {
  Line l;
  l.text = R"(at=root why="frames dropped; \"ring\" runs empty" pick=stream)";
  l.text_col = 1;
  auto t = tokenize(l, "f");
  REQUIRE(t);
  REQUIRE(t->size() == 3);
  CHECK((*t)[1].value == "frames dropped; \"ring\" runs empty");
  CHECK((*t)[2].value == "stream");
}

TEST_CASE("ksl: unterminated quote is S04 with the column of the token") {
  Line l;
  l.text = "a=1 why=\"open";
  l.line = 7;
  l.text_col = 4;
  auto t = tokenize(l, "f.ksl");
  REQUIRE_FALSE(t);
  CHECK(t.error().str() == "f.ksl:7:8: error S04: unterminated quoted string");
}

TEST_CASE("ksl: free text may be one quoted string") {
  Line l;
  l.text = "\"a # b\"";
  auto ok = free_text(l, "f");
  REQUIRE(ok);
  CHECK(*ok == "a # b");
  l.text = "\"a\" extra";
  l.line = 3;
  l.text_col = 9;
  auto bad = free_text(l, "f");
  REQUIRE_FALSE(bad);
  CHECK(bad.error().code == "S04");
  CHECK(bad.error().loc.col == 12);
}

TEST_CASE("ksl: CRLF line endings are tolerated") {
  auto r = parse_records("@block a\r\npurpose x\r\n", "a.ksl");
  REQUIRE(r);
  CHECK((*r)[0].lines[0].text == "x");
}

TEST_CASE("ksl: canonical directive column is 8") {
  CHECK(format_line("in", "push Frame[]") == "in      push Frame[]");
  CHECK(format_line("purpose", "x") == "purpose x");
  CHECK(format_line("verylongdir", "x") == "verylongdir x");
  CHECK(format_line("kind", "") == "kind");
  CHECK(format_free_text("plain text") == "plain text");
  CHECK(format_free_text("has # hash") == "\"has # hash\"");
  CHECK(format_attr("why", "two words") == "why=\"two words\"");
}
