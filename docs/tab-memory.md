# Tab memory / Память вкладок

## English

In **Settings → Tabs**, choose **Inactive tab action** and **Inactive tab timer**.
Both the timer and site exceptions apply to the selected action. The timer is off
by default; the default action is **Freeze**.

- **Freeze** pauses page JavaScript and releases Aura's page image buffers.
  The document, forms and navigation history stay in memory. Selecting the tab
  resumes it. **Sleep tab** remains available for a manual freeze.
- **Unload** closes the page's Chromium browser/document and releases its native
  buffers and per-document tooling. The tab, URL, container, tree position and
  zoom remain. Selecting it creates a new browser and reloads the URL.
  **The DOM and that tab's back/forward history are lost.** Cookies and the
  profile's browsing history are retained. Shared Chromium processes/caches may
  remain; unloading does not guarantee that all related process memory disappears.

For a manual cleanup, use **Ctrl+K → Unload inactive tabs** and confirm the reload
warning, or right-click an inactive tab and choose **Unload tab**. The active tab
and both visible split panes remain loaded. Right-clicking an inactive tab opens
its menu without first selecting it. An unloaded tab has a distinct icon and
an **Unloaded** sidebar status.

Private/pinned tabs, unsaved form values, editable documents/canvas/web components,
child frames, playing audio/video, Web Audio, media capture/calls, active downloads,
POST responses, reader pages and unverified pages stay loaded. Pages with registered
`beforeunload` listeners are kept. Chromium's ordinary non-forced close remains a
final guard; a late background leave prompt cancels unloading. Site exceptions
also protect matching subdomains. This deliberately conservative policy can leave
some complex sites loaded even when they appear idle.

A safety check returns only a busy flag; it does not save or log field values.
A tab switch/navigation cancels a pending check. Aura does not save the DOM to
 disk, submit forms, replay a POST or clear a site's storage to unload it.

## Русский

В **Настройки → Вкладки** выберите **Действие для неактивных вкладок** и
**Таймер неактивных вкладок**. Таймер и исключения сайтов применяются к выбранному
действию. По умолчанию таймер выключен, действие — **Заморозка**.

- **Заморозка** останавливает JavaScript и освобождает буферы изображения Aura.
  Документ, формы и история переходов остаются в памяти. При выборе вкладки
  страница продолжает работу. Команда **Усыпить вкладку** сохраняет ручную заморозку.
- **Выгрузка** закрывает browser/document Chromium и освобождает его буферы и
  инструменты документа. Вкладка, URL, контейнер, место в дереве и масштаб остаются.
  Выбор вкладки создаёт новый browser и загружает URL заново.
  **DOM и история переходов назад/вперёд этой вкладки теряются.** Cookies и общая
  история профиля сохраняются. Общие процессы/cache Chromium могут оставаться;
  выгрузка не гарантирует освобождение всей связанной памяти процесса.

Для ручной очистки используйте **Ctrl+K → Выгрузить неактивные вкладки** и
подтвердите предупреждение, либо правый клик по фоновой вкладке → **Выгрузить вкладку**.
Активная вкладка и обе видимые split-панели остаются загруженными. Правый клик
открывает меню без предварительной активации вкладки. Выгруженная вкладка имеет
отдельную иконку и статус **Выгружена** в боковой панели.

Защищены private/pinned tabs, изменённые формы, редактируемые документы/canvas/
web components, child frames, воспроизведение audio/video, Web Audio, звонки и
захват устройств, текущие загрузки, POST, reader и непроверяемые страницы.
Зарегистрированные обработчики `beforeunload` также защищают страницу. Обычное
закрытие Chromium без force остаётся последней проверкой; поздний фоновый запрос
о выходе отменяет выгрузку. Исключения включают поддомены. Поэтому некоторые
сложные сайты остаются загруженными даже при внешнем отсутствии активности.

Проверка возвращает только busy flag: значения полей не сохраняются и не пишутся
в журнал. Смена вкладки/navigation отменяет незавершённую проверку. Aura не
сохраняет DOM на диск, не отправляет формы, не повторяет POST и не очищает storage
сайта ради выгрузки.
