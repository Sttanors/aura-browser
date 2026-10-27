# Перенос вкладок между окнами / Moving tabs between windows

## Русский

Перетащите вкладку на боковую или верхнюю панель другого окна Aura. Линия вставки
и подпись «Переместить сюда» показывают область приёма. Отпустите кнопку мыши,
чтобы переместить вкладку, либо нажмите Escape для отмены. Окно назначения должно
быть видно; страница и кнопки управления окном не принимают вкладки.

Для клавиатуры откройте **Ctrl+K → Переместить в окно**, выберите окно стрелками
или поиском и нажмите Enter. Та же команда есть в контекстном меню вкладки.
Список показывает только окна того же режима просмотра. Если другого окна нет,
сначала создайте обычное окно через Ctrl+N или приватное через Ctrl+Shift+N.
Недоступное окно остаётся в списке с объяснением причины.

- Перемещается одна живая вкладка: документ, формы, история, cookies, контейнер,
  закрепление, звук и параметры страницы сохраняются. Сама операция не закрывает
  страницу и не вызывает beforeunload. При последующем закрытии обычная защита
  несохранённых данных продолжает работать.
- Обычные вкладки перемещаются только в обычные окна, приватные — только в
  приватные. Перенос между разными профилями не поддерживается. Приватные URL
  не добавляются в сохранённую сессию или резервную копию.
- Вкладка входит в текущее пространство окна назначения. При вставке перед
  вкладкой используется её группа; при выборе окна через меню совместимая
  прежняя группа сохраняется. В дереве переносится одна вкладка: её дети остаются
  в исходном окне и поднимаются к прежнему предку. В новом окне она становится
  корневой. При наведении на дочернюю вкладку линия вставки указывает границу ветки.
- Перенос последней вкладки оставляет стартовую страницу в исходном окне.
  Перетаскивание на рабочий стол для создания нового окна пока не поддерживается.
- Диалоги, незавершённые операции страницы, DevTools, снимок страницы, расчёт
  контрольной суммы и закрытие дубликатов могут временно блокировать перенос.
  Закончите операцию и повторите перенос. Уже начавшееся скачивание передаётся
  новому окну вместе с вкладкой.

## English

Drag a tab onto the sidebar or top strip of another visible Aura window. An
insertion line and **Move tab here** mark the destination. Release to move, or
press Escape to cancel. Pages and window-control buttons do not accept tab drops.

For keyboard access, use **Ctrl+K → Move to window**, select a window with arrows
or search, and press Enter. The tab context menu offers the same command. The list
shows windows of the same browsing mode. Create another normal window with Ctrl+N
or private window with Ctrl+Shift+N if necessary. Unavailable entries explain why.

- One live tab moves with its document, forms, history, cookies, container,
  pinned/muted state and page settings. Moving does not close or reload the page
  or invoke beforeunload. Subsequent closure retains the normal unsaved-data prompt.
- Normal tabs move only into normal windows; private tabs only into private
  windows. Transfers between profiles are unsupported. Private URLs stay out of
  saved sessions and backups.
- The destination's current workspace is used. Inserting before a tab adopts its
  group; the window chooser retains a compatible existing group. Children remain
  in the source window and are promoted to the former ancestor. The moved tab
  becomes a root; hovering a child targets the branch boundary.
- Moving the last tab leaves a new-tab page behind. Dropping onto the desktop to
  create a window is not supported yet.
- Dialogs, pending page operations, DevTools, screenshots, checksum calculation
  and duplicate cleanup can temporarily prevent transfer. Finish the operation
  and retry. An already-started download moves with its source tab.

## Implementation notes

CEF's OSR parent handle cannot be changed through the pinned host API. Aura keeps
that native parent alive until its transferred pages close, even after the former
controller retires. Retired-window callbacks are cleared. Rendering, input, screen
coordinates, IME, CEF callbacks and DevTools events use the destination controller;
no replacement browser or navigation is used. A retained hidden parent can keep
its native window buffers allocated until the last dependent page closes.
