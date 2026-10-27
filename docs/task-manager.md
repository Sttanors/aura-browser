# Диспетчер задач / Task manager

Новая функция в исходниках после v0.4.0-preview.2. Публичные установщики этого
этапа пока не опубликованы.

Открытие: **Shift+Esc**, меню Aura → «Диспетчер задач» или Ctrl+K → поиск
«Диспетчер задач». Сочетание можно изменить в настройках горячих клавиш.

Панель показывает процесс браузера и основные процессы вкладок всех обычных окон
профиля. Вкладки одного renderer собраны в одну строку: CPU и память этого процесса
не складываются повторно. CPU 100% соответствует одному полностью занятому ядру;
на нескольких ядрах значение может превышать 100%. Память — footprint, сообщённый
CEF. Обновление — примерно раз в секунду; неизвестные значения отмечаются как
недоступные. Сбор метрик прекращается при закрытии последней панели диспетчера.

Названия приватных вкладок других окон скрыты. В приватном окне показываются его
собственные приватные вкладки и процесс браузера. Если процесс включает скрытые
вкладки, завершать его из этой панели нельзя.

«Завершить» сначала показывает число затронутых вкладок и до пяти названий. Подтверждение
останавливает весь renderer, включая связанные вкладки и зависание во время
загрузки; несохранённые изменения теряются. Если состав процессов/вкладок или
навигация изменились, Aura отменяет действие и предлагает проверить список снова.
Для восстановления страницы используйте перезагрузку. Сам процесс браузера и
неопределённые/защищённые процессы из этой панели завершить нельзя.

Отдельные фоновые iframe/worker/system tasks без надёжной связи с процессом не
выводятся отдельными строками: их метрики могут дублировать процесс вкладки.
Нативные PID не показываются. Для группировки используется child ID Chromium из
[формата frame identifier закреплённого CEF](https://github.com/chromiumembedded/cef/blob/426920c/libcef/common/frame_util.cc).
Изменение формата при обновлении движка проверяется unit- и GUI-регрессиями;
неизвестный идентификатор не даёт возможности завершения. Завершение выполняет
CefTaskManager по проверенному Task ID; прямого OS kill нет.

---

Open with **Shift+Esc**, the Aura menu, or Ctrl+K → **Task manager**. The shortcut
can be reassigned in Keyboard shortcuts.

The panel lists the browser process and main tab processes across the profile's
normal windows. Tabs sharing a renderer appear together; CPU and memory are
reported once for that process. CPU 100% means one fully occupied core and can
exceed 100% on multiple cores. Memory is CEF's reported footprint. Values refresh
about once a second; unavailable samples are labeled. Sampling stops when the
last task-manager panel closes.

Private titles from other windows are hidden. A private window lists its own
private tabs and the browser process. Processes containing hidden tabs cannot be
ended from that panel.

**End process** asks for confirmation and lists affected tabs. It stops the entire
renderer, including related tabs and pages hung during initial loading. Unsaved
changes are lost. Changes to the process/tab list or navigation invalidate the
confirmation. Reload an affected tab to restore it. The browser process and
unidentified/protected processes cannot be ended here.

Unmapped iframe, worker and system tasks are omitted rather than duplicating
shared process metrics. Native OS PIDs are not displayed. Grouping uses Chromium's
child ID from the pinned CEF frame identifier; unknown formats disable process
termination. CEF terminates the validated task; Aura does not pass an identifier
to an OS kill API.
