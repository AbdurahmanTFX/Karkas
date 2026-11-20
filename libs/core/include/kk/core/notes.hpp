#pragma once
// Notes projects (docs/adr/0038): the same schema of blocks, but a topic (group) holds notes and a
// note (leaf) is a text file — <block folder>/note.txt, or the file its card lists (notes that were
// in the folder before Karkas). Notes link to each other by [[name]] in their text; the schema
// loader reads those links, the schema draws them as "ref" lines. Nothing is generated or built.
#include <functional>
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
// else <block folder>/note.txt.
std::string note_file(const Block& b);
inline constexpr std::string_view kNoteFileName = "note.txt";

// The [[links]] of a text, in order (names trimmed; empty ones and ones spanning lines skipped).
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

}  // namespace kk::core
