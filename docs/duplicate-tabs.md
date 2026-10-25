# Дубликаты вкладок

Найдите **Дубликаты вкладок** в палитре `Ctrl+K` или откройте экран групп вкладок
и нажмите **Дубликаты вкладок** в его заголовке. Обзор показывает одинаковые
полные URL HTTP(S)/file во всех обычных окнах и пространствах текущего профиля.
Адреса с разными параметрами или `#фрагментами` остаются разными. Контейнеры не
смешиваются. Новые/пустые вкладки и служебные страницы расширений не включаются.

Нажмите **Просмотреть дубликаты**. Выберите **Оставить эту вкладку** у нужной
копии; по умолчанию предлагается закреплённая, затем текущая, затем первая.
**Показать вкладку** открывает её пространство/группу и активирует нужное окно.
Число на кнопке **Закрыть лишние** — сколько вкладок сейчас допускается закрыть.
После подтверждения Aura закрывает их по одной обычным механизмом CEF.
Состояние оставляемой вкладки не объединяется с историями/формами других копий.

Закреплённые вкладки, загрузка страницы/подготовка операции, захват медиа,
работающий Web Audio, активные скачивания и открытые диалоги защищены.
Перед каждым закрытием дополнительно проверяются HTML5 аудио/видео, PiP и
вложенные фреймы. При недоступном фрейме, тайм-ауте или изменении страницы
кандидат сохраняется. Поиск обновляется автоматически, но медиа-проверка
выполняется только при попытке закрытия. Вкладка с активным медиа может сначала
попасть в число кандидатов и затем остаться открытой после проверки.

Настоящие запросы `beforeunload` сохраняются и показываются в окне страницы.
Отказ прекращает всю оставшуюся очередь. **Остановить очередь** не начинает
новые закрытия и не отвечает за пользователя на уже открытый запрос страницы.
Закрытие панели не отменяет очередь — для этого есть кнопка остановки.
Уже начатое закрытие может закончиться отдельно. Если сохраняемая вкладка
закрылась, сменила страницу/пространство или контекст, оставшаяся очередь
прекращается. Изменение состава группы между обзором и подтверждением требует
повторного просмотра; новые дубликаты не включаются в уже подтверждённый пакет.

Последняя лишняя вкладка другого окна не закрывает само окно: вместо неё
создаётся новая вкладка согласно настройкам. Обычные закрытые копии попадают в
стандартный список недавно закрытых (`Ctrl+Shift+T` восстанавливает адрес, но не
гарантирует прежнее состояние формы). Приватные копии туда не записываются.
Обычное окно видит собственные приватные копии отдельными группами; приватное
окно видит только свои копии. Чужие приватные окна не раскрываются. Обзор и
очередь находятся только в памяти, без отдельного сохранения адресов на диск.

# Duplicate tabs

Find **Duplicate tabs** in the command palette (`Ctrl+K`) or use the header
button in **Tab groups**. The overview matches complete HTTP(S)/file URLs across
normal windows and workspaces of the current profile. Query strings and fragments
are significant; containers stay separate. New/blank and extension pages are
excluded. Choose **Review duplicates**, select the copy to keep, then confirm
**Close duplicates**. Pinned, current, then first copy is the initial preference.
**Show tab** reveals its workspace/group and presents the destination window.
The kept document is not reloaded or combined with other copies' form/history state.

Pinned or busy tabs, media capture, running Web Audio, active downloads and dialogs
are protected. Before each close, Aura checks HTML5 audio/video, PiP and child
frames in an isolated world. Inaccessible frames, timeouts or changed pages keep
the candidate open. Media checks happen on closure attempts rather than while
browsing the list, so a candidate may subsequently be skipped.

Tabs close serially through CEF's ordinary close path. Real beforeunload prompts
appear in the page's window; declining stops remaining work. **Stop remaining**
does not start further closes or answer an already pending prompt. Closing the panel does not cancel its queue; use the stop button. An in-flight
close may finish separately. Losing/navigating/moving the kept copy stops the
queue. Changed membership before confirmation requires review; newly created
duplicates never join an existing confirmed batch. Closing the last excess tab
preserves its window by opening a new tab according to settings.

Normal closed copies use the existing recently-closed list; reopening restores
their address, not necessarily their form state. Private copies are excluded.
Normal windows see only their own private copies, in separate groups; a private
window sees only its own copies. Other private windows remain hidden. Detection
and cleanup state are in memory and create no separate persistent URL list.
