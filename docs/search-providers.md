# Пользовательские поисковики

Откройте **Настройки → Поисковики** (кнопка в заголовке) или найдите
**Поисковики** в палитре `Ctrl+K`. Нажмите **Добавить поисковик** и заполните:

- Название: например, `Wikipedia`.
- Короткая команда: например, `w`.
- URL: например, `https://en.wikipedia.org/w/index.php?search={searchTerms}`.

После сохранения введите `w browser engine` в адресной строке и нажмите Enter.
Aura подставит `browser engine` вместо `{searchTerms}`, кодируя пробелы,
пунктуацию и UTF-8, чтобы запрос не менял параметры или адрес сервиса.
Команды нечувствительны к регистру английских букв и должны отделяться от
запроса пробелом. Пустой запрос команды не выполняется: одиночное `w`
обрабатывается как обычный текст поиска.

**По умолчанию** назначает пользовательский сервис для запросов без команды.
Обычные адреса сайтов, локальные HTTP-адреса и file:// продолжают открываться
как адреса. Неизвестная команда считается частью обычного запроса.
Подсказка адресной строки показывает выбранный сервис, включая распознанную
короткую команду. Изменение записей не перезагружает уже открытые страницы.

**Использовать встроенный поисковик** возвращает ранее выбранный DuckDuckGo,
Google, Bing или Yandex. Выбор встроенного сервиса в обычных настройках также
отменяет пользовательский поиск по умолчанию. Его удаление с подтверждением
возвращает встроенный сервис; остальные команды продолжают работать.
Сброс настроек возвращает встроенный поиск, сохраняя добавленные записи.
Редактирование сохраняет идентификатор записи и её статус по умолчанию.

Шаблон должен содержать ровно один `{searchTerms}` в пути, параметрах или
фрагменте URL HTTP(S), после адреса сайта. Подстановка в домен, учётные данные
или порт запрещена; другие шаблонные поля не поддерживаются. `javascript:` и
`file:` не допускаются для поисковиков. Ошибка формы сохраняет введённые значения.
Лимиты: 32 поисковика, название до 120 байт UTF-8, команда 1–16 символов
`a-z`, `0-9`, `_`, `-` (первая — буква), URL до 4096 байт,
запрос к пользовательскому сервису до 16 КиБ.

Записи и выбор по умолчанию хранятся в profile.toml отдельно для каждого
профиля и доступны его окнам. Они работают и в приватных вкладках; приватные
запросы не записываются в историю Aura. Отправка поискового запроса выполняется
только после команды навигации, как при обычном поиске. Управление поисковиками
само по себе ничего не загружает с указанных сайтов.

# Custom search engines

Open **Settings → Search engines** using the header button, or find **Search
engines** in the command palette (`Ctrl+K`). Add a name, a keyword such as `w`,
and an HTTP(S) URL containing `{searchTerms}`, for example:
`https://en.wikipedia.org/w/index.php?search={searchTerms}`.

Type `w browser engine` in the address bar. Keywords are case-insensitive and
must be followed by whitespace and a nonempty query. A standalone keyword is
ordinary search text. Query text is encoded as UTF-8 URL data, including spaces,
punctuation and reserved characters. Unknown keywords remain part of an ordinary
query. Normal website addresses, local HTTP URLs and file:// URLs still navigate
normally. The address hint shows the selected provider, including keyword matches.

Choose **Use as default** for searches without keywords. **Use built-in search
engine**, choosing a built-in engine in Settings, or deleting the active custom
default returns to the remembered built-in engine. Other keywords keep working.
Resetting settings returns to built-in search while retaining custom entries.
Editing preserves the entry's identity and default status. Deletion requires
confirmation; validation errors preserve the draft.

The token must occur exactly once after the URL authority, in a path, query or
fragment. Hostnames, credentials and ports cannot contain it. Other placeholders
and executable/local-file schemes are rejected. Limits: 32 entries, 120 UTF-8
bytes per name, a 1–16 character keyword starting with a letter and using only
`a-z`, `0-9`, `_`, `-`, 4096-byte URL templates and 16 KiB custom search queries.
Entries are stored per profile in profile.toml and shared between its windows.
Private tabs can use them without adding searches to Aura history. Searches are
sent only on navigation; editing provider settings does not contact their servers.
