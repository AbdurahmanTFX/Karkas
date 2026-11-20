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

}  // namespace kk::code
