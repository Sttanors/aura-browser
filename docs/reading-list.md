# Прочитать позже / Read later

Откройте **Закладки → Прочитать позже** или найдите **Прочитать позже** через
палитру команд (`Ctrl+K`). На загруженной статье HTTP/HTTPS нажмите
**Сохранить статью офлайн**. Та же команда доступна в палитре.
Aura извлекает заголовок и текст при помощи локального Readability.
Сохранение не отправляет статью стороннему сервису.

**Читать офлайн** открывает новую вкладку с локальной текстовой копией.
Она доступна после перезапуска и без подключения к исходному сайту.
Изображения, видео, скрипты и исходная разметка не сохраняются. CSP запрещает
загрузку ресурсов, скрипты и отправку форм; весь текст экранируется.
Это библиотека статей, а не архив страниц с интерактивными элементами.
Размер и шрифт копии берутся из настроек режима чтения при открытии.

Поиск проверяет заголовок, адрес, автора и весь сохранённый текст, без учёта
регистра английских и русских букв. **Прочитано / Не прочитано** переключается
вручную. Повторное сохранение точно того же URL обновляет текст и дату,
сохраняя отметку о прочтении. Удаление запрашивает подтверждение.
Приватные страницы не сохраняются; чтение существующей копии не меняет её статус.

Библиотека общая для окон одного профиля. Данные записываются атомарно в
`reading-list.toml` внутри каталога профиля; HTML-копии создаются в
`reading-copies/` при открытии. Обычный перенос настроек не включает эти файлы:
для ручного резервного копирования сохраните `reading-list.toml` отдельно.
Данные хранятся локально без дополнительного шифрования, как закладки/история.
Повреждённая или неизвестная версия файла сохраняется без перезаписи;
ошибка записи не меняет библиотеку в памяти.

Лимиты: 100 статей, 512 КиБ текста и метаданных на статью, 8 МиБ суммарно,
до 2000 текстовых блоков. Слишком большая статья отклоняется целиком,
без незаметного обрезания. При смене страницы во время извлечения результат
не сохраняется. Уже открытый текст остаётся в документе вкладки после удаления
статьи из библиотеки; закройте эту вкладку, чтобы убрать его с экрана.

# English

Open **Bookmarks → Read later**, or search for **Read later** in the command
palette (`Ctrl+K`). On a loaded HTTP/HTTPS article, choose **Save article offline**.
Extraction uses local Readability; no translation or archiving service receives
the article. **Open offline** opens a new tab with the saved text and works after
restart without the original website. Images, scripts, video and original markup
are excluded. Text is escaped and a restrictive CSP blocks scripts, resource
requests and forms. Reader font/size preferences apply when opening the copy.

Search covers the title, source URL, author and full text, with English/Russian
case folding. Read state is manual. Saving the same exact URL updates its text
and date while preserving read state. Deletion requires confirmation. Private
pages cannot be saved; opening a copy does not change its read status.

The library is shared between windows of one profile and stored locally,
unencrypted, in `reading-list.toml`. Opening creates HTML in `reading-copies/`.
Back up `reading-list.toml` separately: the settings transfer does not include it.
Malformed/unsupported libraries are preserved without overwriting; failed writes
leave the in-memory library unchanged. Limits: 100 articles, 512 KiB per article,
8 MiB total, 2000 text blocks. Oversized articles are rejected rather than
silently truncated. Navigation during extraction cancels the save. Deleting an
article removes its stored copy; already open documents remain until closed.
