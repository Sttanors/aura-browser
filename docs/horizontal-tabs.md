# Панель вкладок сверху / Horizontal tabs

## Русский

В **Настройки → Вкладки → Расположение вкладок** выберите **Сверху** или **Сбоку**.
Найти настройку можно и через Ctrl+K по словам «Расположение вкладок» либо
`tab_layout`. Смена вида применяется ко всем окнам текущего профиля и сохраняется
после перезапуска. Страницы остаются открытыми: смена вида изменяет размеры области
просмотра, сохраняя документы, формы, навигацию, cookies и контейнеры.

В верхнем режиме боковая панель скрыта и страница занимает всю ширину окна.
Меню с логотипом Aura слева от кнопки «Назад» открывает закладки, историю,
загрузки, настройки, пространства, группы, контейнеры, профили и расширения.
В «Другие инструменты» остаются диспетчер задач, снимки сессий и другие команды.
Сочетания клавиш работают в обоих режимах. Полное название, адрес, контейнер
и группа доступны в подсказке при наведении или фокусе F10/Tab.

- Нажатие выбирает вкладку, крестик закрывает её, «+» создаёт новую. Средняя кнопка
  закрывает вкладку при включённой соответствующей настройке. У закреплённой
  вкладки вместо крестика показан значок закрепления; Ctrl+W остаётся явной командой
  закрытия с обычным beforeunload.
- Правой кнопкой открывается то же контекстное меню, что в боковом режиме.
- При переполнении используйте стрелки или колёсико над верхней панелью. Включённая
  настройка переключения вкладок колёсиком сохраняет своё действие.
  Выбор вкладки через Ctrl+Tab, Ctrl+1…9 или Ctrl+K автоматически показывает её.
- Группы, их сворачивание и дерево сохраняются. Стрелка родителя скрывает или
  показывает дочерние вкладки без выгрузки страниц; заголовок группы сворачивает
  группу, «+» в нём создаёт вкладку в группе. Выбор скрытой страницы раскрывает предков.
- Перетаскивание меняет порядок вкладок/веток внутри текущего окна. Перенос между
  окнами — отдельный следующий пункт. Свободная область перед кнопками окна
  оставлена для перемещения самого окна. Перетаскивание вкладки за пределы
  панели не передаёт странице нажатия мыши.

Скрытые вкладки сохраняют документы и обычные защитные механизмы. Приватные
вкладки отображаются в выбранном виде, но их адреса и дерево не записываются в
сессию или резервную копию. У каждого окна своя позиция прокрутки панели;
профиль сохраняет выбранный вид, а не временную позицию прокрутки.

## English

Choose **Settings → Tabs → Tab bar position → Top / Sidebar**. You can also find
`tab_layout` through Ctrl+K. The preference applies to every window of the current
profile and survives restart. Changing layouts resizes the viewport while retaining
live documents, forms, navigation, cookies and container identities.

Top mode uses the full window width for pages. The Aura logo beside Back opens
bookmarks, history, downloads, settings, workspaces, groups, containers, profiles
and extensions. **More tools** contains the task manager, session snapshots and
other commands. Keyboard shortcuts work in both layouts. Hover or use F10/Tab to see a hint
with the full title, address, container and group.

- Click a tab to select it, its cross to close it, or **+** to create a tab. Middle
  click follows the existing preference. Pinned tabs show a pin instead of a close
  button; Ctrl+W remains an explicit close request and honours beforeunload.
- Right click opens the same context menu as the sidebar.
- Use arrows or the wheel over the strip when it overflows. The existing wheel
  switching preference still applies. Ctrl+Tab, Ctrl+1…9 and Ctrl+K reveal the
  selected tab automatically.
- Group and tree collapse states remain available. Parent disclosures hide/show
  descendants without unloading them; group headers collapse groups and their
  **+** creates a tab in the group. Selecting a hidden tab reveals its ancestors.
- Dragging reorders tabs/branches within the current window. Cross-window dragging
  is a separate planned feature. Blank space before the window buttons is reserved
  for moving the window. A tab drag that leaves the strip does not send mouse
  presses to the page.

Hidden pages keep their documents and normal close protections. Private tabs use
the selected layout, but their URLs and trees stay out of saved sessions/backups.
Strip scrolling is independent per window; the profile remembers the layout rather
than a temporary scroll position.
