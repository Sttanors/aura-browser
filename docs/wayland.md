# Native Wayland development backend / Нативный Wayland

## English

This backend is under development in PR #1. X11/XWayland remains the default;
public v0.4.0-preview.2 installers do not contain it. Explicit opt-in:

```sh
aura-browser --window-system=wayland
```

When launching the development binary directly, supply your usual `--data-dir`.
`--window-system=x11` selects the established backend. Explicit `auto` selects
Wayland when WAYLAND_DISPLAY is present, otherwise X11. The choice is retained
through in-app restarts. A failed explicit Wayland start reports an error instead
of silently switching to X11.

The SDL 3.4.18 Wayland protocol implementation is built statically from the
SHA-256-pinned source. Aura supplies its own Skia raster/Wayland SHM presenter,
with three release-tracked buffers and compositor frame callbacks. SDL's GPU,
OpenGL, Vulkan, KMS and X11 video paths are disabled in this dependency. This
backend always uses CPU rendering, including when --gpu was supplied. The existing
X11 implementation remains separate behind the LinuxWindow interface.

Keyboard/pointer events, native window controls, output/UI scaling and text input
use the Wayland backend. Clipboard transfers use wl_data_device on the focused
client's connection, with nonblocking pipes, a 16 MiB cap and a five-second
transfer deadline. Oversized incoming streams are drained without retaining their
payload until EOF/deadline, then refused. A checked patch disables SDL's duplicate
clipboard owner and guards its keyboard-leave path when that owner is absent.
CEF page paste uses native text insertion. Copy/cut obtain
Chromium's context-menu edit permissions before exposing selected text; cached
selection text alone is insufficient for password-field protection.

Wayland does not expose global window positions. OSR screen-point callbacks do
not fabricate them; live tab transfers remain available through the existing
window picker.

Page and extension-popup copy/cut/paste shortcuts and native context menus use
the same clipboard bridge. Password copy/cut items are disabled. A pending paste
checks the focused frame, document/navigation and active tab/popup again before
inserting; closing the popup or switching tabs discards the old result.
Cross-window drag gestures, complete IME/preedit coverage, checks on additional
compositors, system file choosers and fractional/multi-monitor setups remain work
in progress. This is why item 14 stays open and automatic selection is opt-in.

The native validation harness `python3 scripts/verify_wayland.py` launches a
private Weston 15 Pixman compositor with no DISPLAY/XWayland. A test-only module
injects input through Weston and captures the real surface. It is never loaded
into the user's compositor and is not part of packaged browser binaries. Native
GUI checks passed at compositor scale 1 and 2 with an independent SDL clipboard
peer, including Unicode/multiline transfer, password copy refusal, a 17 MiB
refusal without losing form contents, focus/tab preservation and CEF shutdown.
The same build passed 170 CTest checks and the existing X11 browser, windows,
clipboard, safe-mode and extension-access scenarios. These checks do not establish
full IME or compatibility with every compositor.

## Русский

Backend находится в разработке в PR #1. По умолчанию используется X11/XWayland;
в публичных установщиках v0.4.0-preview.2 нового backend нет. Для явного запуска:

```sh
aura-browser --window-system=wayland
```

При прямом запуске dev-бинарника укажите привычный `--data-dir`.
`--window-system=x11` выбирает старую реализацию, `auto` — Wayland при наличии
WAYLAND_DISPLAY, иначе X11. Выбор сохраняется при перезапуске из интерфейса.
Неудача явного запуска Wayland не скрывается переключением на X11.

SDL 3.4.18 собран статически из исходника с закреплённым SHA-256. Вывод кадров
реализован в Aura через Skia и Wayland SHM: три буфера с ожиданием release и
compositor frame callbacks. GPU/OpenGL/Vulkan/KMS/X11 в этой сборке SDL отключены;
backend работает на CPU, даже с --gpu. Окна X11 остаются отдельной реализацией.

Добавлены нативные окна, события клавиатуры/мыши, кнопки заголовка,
масштабирование и обработка текста. Clipboard использует стандартный
wl_data_device, неблокирующие pipe, лимит 16 МиБ и срок передачи пять секунд.
Превышающий лимит поток дочитывается без сохранения до EOF/deadline, затем
отклоняется. Проверяемый патч SDL отключает второго владельца clipboard и
проверяет null data_device при потере фокуса.
Вставка в CEF выполняется нативно. Перед копированием/вырезанием запрашиваются
разрешения редактирования Chromium: одного кэшированного текста выделения
недостаточно для защиты парольных полей.

Клавиши и контекстное меню Copy/Cut/Paste страниц и попапов расширений используют
общий native clipboard bridge. Копирование/вырезание паролей отключено. После
асинхронного чтения повторно проверяются focus frame, документ/navigation и
активный получатель: закрытие попапа или смена вкладки отбрасывают старую вставку.

Глобальные координаты окон не выдумываются. Перенос живой вкладки остаётся
доступен через выбор окна. Для завершения пункта 14 ещё нужны native DND между
окнами, полная проверка IME/preedit, системные chooser, проверки других compositor
и fractional/multi-monitor сценариев. Поэтому пункт остаётся открытым, а Wayland требует
явного включения.

Проверка `python3 scripts/verify_wayland.py` создаёт отдельный Weston 15 Pixman
без DISPLAY/XWayland. Тестовый модуль подаёт ввод через compositor и снимает
реальную поверхность. В пользовательскую сессию модуль не загружается и в
пакеты браузера не входит. Нативные GUI-проверки прошли при compositor scale 1
и 2 с независимым SDL clipboard peer: Unicode/многострочный текст, защита пароля,
отказ 17 МиБ без потери формы, сохранение формы после смены фокуса/вкладки и
CEF shutdown. На той же сборке прошли 170 CTest и X11 browser, windows, clipboard,
safe-mode и extension-access. Полный IME и все compositor этими тестами не покрыты.

## Implementation references

- [SDL 3.4.18 source/release](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18).
- [SDL native window properties](https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties).
- [Wayland client event dispatch](https://wayland.freedesktop.org/docs/html/apb.html).
- CEF's pinned `cef_render_handler.h` and `cef_context_menu_handler.h` define the
  native selection, IME and edit-permission callbacks used here.
