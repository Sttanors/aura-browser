# Native Wayland / Нативный Wayland

## English

Aura 0.5.0-preview.1 adds an **experimental, opt-in** Linux Wayland backend.
X11/XWayland remains the default. Start with:

```sh
aura-browser --window-system=wayland
```

When launching the binary directly, supply your usual `--data-dir`.
`--window-system=x11` selects the established backend. Explicit `auto` selects
Wayland when WAYLAND_DISPLAY is present, otherwise X11. The choice survives
in-app restarts. A failed explicit Wayland start reports an error instead of
silently switching to X11.

SDL 3.4.18 is built statically from SHA-256-pinned source. Aura presents Skia
raster frames through Wayland SHM, using three release-tracked buffers and
compositor frame callbacks. SDL's GPU, OpenGL, Vulkan, KMS and X11 video paths
are disabled in this dependency. This backend uses CPU rendering even with
`--gpu`. The existing X11 implementation remains separate.

Native controls, keyboard/pointer input, output/UI scaling, text-input-v3
preedit/commit/cancel, system GTK file choosers and live cross-window tab dragging
are supported. Dragging uses standard wl_data_device and a random per-gesture
capability; it accepts only the live in-process tab and rechecks profile,
privacy and container boundaries. Reordering within a window keeps the existing
interaction. Global window positions are not fabricated.

Clipboard uses wl_data_device on the focused client's connection, nonblocking
pipes, a 16 MiB cap and a five-second deadline. Oversized input is drained without
retaining its payload, then refused. Page and extension-popup shortcuts and
context menus share the bridge. Copy/cut check Chromium's edit permissions,
including password protection; paste rechecks frame, document, navigation and
focused recipient after reading. Closing the popup or switching tabs discards
stale paste results. Native editors show preedit without changing the committed
value, mask secret input and cancel composition when the recipient changes.

Validation uses private CPU-only compositors and synthetic profiles, never the
user's desktop. `python3 scripts/verify_wayland.py --scale=1` (also `--scale=2`)
uses Weston 15 Pixman, real protocol input and surface captures, without DISPLAY
or XWayland. The test-only module and independent clipboard peer are excluded
from browser packages. Coverage includes actual CEF paint, Unicode clipboard,
17 MiB refusal without form loss, popup menus, stale-paste guards, live tab DND
without reload, rejection of a private target, IME, UTF-8 file selection,
Save/Cancel and balanced shutdown. `python3 scripts/verify_wayland_cage.py`
checks Cage/wlroots headless Pixman with two virtual outputs and client scale 1.25.

These checks do **not** establish compatibility with every compositor or input
method. Physical mixed-DPI monitors, hardware GPU rendering and external IME
candidate windows have not been validated. Use X11 if your desktop exposes a
compatibility issue; Wayland remains a preview option.

## Русский

В Aura 0.5.0-preview.1 добавлен **экспериментальный Wayland с явным включением**.
По умолчанию остаётся X11/XWayland:

```sh
aura-browser --window-system=wayland
```

При прямом запуске бинарника укажите привычный `--data-dir`. Аргумент
`--window-system=x11` выбирает прежнюю реализацию, `auto` — Wayland при наличии
WAYLAND_DISPLAY, иначе X11. Выбор сохраняется при перезапуске из интерфейса.
Ошибка явного Wayland-запуска не скрывается переключением на X11.

SDL 3.4.18 собран статически из исходника с закреплённым SHA-256. Aura выводит
Skia raster через Wayland SHM: три буфера с ожиданием release и compositor frame
callbacks. GPU/OpenGL/Vulkan/KMS/X11 в этой сборке SDL отключены; backend работает
на CPU даже с `--gpu`. Окна X11 остаются отдельной реализацией.

Поддерживаются управление окнами, клавиатура/мышь, масштабирование, композиция
text-input-v3, GTK-диалоги файлов и живой перенос вкладок между окнами. DND
использует wl_data_device и случайную capability на каждый жест: принимается
только живая вкладка этого процесса, повторно проверяются границы профиля,
приватности и контейнеров. Перестановка внутри окна сохраняется. Глобальные
координаты окон не выдумываются.

Clipboard использует неблокирующие pipe, лимит 16 МиБ и срок передачи пять секунд.
Превышающий лимит поток дочитывается без сохранения и отклоняется. Клавиши и меню
страниц/попапов расширений используют общий bridge. Copy/Cut проверяют разрешения
Chromium, включая запрет копирования паролей. Paste после чтения повторно проверяет
frame, документ/navigation и фокус: смена вкладки или закрытие попапа отбрасывают
старый результат. Preedit виден без изменения сохранённого текста, скрыт в
секретных полях и сбрасывается при смене получателя.

Проверки работают на отдельных CPU-only compositor с временными профилями.
`python3 scripts/verify_wayland.py --scale=1` (также `--scale=2`) запускает Weston 15
Pixman без DISPLAY/XWayland, подаёт реальные protocol-события и снимает поверхность.
Тестовый модуль и независимый clipboard peer в установщики не входят. Проверяются
CEF paint, Unicode, отказ 17 МиБ без потери формы, меню попапов, защита от устаревшей
вставки, DND без reload, отказ приватного получателя, IME, выбор файла с русским
именем, Save/Cancel и сбалансированное закрытие. `scripts/verify_wayland_cage.py`
проверяет Cage/wlroots headless Pixman с двумя виртуальными outputs и client scale 1.25.

Это не гарантия совместимости со всеми compositor/input method. Физические
мониторы с разным DPI, аппаратный GPU и внешние окна кандидатов IME не проверялись.
При проблемах конкретного рабочего стола используйте X11; Wayland остаётся preview.

## Implementation references

- [SDL 3.4.18 source/release](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.18).
- [SDL native window properties](https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties).
- [Wayland client event dispatch](https://wayland.freedesktop.org/docs/html/apb.html).
- The pinned CEF headers define the OSR, IME and edit-permission interfaces.
