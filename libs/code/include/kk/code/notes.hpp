#pragma once
// Notes projects (docs/adr/0038): finding text notes in a folder, cards for the notes that were there
// before Karkas (the files stay in place; folders become topics).
#include <filesystem>
#include <string>
#include <vector>

#include "kk/core/error.hpp"
#include "kk/core/schema.hpp"

namespace kk::code {

// True when the folder (outside hidden folders, build*/, blocks/) holds text notes (.txt).
bool has_notes(const std::filesystem::path& dir);

struct NotesImport {
  std::vector<std::string> topics;  // block ids of the folders
  std::vector<std::string> notes;   // block ids of the .txt files
};
// Cards for the .txt files of the project folder that no card lists yet: a folder with notes is a topic
// (its name the title), a file is a note (its name without .txt the title) listing that file. Ids come
// from the titles (transliterated), unique among their siblings. Nothing but blocks/**/block.ksl is written.
kk::Result<NotesImport> import_notes(const std::filesystem::path& root);

// Edits of notes from the schema (karkas-gui): plain file edits, no KSL diff.
// A [[link]] to `dst` on a new last line of note `src` (its file is created when missing).
kk::VoidResult add_note_link(const core::Schema& s, const std::string& src, const std::string& dst);
// Every [[link]] of note `src` that names `dst` becomes plain text: "[[Name]]" → "Name",
// "[[Name|shown]]" → "shown". Returns how many.
kk::Result<int> remove_note_link(const core::Schema& s, const std::string& src, const std::string& dst);
// A new title (purpose) of note or topic `id`; the [[links]] of other notes that named it by its old title
// name it by the new one (a shown text after '|' is kept). Returns the notes whose text changed.
kk::Result<std::vector<std::string>> retitle_note(const core::Schema& s, const std::string& id, const std::string& title);

}  // namespace kk::code
