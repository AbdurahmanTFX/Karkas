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
      {"used", "исп."},
      {"calls", "вызовы"},
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
      {"Highlighted: %zu", "Подсвечено: %zu"},
      {"Clear", "Сбросить"},
      {"Link colors", "Цвет связей"},
      {"by block", "по блоку"},
      {"by link kind", "по виду связи"},
      {"Hover a block to see only its links. Shift+click keeps a block's links lit (several "
       "blocks can be lit); Shift+double-click opens a group in place.",
       "Наведите на блок — останутся видны только его связи. Shift+щелчок закрепляет подсветку "
       "связей блока (можно несколько блоков); Shift+двойной щелчок раскрывает группу на месте."},
      {"Click a part of the path to go there, or the empty space to type a path (Enter to go).",
       "Щёлкните часть пути, чтобы перейти туда, или пустое место, чтобы ввести путь (Enter — перейти)."},
      {"no block {}", "нет блока {}"},
      {"groups opened in place: positions are not saved", "группы раскрыты на месте: позиции не сохраняются"},
      // projects and folders
      {"Open project", "Открыть проект"},
      {"Open project...", "Открыть проект..."},
      {"Recent projects", "Недавние проекты"},
      {"Show project folder", "Показать папку проекта"},
      {"Open folder", "Открыть папку"},
      {"folder: ", "папка: "},
      {"cannot open the folder: ", "не удалось открыть папку: "},
      {"No Karkas project is open. Choose a folder with project.ksl (for an existing code base: "
       "karkas init there, then karkas import).",
       "Проект Karkas не открыт. Выберите папку с project.ksl (для существующего кода: karkas init "
       "в ней, затем karkas import)."},
      {"Parent folder", "Папка выше"},
      {"Home", "Домой"},
      {"Show hidden folders", "Показывать скрытые папки"},
      {"(Karkas project)", "(проект Karkas)"},
      {"(no folders)", "(нет папок)"},
      {"Pick a green folder (it holds project.ksl); double click enters a folder.",
       "Выберите зелёную папку (в ней есть project.ksl); двойной щелчок — войти в папку."},
      {"Project: %s", "Проект: %s"},
      {"not a folder: ", "не папка: "},
      // IDE shell
      {"EXPLORER", "ПРОВОДНИК"},
      {"SEARCH", "ПОИСК"},
      {"SCHEMA CHANGES", "ИЗМЕНЕНИЯ СХЕМЫ"},
      {"SETTINGS", "НАСТРОЙКИ"},
      {"INSPECTOR", "ИНСПЕКТОР"},
      {"Explorer", "Проводник"},
      {"Search", "Поиск"},
      {"Schema changes", "Изменения схемы"},
      {"Go", "Переход"},
      {"Up one level", "На уровень выше"},
      {"Project root", "Корень проекта"},
      {"Add a block to this level", "Добавить блок на этот уровень"},
      {"Agents and karkas import propose schema changes here; nothing is written before you approve.",
       "Агенты и karkas import предлагают здесь изменения схемы; ничего не записывается, пока вы не утвердите."},
      {"agents working: {}", "работают агенты: {}"},
      {"APPEARANCE", "ВНЕШНИЙ ВИД"},
      {"SCHEMA", "СХЕМА"},
      {"HELP", "СПРАВКА"},
      {"pending", "ожидает"},
      {"approved", "утверждено"},
      {"Block colors", "Цвет блоков"},
      {"by status", "по статусу"},
      {"BLOCKS", "БЛОКИ"},
      {"PORTS", "ПОРТЫ"},
      {"FILES", "ФАЙЛЫ"},
      {"HISTORY", "ИСТОРИЯ"},
      {"INVARIANTS", "ГАРАНТИИ"},
      {"CARD (KSL)", "КАРТОЧКА (KSL)"},
      {"MODEL VIEW (WHAT AGENTS GET)", "ВИД ДЛЯ МОДЕЛИ (ЧТО ПОЛУЧАЮТ АГЕНТЫ)"},
      {"Card", "Карточка"},
      {"Folder", "Папка"},
      {"Click: open here; double click: external editor", "Щелчок — открыть здесь; двойной — во внешнем редакторе"},
      {"Drift: errors and warnings", "Дрейф: ошибки и предупреждения"},
      {"Filter blocks", "Фильтр блоков"},
      {"Highlighted: {}", "Подсвечено: {}"},
      {"Highlighted blocks (click to clear)", "Подсвеченные блоки (щёлкните, чтобы сбросить)"},
      {"Interface language (click to switch)", "Язык интерфейса (щёлкните, чтобы переключить)"},
      {"Layout engine and the time of the last index update", "Движок раскладки и время последнего обновления индекса"},
      {"Mouse wheel zooms the schema, the right button drags it.", "Колесико мыши масштабирует схему, правая кнопка двигает её."},
      {"no agent is working now", "сейчас агенты не работают"},
      {"no test results yet: karkas test", "результатов тестов пока нет: karkas test"},
      {"no tickets yet: karkas task / karkas debug create them", "тикетов пока нет: их создают karkas task / karkas debug"},
      {"opened in place", "раскрыто на месте"},
      {"Open every group in place (wheel zooms)", "Раскрыть все группы на месте (колесико — масштаб)"},
      {"Open in the external editor", "Открыть во внешнем редакторе"},
      {"Open this group", "Открыть эту группу"},
      {"Search blocks, purposes, ports", "Поиск блоков, назначений, портов"},
      {"Show links to shared blocks", "Показать связи с общими блоками"},
      {"Show on the schema", "Показать на схеме"},
      {"Type to search; a click opens the block on the schema.", "Начните вводить; щелчок открывает блок на схеме."},
      {"Undo the last edit", "Отменить последнюю правку"},
      {"id", "id"},
      {"title", "заголовок"},
      {"status", "статус"},
      {"PROJECT", "ПРОЕКТ"},
      {"STATUSES", "СТАТУСЫ"},
      {"Drop it on a function of the other block to add a call", "Отпустите на функции другого блока, чтобы добавить вызов"},
      {"These two points cannot be linked", "Эти две точки нельзя соединить"},
      {"In: links from blocks that call or use this one. Drop a data output here to add an input.",
       "Вход: связи от блоков, которые вызывают или используют этот. Отпустите здесь выход данных, чтобы добавить вход."},
      {"Out: what this block calls or uses. Drag from here to a function of another block to add a call.",
       "Выход: что этот блок вызывает или использует. Тяните отсюда к функции другого блока, чтобы добавить вызов."},
      {"Function {}(): drop a block's out point here to add a call", "Функция {}(): отпустите здесь выход блока, чтобы добавить вызов"},
      {"Remove the link", "Удалить связь"},
      {"Remove {}", "Удалить {}"},
      {"Remove all {} links", "Удалить все связи ({})"},
      {"Appearance", "Внешний вид"},
      {"Models and API", "Модели и API"},
      {"Help", "Справка"},
      {"Right click on a line removes its links; Delete removes the selected line.",
       "Правый щелчок по линии удаляет её связи; Delete удаляет выделенную линию."},
      // models and APIs (ui_models.cpp)
      {"OpenAI-compatible (OpenRouter, DeepSeek, Ollama, LM Studio...)", "OpenAI-совместимый (OpenRouter, DeepSeek, Ollama, LM Studio...)"},
      {"Anthropic (Claude)", "Anthropic (модели Claude)"},
      {"level {}", "уровень {}"},
      {"block {}", "блок {}"},
      {"not defined", "не задан"},
      {"saved to {}", "сохранено в {}"},
      {"{} (pinned: {})", "{} (закреплено: {})"},
      {"by size of the task (role {})", "по размеру задачи (роль {})"},
      {"{} (role {})", "{} (роль {})"},
      {"auto", "авто"},
      {"level {}: {}", "уровень {}: {}"},
      {"by role", "по ролям"},
      {"Model and API for this level or the selected block", "Модель и API для этого уровня или выбранного блока"},
      {"MODEL AND API", "МОДЕЛЬ И API"},
      {"Level {}: every block at this depth", "Уровень {}: все блоки этой глубины"},
      {"by role (default)", "по ролям (по умолчанию)"},
      {"Block {} and everything inside it", "Блок {} и всё внутри него"},
      {"as its level", "как у его уровня"},
      {"now: {}", "сейчас: {}"},
      {"All model settings...", "Все настройки моделей..."},
      {"MODEL", "МОДЕЛЬ"},
      {"New API", "Новый API"},
      {"API \"{}\"", "API «{}»"},
      {"Name", "Имя"},
      {"Provider", "Провайдер"},
      {"Model", "Модель"},
      {"Address", "Адрес"},
      {"empty = https://api.anthropic.com", "пусто = https://api.anthropic.com"},
      {"Key variable", "Переменная ключа"},
      {"empty = ", "пусто = "},
      {"{} is set", "{} задана"},
      {"{} is not set in this environment", "{} не задана в этом окружении"},
      {"Save", "Сохранить"},
      {"Delete", "Удалить"},
      {"it is also defined in ~/.config/karkas/config.toml", "он также задан в ~/.config/karkas/config.toml"},
      {"used by: {}", "используется: {}"},
      {"name: a-z, 0-9, _ or -, not taken", "имя: a-z, 0-9, _ или -, ещё не занятое"},
      {"Open a project to choose its models.", "Откройте проект, чтобы выбрать его модели."},
      {"Each part of the block tree can work with its own model and API. A block's own choice beats its "
       "level; a level beats the role. Karkas never stores keys: an API reads its key from an environment "
       "variable.",
       "Каждая часть дерева блоков может работать со своей моделью и API. Выбор блока важнее его уровня, "
       "уровень важнее роли. Karkas не хранит ключи: API читает ключ из переменной окружения."},
      {"APIs and models", "API и модели"},
      {"Key", "Ключ"},
      {"Add an API (another provider or model)", "Добавить API (другой провайдер или модель)"},
      {"Add API", "Добавить API"},
      {"By level of the tree", "По уровням дерева"},
      {"Level {}", "Уровень {}"},
      {"the project: orchestrator, architect of the top level", "проект: оркестратор, архитектор верхнего уровня"},
      {"By block (with everything inside it)", "По блокам (вместе со всем внутри)"},
      {"No block has its own model. Select a block and choose one in the toolbar or the inspector.",
       "Ни у одного блока нет своей модели. Выберите блок и укажите модель на панели сверху или в инспекторе."},
      {"(no such block)", "(нет такого блока)"},
      {"By role (where nothing is pinned)", "По ролям (где ничего не закреплено)"},
      {"auto (default)", "авто (по умолчанию)"},
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
      {"Existing code becomes blocks without moving files: run karkas import, then "
       "approve the proposed change in the Changes tab.",
       "Существующий код превращается в блоки без переноса файлов: выполните karkas import, затем "
       "утвердите предложенное изменение во вкладке «Изменения»."},
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
