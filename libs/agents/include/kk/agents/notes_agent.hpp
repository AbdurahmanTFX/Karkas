#pragma once
// Agents for notes (docs/adr/0039): one assistant run per request of the human. The assistant reads the
// notes (an overview, search, a note's text, the links to suggest), answers, and what it would change —
// a note's new text, a new note, a new topic — becomes a proposal N-NNNN that the human approves or
// rejects (approved at once with [agents] auto_approve). The model writes no file itself.
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "kk/agents/types.hpp"
#include "kk/core/error.hpp"
#include "kk/core/schema.hpp"

namespace kk::agents {

struct NoteEdit {
  std::string kind;    // "write" (a note's new text) | "note" (a new note) | "topic" (a new topic)
  std::string id;      // the note written, or the id the new note or topic gets
  std::string parent;  // a new note or topic: the topic it goes into ("" = the top)
  std::string title;   // a new note or topic
  std::string text;    // write, note: the whole text
  std::string base;    // write: the text it was written over (an approval refuses a note changed since)
};

struct NotesProposal {
  std::string id;                  // N-0001
  std::string status = "pending";  // pending | approved | rejected
  std::string request;             // the human's words
  std::string scope;               // the note or topic the request was about ("" = all notes)
  std::string summary;             // the assistant's line about its edits
  std::vector<NoteEdit> edits;
  std::int64_t ts = 0;
};

// .karkas/proposals/N-NNNN.json
class NotesProposalStore {
 public:
  explicit NotesProposalStore(std::filesystem::path root);
  [[nodiscard]] kk::Result<std::vector<NotesProposal>> list() const;  // newest first
  [[nodiscard]] kk::Result<NotesProposal> get(const std::string& id) const;
  kk::Result<NotesProposal> create(NotesProposal p);  // with the next id
  kk::VoidResult save(const NotesProposal& p);
  [[nodiscard]] std::filesystem::path dir() const;

 private:
  std::filesystem::path root_;
};

// Applies the edits: topics, then new notes (card and note.txt), then texts. A note whose text changed
// since the proposal, or a new id taken since, refuses the whole proposal: nothing is written. Returns
// the blocks touched.
kk::Result<std::vector<std::string>> apply_notes_proposal(const std::filesystem::path& root, const NotesProposal& p);
// A pending proposal approved (applied) or rejected; the line for the human.
kk::Result<std::string> approve_notes_proposal(const std::filesystem::path& root, const std::string& id);
kk::Result<std::string> reject_notes_proposal(const std::filesystem::path& root, const std::string& id);
// What a proposal changes, note by note: new topics and notes with their text, texts as line diffs.
std::string notes_proposal_preview(const std::filesystem::path& root, const NotesProposal& p);
// A line diff: " kept", "-removed", "+added" lines (a big pair of texts: the old lines, then the new).
std::string text_diff(std::string_view before, std::string_view after);
// N-NNNN ids named in a text, in order, without repeats.
std::vector<std::string> proposals_in(const std::string& text);

// What the assistant sees first: the topics and notes inside `scope` ("" = all) with their titles, first
// words and links, at most `max_notes` notes (then how many more).
std::string notes_overview(const core::Schema& s, const std::string& scope, std::size_t max_notes = 200);

// The assistant's tools: notes_overview, read_note, search_notes, mentions, write_note, new_note,
// new_topic, finish (terminal). Reads see the assistant's own edits of this run.
class NotesToolHost final : public IToolHost {
 public:
  NotesToolHost(const core::Schema& s, std::string scope);
  [[nodiscard]] std::vector<Tool> tools() const override;
  ToolOutcome call(const ToolCall& c) override;
  [[nodiscard]] std::vector<std::string> reads() const override { return reads_; }
  [[nodiscard]] const std::vector<NoteEdit>& edits() const { return edits_; }
  [[nodiscard]] const std::string& answer() const { return answer_; }
  [[nodiscard]] const std::string& summary() const { return summary_; }

 private:
  [[nodiscard]] std::string text_for(const std::string& id) const;  // with this run's edits
  [[nodiscard]] std::string title_for(const std::string& id) const;
  [[nodiscard]] bool known_block(const std::string& id) const;
  [[nodiscard]] bool topic_block(const std::string& id) const;
  std::string fresh_note_id(const std::string& parent, const std::string& title);
  const core::Schema& s_;
  std::string scope_;
  std::vector<NoteEdit> edits_;
  std::map<std::string, std::string> texts_;    // texts read or proposed
  std::set<std::string> new_topics_, new_notes_;
  std::vector<std::string> reads_;
  std::string answer_, summary_;
};

}  // namespace kk::agents
