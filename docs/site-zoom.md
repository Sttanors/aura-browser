# Remembered page zoom / Масштаб сайта

## Русский

В исходниках после `0.4.0-preview.2` доступна настройка **Веб-страницы → Запоминать
масштаб для каждого сайта**. Она включена по умолчанию. Измените масштаб страницы
через `Ctrl +` / `Ctrl −`: новые вкладки этого хоста и повторный запуск используют
выбранный размер. Открытые вкладки того же хоста и контейнера обновляются вместе.

`Ctrl 0` удаляет сохранённое значение текущего сайта и возвращает масштаб из
настройки «Масштаб страницы по умолчанию». Сброс работает и при выключенном
запоминании. Выключение настройки само по себе сохраняет старые правила, но не
применяет их; повторное включение применяет оставшиеся правила сразу.

Контейнеры имеют отдельные значения. Приватные вкладки не наследуют обычные
значения, хранят свои только в памяти и забывают их при завершении Aura.
Схема HTTP/HTTPS, порт и путь не разделяют хост; `localhost` и `127.0.0.1` — разные
хосты. Начальная страница и масштаб интерфейса настраиваются отдельно.

Хранится до 1000 правил. При достижении лимита новый масштаб всё равно применяется
к текущей странице, но сообщение предупреждает, что он не сохранён.

## English

Source builds after `0.4.0-preview.2` provide **Webpages → Remember zoom for each
site**, enabled by default. Adjust a page with `Ctrl +` / `Ctrl −`; the chosen
level applies to new tabs, matching open pages and later launches.

`Ctrl 0` removes the site's override and restores the configured default page zoom,
even when remembering is disabled. Disabling the preference retains existing
rules without applying them; enabling it applies them immediately.

Containers keep separate levels. Private tabs use a separate memory-only set,
cleared when Aura exits. Keys are hosts across schemes, ports and paths;
`localhost` and `127.0.0.1` are distinct. Start-page and interface zoom remain
separate controls. At the 1000-rule limit, zoom still works for the current page
and a message explains that its new level was not saved.
