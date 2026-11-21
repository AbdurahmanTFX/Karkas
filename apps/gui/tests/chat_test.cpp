// The orchestrator chat: messages become karkas commands; a fake karkas stands in for the
// CLI (no models, no network).
#include <doctest/doctest.h>

#include <sys/stat.h>

#include "chat.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"
#include "kk/test/support.hpp"

using namespace kk;
using gui::ChatEntry;
using gui::ChatMode;

namespace {

#ifndef _WIN32  // the fake karkas is a shell script (docs/adr/0046)
std::filesystem::path fake_cli(const test::TempDir& d, const std::string& body) {
  auto p = d.path() / "karkas";
  REQUIRE(core::write_file(p, "#!/bin/sh\n" + body));
  ::chmod(p.c_str(), 0755);
  return p;
}
#endif

std::vector<ChatEntry> of_kind(const std::vector<ChatEntry>& all, ChatEntry::Kind k) {
  std::vector<ChatEntry> out;
  for (const auto& e : all) {
    if (e.kind == k) out.push_back(e);
  }
  return out;
}

}  // namespace

TEST_CASE("chat: a message in block or ticket mode stays inside its block or goes to its ticket (ADR 0034)") {
  std::string err;
  gui::ChatScope block{gui::ChatScope::Kind::Block, "stream.ring"};
  gui::ChatScope ticket{gui::ChatScope::Kind::Ticket, "T-0003"};
  CHECK(gui::chat_command(ChatMode::Task, "add reset", &err, block) == std::vector<std::string>{"task", "--block", "stream.ring", "add reset"});
  CHECK(gui::chat_command(ChatMode::Bug, "drops frames", &err, block) ==
        std::vector<std::string>{"debug", "--block", "stream.ring", "drops frames"});
  CHECK(gui::chat_command(ChatMode::Task, " also on seek ", &err, ticket) ==
        std::vector<std::string>{"ticket", "run", "T-0003", "--note", "also on seek"});
  CHECK(gui::chat_command(ChatMode::Bug, "also on seek", &err, ticket) ==
        std::vector<std::string>{"ticket", "run", "T-0003", "--note", "also on seek"});
  // A /command is that command in any mode.
  CHECK(gui::chat_command(ChatMode::Task, "/approve C-0002", &err, block) == std::vector<std::string>{"approve", "C-0002"});
  CHECK(block.str() == "block stream.ring");
  CHECK(ticket.str() == "ticket T-0003");
  CHECK(gui::ChatScope{}.str().empty());
}

TEST_CASE("chat: a message is the karkas command a user would type (ADR 0019)") {
  std::string err;
  CHECK(gui::chat_command(ChatMode::Task, "  add a volume knob ", &err) == std::vector<std::string>{"task", "add a volume knob"});
  CHECK(gui::chat_command(ChatMode::Bug, "crackle after 10 s", &err) == std::vector<std::string>{"debug", "crackle after 10 s"});
  CHECK(gui::chat_command(ChatMode::Task, "/ticket run T-0003", &err) == std::vector<std::string>{"ticket", "run", "T-0003"});
  CHECK(gui::chat_command(ChatMode::Task, R"(/debug "no sound" --fact 'probe fill min=0')", &err) ==
        std::vector<std::string>{"debug", "no sound", "--fact", "probe fill min=0"});
  CHECK(gui::chat_command(ChatMode::Task, "/approve C-0002", &err) == std::vector<std::string>{"approve", "C-0002"});
  CHECK(gui::chat_command(ChatMode::Task, "   ", &err).empty());
  CHECK(gui::chat_command(ChatMode::Task, "/mcp", &err).empty());
  CHECK(core::contains(err, "the chat runs these commands"));
  CHECK(gui::chat_command(ChatMode::Task, "/", &err).empty());
  CHECK(gui::ticket_in("created T-0004") == "T-0004");
  CHECK(gui::ticket_in("@ticket T-0012 kind=bug") == "T-0012");
  CHECK(gui::ticket_in("orchestrator root -> stream") == "");
  CHECK(gui::changes_in("C-0002 needs human approval (karkas approve C-0002); see C-0003") ==
        std::vector<std::string>{"C-0002", "C-0003"});
}

#ifndef _WIN32
TEST_CASE("chat: progress streams in, the result ends the command, the conversation is kept") {
  test::TempDir d{"chat"};
  auto root = d.path() / "proj";
  std::filesystem::create_directories(root / ".karkas");
  auto cli = fake_cli(d, "echo \"$@\" > \"$(dirname \"$0\")/args\"\n"
                         "echo '· created T-0007' >&2\n"
                         "echo '· orchestrator root -> stream' >&2\n"
                         "printf '@ticket T-0007 kind=feature\\nstatus  escalated\\nnote C-0001 needs human approval\\n'\n");
  {
    gui::Chat chat(root, cli, {});
    REQUIRE(chat.send(ChatMode::Task, "add a volume knob"));
    REQUIRE(chat.wait(std::chrono::seconds(10)));
    CHECK(core::trim(test::read(d.path() / "args")) == "--project " + root.string() + " task add a volume knob");
    auto all = chat.entries();
    REQUIRE(all.size() == 4);
    CHECK(all[0].kind == ChatEntry::Kind::User);
    CHECK(all[0].mode == "task");
    auto progress = of_kind(all, ChatEntry::Kind::Progress);
    REQUIRE(progress.size() == 2);
    CHECK(progress[1].text == "orchestrator root -> stream");
    CHECK(progress[1].ticket == "T-0007");
    auto result = of_kind(all, ChatEntry::Kind::Result);
    REQUIRE(result.size() == 1);
    CHECK(result[0].ticket == "T-0007");
    CHECK(core::contains(result[0].text, "status  escalated"));
    CHECK(chat.current_ticket() == "T-0007");
    // A message that is not a command the chat runs starts nothing.
    CHECK_FALSE(chat.send(ChatMode::Task, "/gui"));
    CHECK(of_kind(chat.entries(), ChatEntry::Kind::Error).size() == 1);
  }
  gui::Chat again(root, cli, {});
  CHECK(again.entries().size() == 6);
  again.clear();
  CHECK(again.entries().empty());
  CHECK_FALSE(std::filesystem::exists(root / ".karkas" / "chat.jsonl"));
}
#endif

#ifndef _WIN32
TEST_CASE("chat: Stop is Ctrl+C for the command, Kill ends its process group") {
  test::TempDir d{"chat"};
  auto root = d.path() / "proj";
  std::filesystem::create_directories(root / ".karkas");
  {
    auto cli = fake_cli(d, "trap 'echo \"karkas: T-0002 stopped by the user at stream; continue with: karkas ticket run T-0002\" >&2; exit 130' INT\n"
                           "echo '· created T-0002' >&2\n"
                           "sleep 30 &\nwait $!\n");
    gui::Chat chat(root, cli, {});
    REQUIRE(chat.send(ChatMode::Bug, "crackle"));
    for (int i = 0; i < 200 && chat.current_ticket().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(chat.busy());
    chat.stop();
    CHECK(chat.stopping_for().has_value());
    REQUIRE(chat.wait(std::chrono::seconds(10)));
    auto info = of_kind(chat.entries(), ChatEntry::Kind::Info);
    REQUIRE(info.size() == 2);
    CHECK(core::contains(info[1].text, "continue with: karkas ticket run T-0002"));
    CHECK(of_kind(chat.entries(), ChatEntry::Kind::Error).empty());
  }
  {
    auto cli = fake_cli(d, "trap '' INT\nsleep 30 &\nwait $!\n");
    gui::Chat chat(root, cli, {});
    REQUIRE(chat.send(ChatMode::Task, "x"));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    chat.stop();
    CHECK_FALSE(chat.wait(std::chrono::milliseconds(500)));  // ignores SIGINT
    chat.kill();
    CHECK(chat.wait(std::chrono::seconds(5)));
  }
}
#endif

TEST_CASE("chat: in a notes project a message is a request to the notes assistant (ADR 0039)") {
  std::string err;
  gui::ChatScope note{gui::ChatScope::Kind::Block, "plan.start"};
  CHECK(gui::chat_command(ChatMode::Task, " допиши план ", &err, {}, true) == std::vector<std::string>{"notes", "допиши план"});
  CHECK(gui::chat_command(ChatMode::Bug, "свяжи", &err, note, true) == std::vector<std::string>{"notes", "--block", "plan.start", "свяжи"});
  CHECK(gui::chat_command(ChatMode::Task, "/proposal approve N-0001", &err, {}, true) ==
        std::vector<std::string>{"proposal", "approve", "N-0001"});
  CHECK(gui::chat_command(ChatMode::Task, "/notes \"что нового?\"", &err) == std::vector<std::string>{"notes", "что нового?"});
}
