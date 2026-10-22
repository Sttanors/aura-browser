# Tab organization / Организация вкладок

These additions are in the source branch after v0.3.0-preview.1. The published
installers for that release do not contain them. See [progress](features-progress.md)
for completed checks and remaining platforms.

## English

**Workspaces — Ctrl+Shift+E.** Create, rename or switch a workspace through the Aura
menu. Each workspace remembers its selected tab. To move a tab, open its context
menu, choose **Move to workspace**, then **Move here** in the target row. Removing a
workspace moves its tabs into the default workspace without closing documents.
Workspaces share website cookies; they provide organization rather than separate
site identities.

**Groups — Ctrl+Alt+G.** Name a group and pick one of six colors. New tabs inherit
the current group. Click a sidebar heading to collapse/expand it; the plus button
creates a tab in that group. Drag a tab onto a group heading to assign it. The
manager can rename, recolor, assign or ungroup a tab. Removing a group leaves its
pages open. Ctrl+Tab and Ctrl+1…9 follow the visual order and reveal a collapsed
group when selecting one of its tabs. Collapsing a group does not pause pages.

Workspace/group membership and collapsed state survive normal session restore.
Private tab addresses are excluded from saved sessions and backups. Workspace and
group catalogs are unavailable for editing in private windows.

**Sleeping tabs — Ctrl+Alt+S.** The tab context menu can pause a website after
switching away from it. Selecting it resumes the same document and unsaved forms.
Settings → Tabs offers automatic sleep after 5, 15, 30 or 60 minutes (off by default)
and domain exceptions. Pinned/private/media/capture/DevTools/download activity is
protected. Inaccessible child frames conservatively keep a page awake. Sleep stops
page timers and releases native image buffers; the DOM stays in renderer memory.

**Two websites side by side — Ctrl+Alt+V.** Open two sites in the current workspace,
then use the shortcut or Aura menu. Click a pane to make its navigation, find and
keyboard focus active. Drag the divider to resize; each pane keeps at least 220
logical pixels. The close control exits split while keeping the documents open.
Normal pairs and the preferred width are restored with the session. Private pairs
are excluded. Opening a start tab, switching workspaces or closing a pane ends split.

**Site containers — Ctrl+Alt+C.** Each named container has its own persistent
cookies, localStorage, IndexedDB and cache. Tabs, links and popups inherit the
container. The manager opens a new tab or a copy of the current URL in another
container; the original stays open. Private tabs cannot be copied into one.
History, bookmarks, settings and the password vault are shared. Extension popups
are available in the default container. Close the container's tabs in every window
before removing it; its website data is deleted on the next Aura launch. Unknown
container IDs are never restored under the default identity.

## Русский

**Пространства — Ctrl+Shift+E.** Создавайте, переименовывайте и переключайте их в
меню Aura. Каждое пространство запоминает выбранную вкладку. Для переноса откройте
контекстное меню вкладки → **Переместить в пространство** → **Перенести** в нужной
строке. Удаление пространства переносит вкладки в основное без закрытия документов.
Cookies сайтов общие: пространства предназначены для организации задач.

**Группы — Ctrl+Alt+G.** Задайте имя и один из шести цветов. Новые вкладки наследуют
текущую группу. Нажатие на заголовок в sidebar сворачивает/разворачивает группу;
плюс создаёт вкладку внутри неё. Перетащите вкладку на заголовок, чтобы добавить её
в группу. Менеджер позволяет менять имя и цвет, добавлять вкладки или снимать
группировку. Удаление группы сохраняет открытые страницы. Ctrl+Tab и Ctrl+1…9
следуют визуальному порядку и раскрывают группу выбранной вкладки. Сворачивание
группы не останавливает страницы.

При обычном восстановлении сессии сохраняются пространства, группы и сворачивание.
Адреса приватных вкладок не попадают в сессии и резервные копии. В приватных окнах
редактирование каталогов пространств и групп отключено.

**Спящие вкладки — Ctrl+Alt+S.** Команда также есть в контекстном меню. Выбор
вкладки пробуждает прежний документ и заполненные формы. Настройки → Вкладки:
автосон через 5, 15, 30 или 60 минут (по умолчанию выключен), домены-исключения.
Закреплённые, приватные, media/capture/DevTools/download вкладки защищены.
Недоступные дочерние frame консервативно сохраняют страницу активной. Сон
останавливает таймеры и освобождает native bitmap; DOM остаётся в памяти renderer.

**Два сайта рядом — Ctrl+Alt+V.** Откройте два сайта в пространстве и используйте
команду или меню Aura. Нажатие на сторону выбирает её для навигации, поиска и
ввода. Разделитель меняет ширину; каждая сторона получает минимум 220 logical px.
Крестик завершает разделение, сохраняя документы открытыми. Обычные пары и ширина
восстанавливаются с сессией; пары с приватной вкладкой не записываются.
Начальная вкладка, смена пространства или закрытие стороны завершают разделение.

**Контейнеры сайтов — Ctrl+Alt+C.** У каждого отдельные постоянные cookies,
localStorage, IndexedDB и cache. Вкладки, ссылки и popup наследуют контейнер.
Менеджер открывает новую вкладку или копию текущего URL в другом контейнере;
исходник остаётся открытым. Приватную вкладку скопировать в него нельзя.
История, закладки, настройки и vault общие. Окна расширений доступны в default
контейнере. Перед удалением закройте его вкладки во всех окнах; данные сайтов
очистятся при следующем запуске. Неизвестный id не заменяется default identity.
