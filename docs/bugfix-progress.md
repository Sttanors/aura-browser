# Исправление багов аудита 21 октября

Порядок: исправление → запись → проверка → следующий пункт.

## 1. Повреждённый профиль — 22 октября 2026

При сохранении ротируется parseable `profile.toml.bak`. Повреждённый основной
TOML переносится в уникальный `profile.toml.corrupt[.N]`, загружается резервная
копия или defaults. Повреждённая копия тоже сохраняется. UI сообщает о recovery.
Ошибки файловой системы не маскируются: если исходник нельзя сохранить, запуск
не продолжает перезаписывать его.

Проверка: сборка Linux PASS; новый
`Profile.CorruptSettingsRecoverBackupWithoutLosingOriginalBytes` PASS: язык,
закладка, точные повреждённые bytes, обе повреждённые копии, повторное сохранение.

## 2. Выход и несохранённые формы — 22 октября 2026

Подтверждение выхода Aura больше не трактуется как разрешение обойти
`beforeunload` сайтов. Ctrl+Q, restart и update используют обычное закрытие CEF.
Отмена сбрасывает transaction выхода, freeze сессии и restart/install intent.
Уже открытый beforeunload dialog сохраняется до ответа пользователя.

Проверка: Linux build PASS; расширенный `--suite=windows` PASS на отдельном
Weston Pixman/Xwayland SHM: Ctrl+Q на защищённой странице → prompt → Escape →
тот же живой документ, последующая работа/выход; остальные три запуска suite
проверяют normal/private окна, beforeunload окна, сессии и crash recovery.
Лог: `/tmp/aura-bugfix-windows-2.log`.

## 3. Настоящие popup-контексты — 22 октября 2026

OnBeforePopup разрешает создание Chromium context с отдельным BrowserClient,
но встраивает его в обычную Aura tab. Не выполняется повторная LoadURL и не
подменяется WindowProxy; политика блокировки popup и private context сохраняются.
Учтены pending creation/abort и закрытие до OnAfterCreated.

Проверка: Linux build PASS; новый `--suite=popups` PASS: ненулевой возвращённый
WindowProxy, opener и двусторонний postMessage с проверками source/origin,
повторное использование named window без третьей вкладки, window.close,
normal/private и balanced CEF shutdown. Лог: `/tmp/aura-bugfix-popups.log`.

## 4. Стартовая страница и история — 22 октября 2026

Native start page представлена отдельным `about:blank#aura-newtab` в CEF history,
с отображением `aura://newtab` в оболочке. Старый документ действительно покидается
с beforeunload, состояние UI коммитится на OnAddressChange. Back/Forward больше
не пропускают последний сайт и восстанавливают штатное mute-состояние вкладки.

Проверка: Linux build PASS; новый `--suite=navigation` PASS: A → B → start → Back B
→ Forward start → Back B, IsAudioMuted=false, отмена перехода с protected document
оставляет документ B. Лог: `/tmp/aura-bugfix-navigation.log`.

## 5. Отмена закрытия и недавно закрытые вкладки — 22 октября 2026

В recently_closed добавляется только фактически закрытый tab при удалении после
OnBeforeClose, а не попытка закрытия. Отмена beforeunload снимает remember intent.
Private tabs по-прежнему исключены.

Проверка: Linux build PASS; расширенный `--suite=navigation` PASS: Ctrl+W → Escape
→ Ctrl+Shift+T не создаёт duplicate; после реального Ctrl+W тот же Ctrl+Shift+T
возвращает B. Повторно прошли история/start/audio/beforeunload из пункта 4.
Лог: `/tmp/aura-bugfix-navigation-5.log`.

## 6. Распознавание адресов — 22 октября 2026

Проверяется authority отдельно от path/query: структура hostname, числовой port
1–65535, bracketed IPv6 через существующий OpenSSL IP parser. Single-label host
с портом открывается как intranet HTTP; выражения с операторами идут в поиск.
Unicode домены и знаки в пути не отбрасываются.

Проверка: Linux build PASS; Navigation.* 3/3 PASS, включая новый сценарий IPv6,
intranet, выражения, Unicode, malformed IPv6, invalid ports и пустые labels.

## 7. Большой буфер X11 — 22 октября 2026

Добавлен INCR receive с подтверждением чанков, чтением остатка property,
ограничением 16 MiB и тайм-аутом ожидания. Поддержан пустой INCR size hint xclip;
лимит применяется к фактическим данным, частичный текст не вставляется.
При проверке обнаружен связанный квадратичный elide: заменён binary search по
UTF-8 boundaries; редактор рисует ограниченное окно вокруг caret, не обрезая value.

Проверка: Linux build PASS; новый `--suite=clipboard` PASS: обычный Unicode 25 bytes
и настоящий xclip INCR 2,100,005 bytes → native address editor → copy обратно,
полное byte-for-byte равенство, SHA-256
`da05fd143235cb0779089b5ed8f8d9370baf50b7e01a4cdc9d92a9ee1b99058b`.
Предыдущие неуспешные прогоны выявили пустой INCR header; итоговый успешный лог
`/tmp/aura-bugfix-clipboard-4.log`, результаты в `artifacts/clipboard-smoke`.

## 8. Сосуществование .run и системного пакета — проверка выполняется

Добавлены post-install/upgrade hooks DEB/Arch/RPM. Для обычных локальных аккаунтов
проверяются стандартные пользовательские ярлыки; файловые операции выполняются
после drop uid через runuser. Только managed regular files перемещаются в
уникальный launcher backup, profile и старый runtime не удаляются. Symlink и
unmanaged override сохраняются. Для custom XDG_DATA_HOME helper можно запустить
от пользователя вручную с его окружением.

Новый .run отказывается создавать shadowing installation при наличии системного
launcher/desktop entry. Проверка использует отдельный system-root в тестах.
25/25 Python tests PASS; 88/88 CTest PASS. Собраны отдельные тестовые packages с
новыми hooks из прежнего Debian runtime только для проверки packaging migration;
они не являются новыми опубликованными сборками приложения. Проверка реальной
install/reinstall/remove миграции в Debian/Arch/Fedora ещё выполняется.

Финальные GUI regression suites и Windows CI также должны подтвердить весь набор.
Первый browser regression прошёл реальные пользовательские действия, но обнаружил
пропущенный audit counter `page_opened_tabs` на новом CEF popup path. Счётчик
добавлен после успешного OnAfterCreated; assertions не ослаблялись.
