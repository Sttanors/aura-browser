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
| 9 | Перевод страниц и выделения | Онлайн, только по явной команде, провайдер выбирается пользователем |
| 10 | Центр разрешений сайта | В очереди |
| 11 | Панель данных сайтов | В очереди |
| 12 | Менеджер закладок | В очереди |
| 13 | Снимки сессий | В очереди |
| 14 | Скриншоты страницы | В очереди |
| 15 | Настраиваемые горячие клавиши | В очереди |

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
clipboard-regression,sleep-regression}.log. Checkpoint 8 завершён; Windows CI для
reader ещё не выполнен. Установочные пакеты не обновлялись.
