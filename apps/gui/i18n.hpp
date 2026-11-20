#pragma once
// Interface languages of karkas-gui. Keys are the English texts; tr() returns the text
// for the current language (falls back to English). Schema data (ids, KSL, purposes)
// stays as written.
#include <string>
#include <string_view>

namespace kk::gui {

enum class Lang { En, Ru };

void set_lang(Lang l);
Lang lang();
const char* lang_code(Lang l);          // "en" | "ru"
Lang lang_from_code(std::string_view c);  // unknown → En
const char* lang_name(Lang l);          // "English" | "Русский"
Lang lang_from_env();                    // LANG / LC_ALL / LC_MESSAGES

const char* tr(const char* en);
// Window or popup title with a language-independent ImGui id: "Блоки###Blocks".
std::string title(const char* en);
// Display names of block kinds and statuses (data values stay English).
const char* tr_kind(const std::string& kind);
// Notes projects (ADR 0038): a group is a topic, a leaf a note in tr_kind.
void set_notes_kinds(bool on);
bool notes_kinds();
const char* tr_status(const std::string& status);
const char* tr_ticket_status(const std::string& status);

}  // namespace kk::gui
