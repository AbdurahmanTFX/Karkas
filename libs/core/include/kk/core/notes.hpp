#pragma once
// Notes projects (docs/adr/0038): the same schema of blocks, but a topic (group) holds notes and a
// note (leaf) is a text file — <block folder>/note.md (note.txt when it was written before notes were
// Markdown, docs/adr/0040), or the file its card lists (notes that were in the folder before Karkas).
// Notes link to each other by [[name]] in their text, Markdown notes also by [text](file.md); the schema
// loader reads those links, the schema draws them as "ref" lines. Nothing is generated or built.
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kk/core/model.hpp"

namespace kk::core {

struct Schema;

// project.ksl says `lang notes`.
bool is_notes_project(const Project& p);

// The text file of a note, relative to the project root ("" for a topic): the first listed file,
// else <block folder>/note.md, or note.txt where the loader found only that (Block::note_txt).
std::string note_file(const Block& b);
inline constexpr std::string_view kNoteFileName = "note.md";
inline constexpr std::string_view kOldNoteFileName = "note.txt";

// The note file a Markdown link of note file `from` names, from the project root: a destination ending
// in .md, .markdown or .txt, relative to the note's folder ("/..." from the project root), "%20" decoded,
// "#part" dropped. nullopt for URLs, anchors and other files.
std::optional<std::string> note_link_file(std::string_view from, std::string_view href);

// The file an image of a note shows (docs/adr/0041), from the project root, when it is there:
// ![alt](src) relative to the note's folder ("/…" from the project root, "%20" decoded); ![[name]] (an
// Obsidian embed) in the note's folder, at the project root, else the file of that name anywhere in the
// project (hidden folders and build trees skipped) nearest to the note. URLs give nullopt (never fetched).
class MediaFinder {
 public:
  explicit MediaFinder(std::filesystem::path root);
  std::optional<std::string> find(std::string_view note_file, std::string_view src, bool wiki);

 private:
  std::filesystem::path root_;
  std::optional<std::multimap<std::string, std::string>> by_name_;  // file name → files, made at the first need
};

// The [[links]] of a text, in order (names trimmed; empty ones, ones spanning lines and embeds of image
// files, ![[file.png]], skipped).
std::vector<NoteRef> parse_note_refs(std::string_view text);

// The text with each [[link]] replaced by what `f` returns for it, the brackets included (nullopt keeps it).
// `f` gets the name and the whole text between the brackets ("name|shown"); the same links as
// parse_note_refs.
using RefRewrite = std::function<std::optional<std::string>(const std::string& name, const std::string& inner)>;
std::string rewrite_note_refs(std::string_view text, const RefRewrite& f);

// The blocks a [[name]] fits: a block id ("ideas.game", or with '/'), else the notes and topics whose
// title (purpose) or last id segment equals the name, ignoring case and repeated spaces.
std::vector<std::string> resolve_note(const Schema& s, std::string_view name);

// The [[name]] that names block `id` and only it: its title when that is unique, else its id.
std::string note_link_name(const Schema& s, const std::string& id);

// A block id segment from a title: Cyrillic transliterated, lowercase, words joined by '_', starting
// with a letter ("Идеи для игры" → "idei_dlya_igry"); "note" when nothing is left.
std::string ident_from_title(std::string_view title);

// Case-folded, space-collapsed form of a title (ASCII and Cyrillic letters).
std::string fold_title(std::string_view s);
// The text in lowercase (ASCII and Cyrillic letters), byte for byte as long as the text: offsets stay.
std::string lower_text(std::string_view s);

// Where a text names `title` as plain words (docs/adr/0038, agents for notes): whole-word occurrences,
// ignoring case, outside [[links]] and [text](links); byte offsets into the text. A title shorter than 3
// letters finds nothing.
std::vector<std::size_t> find_mentions(std::string_view text, std::string_view title);

}  // namespace kk::core
