# Орфография и словарь / Spelling and dictionary

## Русский

В **Настройки → Веб-страницы** включите **Проверять орфографию** и выберите
русский, английский или оба языка. В редактируемом поле нажмите правой кнопкой
по подчёркнутому слову и выберите исправление. Проверка учитывает `spellcheck=false`
и не проверяет пароли. Сайт может управлять проверкой собственных редакторов.

Команда **Ctrl+K → Словарь орфографии** открывает список слов. То же действие есть
в настройках и контекстном меню поля. Введите новое слово и нажмите Enter или
**Добавить**. Поле также фильтрует список; **Удалить** снова включает проверку
этого слова. Ошибка сохраняет введённое значение. Tab/F10 переключают фокус,
Enter активирует кнопку, Page Up/Down прокручивают список, Escape закрывает панель.

Обычные слова сохраняются в текущем профиле и применяются ко всем его окнам и
контейнерам без перезагрузки страниц. В приватном режиме команда **Запомнить для
приватной сессии** добавляет слово только в память. Приватные окна используют
сохранённые слова профиля и общий временный список дополнений; он исчезает после
закрытия последней приватной вкладки. Редактор приватного словаря показывает
только временные дополнения. Они не записываются в профиль или резервную копию.

Каждый список ограничен 500 словами; одно слово — до 99 байт UTF-8 без пробелов
и управляющих символов. Предел длины задаёт словарь движка; байты отличаются от числа букв. Проверка работает на устройстве, словари входят в установку; отправка текста
в удалённую службу орфографии отключена. При отсутствии файлов словарей панель
предлагает переустановить Aura.

## English

In **Settings → Webpages**, enable **Check spelling** and choose Russian, English,
or both. Right-click an underlined word in an editable field to select a native
correction. Passwords and `spellcheck=false` fields are excluded. Sites may control
spelling in their own editors.

Open **Ctrl+K → Spelling dictionary**, or use Settings/the field context menu.
Enter a new word and press Enter or **Add word**. The field also filters the list;
**Delete** makes the word eligible for checking again. Invalid input remains in the
field. Tab/F10 move focus, Enter activates a button, Page Up/Down scroll the list,
and Escape closes it.

Saved words belong to the current profile and update its windows and containers
without reloading pages. **Remember for this private session** stores additions
only in memory. Private windows use saved profile words plus a shared temporary
list, cleared after the last private tab closes. The private dictionary editor
shows only temporary additions. They never enter the profile or its backup.

Each list allows 500 words, up to 99 UTF-8 bytes per word, excluding spaces and
control characters. Bytes are not the same as letters. Checking stays on-device;
the installation includes the dictionaries and the remote spelling service is
disabled. Missing dictionary files produce a reinstall message in the editor.

## Implementation and bundled data

CEF supplies native markers, suggestions and `ReplaceMisspelling`. Its
`IsSpellCheckEnabled` flag is not used as a marker gate: pinned CEF 156 can return
false while also reporting a misspelled word and valid suggestions. Correction
commands require a native word, editable field and unchanged navigation serial.

Only the `SpellCheckCustomDictionaryAPI` Blink feature is enabled. Per-document
add/remove operations run in an isolated Aura world, including new frames.
Future-document registrations carry generation guards. Aura does not write CEF's
native Custom Dictionary, which would persist private additions.

Bundled Chromium dictionaries and original notices are in
[resources/Dictionaries](../resources/Dictionaries/). The manifest pins revision,
file sizes and SHA-256; the packaging check verifies their archive contents.
Linux seeds the profile cache's Dictionaries directory before CEF initialization;
Windows uses the bundled Dictionaries directory beside the executable.

Primary references: [Chromium dictionaries](https://chromium.googlesource.com/chromium/deps/hunspell_dictionaries/+/eb106bd524eea61487644fffeec0d229fa41791a/),
[document dictionary explainer](https://github.com/Igalia/explainers/blob/main/spell-check-dictionary/README.md).
