#include "kk/agents/notes_agent.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <nlohmann/json.hpp>
#include <regex>

#include "kk/code/notes.hpp"
#include "kk/core/model.hpp"
#include "kk/core/notes.hpp"
#include "kk/core/paths.hpp"
#include "kk/core/text.hpp"

namespace kk::agents {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr std::size_t kMaxText = 20000;  // characters of one note the assistant writes

std::string jstr(const json& j, const char* key) {
  auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

json edit_json(const NoteEdit& e) {
  json j = {{"kind", e.kind}, {"id", e.id}};
  if (!e.parent.empty()) j["parent"] = e.parent;
  if (!e.title.empty()) j["title"] = e.title;
  if (e.kind != "topic") j["text"] = e.text;
  if (e.kind == "write") j["base"] = e.base;
  return j;
}

NoteEdit edit_from(const json& j) {
  return NoteEdit{jstr(j, "kind"), jstr(j, "id"), jstr(j, "parent"), jstr(j, "title"), jstr(j, "text"), jstr(j, "base")};
}

std::string read_text(const fs::path& root, const std::string& rel) {
  if (rel.empty()) return {};
  return core::read_file(root / rel).value_or("");
}

std::string first_words(const std::string& text, std::size_t max_bytes) {
  std::string out;
  for (const auto& l : core::split(text, '\n')) {
    auto t = std::string(core::trim(l));
    if (t.empty()) continue;
    out += (out.empty() ? "" : " / ") + t;
    if (out.size() >= max_bytes) break;
  }
  if (out.size() > max_bytes) out = core::truncate_utf8(out, max_bytes) + "…";
  return out;
}

}  // namespace

// --- proposals ------------------------------------------------------------------------------

NotesProposalStore::NotesProposalStore(fs::path root) : root_(std::move(root)) {}

fs::path NotesProposalStore::dir() const { return root_ / ".karkas" / "proposals"; }

kk::Result<std::vector<NotesProposal>> NotesProposalStore::list() const {
  std::vector<NotesProposal> out;
  std::error_code ec;
  std::vector<std::string> ids;
  for (const auto& e : fs::directory_iterator(dir(), ec)) {
    auto name = e.path().filename().string();
    if (name.starts_with("N-") && e.path().extension() == ".json") ids.push_back(e.path().stem().string());
  }
  std::sort(ids.rbegin(), ids.rend());
  for (const auto& id : ids) {
    if (auto p = get(id)) out.push_back(std::move(*p));
  }
  return out;
}

kk::Result<NotesProposal> NotesProposalStore::get(const std::string& id) const {
  auto text = core::read_file(dir() / (id + ".json"));
  if (!text) return kk::fail("notes", "no proposal " + id);
  auto j = json::parse(*text, nullptr, false);
  if (!j.is_object()) return kk::fail("notes", "proposal " + id + " is not readable");
  NotesProposal p;
  p.id = id;
  p.status = jstr(j, "status");
  p.request = jstr(j, "request");
  p.scope = jstr(j, "scope");
  p.summary = jstr(j, "summary");
  p.ts = j.value("ts", std::int64_t{0});
  if (j.contains("edits") && j["edits"].is_array()) {
    for (const auto& e : j["edits"]) p.edits.push_back(edit_from(e));
  }
  return p;
}

kk::VoidResult NotesProposalStore::save(const NotesProposal& p) {
  json edits = json::array();
  for (const auto& e : p.edits) edits.push_back(edit_json(e));
  json j = {{"status", p.status}, {"request", p.request}, {"scope", p.scope}, {"summary", p.summary}, {"ts", p.ts}, {"edits", edits}};
  return core::write_file(dir() / (p.id + ".json"), j.dump(2, ' ', false, json::error_handler_t::replace) + "\n");
}

kk::Result<NotesProposal> NotesProposalStore::create(NotesProposal p) {
  int last = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir(), ec)) {
    auto stem = e.path().stem().string();
    if (stem.starts_with("N-")) last = std::max(last, std::atoi(stem.c_str() + 2));
  }
  p.id = fmt::format("N-{:04}", last + 1);
  if (p.ts == 0) p.ts = core::unix_now();
  if (auto r = save(p); !r) return std::unexpected(r.error());
  return p;
}

kk::Result<std::vector<std::string>> apply_notes_proposal(const fs::path& root, const NotesProposal& p) {
  auto s = core::load_schema(root);
  if (!s) return std::unexpected(s.error());
  // Check everything first: nothing is written when one edit cannot be applied.
  std::set<std::string> made;
  std::vector<std::string> problems;
  auto topic_known = [&](const std::string& id) {
    if (id.empty() || made.contains(id)) return true;
    const auto* b = s->find(id);
    return b != nullptr && b->kind == core::BlockKind::Group;
  };
  for (const auto& e : p.edits) {
    if (e.kind == "topic" || e.kind == "note") {
      if (s->find(e.id) != nullptr) problems.push_back(e.id + " exists already");
      if (!topic_known(e.parent)) problems.push_back("no topic " + e.parent + " for " + e.id);
      if (core::trim(e.title).empty()) problems.push_back(e.id + " has no title");
      made.insert(e.id);
    } else if (e.kind == "write") {
      if (made.contains(e.id)) continue;
      const auto* b = s->find(e.id);
      if (b == nullptr || b->kind == core::BlockKind::Group) {
        problems.push_back("no note " + e.id);
      } else if (read_text(root, core::note_file(*b)) != e.base) {
        problems.push_back(fmt::format("{} changed since the proposal", e.id));
      }
    } else {
      problems.push_back("unknown edit " + e.kind);
    }
  }
  if (!problems.empty()) return kk::fail("notes", "the proposal cannot be applied: " + core::join(problems, "; "));
  std::vector<std::string> touched;
  auto card = [&](const NoteEdit& e) -> kk::VoidResult {
    core::Block b;
    b.id = e.id;
    b.purpose.value = std::string(core::trim(e.title));
    b.purpose.set = true;
    bool topic = e.kind == "topic";
    b.kind_text.value = topic ? "group" : "leaf";
    b.kind_text.set = true;
    b.kind = topic ? core::BlockKind::Group : core::BlockKind::Leaf;
    auto dir = s->blocks_root() / core::id_to_rel_path(e.id);
    if (auto r = core::write_file(dir / "block.ksl", core::write_block(b)); !r) return r;
    if (!topic) return core::write_file(dir / std::string(core::kNoteFileName), e.text);
    return {};
  };
  for (const auto* kind : {"topic", "note"}) {
    for (const auto& e : p.edits) {
      if (e.kind != kind) continue;
      if (auto r = card(e); !r) return std::unexpected(r.error());
      touched.push_back(e.id);
    }
  }
  for (const auto& e : p.edits) {
    if (e.kind != "write" || made.contains(e.id)) continue;
    const auto* b = s->find(e.id);
    if (auto r = core::write_file(root / core::note_file(*b), e.text); !r) return std::unexpected(r.error());
    touched.push_back(e.id);
  }
  return touched;
}

kk::Result<std::string> approve_notes_proposal(const fs::path& root, const std::string& id) {
  NotesProposalStore store(root);
  auto p = store.get(id);
  if (!p) return std::unexpected(p.error());
  if (p->status != "pending") return kk::fail("notes", fmt::format("{} is {} already", id, p->status));
  auto r = apply_notes_proposal(root, *p);
  if (!r) return std::unexpected(r.error());
  p->status = "approved";
  if (auto w = store.save(*p); !w) return std::unexpected(w.error());
  return fmt::format("approved {}: {}\n", id, core::join(*r, ", "));
}

kk::Result<std::string> reject_notes_proposal(const fs::path& root, const std::string& id) {
  NotesProposalStore store(root);
  auto p = store.get(id);
  if (!p) return std::unexpected(p.error());
  if (p->status != "pending") return kk::fail("notes", fmt::format("{} is {} already", id, p->status));
  p->status = "rejected";
  if (auto w = store.save(*p); !w) return std::unexpected(w.error());
  return fmt::format("rejected {}\n", id);
}

std::string text_diff(std::string_view before, std::string_view after) {
  auto a = core::split(before, '\n', false);
  auto b = core::split(after, '\n', false);
  if (!a.empty() && a.back().empty()) a.pop_back();
  if (!b.empty() && b.back().empty()) b.pop_back();
  std::string out;
  const std::size_t n = a.size();
  const std::size_t m = b.size();
  if (n * m > 4'000'000) {  // too big for the table: the old text, then the new
    for (const auto& l : a) out += "-" + l + "\n";
    for (const auto& l : b) out += "+" + l + "\n";
    return out;
  }
  std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m; j-- > 0;) lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
  }
  std::size_t i = 0;
  std::size_t j = 0;
  while (i < n || j < m) {
    if (i < n && j < m && a[i] == b[j]) {
      out += " " + a[i] + "\n";
      ++i;
      ++j;
    } else if (i < n && (j == m || lcs[i + 1][j] >= lcs[i][j + 1])) {
      out += "-" + a[i++] + "\n";  // what goes, before what comes in its place
    } else {
      out += "+" + b[j++] + "\n";
    }
  }
  return out;
}

std::string notes_proposal_preview(const fs::path& root, const NotesProposal& p) {
  auto s = core::load_schema(root);
  std::string out;
  for (const auto& e : p.edits) {
    if (e.kind == "topic") {
      out += fmt::format("+++ new topic {} \"{}\" in {}\n", e.id, e.title, e.parent.empty() ? "the top level" : e.parent);
    } else if (e.kind == "note") {
      out += fmt::format("+++ new note {} \"{}\" in {}\n", e.id, e.title, e.parent.empty() ? "the top level" : e.parent);
      for (const auto& l : core::split(e.text, '\n', false)) out += "+" + l + "\n";
    } else {
      const core::Block* b = s ? s->find(e.id) : nullptr;
      std::string title = b != nullptr ? b->purpose.value : e.id;
      std::string now = b != nullptr ? read_text(root, core::note_file(*b)) : std::string();
      out += fmt::format("~~~ {} \"{}\"{}\n", e.id, title, p.status == "pending" && now != e.base ? " (changed since the proposal)" : "");
      out += text_diff(e.base, e.text);
    }
  }
  return out;
}

std::vector<std::string> proposals_in(const std::string& text) {
  static const std::regex re(R"((?:^|[^A-Za-z0-9_-])(N-[0-9]{4,})\b)");
  std::vector<std::string> out;
  for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
    auto id = (*it)[1].str();
    if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
  }
  return out;
}

std::string notes_overview(const core::Schema& s, const std::string& scope, std::size_t max_notes) {
  std::string out = scope.empty() ? "@notes " + s.project.name + "\n" : "@notes " + scope + "\n";
  std::size_t shown = 0;
  std::size_t more = 0;
  for (const auto& b : s.blocks) {
    if (!scope.empty() && b.id != scope && !core::Schema::is_under(scope, b.id)) continue;
    std::string indent(static_cast<std::size_t>(std::max(0, b.depth - 1)) * 2, ' ');
    if (b.kind == core::BlockKind::Group) {
      out += fmt::format("{}topic {} \"{}\" ({} inside)\n", indent, b.id, b.purpose.value, b.children.size());
      continue;
    }
    if (shown >= max_notes) {
      ++more;
      continue;
    }
    ++shown;
    std::vector<std::string> links;
    for (const auto& r : b.refs) {
      if (!r.target.empty() && std::find(links.begin(), links.end(), r.target) == links.end()) links.push_back(r.target);
    }
    auto text = read_text(s.root, core::note_file(b));
    out += fmt::format("{}note {} \"{}\": {}{}\n", indent, b.id, b.purpose.value, text.empty() ? "(empty)" : first_words(text, 120),
                       links.empty() ? "" : " -> " + core::join(links, ", "));
  }
  if (more > 0) out += fmt::format("... {} more notes (search_notes finds them)\n", more);
  if (shown == 0 && more == 0) out += "(no notes yet)\n";
  return out;
}

// --- the assistant's tools ------------------------------------------------------------------

NotesToolHost::NotesToolHost(const core::Schema& s, std::string scope) : s_(s), scope_(std::move(scope)) {}

std::vector<Tool> NotesToolHost::tools() const {
  auto obj = [](json props, std::vector<std::string> required = {}) {
    json j = {{"type", "object"}, {"properties", std::move(props)}};
    if (!required.empty()) j["required"] = required;
    return j.dump();
  };
  auto str = [](const char* d) { return json{{"type", "string"}, {"description", d}}; };
  return {
      {"notes_overview", "The topics and notes of a topic (or of all notes): ids, titles, first words, links.",
       obj({{"topic", str("topic id; empty for all notes")}})},
      {"read_note", "The whole text of a note, its links and the notes that link to it.", obj({{"note", str("note id")}}, {"note"})},
      {"search_notes", "Notes whose title or text holds all these words (any case), with the lines found.",
       obj({{"text", str("words")}}, {"text"})},
      {"mentions", "Plain words in notes that name another note's title without a [[link]]: links worth adding.",
       obj({{"note", str("note id; empty for the notes of the request")}})},
      {"write_note", "Propose the whole new text of an existing note (keep what stays; links are [[Title]]).",
       obj({{"note", str("note id")}, {"text", str("the whole new text")}}, {"note", "text"})},
      {"new_note", "Propose a new note in a topic; returns its id.",
       obj({{"topic", str("topic id; empty for the top level")}, {"title", str("a short title")}, {"text", str("its text")}}, {"title", "text"})},
      {"new_topic", "Propose a new topic (notes go into it); returns its id.",
       obj({{"parent", str("topic id; empty for the top level")}, {"title", str("a short title")}}, {"title"})},
      {"finish", "End: what you tell the human, and one line about the edits you propose (empty when none).",
       obj({{"answer", json{{"type", "string"}, {"maxLength", 3000}, {"description", "for the human"}}},
            {"summary", json{{"type", "string"}, {"maxLength", 200}, {"description", "one line about your edits"}}}},
           {"answer"})},
  };
}

bool NotesToolHost::exists(const std::string& id) const { return s_.find(id) != nullptr || new_topics_.contains(id) || new_notes_.contains(id); }

bool NotesToolHost::is_topic(const std::string& id) const {
  if (new_topics_.contains(id)) return true;
  const auto* b = s_.find(id);
  return b != nullptr && b->kind == core::BlockKind::Group;
}

std::string NotesToolHost::title_of(const std::string& id) const {
  if (const auto* b = s_.find(id)) return b->purpose.value;
  for (const auto& e : edits_) {
    if (e.id == id && !e.title.empty()) return e.title;
  }
  return id;
}

std::string NotesToolHost::text_of(const std::string& id) const {
  if (auto it = texts_.find(id); it != texts_.end()) return it->second;
  const auto* b = s_.find(id);
  return b != nullptr ? read_text(s_.root, core::note_file(*b)) : std::string();
}

std::string NotesToolHost::new_id(const std::string& parent, const std::string& title) {
  auto seg = core::ident_from_title(title);
  auto base = parent.empty() ? seg : parent + "." + seg;
  auto id = base;
  std::error_code ec;
  for (int n = 2; exists(id) || fs::exists(s_.blocks_root() / core::id_to_rel_path(id), ec); ++n) id = base + "_" + std::to_string(n);
  return id;
}

ToolOutcome NotesToolHost::call(const ToolCall& c) {
  auto j = json::parse(c.input_json, nullptr, false);
  if (!j.is_object()) return {"input is not a JSON object", true, false};
  auto err = [](std::string m) { return ToolOutcome{std::move(m), true, false}; };
  if (c.name == "notes_overview") {
    auto topic = jstr(j, "topic");
    if (!topic.empty() && !is_topic(topic)) return err("no topic " + topic + " (notes_overview with no topic lists all)");
    auto out = notes_overview(s_, topic);
    for (const auto& e : edits_) {
      if (e.kind != "write") out += fmt::format("(proposed in this run) {} {} \"{}\"\n", e.kind, e.id, e.title);
    }
    return {out, false, false};
  }
  if (c.name == "read_note") {
    auto id = jstr(j, "note");
    if (!exists(id)) return err("no note " + id + " (ids are in notes_overview)");
    if (is_topic(id)) return err(id + " is a topic: notes_overview with topic=" + id + " lists its notes");
    reads_.push_back(id);
    std::vector<std::string> out_links, in_links;
    for (const auto& l : s_.links()) {
      if (l.kind != core::LinkKind::Ref) continue;
      if (l.src_block == id) out_links.push_back(l.dst_block);
      if (l.dst_block == id) in_links.push_back(l.src_block);
    }
    auto text = text_of(id);
    std::string r = fmt::format("@note {} \"{}\"\n", id, title_of(id));
    if (!out_links.empty()) r += "links: " + core::join(out_links, ", ") + "\n";
    if (!in_links.empty()) r += "linked from: " + core::join(in_links, ", ") + "\n";
    return {r + "--- text\n" + (text.empty() ? "(empty)\n" : text), false, false};
  }
  if (c.name == "search_notes") {
    auto words = core::split(core::fold_title(jstr(j, "text")), ' ');
    if (words.empty()) return err("search_notes needs words");
    std::string out;
    int hits = 0;
    for (const auto& b : s_.blocks) {
      if (b.kind == core::BlockKind::Group) continue;
      auto text = text_of(b.id);
      auto all = core::fold_title(b.purpose.value + " " + text);
      bool every = std::all_of(words.begin(), words.end(), [&](const std::string& w) { return core::contains(all, w); });
      if (!every) continue;
      std::string line;
      for (const auto& l : core::split(text, '\n')) {
        auto f = core::fold_title(l);
        if (std::any_of(words.begin(), words.end(), [&](const std::string& w) { return core::contains(f, w); })) {
          line = std::string(core::trim(l));
          break;
        }
      }
      out += fmt::format("{} \"{}\": {}\n", b.id, b.purpose.value, line.empty() ? first_words(text, 100) : core::truncate_utf8(line, 200));
      if (++hits >= 30) {
        out += "... more (use more words)\n";
        break;
      }
    }
    return {out.empty() ? "nothing found\n" : out, false, false};
  }
  if (c.name == "mentions") {
    auto id = jstr(j, "note");
    std::vector<code::Mention> found;
    if (!id.empty()) {
      if (!exists(id)) return err("no note " + id);
      found = code::unlinked_mentions(s_, id);
      auto of = code::mentions_of(s_, id);
      found.insert(found.end(), of.begin(), of.end());
    } else {
      for (const auto& b : s_.blocks) {
        if (b.kind == core::BlockKind::Group) continue;
        if (!scope_.empty() && b.id != scope_ && !core::Schema::is_under(scope_, b.id)) continue;
        auto m = code::unlinked_mentions(s_, b.id);
        found.insert(found.end(), m.begin(), m.end());
        if (found.size() >= 40) break;
      }
    }
    std::string out;
    for (const auto& m : found) out += fmt::format("{} names \"{}\" ({}) at line {}: {}\n", m.src, title_of(m.dst), m.dst, m.line, m.text);
    return {out.empty() ? "no plain mentions of titles\n" : out, false, false};
  }
  if (c.name == "write_note") {
    auto id = jstr(j, "note");
    auto text = jstr(j, "text");
    if (!exists(id) || is_topic(id)) return err("no note " + id + " (new_note makes one)");
    if (text.size() > kMaxText) return err(fmt::format("the text is {} characters: at most {}", text.size(), kMaxText));
    if (!text.empty() && !text.ends_with('\n')) text += '\n';
    // A note new in this run: its own edit gets the text.
    for (auto& e : edits_) {
      if ((e.kind == "note" || e.kind == "write") && e.id == id) {
        e.text = text;
        texts_[id] = text;
        return {"proposed: the new text of " + id, false, false};
      }
    }
    edits_.push_back(NoteEdit{"write", id, "", "", text, text_of(id)});
    texts_[id] = text;
    return {"proposed: the new text of " + id + " (the human approves it)", false, false};
  }
  if (c.name == "new_note" || c.name == "new_topic") {
    bool topic = c.name == "new_topic";
    auto parent = jstr(j, topic ? "parent" : "topic");
    auto title = std::string(core::trim(jstr(j, "title")));
    if (title.empty()) return err("a title is needed");
    if (title.find_first_of("[]|\n") != std::string::npos) return err("a title cannot hold [ ] | or a line break");
    if (!parent.empty() && !is_topic(parent)) return err("no topic " + parent);
    auto text = jstr(j, "text");
    if (text.size() > kMaxText) return err(fmt::format("the text is {} characters: at most {}", text.size(), kMaxText));
    if (!topic && !text.empty() && !text.ends_with('\n')) text += '\n';
    auto id = new_id(parent, title);
    edits_.push_back(NoteEdit{topic ? "topic" : "note", id, parent, title, topic ? "" : text, ""});
    (topic ? new_topics_ : new_notes_).insert(id);
    if (!topic) texts_[id] = text;
    return {fmt::format("proposed: new {} {} \"{}\"", topic ? "topic" : "note", id, title), false, false};
  }
  if (c.name == "finish") {
    answer_ = jstr(j, "answer");
    summary_ = jstr(j, "summary");
    return {"done", false, true};
  }
  return err("unknown tool " + c.name);
}

}  // namespace kk::agents
