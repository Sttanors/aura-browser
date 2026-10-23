# Развитие Aura: 15 функций

Начало: 22 октября 2026. Порядок: реализация → запись → проверка → следующий пункт.
Функция не считается завершённой по наличию кнопки: проверяется её реальное
поведение, сохранение данных и взаимодействие с приватными вкладками.

| № | Функция | Состояние |
|---|---|---|
| 1 | Рабочие пространства | Реализовано, Linux проверки PASS |
| 2 | Группы вкладок | Реализовано, Linux проверки PASS |
| 3 | Спящие вкладки | Реализовано, Linux проверки PASS |
| 4 | Две страницы рядом | Реализовано, Linux проверки PASS |
| 5 | Контейнеры сайтов | Реализовано, Linux проверки PASS |
| 6 | Полноценные профили | Реализовано, Linux проверки PASS |
| 7 | Палитра команд | Реализовано, Linux проверки PASS |
| 8 | Режим чтения | Реализовано, Linux проверки PASS |
| 9 | Перевод страниц и выделения | Реализовано, Linux/offline/live проверки PASS |
| 10 | Центр разрешений сайта | Реализовано, Linux проверки PASS |
| 11 | Панель данных сайтов | Реализовано, Linux проверки PASS |
| 12 | Менеджер закладок | Реализовано, Linux проверки PASS |
| 13 | Снимки сессий | Реализовано, Linux проверки PASS |
| 14 | Скриншоты страницы | Реализовано, Linux проверки PASS |
| 15 | Настраиваемые горячие клавиши | Реализовано, Linux проверки PASS |

## Дизайн и правила взаимодействия

Продукт: нативный браузер Aura для повседневной работы в Linux и Windows.
Пользователь: человек с несколькими задачами и сайтами, которому нужны понятные
действия без потери вкладок, настроек и приватности.
Задача первого экрана: выбрать набор вкладок или создать новый, сохранив текущий.

План по gui-genius: действующие theme tokens, без отдельной палитры функций.
Dark base: background #0D111B, surface #182033, text #EDF3FF, muted #99A7C1,
accent #A7B6FF; light и image themes берутся из того же interface_theme.
Inter Variable: заголовок 24/600, основной текст 13/400, подписи 11/400,
кнопки 13/600. Отступы 8/16/24 px, видимые keyboard focus и hover.

Сохраняется боковая панель Aura. Название текущего пространства заменяет
статичный Personal space; меню управления открывается из меню Aura и Ctrl+Shift+E.
Обычный список вкладок показывает только текущее пространство. Управление —
компактный список с одной основной кнопкой создания, а не сетка карточек.

```text
Aura / текущее пространство       Рабочие пространства
Новая вкладка                    Название [____________] [Создать]
Вкладки текущего пространства     Личное       [Открыть] [Изменить]
                                 Работа       [Открыть] [Изменить] [Удалить]
```

Пространства организационные: cookies остаются общими; изоляцию даст пункт 5.
Переключение не закрывает документы и не принимает beforeunload за пользователя.
Удаление пространства переносит вкладки в основное, сохраняя документы.
Private tabs не сериализуются. Default/hover/focus/active/disabled/empty/error
состояния, RU/EN и 75–200% должны проверяться на настоящем UI.

Проверка плана: специфичность обеспечивают сохранение левой панели Aura,
работа с живыми CEF вкладками без повторной загрузки и явное разделение
организации вкладок и изоляции сайтов. Дополнительная декоративная сетка не нужна.


## 1. Рабочие пространства — реализация и первая проверка

Реализованы create/rename/switch/remove и перенос tab через контекстное меню.
Текущий workspace виден в sidebar; менеджер — Ctrl+Shift+E и меню Aura.
Вкладки остаются живыми, CEF не пересоздаётся при переключении. Cookies общие;
workspace не обещает изоляцию контейнера. Private windows не редактируют workspace.
При удалении вкладки переносятся в основной workspace без закрытия документов.

Profile schema 6 сохраняет catalog, workspace каждого normal tab, выбранное
пространство окна и выбранную вкладку каждого пространства, в том числе неактивного.
Schema 5 мигрирует в Personal space. Private URL не записываются ни в основной
profile.toml, ни в его резервные копии.

Проверено: Linux build; 89/89 CTest (включая catalog/name validation,
roundtrip/removal/selection), 25 Python в bootstrap suite; native GUI два запуска —
PASS: live beforeunload documents A/B, create/rename/duplicate validation,
Ctrl+1/2/3/Tab только внутри workspace, normal restart и выбор inactive workspace,
private sentinel exclusion, перенос при remove без потери живой защищённой страницы.
Логи /tmp/aura-workspaces-{build,ctest,gui-final}.log.

Визуальная критика: убрано лишнее пустое место у короткого списка (высота панели
зависит от числа строк), увеличены Rename/Remove controls для русских надписей,
добавлены отсутствующие переводы Open/Current. Проверка старых browser/windows/
interface сценариев ещё выполняется; checkpoint 1 пока не закрыт.


Checkpoint 1: browser/windows/interface regression suites — PASS. Сохранены
последние выбранные normal tabs каждого workspace, включая неактивные; это
проверено после перезапуска на трёх вкладках с разными workspace ids.
Просмотрены RU/EN screenshots после перевода Open/Current и изменения высоты.
Рабочие пространства завершены в указанном объёме. Переход к группам вкладок.
Windows CI будет выполнен отдельным checkpoint после следующих изменений модели.

## 2. План групп вкладок

В sidebar группы получают именованный заголовок, цветную метку и отдельный
элемент сворачивания; цвет дублируется именем и формой. Группировка действует
внутри пространства. Удаление группы разгруппировывает вкладки без закрытия.
Менеджер позволяет назвать группу и выбрать цвет; контекстное меню вкладки —
назначить/снять группу. Активная скрытая вкладка остаётся живой; выбор через
клавиатуру раскрывает её группу. Сессия сохраняет membership и collapse state.
Основная палитра/Inter/масштабирование остаются как в плане пункта 1.

## 2. Группы вкладок — реализация и проверки

Реализованы именованные группы внутри workspace: шесть цветов, rename, assign,
ungroup, collapse/expand, создание вкладки из заголовка и перенос drag-and-drop.
Ctrl+Alt+G и контекстное меню открывают управление. Удаление группы оставляет
документы открытыми. Ctrl+1…9 / Ctrl+Tab следуют визуальному порядку и раскрывают
выбранную группу. При restore сохранённое сворачивание не сбрасывается.
Schema 7 сохраняет каталог, цвет, collapse и membership обычных вкладок;
приватные URL по-прежнему не сериализуются.

Проверено: Linux build; 90/90 CTest; native GUI два запуска — PASS: create,
rename, duplicate rejection, цвет, collapse/reveal живых beforeunload-форм без
пересоздания CEF, visual keyboard order, restart, ungroup/remove, private sentinel.
Визуально просмотрены EN и RU при 900×620. По результатам убрана лишняя высота
панели, расширены кнопка создания и подписи цветов на русском.
Логи: /tmp/aura-groups-{build,ctest-final,gui-final}.log.
Windows CI пока не выполнялся для этих изменений.

Checkpoint 2: повторный GUI groups и регрессия workspaces — PASS после исправления
ширины русских кнопок. Локальный commit 5631cb7.

## 3. План спящих вкладок

Фоновая страница переводится в Chromium lifecycle frozen; DOM, формы и история
остаются в памяти. Освобождаются native OSR bitmap buffers; это не полная выгрузка
renderer. При выборе вкладка автоматически просыпается. Меню вкладки и Ctrl+Alt+S
позволяют усыпить текущую вкладку, предварительно выбрав другую.
Автоматический сон по таймауту выключен по умолчанию. Исключения по доменам,
pinned/private/медиа/capture/загрузка/DevTools/dialog защищают от остановки работы.
Статус «Спит» виден в строке вкладки. Дизайн использует действующие tokens и Inter.

## 3. Спящие вкладки — реализация и первая проверка

Ctrl+Alt+S / «Усыпить вкладку» в контекстном меню переводят страницу в frozen
через Chromium DevTools lifecycle. Выбор вкладки пробуждает её без reload.
Native view/popup buffers освобождаются, поздние paint events игнорируются до
пробуждения. DOM сохраняется в renderer: это экономия фоновой работы и памяти
изображения, а не полная выгрузка Chromium. Настройки Tabs: Off / 5 / 15 / 30 / 60
минут и домены-исключения. Новые настройки сохраняются через общий каталог.

Защищены active, pinned, private, loading/error, capture-granted, WebAudio running,
playing audio/video/PiP, вкладки с dialog, окна с DevTools и активные downloads.
Проверка media выполняется в isolated world; inaccessible frames консервативно
сохраняют страницу активной. Исключения включают поддомены. В sidebar — «Спит» / z.

Первая проверка: Linux build, 90/90 CTest, GUI PASS с реальным пятиминутным
таймаутом (без ускорения часов). Проверено: остановка JS timer, нулевой размер
native bitmap, прежний token документа и несохранённая форма после wake,
pinned/private/WebAudio/domain exclusions, отсутствие private URL в profile/bak.
Лог /tmp/aura-sleep-gui.log. Дополнительная проверка отмены выхода из frozen формы
и итоговой версии кода ещё выполняется; checkpoint 3 пока открыт.

Официальный протокол lifecycle:
https://raw.githubusercontent.com/ChromeDevTools/devtools-protocol/master/pdl/domains/Page.pdl

Checkpoint 3: итоговый Linux build; 91/91 CTest; повторный GUI manual/exceptions —
PASS, включая Ctrl+Q на frozen beforeunload форме → Escape → сохранение данных и
продолжение работы. Pin проверен по реальному state, WebAudio — по running context.
Обычный browser regression suite — PASS. RU sidebar просмотрен; статус сокращён
до «Спит», чтобы он не обрезался. Логи /tmp/aura-sleep-{build-final,ctest-final,
gui-final,regression}.log. Таймаут 5 минут проверен отдельным полным прогоном выше.
Спящие вкладки завершены в описанном объёме. Windows CI пока не запускался.

## 4. План двух страниц рядом

Ctrl+Alt+V и меню Aura включают две существующие вкладки текущего пространства.
Каждая сторона — настоящий CEF browser с независимыми history, формой и прокруткой.
Над каждой страницей — её адрес/название, активная сторона отмечается рамкой.
Нажатие в страницу переводит keyboard focus; общая адресная строка и find относятся
к активной стороне. Разделитель изменяет ширину; выход возвращает полную страницу.
Смена workspace/newtab или закрытие стороны завершает split без закрытия другой
страницы. Видимые стороны не подлежат автосну. Сохраняются Inter и theme tokens.

Платформенный checkpoint пунктов 1–3: main ed4edd1 опубликован. Windows CI
[39757245526](https://github.com/Sttanors/aura-browser/actions/runs/39757245526) —
SUCCESS: compile, unit, базовый real browser integration, portable package и
build/install проверка Windows installer. Специализированные GUI сценарии
пространств/групп/сна пока выполнены только на Linux. Файлы релиза не заменялись.

## 4. Две страницы рядом — реализация и первая проверка

Ctrl+Alt+V / меню Aura включает текущий сайт и другую открытую страницу workspace.
Каждая сторона сохраняет собственный CEF browser, history, scroll и DOM. Выбор
стороны меняет keyboard focus и адресную строку; native input, IME, select popup и
контекстное меню используют её координаты. Заголовок каждой стороны показывает
название и host, активная сторона отмечена рамкой. Разделитель меняет ширину;
минимальная ширина каждой стороны 220 logical px. Предпочтение 25–75% сохраняется.
Schema 8 сохраняет индексы сторон для обычных вкладок; пары с private не пишутся.
Newtab/workspace change/закрытие стороны завершают split, оставляя другую страницу.

Первая проверка: Linux build и 92/92 CTest; GUI три запуска — PASS: live forms,
независимые scroll/navigation/back, native divider drag, 900×620, restore, RU/EN,
ввод в обе стороны при 75% и 200%, private exclusion и balanced CEF close. Просмотрены
скриншоты 200% и малого окна. Sleep и interface regression suites — PASS.
Уточняющая проверка видимого select popup и отрисовки ещё выполняется.

Checkpoint 4: GUI на изолированном дисплее 144 Гц — PASS, включая настоящий
видимый CEF select popup, 75/200% и private pair exclusion. Rendering regression —
PASS во всех 11 комбинациях zoom 75–200%, dark/light и device scale 1/1.25:
full painter и repeated page-only repaint совпадают на rounded corners (≤1 channel).
Логи /tmp/aura-split-{gui-popup,rendering-regression}.log. Пункт 4 завершён;
Windows CI для него пока не выполнялся.

## 5. План контейнеров сайтов

Контейнер — именованный persistent CefRequestContext с отдельными cookies,
localStorage/IndexedDB и cache. Вкладки/links/popups наследуют контейнер; private
всегда использует отдельный непостоянный context. История, закладки и настройки
общие; полную изоляцию профиля даст пункт 6. Контейнер обозначается именем у адреса
и в sidebar, цвет дополняет имя. Ctrl+Alt+C / меню открывают каталог, create/rename,
новую вкладку или копию текущей страницы в другом контейнере (исходная остаётся).

Удаление разрешено после фактического закрытия всех вкладок контейнера во всех
окнах. Каталог и сохранённые вкладки удаляются сразу; очистка его native cache
отложена до следующего запуска после успешного CEF singleton initialization,
когда старых request contexts уже нет. Это явно объясняется в подтверждении.
Дизайн использует существующие tokens/Inter и компактный список RU/EN.

Промежуточная проверка контейнеров нашла два ограничения CEF Chrome runtime:
вложенная cache-папка приводит к незаметному OTR fallback, а новый persistent
context инициализируется асинхронно и не готов для CreateBrowserSync сразу.
Исправлено: cache/AuraContainer-<id> непосредственно под root_cache_path;
OnRequestContextInitialized handler сообщает готовность, tick создаёт browser
только после неё. Пока context готовится, закрытие вкладки не передаёт в CEF
висячий client; ожидание ограничено 30 сек, ошибка показывается в странице.
Проверка storage restart повторяется. Приватный режим не получает persistent id.

Источник ограничения профилей:
https://raw.githubusercontent.com/chromiumembedded/cef/master/libcef/browser/chrome/chrome_browser_context.cc

## 5. Контейнеры — итоговая реализация и проверки

Каталог до 32 контейнеров с именем и цветом; Ctrl+Alt+C, меню Aura и контекстное
меню вкладки. New tab создаёт новую identity tab; Open page copy открывает URL в
выбранном context, оставляя исходник. Имя видно в sidebar и отдельной кнопке у
адреса (нажатие открывает каталог). Приватные вкладки не копируются в persistent
context. Окна расширений ограничены default container, options открываются в нём.
История, bookmarks, настройки и vault общие; это не отдельные полные профили.

Schema 9 сохраняет container id. Неизвестный id исключает сохранённую вкладку из
restore, а не подменяет identity на default. Ссылки, настоящие popup, новые и
восстановленные вкладки сохраняют id; context общий для всех окон данного каталога.
Удаление блокируется живыми вкладками во всех окнах, убирает связанные session и
recently closed entries; native profile очищается после следующего CEF init.
Cleanup не следует symlinks; не затрагивает default profile. Clear cookies/cache
охватывает и contexts контейнеров, включая неоткрытые в текущем окне.

Проверено: Linux build, 95/95 CTest (catalog/roundtrip, неизвестный id, удаление
сохранённых вкладок, deferred cleanup и symlink guard); native GUI три запуска —
PASS: раздельные настоящие cookies/localStorage/IndexedDB, наследование tab/link/
popup/second window, private isolation, restart без повторного seed данных,
межоконная блокировка удаления, закрытие при context initialization, cleanup при
следующем запуске и сохранность default data. Private sentinel отсутствует в
profile.toml и backups. EN/RU 900×620 просмотрены; устранено наложение имени на URL.
Попутно нормализован pending native newtab URL для restore/recently closed.
Popup и split regression suites — PASS. Логи /tmp/aura-containers-{build-final,
ctest-final,gui-final,popups-regression,split-regression}.log.
Windows CI 39783374341 на 4d19af1: SUCCESS (build, tests и installer). Checkpoint 5 завершён.

## 6. План полноценных профилей

Отдельный профиль запускается отдельным процессом Aura с собственным data-dir:
настройки, история, extensions, vault, сессии и Chromium storage не разделяются.
Создание не копирует существующие данные (только язык интерфейса). Каталог профилей
хранит имена и локальные id; пути данных выводятся из его корня. Open использует
существующий механизм CEF singleton для повторного открытия уже работающего профиля.

Ctrl+Alt+P, меню Aura и нижняя кнопка sidebar открывают native manager: create,
rename, open, archive/restore. Архив скрывает запись и сохраняет файлы; физическое
удаление профилей не входит в этот пункт, чтобы каталог не удалял работающие vault
и Chromium базы. Каталог обновляется атомарно под межпроцессной блокировкой.
Текущий профиль виден в footer и заголовке окна. RU/EN, keyboard focus и малый экран
используют действующие tokens/Inter и компактные строки управления.

## 6. Профили — итоговая реализация и проверки

Менеджер Ctrl+Alt+P, меню Aura и footer: создание с открытием, переименование,
открытие, архивирование и восстановление. Отдельный процесс/data-dir для каждого
профиля. Только язык наследуется при создании; настройки, history/bookmarks,
extensions, passwords, sessions и Chromium storage используют собственные файлы.
Повторное открытие работающего профиля использует CEF singleton, создавая окно в
его процессе. Имя видно в footer и заголовке; каталог перечитывается при активации.

Каталог обновляется атомарно под неблокирующим межпроцессным file lock, максимум
32 записи. Stale writers сначала перечитывают диск. Повреждённый каталог читается
из backup; исходные байты сохраняются отдельно, оба повреждённых файла не
перезаписываются. Каталог дочернего профиля принимается только для пути, реально
принадлежащего этому каталогу. Архивирование обратимо и сохраняет все файлы.
Схема настроек 10. Linux launch_process reap-ит только собственный запущенный PID,
не затрагивая subprocesses Chromium.

Проверено: build, 99/99 CTest, включая независимость файлов/настроек, блокировку,
конкурирующие экземпляры каталога, archive/restore без удаления, backup recovery и
отказ от чужого корня. Native GUI PASS: отдельные процессы и настоящие cookies,
localStorage/IndexedDB, history/bookmarks/settings, singleton forwarding, rename,
archive/restore при работающем втором профиле, перезапуск с сохранением данных.
EN/RU 900×620 просмотрены: кнопки и подписи помещаются, декоративные карточки не
добавлялись. Логи /tmp/aura-profiles-{build,ctest,gui}.log, screenshots в
artifacts/profiles-smoke. Windows CI на 2fe98f6 (39829001019) включает этот пункт: build/tests/installer PASS.
Отдельная интерактивная profile GUI suite пока выполнена только в Linux.
Checkpoint 6 завершён; установочные пакеты пока не пересобирались.

Отдельная плановая CEF security maintenance 39782939594 сообщает о новой версии
Chromium 156.0.8078.17 относительно pinned 154.0.8037.98. Это не ошибка компиляции:
перед следующим релизом нужно обновить dependency lock и проверить Linux/Windows
с новым CEF. Пакеты со старым движком в рамках этой работы не публикуются.

## 7. План палитры команд

Задача: найти вкладку в текущем окне (включая другие рабочие пространства),
конкретную настройку или безопасное действие, не вспоминая расположение меню.
Ctrl+K и меню Aura; native overlay в действующих theme tokens/Inter, ширина до
900 px. Компоновка: заголовок → подписанное поле поиска → строки название/контекст
→ подсказка клавиш. Одна подсвеченная строка; без карточек и декоративных иконок.
Поиск RU/EN, слова в любом порядке; точное имя и начало названия выше описания.
Стрелки/Tab выбирают, Enter выполняет, Escape закрывает, колесо прокручивает.
Переход к настройке открывает её существующий редактор, без скрытого изменения.
Приватные и обычные вкладки не показываются друг другу; query не сохраняется.

## 7. Палитра команд — итоговая реализация и проверки

Ctrl+K / меню Aura открывают поиск вкладок текущего окна во всех его рабочих
пространствах, полного каталога настроек и 26 действий. Слова RU/EN в любом
порядке; точные имена и ids выше совпадений в описании. Результаты не запускают
сетевой поиск и не сохраняют query. Private tabs видны только при поиске из
private tab. Переход к вкладке сохраняет DOM и раскрывает её группу/пространство;
настройка открывается по точному id в существующем редакторе.

Стрелки/Tab/PageUp/PageDown выбирают результат, Enter выполняет, Escape закрывает.
Disabled/empty состояния не выполняют действие. Колесо и мышь поддерживаются;
вставка UTF-8 ограничена 512 bytes и проверяет актуальность editor при callback.
Высота зависит от числа результатов; после просмотра убрано лишнее пустое поле,
без скачка строки ввода. Название/контекст/shortcut читаются в RU 900×620.

Проверено: build, 99/99 CTest, isolated native GUI PASS — переход к настоящей
вкладке другого workspace без перезагрузки и beforeunload, точный setting editor,
создание вкладки действием, disabled/empty, стрелки/Tab/paging, RU uppercase и
порядок слов, English ids в RU, private isolation и отсутствие private sentinel
в profile/backups. Базовый browser regression — PASS. Логи
/tmp/aura-commands-{build-final,ctest,gui-final,browser-regression}.log.
Checkpoint 7 завершён; установщики пока не обновлялись.

## 8. План режима чтения

Задача: читать длинную статью без меню/баннеров сайта и вернуться к исходной форме
без перезагрузки. По явной команде Ctrl+Alt+R, меню Aura или палитре Mozilla
Readability работает в isolated world над clone документа. В overlay выводится
только текст через textContent, ни один HTML-фрагмент статьи не вставляется.
Исходный DOM не заменяется. Страница без статьи показывает понятное сообщение.

Дизайн: viewport overlay с верхней строкой Reader mode / Exit, строка настроек
шрифта, размера и ширины; центрированная колонка с заголовком/автором/абзацами.
Тёмный фон #0D111B, текст #EDF3FF, muted #99A7C1, accent #A7B6FF; светлая схема
#FAFBFF/#182033. Inter/system для управления, serif или sans для статьи, 18–32 px,
line-height 1.7, 560/720/900 px максимум ширины. Controls wrap на узком экране.
Tab/Shift+Tab, visible focus, Escape и Exit закрывают; без декоративных карточек.
Размер/ширина/семейство сохраняются, содержимое статьи не записывается на диск.

Источники: https://github.com/mozilla/readability/blob/main/README.md и SECURITY.md.
Readability не sanitizer: поэтому извлечённый HTML не отображается; только plain
text nodes из detached clone. Закрепляются commit, checksum и Apache-2.0 license.

Windows CI 39829001019 на checkpoint 7 (2fe98f6): SUCCESS, включая compile,
99 tests, базовый browser integration и установку Windows package.

## 8. Режим чтения — реализация перед завершающей проверкой

Ctrl+Alt+R / меню Aura / палитра, HTTP(S) main frame. Mozilla Readability на clone
в отдельном execution world, только plain text paragraphs/headings/pre/quotes
через textContent. Закрытый shadow root внутри modal dialog изолирует оформление
и controls. Исходная страница не заменяется: форма/DOM/beforeunload/scroll остаются.
Select/copy, scrolling, Escape/Exit; serif/sans, 18–32 px и 560/720/900 px колонка.
Параметры входят в общий каталог настроек и сохраняются; текст не пишется в profile.
Статья определяется после явной команды; DOM parsing ограничен 50 000 элементов,
текст 2 млн UTF-16 units, без сетевого провайдера и без отображения HTML статьи.

Readability ab4027a8b37669745016869a37a504727992b2ba, Apache-2.0 license и SHA256
в bundled manifests. Runtime build и portable installer payload включают reader
assets; Windows installer CI проверяет их наличие. Lifecycle callbacks проверяют
живость controller, tab и navigation serial; timeout 30 сек и очистка при смене
execution context. Вкладка не засыпает во время подготовки reader.

GUI уже PASS: настоящая статья >14k characters/47 блоков, исходные DOM token,
form, beforeunload и scroll неизменны; font/size/width, restart, CSP+Trusted Types,
empty article/private/RU 900×620, переход из палитры. Дополнительный тест Copy
выявил зависание CEF clipboard в CPU-only X11; Ctrl+C для reader с selection
теперь использует native clipboard Aura. Выделенный текст берётся через CEF
OnTextSelectionChanged, хранится только в памяти и очищается при навигации/close.
Копирование проверено внешним xclip. Убрано зависшее Preparing reader toast после
успешного открытия. CTest 99/99 и Python 26/26 PASS; palette regression PASS.
Завершающие regression clipboard (2.1 MB UTF-8) и manual sleep — PASS.
Слишком большие статьи отклоняются с отдельным сообщением, без скрытого обрезания
текста (2000 блоков). Это также проверено настоящей статьёй из 2100 абзацев.
Логи /tmp/aura-reader-{build-final,ctest-final,gui-final,commands-regression,
clipboard-regression,sleep-regression}.log. Checkpoint 8 завершён; Windows CI 39832550104 на d401ad8 (включая UI wiring): SUCCESS,
compile/tests и Windows installer integration. Установочные пакеты не обновлялись.

## 9. План перевода по явной команде

Выбор владельца: онлайн, только вручную, с выбором провайдера. Native panel:
провайдер Google Translate / Microsoft Translator (текст), целевой язык, выбранный текст / адрес
публичной страницы, preview и одна кнопка Open translation. Панель не отправляет
ничего при открытии или смене options; провайдер открывается новой вкладкой после
нажатия кнопки. Для публичных страниц используется Google Translate Website;
Microsoft Translator здесь поддерживает текст, page mode недоступен без скрытой смены провайдера.
Подписанные/authenticated страницы могут не открываться через внешний URL translator.
Приватность и container исходной вкладки наследуются новой вкладкой.

Палитра команд, Ctrl+Alt+T, меню Aura, контекстное меню выделения. Для текста
используется actual CEF selection (включая reader/frames), snapshot перед показом
panel; перед отправкой проверяется исходная navigation serial. Язык/провайдер
сохраняются в profile, текст не помещается в preferences. Внешняя вкладка следует
обычной политике history/session этого профиля; private tabs исключаются как ранее.

Дизайн gui-genius: прежние palette tokens/Inter, 900 px overlay, отступы 8/16/24;
provider → language → content → preview → disclosure/send. RU/EN, keyboard focus,
empty selection, unsupported provider/mode, stale page и oversize error. Без
декоративных карточек и автоматических запросов. Исходная страница остаётся открыта.
Источник Google Websites: https://support.google.com/translate/answer/2534559
Microsoft Translator: https://www.bing.com/translator

## 9. Перевод — реализация перед завершающей проверкой

Native selector Google/Microsoft Translator, восемь вариантов языка (включая Same as Aura), page
URL / selection, preview, Cancel/Open translation; выбранные options отмечаются
галочкой. Нет запросов на открытие панели/изменение options; новая вкладка только
после Send. Google website route / Google text / Microsoft Translator text — HTTPS, UTF-8 query
encoded, target/provider из allowlist. Поддержка text-only у Microsoft Translator
явная: page mode не меняет провайдера автоматически. Selected text ограничен
5000 UTF-8 bytes: длинное выделение отклоняется, без усечения. Page URL максимум
16384 bytes, только HTTP(S), без userinfo. Source tab/navigation serial проверяются.

Ctrl+Alt+T, меню Aura, палитра (page/selection), CEF context menu; context action
привязан к исходному tab id. Новая вкладка наследует private/workspace/group/
container; исходный документ не заменяется. Настройки провайдера/языка сохраняются
общим settings catalog. Snapshot очищается при закрытии/смене panel/address.
Provider URL далее подчиняется обычной history/session policy (private исключён).

Первый native GUI PASS при блокировке внешних запросов: нет sends до команды и
после Cancel, правильные URI для Google/Microsoft Translator и Unicode, context selection,
empty/oversize/stale ошибки, source DOM сохранён, private state наследуется,
preferences restart/RU 900×620. CTest 100/100 PASS, включая query injection,
неподдерживаемые scheme/provider/target, userinfo и границы размера.

Обнаружен и исправлен общий navigation bug: при failed initial navigation UI
показывал about:blank, а Reload повторял пустую страницу. OnLoadError сохраняет
failed URL, Retry загружает этот адрес; lock не показывается до успешной загрузки.
Повтор отказавшей provider navigation проверен через заблокированный proxy.
Reader context-menu Copy теперь тоже использует native clipboard для plain text.
Final offline GUI PASS: наследование настоящего container context, private state,
URI routing, no-send/cancel, stale/empty/long, context menu, RU/EN и restart.
Live provider GUI PASS через саму кнопку Aura: Google и Microsoft вернули Привет
для синтетического Hello; Google website route перевёл публичный example.com в
русскую страницу. Только тестовые данные и пустой profile; CDP доступен на loopback
только в opt-in QA. В общую offline suite сетевой тест не добавлен.
Reader regression с Ctrl+C и настоящим context-menu Copy — PASS. Базовый browser
regression — PASS. Build и 100/100 CTest PASS. RU/EN скриншоты просмотрены; выбранные
варианты имеют check mark, Same as Aura помещается на малом экране. README RU/EN
теперь явно отличает функции main от последней packaged preview.

Логи /tmp/aura-translation-{build-final,ctest-final,gui-final,online-final,
reader-regression,browser-regression}.log. Checkpoint 9 завершён. Windows CI для
этого checkpoint ещё не выполнен; установщики не обновлялись.

Microsoft Chinese target сопоставлен с zh-Hans по официальному language support:
https://learn.microsoft.com/en-us/azure/ai-services/translator/language-support

## 10. План центра разрешений

Задача: видеть и отзывать доступ конкретного сайта, не меняя остальные сайты и
контейнеры. Текущий сайт по умолчанию, searchable picker сохранённых site scopes;
8 строк camera/microphone/notifications/location/clipboard/MIDI/sensors/controls, выбор
Default/Ask/Allow/Block, отдельный Revoke и Reset site.
Нормальные правила сохраняются в profile; private правила только в памяти Aura.

Один site scope = canonical HTTP(S) origin + container. Scope следует Chromium:
NOTIFICATIONS — requesting origin; остальные перечисленные типы — top origin.
Схема/порт различаются, wildcard/userinfo/path запрещены. Browser callback и
native ContentSettings используют один resolver. Камера/микрофон используют scoped native media callback CEF Alloy с собственным
диалогом Aura; остальные запросы — OnShowPermissionPrompt. Actual capture status
берётся из OnMediaAccessChange. PTZ имеет отдельную строку Camera controls и отдельное правило; Allow обычной
камеры не предоставляет управление её положением. Неизвестные permission bits отклоняются.

Native panel в tokens/Inter (dark #0D111B/#182033/#EDF3FF/#99A7C1/#A7B6FF), до
900 px. Заголовок → site/context → список строк с dropdown и Revoke → reset/choose.
Узкие окна прокручивают строки, Tab/focus и клавиши paging; picker имеет поиск и
empty state. Default/explicit/native состояния подписаны. Reset-маркеры сохраняют
удаление старых native exceptions для ещё не открытых container contexts.
Private notification Allow не предлагается из-за ограничений Chrome incognito;
insecure HTTP получает только Block/Default. Активный capture виден вне панели.

Проверка: fake camera/audio, без --use-fake-ui-for-media-stream и без доступа к
реальным устройствам; native queries и реальные MediaStream tracks, отзыв,
origin/port/container/private isolation, prompt cancellation и restart.
Источники: bundled CEF cef_request_context.h, cef_permission_handler.h,
cef_display_handler.h; Точный tag подтверждён CEF 14c5a089 CHROMIUM_BUILD_COMPATIBILITY.txt:
https://chromium.googlesource.com/chromium/src/+/refs/tags/154.0.8037.98/components/content_settings/core/browser/content_settings_registry.cc
В этой ревизии Sensors также допускает Ask.

### 10. Реализация и проверка

Profile schema 11: нормальные origin/container exceptions сохраняются, private
правила остаются в BrowserState. Поддерживаются Default/Ask/Allow/Block, отдельный
Revoke и Reset, scope согласно pinned Chromium, строгая проверка canonical origin.
Native ContentSettings обновляются в соответствующем request context; unknown
permission bits блокируются. Private notifications не получают Allow/Ask.

GUI выявил важную особенность windowless CEF Alloy: default media handling всегда
отказывает, поэтому используется CefMediaAccessCallback с scoped решением Aura.
Нативный MediaStream registrar сообщает actual video/audio через OnMediaAccessChange.
Отзыв ContentSetting действительно завершает tracks; фиктивные флаги и перезагрузка
страницы не используются. Навигация отменяет prompt без запоминания отказа.
Native dropdown теперь обрабатывает Escape отдельно от parent panel, поддерживает
стрелки/Home/End/Tab/Enter и пропускает disabled choices. Удалены лишние украшения;
малые окна показывают scrollbar, подписи полностью RU/EN.

104/104 CTest PASS. Изолированный Weston Pixman/Xwayland SHM GUI PASS с настоящими
синтетическими MediaStream, без fake-ui и без физических устройств: camera/mic
allow/revoke, Ask/accept, notification deny, отмена beforeunload сохраняет active
capture, отзыв сохраняет DOM/form; navigation-dismiss не сохраняет отказ.
Origin/port и top-origin iframe isolation, container/private isolation,
private notification disabled choices, отсутствие private origin в profile/backup,
restart/default reset, searchable site picker и keyboard dropdown, RU 900×620 PASS.
Логи /tmp/aura-permissions-{build-final,ctest-final,gui-final}.log.
Установщики не изменены; Windows проверка этого checkpoint ещё не выполнена.

Базовая browser GUI regression и offline translation/menu regression — PASS.

## 11. План панели данных сайтов

Продукт и аудитория те же. Задача: оценить дисковое хранилище выбранного сайта и
удалить его данные, понимая, где это завершит вход в аккаунт. Контекст закреплён
за исходной вкладкой: container/private не подменяются при переключении вкладок.
Текущий origin и список открытых/недавних сайтов этого контекста; список не выдаётся
за полный индекс диска. Quota estimate отдельно от cookies; localStorage не входит
в quota estimate, но входит в очистку. Cookies имеют отдельное действие и пояснение
про общие порты/родительские domain cookies.

Дизайн gui-genius: те же 5 theme tokens и Inter 24/13/11, окно 900×620, выравнивание
слева по 28 px. Header → origin/context → размер и типы → два понятных действия →
status/refresh. Точный origin вместо неявного объединения поддоменов. Пример:
[Данные сайта] [Выбрать сайт] / example.org / Контейнер: Работа /
[Хранилище 1.2 MB] [Cookies 4] / [Удалить хранилище] [Удалить cookies].
Никаких декоративных диаграмм: числа и последствия важнее визуального шума.
Loading/error/empty/success, RU/EN и клавиатура; destructive confirmation называет
origin и последствия. Нет сетевых запросов и чтения значений cookies в отчёт.
Проверка: реальные IDB/cache/localStorage/cookies, разные origin/port/container/
private, отмена удаления, повторный замер и восстановление после перезапуска.

Windows CI 39908268381 для 4198e68 (пункт 10) — SUCCESS; перевод c4aba8f также прошёл Windows CI 39837402503. Пакеты релиза не заменялись.

### 11. Реализация (финальные проверки выполняются)

Ctrl+Alt+D, меню Aura и палитра команд открывают Site data. Scope закреплён за
исходным tab id/container/private; запросы выполняются его DevTools session и
request context, без fallback в default context. Picker читает открытые и недавние
origins; он не навигирует страницу и не выдаёт список за полную инвентаризацию диска.

Storage.getUsageAndQuota показывает quota-managed estimate и breakdown, с явным
пояснением, что Local Storage/cookies в сумму не входят. Исправлен разбор int/double
CEF JSON: целочисленный usage прежде ошибочно отображался нулём. Значения cookies
не читаются: native visitor считает совпадающие domains, включая parent domain
cookies; httpOnly/path/port не скрывают записи от очистки. Visitor delete + FlushStore
завершаются до повторного замера. Подтверждение называет origin/context и последствия.

Удаление хранилища использует Storage.clearDataForOrigin с отдельными storageTypes,
исключая cookies и permissions; sessionStorage не обещается к удалению. Cookies
удаляются отдельным действием и могут завершить вход на общих портах/поддоменах.
Проверяются serial/lifetime/context, timeout 20 секунд и исчезновение исходной вкладки.
Native confirmation меняет высоту по тексту, чтобы не обрезать последствия удаления.

### 11. Финальная проверка

104/104 CTest PASS. Native GUI PASS (2 запуска): quota estimate на несжимаемых
синтетических данных; отмена обоих подтверждений сохраняет данные; удаление
IDB/cache/localStorage/service worker для A сохраняет B другого порта и cookies.
Отдельная очистка cookies удаляет host/path/HttpOnly записи на общих портах,
сохраняет localhost другого host, container и private context. Private delete не
затрагивает normal/container. Переключение вкладки при открытой панели не меняет
её scope; picker не навигирует документ. Restart сохраняет remaining container
storage и результаты удаления. Private URLs отсутствуют в profile/backup.

RU 900×620, пустой поиск, keyboard picker, confirmation с переносами и полными
последствиями просмотрены. Малый экран суммирует скрытые типы в «Прочие»; полный
размер не теряется. Лишняя высота большой панели убрана. Permissions regression и
command palette regression — PASS. Логи /tmp/aura-site-data-{build-final,ctest-final,
gui-final,permissions-regression,commands-regression}.log. Checkpoint 11 завершён.
Установщики не обновлялись; Windows CI для этого checkpoint ожидается после push.

## 12. План менеджера закладок

Задача: организовать сохранённые страницы и переносить их в другой браузер.
Папки, теги, поиск по названию/адресу/тегам, выбор нескольких записей, перенос,
назначение тегов, удаление с Undo, HTML export с native Save dialog. Существующие
bookmarks мигрируют в «Без папки», import и адресные подсказки сохраняются.

GUI-genius: те же theme tokens (background #0D111B, surface #182033, text #EDF3FF,
muted #99A7C1, accent #A7B6FF), Inter 24/13/11, шаг 8/16/24. Один список вместо
карточек: [Поиск] [Папка v] → [✓ название / адрес / теги] → [массовые действия] →
[Новая папка] [Экспорт HTML]. Edit только по явной кнопке; поля с подписями и
ошибкой возле формы. На узком экране прокручиваются строки, нижние действия видны.
Primary — сохранить изменения в editor; разрушение заменяется доступным Undo.
Не добавлять отдельный dashboard или декоративные счётчики.

Profile schema 12, stable folder ids и metadata в Visit с defaults для старого
формата. Export — Netscape HTML, escaped text/attributes, только безопасные HTTP(S)
адреса. Native Save + atomic replacement сохраняет предыдущий файл при ошибке.
Проверка: migration/round trip/escaping/limits, настоящие UI search/edit/folder/bulk/
undo/save chooser, restart, RU/EN и импорт экспортированного HTML обратно.

### 12. Реализация (финальная проверка впереди)

Profile schema 12 хранит stable folder ids/counter и tags; старые записи остаются
в «Без папки». Менеджер заменяет прежний простой список: RU/EN token search,
редактирование title/HTTP(S)/folder/tags, searchable folder picker без обрезания
большого каталога, выбор скрытых строк, bulk move/tag/delete и Undo. Undo вставляет
удалённые записи в прежние позиции, сохраняет сторонние изменения и не заменяет
уже существующий URL; удалённая позже папка заменяется Unfiled при восстановлении.
Поиск кешируется по shared profile revision, query и folder, не сканируя весь
каталог при каждом кадре; изменения из другого окна инвалидируют кеш.

Native Save: Windows GetSaveFileNameW / Linux zenity, отдельный cancel/error/success,
UTF-8 paths. Atomic export пишет новый temp O_EXCL/0600 и flush/replace, прежний файл
не обрезается при ошибке. Netscape HTML экранирует HREF/title/folder/tags и не
экспортирует исполняемые схемы. HTML import восстанавливает folders/tags, вложенные
папки уплощаются в названия путей; imported ids сопоставляются local catalog.
110/110 CTest PASS на первом проходе: migration, limits, RU tags, metadata restart,
HTML safety/round trip/nested folders и atomic binary overwrite/failure.
Первый GUI прошёл edit/validation, folders/bulk/undo, реальный Save cancel и HTML
export/restart, но выявил перехват Escape полем поиска; исправлено до финальной
проверки. Полные RU/EN screenshots получены, итоговая проверка ещё не завершена.

Windows build/CTest пункта 11 прошли; integration 39910494609 упал на transient sharing violation при чтении atomic QA report (KeyError пустого dict). Отдельный 6111f3d сохраняет last complete report в Windows smoke reader; повторный CI запущен автоматически.

### 12. Финальная проверка

110/110 CTest PASS. GUI PASS, 2 запуска: RU uppercase search и tags, editor с
отказом javascript URL, folder create/rename/remove, keyboard folder picker,
выбор всех 19 отфильтрованных записей включая скрытые, bulk move/tag/delete/Undo,
сохранение порядка после Undo, native Save cancel и UTF-8 HTML filename, escaped
markup и полные metadata после restart. Private страницы не попадают в bookmarks
автоматически и не сохраняются в profile/backup. RU 900×620 просмотрен: фиксированные
подписи не обрезаются; bulk geometry общая для draw/pointer/report и сжимается по
ширине. Лишние карточки/счётчики не добавлены. Исправлены Escape и selection/scroll
папок, чтобы клавиатура работала и в коротком списке.

Import GUI regression, базовый browser GUI regression и command palette regression
PASS. Логи /tmp/aura-bookmarks-{build-final,ctest-final,gui-final,import-regression,
browser-regression,commands-regression}.log. Checkpoint 12 завершён. README RU/EN
описывают менеджер; устаревшее точное число настроек заменено описанием разделов.
Установщики не заменялись; Windows проверка пункта 12 ожидается после push.

## 13. План именованных снимков сессий

Задача: вручную сохранить набор обычных окон и восстановить его позже, оставив
текущие окна открытыми. Снимок хранит URL и layout (порядок, pinned, active,
workspace/group/container, split pair), а не DOM/формы/cookies. Private исключён.
Именование, rename/update/delete и Undo удаления; restore показывает точное число
окон/вкладок и требует явной команды. Не более 32 снимков, 32 окон и 200 вкладок
на снимок; превышение — ошибка целиком, без тихого усечения.

GUI-genius: theme tokens background #0D111B / surface #182033 / text #EDF3FF /
muted #99A7C1 / accent #A7B6FF, Inter 24/13/11, шаг 8/16/24. Native panel 900 px:
[Название] [Сохранить] → name / windows+tabs+date → Restore/Rename/Update/Delete.
Footer — Undo и Close. Loading/empty/error, disabled restore при удалённом container,
keyboard/scroll и RU/EN. Не добавлять декоративную timeline: важны состав и действие.

Schema 13 хранит снимки с metadata workspace/group. Restore сопоставляет или создаёт
организационные definitions, не изменяя существующие живые группы. Удалённый
container запрещает restore, без fallback в default identity; id контейнера из
снимка не переиспользуется при создании другого container. Profile atomic save и
backup распространяются на снимки. Проверка: round trip/limits/missing container,
настоящие дополнительные окна, исходный DOM сохранён, private отсутствует на диске,
restart, active/pinned/group/workspace/split и удаление/Undo.

### 13. Реализация (финальная проверка впереди)

Schema 13, named_sessions с monotonic ids/revisions, датами и metadata только
используемых workspace/group. Общий capture_session используется и для auto-save,
и для снимков; private-only остаток не стирает обычную сессию. Если активна private
вкладка смешанного окна, выбирается обычная вкладка того же workspace.

Restore открывает дополнительные нормальные окна, проверяет revision после
подтверждения, общий лимит окон и все container ids. Metadata подготавливается
транзакционно с rollback, без копирования больших history/bookmark vectors.
Существующие группы не переименовываются и не меняют цвет/сворачивание; при
несовместимых definitions создаётся отдельная группа. Удалённые container ids,
оставшиеся в снимках, учитываются генератором новых container ids.

Native Ctrl+Alt+E / Aura / command palette. Rename, подтверждённый Update,
Delete/Undo, disabled Restore и подпись missing container, private-only ошибка.
Границы/типы/unknown private metadata проверяются при чтении, без тихого усечения
или подмены identity. 114/114 CTest и первый 3-launch real-window GUI PASS: layout,
private window + mixed private tab, DOM/form сохранены, missing identity blocked,
restart и private-only save/update reject. Визуальная проверка выявила отсутствующий
перевод Restore: добавлен. Ширина кнопок confirmation вычисляется по надписи,
пустая высота панели убрана; финальные регрессии впереди.

Windows CI 39912982714 для 6111f3d — SUCCESS (пункт 11). CI 39913557829 собрал пункт
12, но unit test преобразовывал UTF-8 export filename через Windows code page;
4d30e7a исправляет только тест через utf8_path. Повторный Windows CI запущен.

### 13. Финальная проверка

114/114 CTest PASS. Итоговый 3-launch GUI PASS: реальные новые окна с active/pinned/
workspace/group/container/split; исходный DOM token и защищённая форма сохранены.
Private window и private tab смешанного окна исключены; приватные URL отсутствуют
в profile и backup. Cancel restore/update не меняет снимок/окна; rename/update/
delete/Undo сохраняются. После удаления container Restore disabled, после restart
старый id не выдаётся новому container; явный Update заменяет снимок текущими
окнами. Private-only save/update отклоняются, прежний снимок остаётся.

RU/EN screenshots просмотрены; чёрный снимок перекрытого окна в QA устранён
windowraise только на приватном display. Русские Restore/Update помещаются целиком,
неиспользуемая высота панели убрана. Split, workspaces и permissions regressions
PASS. Логи /tmp/aura-sessions-{build-final,ctest-final,gui-final,split-regression,
workspaces-regression,permissions-regression}.log. Checkpoint 13 завершён.
Windows CI пункта 13 ожидается после push; установщики не обновлялись.

## 14. План скриншотов страницы

Задача: сохранить видимую часть, выбранную область или весь документ в PNG.
Снимок только текущего web view, без chrome Aura; явная команда и native Save,
private разрешён по команде пользователя и не добавляет запись в профиль.
Источник привязан к tab/navigation serial, при смене страницы результат отменяется.

GUI-genius: текущие theme tokens (#0D111B/#182033/#EDF3FF/#99A7C1/#A7B6FF),
Inter 24/13/11, шаг 8/16/24. Native panel: source → 3 выбора с пояснениями → Capture.
Area selector поверх активной страницы: drag, arrows, Shift+arrows resize,
Enter Save, Escape Cancel; рамка/размер и понятные кнопки. Для reader full-page
недоступен, visible/region работают. Loading/error/empty/success, RU/EN и scaling.
Без декоративной галереи: результат сохраняется native Save dialog.

CDP Page.getLayoutMetrics + captureScreenshot(fromSurface), CSS→DIP page zoom и
native device scale. Большие страницы уменьшаются до 32 MP/32766 px на сторону;
output size/reduced видимы. Это отдельный предел screenshot, wallpaper не меняется.
Base64/PNG header/size проверяются до записи, atomic export из пункта 12.
Точный API проверен по pinned Chromium tag 154.0.8037.98 Page.pdl/page_handler.cc:
https://chromium.googlesource.com/chromium/src/+/refs/tags/154.0.8037.98/third_party/blink/public/devtools_protocol/domains/Page.pdl
Проверка: реальные PNG dimensions/pixels (scroll/full/region), form/scroll сохранены,
site/interface zoom, split source, cancel/stale/private и большой документ.

## 14. Реализация и проверка

Ctrl Shift S, меню Aura и палитра команд открывают native панель трёх режимов.
Область поддерживает drag, стрелки, Shift+стрелки, Ctrl для шага 1 px, Tab/Enter/Escape.
PNG сохраняется только после native Save, атомарно; отмена не создаёт файл.
Источник привязан к navigation serial, закрытие/смена страницы отменяет результат.

При проверке пикселей обнаружена особенность CEF OSR: SetSize пустой, поэтому
Page.captureScreenshot выдавал размер всей страницы, но заполнял его повторением
верхней части. Добавлен реальный GetViewRect для ограниченной capture surface;
device emulation удерживает layout viewport. После захвата метрики возвращаются,
scroll восстанавливается через isolated world исходного документа. Ошибка/отмена
также очищают override; промежуточный огромный buffer не попадает в native UI.
Источник: https://github.com/chromiumembedded/cef/blob/master/libcef/browser/osr/render_widget_host_view_osr.cc

118/118 CTest PASS. Реальные PNG проверены по цветным полосам документа:
visible после scroll, full 2600 px, область 200×130, tall 50000→32766 px;
несохранённая форма/DOM token/scroll сохранены. Native Save cancel и кириллический
путь PASS. Второй запуск: RU, interface 150% (auto-fit 145%), page 125%, DPR 1.5,
private PNG и full 4875 px, keyboard selector cancel, stale navigation reject PASS.
RU UI просмотрен: укорочена обрезанная кнопка «Сохранить», убран лишний outline toolbar.
Проверка split и повтор после финального форматирования ожидаются ниже.

Финальная проверка: 118/118 CTest PASS, screenshot GUI включая split active pane
(отдельный PNG 1800 px с цветами только выбранной страницы) PASS; базовый browser
и reader GUI regressions PASS. Протоколы /tmp/aura-screenshot-{build-final,
ctest-final,gui-final,browser-regression,reader-regression}.log, PNG/UI —
artifacts/screenshot-smoke. Checkpoint 14 завершён. Windows CI после push.

## 15. План настраиваемых горячих клавиш

Задача экрана: найти команду, нажать сочетание и сразу понять, принято ли оно.
Native panel: поиск → список «команда / сочетание / сброс / отключить» →
восстановить стандартные / закрыть. Отдельный режим записи перехватывает клавиши;
Escape отменяет запись, конфликт/системное сочетание оставляет старое назначение.
GUI-genius: текущие 5 theme tokens, Inter 24/13/11, шаг 8/16/24, без карточек на
каждую команду. Состояния focus/hover/recording/error/disabled/empty, RU/EN/scale.
Сочетания — настройка профиля, общая для его окон; private не меняет область настроек.
Системные/редакторские клавиши, AltGr, IME и modal dialogs защищены. Стандартные
альтернативы F5/F3 и Ctrl+Tab/цифры сохраняются и зарезервированы от конфликтов.
Проверка: модель конфликтов/reserved/persistence и реальные remap/old binding off,
restart/несколько окон/поиск RU/keyboard capture/modal/редакторский clipboard.

## 15. Реализация и первая проверка

Общий каталог 34 команд, Primary bindings + overrides (включая disable), schema14.
Проверка конфликтов учитывает все эффективные назначения; reset одной команды тоже
может конфликтовать и тогда не меняет настройки. Некорректный импорт overrides
возвращает стандартные сочетания; clipboard/undo/redo, Ctrl+цифры/Tab, Alt+F4,
Ctrl+Alt+F1…F12, UI reset и F3/F5/F6/F7/F10 защищены. AltGr/Super не записываются.
Native panel с поиском RU/EN/id/key, capture/cancel, disable/reset/reset-all;
обработчик переводит сочетание в существующий dispatcher, сохраняя dialog/editor
и приватные ограничения. Перезапись обновляется сразу во всех окнах профиля.
Меню/палитра/address/tooltips показывают актуальные сочетания.

122/122 CTest PASS (conflict atomicity, reset conflict, reserved keys, disable all,
roundtrip и invalid persisted config). Реальный GUI: новое/старое сочетание,
conflict/reserved/cancel, disable/reuse/reset-conflict, restart RU, общие окна,
защищённый beforeunload и native clipboard editing, reset-all PASS.
Визуальная проверка выявила обрезанную RU кнопку «Отключить»: увеличена ширина;
высота панели теперь зависит от результатов, без пустой декоративной области.

Windows CI 0456b9a: сборка/unit PASS, GUI упал на прежних фиксированных координатах
меню New window (после роста меню выбиралось Private window). В 1340687 тест
переведён на menu_ui item id/bounds для обычного и private окон; повтор ожидается.
Предыдущие ca665b1 и 4d30e7a Windows CI SUCCESS.

Финальная проверка пункта 15: 122/122 CTest PASS; hotkeys GUI PASS, включая
RU uppercase clipboard search, Tab→record→Escape, Mod5 AltGr/Super не вызывают
команду. Native X11 Mod5 и Windows RightAlt+Ctrl теперь передают флаг AltGr;
Super/Win тоже отделён от пользовательского сочетания. Переназначения действуют
при открытом popup расширения, сохраняя редакторские клавиши самого расширения.
Скриншоты, палитра команд, настройки (content/permissions/downloads/data/reset/
small RU/restart) и upgrade/restart расширений — GUI regressions PASS.
RU 900×620 UI просмотрен: кнопки/надписи не обрезаны, длинный inline hint в sidebar
скрывается вместо выхода за кнопку. Все GUI — isolated CPU Weston/Xwayland.
Логи /tmp/aura-hotkeys-{build-final,ctest-final,gui-final,screenshot-regression,
commands-regression,settings-regression,extensions-regression}.log.
Checkpoint 15 завершён. Все 15 функций реализованы в исходниках и проверены на Linux.
Установщики и версия preview не менялись; перед новым релизом остаются Windows CI
на последнем commit и обновление pinned CEF/Chromium.

## Выпуск 0.4.0 Preview 1 — подготовка

Windows CI ec74106 SUCCESS (39950272377): все 15 функций в исходниках собраны,
unit/integration/portable/installer lifecycle проверены. Для публикации новых
пакетов версия 0.4.0-preview.1, channel sequence 6. CEF обновляется с Chromium
154.0.8037.98 до актуального stable 156.0.8078.48 (официальный index 23 октября).
Архивы обеих платформ сверены с size/SHA-1 официального индекса; в dependency
lock закреплены вычисленные SHA-256. Подписание канала остаётся локальным,
закрытый ключ не передаётся в CI. До публикации: сборки и GUI после смены CEF,
Debian ABI runtime + .deb/.rpm/.pkg.tar.gz + .run, Windows CI exact commit,
подписанные хеши всех семи assets и проверка публичного канала.
