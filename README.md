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

## Работа с проектом

    karkas task "добавить эффект reverb"
    karkas debug "звук трещит" --fact "contract stream.ring FAIL"
    karkas ticket list | show T-0001 --path
    karkas change list | show C-0001 ; karkas approve C-0001
    karkas check | index --watch | view stream | query path io.reader mix
    karkas cost --by role
    karkas mcp --project .             # для Claude Code: claude mcp add karkas -- karkas mcp --project <dir>
    karkas bench run --scenario s3 --method C --reps 3 ; karkas bench report

Ключ API — только в переменной окружения ANTHROPIC_API_KEY. Модели и цены — в karkas.toml
(пример — templates/target/karkas.toml; цены сверить с официальной страницей).
