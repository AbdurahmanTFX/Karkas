// Agents for notes (docs/adr/0039): the assistant reads the notes and proposes edits; the human approves
// or rejects them. A scripted model stands in for the model (no network, no money).
#include <doctest/doctest.h>

#include "kk/agents/engine.hpp"
#include "kk/agents/notes_agent.hpp"
#include "kk/core/notes.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"
#include "scripted.hpp"

using namespace kk;
using json = nlohmann::json;

namespace {

void vault(const test::TempDir& d) {
  test::write(d / "project.ksl", "@project vault\nlang notes\n");
  test::write(d / "blocks/plan/block.ksl", "@block plan\npurpose План\nkind group\n");
  test::write(d / "blocks/plan/start/block.ksl", "@block plan.start\npurpose Запуск\nkind leaf\n");
  test::write(d / "blocks/plan/start/note.txt", "Шаг 1: собрать команду.\nБюджет — отдельно.\n");
  test::write(d / "blocks/money/block.ksl", "@block money\npurpose Бюджет\nkind leaf\n");
  test::write(d / "blocks/money/note.txt", "100 000 на старт.\n");
}

struct Rig {
  test::TempDir d{"notes_agent"};
  test::ScriptedModel model;
  test::FakeTests tests;
  std::unique_ptr<agents::Engine> engine;
  explicit Rig(bool auto_approve = false) {
    vault(d);
    auto cfg = test::test_config();
    cfg.auto_approve = auto_approve;
    agents::EngineOptions o;
    o.providers = model.factory();
    o.tests = &tests;
    auto e = agents::Engine::create(d.path(), cfg, o);
    REQUIRE_MESSAGE(e, (e ? "" : e.error().str()));
    engine = std::move(*e);
  }
  std::string request(std::size_t i) { return test::ScriptedModel::request_text(model.calls.at(i).req); }
  // The text of the tool result the model got last in request i.
  std::string tool_result(std::size_t i) {
    auto t = request(i);
    return t;
  }
};

agents::ToolCall call(const std::string& name, const json& input) { return {"c1", name, input.dump()}; }

}  // namespace

TEST_CASE("notes agent: the tools read the notes and collect proposed edits, reads see the run's own edits") {
  test::TempDir d{"notes_tools"};
  vault(d);
  auto s = core::load_schema(d.path());
  REQUIRE(s);
  agents::NotesToolHost host(*s, "");
  auto ov = host.call(call("notes_overview", json::object()));
  CHECK(core::contains(ov.content, "topic plan \"План\" (1 inside)"));
  CHECK(core::contains(ov.content, "note plan.start \"Запуск\": Шаг 1: собрать команду. / Бюджет — отдельно."));
  CHECK(core::contains(host.call(call("search_notes", {{"text", "КОМАНДУ"}})).content, "plan.start \"Запуск\": Шаг 1: собрать команду."));
  CHECK(core::contains(host.call(call("search_notes", {{"text", "нет такого"}})).content, "nothing found"));
  // "Бюджет" is a title named in plain words.
  CHECK(core::contains(host.call(call("mentions", {{"note", "plan.start"}})).content, "plan.start names \"Бюджет\" (money) at line 2"));
  CHECK(host.call(call("read_note", {{"note", "plan"}})).is_error);  // a topic

  auto w = host.call(call("write_note", {{"note", "plan.start"}, {"text", "Шаг 1: собрать команду.\n[[Бюджет]] — отдельно."}}));
  CHECK_FALSE(w.is_error);
  CHECK(core::contains(host.call(call("read_note", {{"note", "plan.start"}})).content, "[[Бюджет]] — отдельно.\n"));
  auto t = host.call(call("new_topic", {{"title", "Идеи"}}));
  CHECK(core::contains(t.content, "new topic idei"));
  auto n = host.call(call("new_note", {{"topic", "idei"}, {"title", "Реклама"}, {"text", "Листовки"}}));
  CHECK(core::contains(n.content, "new note idei.reklama"));
  CHECK(host.call(call("write_note", {{"note", "idei.reklama"}, {"text", "Листовки и радио"}})).content.find("proposed") == 0);
  CHECK(host.call(call("new_note", {{"topic", "nowhere"}, {"title", "X"}, {"text", ""}})).is_error);
  CHECK(host.call(call("new_note", {{"title", "a [b]"}, {"text", ""}})).is_error);
  CHECK(host.call(call("write_note", {{"note", "nothing"}, {"text", "x"}})).is_error);
  auto f = host.call(call("finish", {{"answer", "Связал запуск с бюджетом."}, {"summary", "ссылка и тема идей"}}));
  CHECK(f.terminal);
  REQUIRE(host.edits().size() == 3);
  CHECK(host.edits()[0].kind == "write");
  CHECK(host.edits()[0].base == "Шаг 1: собрать команду.\nБюджет — отдельно.\n");
  CHECK(host.edits()[2].kind == "note");
  CHECK(host.edits()[2].text == "Листовки и радио\n");  // the later write went into the new note
  CHECK(host.answer() == "Связал запуск с бюджетом.");
  CHECK(test::read(d / "blocks/plan/start/note.txt") == "Шаг 1: собрать команду.\nБюджет — отдельно.\n");  // nothing written
}

TEST_CASE("notes agent: a request becomes an answer and a proposal, nothing is written until it is approved") {
  Rig rig;
  rig.model.tool("search_notes", {{"text", "бюджет"}});
  rig.model.tool("read_note", {{"note", "plan.start"}});
  rig.model.tool("write_note", {{"note", "plan.start"}, {"text", "Шаг 1: собрать команду.\n[[Бюджет]] — отдельно.\nШаг 2: офис.\n"}});
  rig.model.tool("new_note", {{"topic", "plan"}, {"title", "Офис"}, {"text", "Аренда, см. [[Бюджет]]."}});
  rig.model.tool("finish", {{"answer", "Добавил шаг 2 и заметку про офис."}, {"summary", "шаг 2 и офис"}});
  auto r = rig.engine->notes("допиши план: нужен офис", "plan.start");
  REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
  CHECK(core::contains(*r, "Добавил шаг 2 и заметку про офис."));
  CHECK(core::contains(*r, "proposal N-0001: шаг 2 и офис (~ plan.start, + plan.ofis)"));
  CHECK(core::contains(*r, "karkas proposal approve N-0001"));
  REQUIRE(rig.model.calls.size() == 5);
  auto first = rig.request(0);
  CHECK(core::contains(first, "assistant of a notes project"));  // the role's prompt
  CHECK(core::contains(first, "note money \"Бюджет\": 100 000 на старт."));  // the overview
  CHECK(core::contains(first, "Request of the human: допиши план: нужен офис"));
  CHECK(core::contains(first, "The request is about the note plan.start \"Запуск\""));
  CHECK(core::contains(rig.request(2), "Шаг 1: собрать команду."));  // read_note's text reached the model
  // Proposed, not written.
  CHECK(test::read(rig.d / "blocks/plan/start/note.txt") == "Шаг 1: собрать команду.\nБюджет — отдельно.\n");
  CHECK_FALSE(std::filesystem::exists(rig.d / "blocks/plan/ofis"));
  agents::NotesProposalStore store(rig.d.path());
  auto p = store.get("N-0001");
  REQUIRE(p);
  CHECK(p->status == "pending");
  CHECK(p->scope == "plan.start");
  auto preview = agents::notes_proposal_preview(rig.d.path(), *p);
  CHECK(core::contains(preview, "~~~ plan.start \"Запуск\"\n Шаг 1: собрать команду.\n-Бюджет — отдельно.\n+[[Бюджет]] — отдельно.\n+Шаг 2: офис.\n"));
  CHECK(core::contains(preview, "+++ new note plan.ofis \"Офис\" in plan\n+Аренда, см. [[Бюджет]].\n"));
  CHECK_FALSE(core::contains(preview, "[[Бюджет]].\n+\n"));  // no empty line for the text's last line break

  auto ok = agents::approve_notes_proposal(rig.d.path(), "N-0001");
  REQUIRE_MESSAGE(ok, (ok ? "" : ok.error().message));
  CHECK(test::read(rig.d / "blocks/plan/start/note.txt") == "Шаг 1: собрать команду.\n[[Бюджет]] — отдельно.\nШаг 2: офис.\n");
  CHECK(test::read(rig.d / "blocks/plan/ofis/note.txt") == "Аренда, см. [[Бюджет]].\n");
  auto s = core::load_schema(rig.d.path());
  REQUIRE(s);
  REQUIRE(s->find("plan.ofis") != nullptr);
  CHECK(s->find("plan.ofis")->purpose.value == "Офис");
  int refs = 0;
  for (const auto& l : s->links()) refs += l.kind == core::LinkKind::Ref && l.dst_block == "money" ? 1 : 0;
  CHECK(refs == 2);
  CHECK(store.get("N-0001")->status == "approved");
  CHECK_FALSE(agents::approve_notes_proposal(rig.d.path(), "N-0001"));  // once
}

TEST_CASE("notes agent: a note changed since the proposal refuses it whole, a rejected one writes nothing") {
  Rig rig;
  rig.model.tool("write_note", {{"note", "money"}, {"text", "200 000 на старт.\n"}});
  rig.model.tool("new_topic", {{"title", "Риски"}});
  rig.model.tool("finish", {{"answer", "Поднял бюджет."}, {"summary", "бюджет"}});
  REQUIRE(rig.engine->notes("подними бюджет"));
  test::write(rig.d / "blocks/money/note.txt", "150 000 на старт.\n");  // the human edited it meanwhile
  auto r = agents::approve_notes_proposal(rig.d.path(), "N-0001");
  REQUIRE_FALSE(r);
  CHECK(core::contains(r.error().message, "money changed since the proposal"));
  CHECK_FALSE(std::filesystem::exists(rig.d / "blocks/riski"));  // nothing of it
  CHECK(core::contains(agents::notes_proposal_preview(rig.d.path(), *agents::NotesProposalStore(rig.d.path()).get("N-0001")),
                       "(changed since the proposal)"));
  REQUIRE(agents::reject_notes_proposal(rig.d.path(), "N-0001"));
  CHECK(agents::NotesProposalStore(rig.d.path()).get("N-0001")->status == "rejected");
  CHECK(test::read(rig.d / "blocks/money/note.txt") == "150 000 на старт.\n");
}

TEST_CASE("notes agent: a question gets an answer and no proposal, auto applies one at once, task goes to the assistant") {
  {
    Rig rig;
    rig.model.tool("read_note", {{"note", "money"}});
    rig.model.tool("finish", {{"answer", "На старт 100 000."}});
    auto r = rig.engine->notes("сколько денег на старт?");
    REQUIRE(r);
    CHECK(core::contains(*r, "На старт 100 000."));
    CHECK_FALSE(core::contains(*r, "proposal"));
    CHECK(agents::NotesProposalStore(rig.d.path()).list()->empty());
  }
  {
    Rig rig(/*auto_approve=*/true);
    rig.model.tool("new_note", {{"title", "Команда"}, {"text", "Ищем двух людей."}});
    rig.model.tool("finish", {{"answer", "Завёл заметку."}, {"summary", "команда"}});
    auto r = rig.engine->task("заведи заметку про команду");  // a notes project: the assistant, not a ticket
    REQUIRE_MESSAGE(r, (r ? "" : r.error().str()));
    CHECK(core::contains(*r, "auto: approved N-0001"));
    CHECK(test::read(rig.d / "blocks/komanda/note.txt") == "Ищем двух людей.\n");
    CHECK_FALSE(std::filesystem::exists(rig.d / ".karkas/tickets/T-0001.ksl"));
  }
  {
    Rig rig;
    CHECK_FALSE(rig.engine->notes("что-то", "nowhere"));  // an unknown scope, before any model call
    CHECK(rig.model.calls.empty());
  }
  CHECK(agents::proposals_in("see N-0002 and N-0002, C-0001, xN-0003") == std::vector<std::string>{"N-0002"});
  CHECK(agents::text_diff("a\nb\n", "a\nc\n") == " a\n-b\n+c\n");
}
