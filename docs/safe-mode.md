# Safe diagnostic mode / Безопасный режим диагностики

## English

Available in Aura 0.5.0-preview.1.

Open **Settings → General → Diagnostics**, or search `diagnostics` in **Ctrl+K**.
Choose **Restart in safe mode**. The confirmation restores regular tabs and closes
private tabs; pages with unsaved work can cancel through their normal beforeunload
prompt. Choose **Restart in normal mode** in the same panel to restore normal
operation. Saved preferences and enabled/pinned extension choices remain unchanged.
Closing safe mode and launching Aura normally also returns to normal operation.
A normal shortcut while safe Aura is already running opens another safe window;
all windows of that process share the mode.

Safe mode temporarily pauses:

- Installed and command-line extensions, their popups and site-access controls.
- Aura page injections: Dark Reader, cosmetic filtering, custom spelling word
  injection, password form helpers, reader/offline article extraction and page
  capture tools. Native Chromium spelling and website JavaScript follow the
  profile's ordinary preferences.
- Network ad filtering, automatic tab sleeping and automatic application/filter
  update checks.
- GPU rendering, even if `--gpu` was also supplied.

This is a troubleshooting mode. It uses your existing profile, including cookies,
history and saved settings. Its purpose is to isolate add-on, script and rendering
problems; use private browsing when you want a private session.

**Save diagnostics report** opens the system save dialog. The JSON includes Aura
version/platform, current mode, renderer type, window/tab/browser lifecycle counts,
renderer crash counts and up to 128 fixed event codes from this launch. It never
reads old log files or includes website addresses, titles, page text, credentials,
profile paths or arbitrary console/error messages. Nothing is uploaded.

By default `profile/diagnostics/events.log` contains only fixed event codes, with
rotation at 1 MiB and two backups. Chromium file logging is disabled. Existing
`aura.log` and `cef.log` files are retained and excluded from exports.

Command-line usage (supply your normal profile path if launching the binary directly):

```sh
aura-browser --safe-mode
aura-browser --safe-mode --diagnostic-report=/path/to/aura-diagnostics.json
```

`--diagnostic-verbose` explicitly enables raw application and Chromium file logs
in `profile/diagnostics/verbose.log` and `cef-verbose.log`. These can contain visited
addresses and other private data; review them before sharing. The sanitized JSON
still excludes their contents. The existing developer flag `--report` produces a
separate detailed test report that can include URLs/titles/profile paths; it is
not the shareable diagnostic report.

## Русский

Функция доступна в Aura 0.5.0-preview.1.

Откройте **Настройки → Основные → Диагностика** или найдите `diagnostics` через
**Ctrl+K**. Нажмите **Перезапустить в безопасном режиме**. Обычные вкладки откроются
заново, приватные закроются. Страница с несохранённой работой может отменить
перезапуск своим обычным диалогом beforeunload. Для возврата используйте
**Перезапустить в обычном режиме** в той же панели или закройте Aura и запустите
её обычным способом. Настройки, включённые расширения и закреплённые значки
сохраняются. Пока безопасный процесс работает, обычный ярлык открывает ещё одно
его безопасное окно.

На время отключаются расширения, внедрения Aura (Dark Reader, косметические
фильтры, внедрение пользовательского словаря, помощники форм паролей,
режим чтения/сохранение статьи, инструменты снимков), сетевой блокировщик,
автоматический сон вкладок, автоматические проверки обновлений и GPU.
JavaScript самих сайтов и штатная орфография Chromium следуют настройкам профиля.
Используется существующий профиль с cookies и историей: режим предназначен для
поиска причин сбоев. Для приватной сессии используйте приватный режим.

**Сохранить отчёт диагностики** открывает системный диалог. JSON содержит версию,
платформу, режим, тип рендеринга, технические счётчики и последние фиксированные
коды событий текущего запуска. В нём нет адресов, заголовков, текста страниц,
логинов, путей профиля, произвольных сообщений ошибок/консоли и старых журналов.
Автоматической отправки нет. Такой же отчёт создаёт `--diagnostic-report=ФАЙЛ`.

По умолчанию `profile/diagnostics/events.log` содержит только коды событий,
ротация — 1 МиБ и две резервные копии. Файловый журнал Chromium отключён.
Старые `aura.log`/`cef.log` сохраняются, но не включаются в отчёт.

Только явный `--diagnostic-verbose` включает полные журналы `verbose.log` и
`cef-verbose.log` в каталоге diagnostics. Они могут содержать личные данные.
Обычный JSON-отчёт и при этом не включает их содержимое. Разработческий `--report`
остаётся подробным тестовым отчётом с адресами и путями, не предназначенным
для публичного обмена.
