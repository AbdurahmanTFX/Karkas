#include "i18n.hpp"

#include <atomic>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace kk::gui {

namespace {

std::atomic<int> g_lang{static_cast<int>(Lang::En)};

const std::unordered_map<std::string_view, const char*>& ru() {
  static const std::unordered_map<std::string_view, const char*> m = {
      // windows and menus
      {"Blocks", "Блоки"},
      {"Schema", "Схема"},
      {"Inspector", "Инспектор"},
      {"Code", "Код"},
      {"Details", "Подробности"},
      {"Settings", "Настройки"},
      {"Settings...", "Настройки..."},
      {"Quit", "Выход"},
      {"View", "Вид"},
      {"indexing...", "индексация..."},
      // toolbar
      {"All levels", "Все уровни"},
      {"use links", "связи use"},
      {"zoom", "масштаб"},
      {"Undo", "Отменить"},
      {"Add block", "Добавить блок"},
      {"index {:.0f} ms", "индекс {:.0f} мс"},
      {"the project", "проект"},
      // node and background menus
      {"Open", "Открыть"},
      {"Add port...", "Добавить порт..."},
      {"Rename...", "Переименовать..."},
      {"Split...", "Разделить..."},
      {"Merge into...", "Слить с..."},
      {"Remove", "Удалить"},
      {"Open card in editor", "Открыть карточку в редакторе"},
      {"Add block...", "Добавить блок..."},
      {"Fit to view", "Вписать в окно"},
      {"Reset size", "Сбросить размер"},
      {"Auto layout (reset positions and sizes)", "Авто-раскладка (сбросить позиции и размеры)"},
      {"links", "связи"},
      // inspector and code
      {"Select a block in the tree or on the schema.", "Выберите блок в дереве или на схеме."},
      {"Enter", "Войти"},
      {"Card (KSL)", "Карточка (KSL)"},
      {"Ports", "Порты"},
      {"Invariants", "Гарантии"},
      {"History", "История"},
      {"Files", "Файлы"},
      {"Model view (what agents get)", "Вид для модели (что получают агенты)"},
      {"thread", "поток"},
      {"Pick a file in the inspector (double click opens the external editor).",
       "Выберите файл в инспекторе (двойной щелчок открывает внешний редактор)."},
      {"Open in editor", "Открыть в редакторе"},
      // bottom tabs
      {"Tickets", "Тикеты"},
      {"Agents", "Агенты"},
      {"Build & tests", "Сборка и тесты"},
      {"Costs", "Затраты"},
      {"Drift", "Дрейф"},
      {"Changes", "Изменения"},
      {"(select a ticket)", "(выберите тикет)"},
      {"Working now: %zu", "Работают сейчас: %zu"},
      {"role", "роль"},
      {"block", "блок"},
      {"tier", "уровень"},
      {"outcome", "исход"},
      {"tokens in/out", "токены вх/вых"},
      {"cache read", "чтение кэша"},
      {"usd", "$"},
      {"contract", "контракт"},
      {"unit", "модульные"},
      {"integration", "интеграционные"},
      {"failures", "ошибки"},
      {"(stale)", "(устарело)"},
      {"build failed", "сборка не прошла"},
      {"total $%.4f · %zu runs · cache read share %.1f%%", "итого $%.4f · запусков %zu · доля чтения из кэша %.1f%%"},
      {"by block", "по блокам"},
      {"by model", "по моделям"},
      {"by ticket", "по тикетам"},
      {"no drift", "дрейфа нет"},
      {"no proposed changes", "предложенных изменений нет"},
      {"Approve", "Утвердить"},
      {"Reject", "Отклонить"},
      // dialogs
      {"Edit", "Правка"},
      {"KSL diff", "Изменения KSL"},
      {"New block in %s", "Новый блок в %s"},
      {"name", "имя"},
      {"kind", "вид"},
      {"purpose", "назначение"},
      {"Preview", "Предпросмотр"},
      {"Cancel", "Отмена"},
      {"New port of %s", "Новый порт блока %s"},
      {"dir", "направление"},
      {"signature: name(params) -> R", "сигнатура: имя(параметры) -> R"},
      {"type", "тип"},
      {"new name", "новое имя"},
      {"merge into (block id)", "слить с (id блока)"},
      {"new block name", "имя нового блока"},
      {"ports to move (comma separated)", "переносимые порты (через запятую)"},
      {"Rename %s", "Переименовать %s"},
      {"Merge %s into", "Слить %s с"},
      {"Split %s", "Разделить %s"},
      {"Apply", "Применить"},
      {"Close", "Закрыть"},
      {"{} → {}: {} {} links", "{} → {}: связей {}: {}"},
      // empty schema
      {"No blocks on this level.", "На этом уровне нет блоков."},
      {"Right click → Add block, or the \"Add block\" button above.",
       "Правый щелчок → «Добавить блок…» или кнопка «Добавить блок» сверху."},
      {"The schema has no blocks yet.", "В схеме пока нет ни одного блока."},
      {"karkas init only creates the project frame (project.ksl, karkas.toml, .karkas/). "
       "Blocks appear when the architect designs the project (karkas design) or when you add "
       "them here (\"Add block\").",
       "karkas init создаёт только каркас проекта (project.ksl, karkas.toml, .karkas/). Блоки "
       "появляются, когда архитектор проектирует проект (karkas design) или когда вы добавляете их "
       "здесь («Добавить блок»)."},
      {"%zu code files of this project are outside blocks (D06):", "Файлов кода вне блоков (D06): %zu"},
      {"... and %zu more", "... и ещё %zu"},
      {"Cutting existing code into blocks automatically (import) is not implemented yet "
       "(stage 11 of the spec).",
       "Автоматическая нарезка существующего кода на блоки (импорт) пока не реализована (этап 11 "
       "спецификации)."},
      // settings
      {"Language", "Язык"},
      {"Default block size", "Размер блоков по умолчанию"},
      {"Interface text size", "Размер текста интерфейса"},
      {"Reset block sizes in this view", "Сбросить размеры блоков на этом уровне"},
      {"Layout engine: %s", "Движок раскладки: %s"},
      {"Drag the corner of a block to resize it; the text follows the size.",
       "Тяните за угол блока, чтобы изменить размер; текст подстраивается под размер."},
      {"Settings are saved to %s", "Настройки сохраняются в %s"},
      // worker messages
      {"applied: ", "применено: "},
      {"undone: ", "отменено: "},
      {"undo failed: ", "отмена не удалась: "},
      {"rejected", "отклонено"},
      {"editor: ", "редактор: "},
      {"cannot start the editor: ", "не удалось запустить редактор: "},
  };
  return m;
}

}  // namespace

void set_lang(Lang l) { g_lang = static_cast<int>(l); }
Lang lang() { return static_cast<Lang>(g_lang.load()); }
const char* lang_code(Lang l) { return l == Lang::Ru ? "ru" : "en"; }
Lang lang_from_code(std::string_view c) { return c.starts_with("ru") ? Lang::Ru : Lang::En; }
const char* lang_name(Lang l) { return l == Lang::Ru ? "Русский" : "English"; }

Lang lang_from_env() {
  for (const char* v : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
    const char* s = std::getenv(v);
    if (s != nullptr && *s != '\0') return lang_from_code(s);
  }
  return Lang::En;
}

const char* tr(const char* en) {
  if (lang() == Lang::En) return en;
  auto it = ru().find(en);
  return it == ru().end() ? en : it->second;
}

std::string title(const char* en) { return std::string(tr(en)) + "###" + en; }

const char* tr_kind(const std::string& k) {
  if (lang() == Lang::En) return k.c_str();
  if (k == "group") return "группа";
  if (k == "leaf") return "лист";
  if (k == "entry") return "вход";
  if (k == "shared") return "общий";
  return k.c_str();
}

const char* tr_status(const std::string& s) {
  if (lang() == Lang::En) return s.c_str();
  if (s == "design") return "проектирование";
  if (s == "api_frozen") return "api заморожен";
  if (s == "tests_ready") return "тесты готовы";
  if (s == "implemented") return "реализован";
  if (s == "passing") return "проходит";
  if (s == "failing") return "падает";
  if (s == "drift") return "дрейф";
  if (s == "needs_review") return "на проверку";
  return s.c_str();
}

}  // namespace kk::gui
