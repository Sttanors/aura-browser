# Древовидные вкладки / Tree tabs

## Русский

В «Настройки → Вкладки → Древовидные вкладки» можно включить дерево или вернуть
плоский список. По умолчанию дерево включено. Вкладка, открытая ссылкой или popup
со страницы, становится её дочерней. Обычная новая вкладка — самостоятельный корень.
Команда «Новая дочерняя вкладка» создаёт новую стартовую страницу внутри ветки.

- Нажмите стрелку слева от вкладки, чтобы свернуть/раскрыть её потомков. Страницы
  продолжают работать; формы, история навигации и содержимое не перезагружаются.
- F10 переводит фокус в интерфейс, Tab выбирает стрелку, Enter/Space переключает её.
  В Ctrl+K доступны «Свернуть ветку», «Развернуть ветку», «Отделить вкладку» и
  «Новая дочерняя вкладка»; те же команды есть в контекстном меню вкладки.
- Ctrl+Tab и Ctrl+1…9 используют порядок дерева, включая скрытые вкладки. При
  выборе скрытой вкладки через клавиатуру или Ctrl+K её предки раскрываются.
  Сворачивание активной ветки сохраняет текущую страницу и выделяет видимого предка.
- Перетаскивание строки переносит её ветку рядом с целью, на тот же уровень.
  Перенос на потомка игнорируется. Перенос на заголовок группы перемещает ветку
  целиком. Контейнер и приватность страниц сохраняются; несовместимая связь с
  новым родителем отбрасывается. Изменение группы/пространства через меню относится
  к одной вкладке: она отделяется, её дети остаются в прежнем контексте.
- Закрытие родителя закрывает только его страницу. После принятого beforeunload
  дети переходят к его родителю или в корень. При отказе сохраняются и страница,
  и прежние связи. «Отделить вкладку» поднимает её детей аналогично и делает саму
  вкладку корнем. Закреплённые вкладки сохраняют обычную защиту от закрытия кнопкой.

Связи ограничены окном, пространством, группой, контейнером и режимом приватности.
Обычные деревья и сворачивание сохраняются при перезапуске и в снимках сессий;
приватные деревья существуют только в памяти. Плоский режим сохраняет структуру,
но показывает все строки. Повторное открытие закрытой вкладки восстанавливает её
как корень; снимки сессий сохраняют структуру, но не содержимое форм. Визуальный
отступ ограничен тремя ступенями; у более глубоких вкладок указан уровень,
реальная глубина дерева сохраняется.

## English

Enable **Settings → Tabs → Tree tabs** (on by default), or turn it off for a flat
list. Links and popups opened from a page become its child tabs. A regular new tab
is a root; **New child tab** creates a start page inside the current branch.

- Use the disclosure arrow to collapse/expand descendants. Pages stay alive;
  forms, navigation history and documents are not reloaded.
- Press F10 to focus the interface, Tab to select a disclosure, then Enter/Space.
  **Collapse branch**, **Expand branch**, **Detach tab** and **New child tab** are
  also available in Ctrl+K and the tab context menu.
- Ctrl+Tab and Ctrl+1…9 follow tree order, including hidden tabs. Selecting a
  hidden tab reveals its ancestors. Collapsing the active branch leaves its page
  active and highlights its visible ancestor.
- Dragging moves a whole branch beside the target at the same level. Dropping
  onto a descendant is ignored; dropping onto a group header moves the branch.
  Containers and privacy stay unchanged; incompatible parent links are removed.
  Moving one tab through the workspace/group menu detaches it and promotes its
  children inside their original context.
- Closing a parent closes only that page and promotes its children once the close
  succeeds. Declining beforeunload keeps both the page and its original tree.
  **Detach tab** promotes children similarly and makes the selected tab a root.
  Pinning retains the usual protection against closing with the sidebar button.

Relationships stay within one window, workspace, group, container and privacy
mode. Normal trees and collapse state persist across restarts and session
snapshots. Private trees stay in memory. Flat mode retains relationships but shows
all tabs. Reopened closed tabs return as roots. Snapshots preserve structure, not
unsaved forms. Visual indentation stops after three steps; deeper tabs show their level while
the full logical depth remains intact.
