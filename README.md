# Karkas

Среда разработки, в которой проект для ИИ — иерархическая схема блоков (docs/SPEC.txt).
C++23, CMake + Ninja. Состояние по этапам — docs/progress.md, отступления от спецификации — docs/adr/.

## Сборка

    cmake --preset dev && cmake --build --preset dev && ctest --preset dev
    cmake --preset headless && cmake --build --preset headless     # без GUI

Для Graphviz-раскладки: `sudo pacman -S graphviz`, затем пересобрать (иначе встроенный Сугияма).

## Новый проект (13.1)

    karkas init myproj --idea "..."    # project.ksl, karkas.toml, .karkas/
    cd myproj
    karkas design                      # архитектор: верхний уровень → ОСТАНОВКА
    karkas view / karkas gui           # посмотреть, поправить карточки
    karkas approve L0
    karkas expand --all                # раскрытие групп и api/ листьев → ОСТАНОВКА
    karkas approve api                 # заморозка + gen cmake/skel/mocks/probe
    karkas implement all               # тестировщики, исполнители, корень сборки
    karkas test

Из GUI: «Karkas → Новый проект…» (Ctrl+N) — в новой папке или в существующей.

## Существующий проект

    cd ~/мой/проект
    karkas init .                      # каркас: project.ksl, karkas.toml, .karkas/
    karkas import --dry-run            # что получится: группы по папкам, блоки по парам файлов
    karkas import                      # предложение C-NNNN → утвердить во вкладке «Изменения»
                                       # GUI или karkas approve C-NNNN

Из GUI: «Новый проект → В существующей папке» с галочкой «Разрезать код на блоки» делает
то же самое за один шаг.

Код не переносится: карточки лежат в blocks/, а в них строка `files` указывает на
существующие файлы (docs/adr/0009). Свой CMakeLists.txt остаётся как был.

## Работа с проектом

    karkas task "добавить эффект reverb"
    karkas debug "звук трещит" --fact "contract stream.ring FAIL"
    karkas ticket list | show T-0001 --path
    karkas change list | show C-0001 ; karkas approve C-0001
    karkas check | index --watch | view stream | query path io.reader mix
    karkas cost --by role
    karkas mcp --project .             # для Claude Code: claude mcp add karkas -- karkas mcp --project <dir>
    karkas bench run --scenario s3 --method C --reps 3 ; karkas bench report

## GUI

    karkas gui                         # или karkas-gui --project <dir>

Окно устроено как IDE (по образцу VSCode/Godot): слева полоса активности — проводник блоков,
поиск, изменения схемы, настройки; в центре редактор с вкладками (схема и открытые файлы
кода); справа инспектор; снизу тикеты, агенты, сборка, затраты, дрейф; внизу строка состояния.

Навигация: адресная строка сверху (щелчок по части пути или ввод пути); наведение на блок
оставляет видимыми только его связи, Shift+щелчок закрепляет подсветку (можно несколько
блоков), Shift+двойной щелчок раскрывает группу на месте (закрыть — щелчок по полосе пути
на рамке). Проекты: «Karkas → Открыть проект…» (Ctrl+O), недавние, «Открыть папку».

Связи удаляются правым щелчком по линии (одна связь или все связи линии) или клавишей Delete
по выделенной линии; перед удалением показывается изменение KSL.

Настройки открываются отдельным окном (шестерёнка или Ctrl+,): внешний вид, схема, модели и
API, справка. Язык интерфейса (English / Русский), размер блоков по умолчанию и размер текста
сохраняются в ~/.config/karkas/gui.json. Размер блока
меняется перетаскиванием его правого нижнего угла (минимум 120 × 56), текст масштабируется
под размер; позиции и размеры хранятся в .karkas/layout/.

## Модели и API по уровням

Каждому уровню дерева блоков и каждому блоку можно дать свою модель и свой API
(docs/adr/0011). В GUI — кнопка модели на панели схемы (уровень и выбранный блок), раздел
«Модель» в инспекторе и страница «Модели и API» в настройках. В karkas.toml:

    [models.local]                     # ещё один API рядом с strong/medium/cheap
    provider    = "openai_compat"
    model       = "qwen3-coder"
    base_url    = "http://localhost:11434/v1"
    api_key_env = "OLLAMA_API_KEY"     # имя переменной; сам ключ в файлы не пишется

    [levels]
    0 = "strong"                       # проект: оркестратор, архитектор верхнего уровня
    2 = "cheap"

    [blocks]
    "world.pickups" = "local"          # блок и всё внутри него

Порядок: свой блок → уровень → роль ([roles]) и маршрутизация по размеру задачи.
Ключи — только в переменных окружения (по умолчанию ANTHROPIC_API_KEY / OPENAI_API_KEY).
Цены моделей — в [prices] (сверить с официальной страницей).
