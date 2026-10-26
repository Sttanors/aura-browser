# Aura 2 — browser for Linux and Windows

<img src="resources/branding/aura.svg" width="96" alt="Aura Browser logo">

**English** | [Русский](README.ru.md)

Aura is a desktop browser built with **C++20, Chromium Embedded Framework (CEF OSR), and Skia**.
The browser interface is drawn natively with Skia; HTML is used for the websites you visit.
Linux uses X11/XWayland by default; an opt-in [native Wayland backend](docs/wayland.md)
is under development. Windows uses Win32. Both versions share the browser and interface code.

**Developed with substantial AI assistance (OpenAI Codex)** for design, implementation,
documentation and automated checks. The project owner directs the product and makes product decisions.
AI assistance does not imply that the software is error-free. Aura is currently a preview.

**Release 0.4.0 Preview 1** includes all 15 additions recorded in the
[feature progress journal](docs/features-progress.md) (Russian): workspaces, groups, containers, profiles,
reader/translation tools, site controls, bookmark/session managers, screenshots and configurable hotkeys.

Development branch: [safe diagnostic mode](docs/safe-mode.md) temporarily pauses add-ons,
page injections, ad filtering and GPU, with a sanitized local report. This addition
is not included in the published installers yet.

## Download and install

Get the files from [GitHub Releases](https://github.com/Sttanors/aura-browser/releases/tag/v0.4.0-preview.2).
SHA-256 checksum files accompany each download. Installers contain the browser runtime. Native Linux package managers can download system-library dependencies.

| Platform | Installer | How to install |
|---|---|---|
| Windows 10/11 x64 | `aura2-windows64-setup.exe` | Open the installer; choose English or Russian |
| Linux x86_64 | `aura2-linux64.run` | Run `bash ~/Downloads/aura2-linux64.run` (adjust the path) |

### Native Linux packages

Native packages install system-wide into `/opt/aura-browser`, with `/usr/bin/aura-browser`
and an applications-menu entry. They use the same per-user profile as the `.run` installer:
`${XDG_DATA_HOME:-~/.local/share}/aura-browser-profile`. Package removal retains that profile.
Close Aura before upgrading.

| Distribution (x86_64) | Install downloaded package | Remove |
|---|---|---|
| Arch Linux / CachyOS (current rolling) | `sudo pacman -U ./aura-browser-0.4.0rc2-1-x86_64.pkg.tar.gz` | `sudo pacman -R aura-browser` |
| Debian 13+ | `sudo apt install './aura-browser_0.4.0-preview.2-1_amd64.deb'` | `sudo apt remove aura-browser` |
| Fedora 44 | `sudo dnf install './aura-browser-0.4.0-preview.2-1.x86_64.rpm'` | `sudo dnf remove aura-browser` |

Native package managers install required system libraries. These packages are built against Debian 13's
ABI, rather than the newer ABI of the original development machine; Debian 12 is not a supported target.
If you previously installed with `.run`, first close Aura and run `~/.local/bin/aura-browser-uninstall`.
It retains your profile and removes the old user-level launcher/menu entry that would shadow the system package.
Then install your distribution's package. Choose one installation method per user.

On Windows, Aura installs for the current user into `%LOCALAPPDATA%\Programs\Aura Browser`.
Open **Aura Browser** from the Start menu; a desktop shortcut is optional.
Remove it through Windows **Installed apps**. Shortcut launches use the profile at
`%LOCALAPPDATA%\Aura Browser\profile`.

For the Linux **`.run` installer**, run it as your regular user, **without sudo**. It installs into
`${XDG_DATA_HOME:-~/.local/share}/aura-browser` and adds an applications-menu entry.
The launcher is `~/.local/bin/aura-browser`; use the menu if that directory is not on your PATH.
Remove the application with `~/.local/bin/aura-browser-uninstall`.
The separate profile lives in `${XDG_DATA_HOME:-~/.local/share}/aura-browser-profile`.
Installation requires Bash, tar, gzip and sha256sum. Your distribution must provide the CEF/X11 runtime libraries.

The per-user Windows and `.run` installers preserve the profile on reinstall and uninstall. Close Aura before reinstalling.
Portable profiles are not migrated automatically. Installation does not change your default browser.
This preview has **no trusted Windows publisher signature**. The source now includes
daily update checks and an Ed25519-authenticated download/install flow; channel
publication and release validation are tracked in [the progress journal](docs/browser-roadmap-progress.md).
See [signed updates](docs/update-channel.md) for the release procedure.

### Portable downloads

Download `aura2-windows64.zip` or `aura2-linux64.zip`, then extract the **complete** archive.
Launch `Aura2/aura_browser.exe` on Windows or `Aura2/aura_browser` on Linux.
Keep all runtime files together. Direct launches create `profile/` beside the executable.

## Features

Release additions: [workspaces and tab groups](docs/tab-organization.md).
Their implementation and platform checks are tracked in [feature progress](docs/features-progress.md).

- Independent Chromium tabs: create, close, restore, reorder, pin, duplicate and mute.
- Unicode address bar, clipboard, history/bookmark suggestions and selectable search engines:
  DuckDuckGo, Google, Bing and Yandex.
- Navigation, reload/stop, site zoom, favicons, page titles and loading/error states.
- Native start page with search, editable quick links, imported wallpaper and automatic theme colors.
- Persistent cookies, cache, history, bookmarks, settings and URL-session restoration.
- Private tabs with a separate in-memory CEF request context.
- Downloads with progress, pause/resume, cancellation and folder opening.
- Find in page, context menus, JavaScript dialogs, Chromium DevTools and CEF printing.
- Six light/dark themes, accent colors and independently adjustable interface and start-page scales.
- Russian and English interface, including settings, menus and notifications.
- Built-in ad/tracker blocking, Dark Reader and signed Manifest V3 extensions.

Aura supports separate normal/private windows, session recovery and a password vault
with encrypted manual transfer (see below). Private addresses are excluded from Aura history/session;
downloaded files remain on disk. Session restoration keeps URLs, order, active and pinned tabs;
back/forward stacks and form contents are not restored. Update installation requires an explicit
action; native Linux packages use their package manager. Full CJK IME, DRM/codecs, printing and
different window managers need further validation.

If upgrading from preview.3.1, close Aura and install 0.4.0-preview.1 manually once;
that older published build did not include the new updater. Profiles are retained.
Future updates use signed metadata; native Linux packages continue through their package manager.

### Rendering and scrolling

Software rendering is the default on both platforms; Chromium's sandbox remains enabled.
Linux uses Skia raster/X11 with MIT-SHM and a two-buffer XPresent queue when available,
falling back to an offscreen-pixmap copy. Windows uses Skia raster and a persistent native DIB.
CEF frame rate follows the monitor, including 120/144/165 Hz; actual smoothness depends on page,
window size and CPU performance. Linux reads refresh rate through Wayland output information or RandR.

Version 0.4.0-preview.1 reduces antialiased raster clipping to the page perimeter, uses the same painter for
full/page-only redraws, and discovers Wayland refresh without blocking startup. Measurements,
75–200%/fractional-DPI pixel checks and limits are in [Linux performance](docs/linux-performance.md).
Available in the 0.4.0-preview.1 installers.

The experimental Linux EGL/GLES path remains in the source. It is not considered stable on the original
test machine after a complete system freeze. Native Wayland and Vulkan are not implemented.

See the [compatibility matrix](docs/compatibility.md) for tested APIs/sites and codec limits.
The supplied runtime does not provide H.264, HEVC or Widevine. WebM VP9+Opus playback is verified.
Linux WebGL is unavailable in the default software test; Windows context creation succeeds,
with full WebGL rendering unverified.

## Settings and customization

Open **Settings** from the sidebar or the toolbar gear. Settings are organized into **ten categories**,
searchable by English or Russian names/descriptions. **Ctrl F** focuses settings search.
**General → Interface language** switches between English and Russian immediately; website text is unaffected.

The **bookmark manager** (Ctrl B) adds folders, tags, search and bulk move/tag/delete with undo. **Export all as HTML** opens a Save dialog; HTML imports restore folders and tags. Save the current page with Ctrl D.

The **site data panel** (Ctrl Alt D) estimates quota-managed storage for an origin in the selected container. Delete its local storage, databases, caches and service workers separately from cookies; cookie deletion can sign you out across shared ports and subdomains. Open pages may recreate data.

The **site permissions center** (Ctrl Alt I or the address-bar icon) manages camera, microphone, notifications, location, clipboard, MIDI, sensors and camera controls per site and container. Revoke stops active capture; private rules last until Aura closes.

| Category | Options |
|---|---|
| General | Language, search engine, session restore, homepage, startup and new-tab behavior, site languages |
| Tabs | Position, background links, selection after close, exit confirmation, middle-click, wheel, sound |
| Appearance | Interface scale 75–200%, six themes, accent, site icons/addresses, link previews, animations |
| Web pages | Zoom, font family/size/minimum, JavaScript, images, website fonts, Local Storage, form/Tab navigation |
| Privacy | History size/retention, suggestion sources/count, clear on exit, DNT and Referer |
| Permissions | Popups, JS dialogs, notifications, location, camera, microphone, clipboard, MIDI, sensors |
| Downloads | Folder, filename prompt, date suffix, panel visibility/closing, notifications |
| Protection | Ad/tracker blocking, cosmetic filters, Dark Reader, brightness/contrast/sepia, exceptions, extensions |
| Data | Import bookmarks/history, clear history/cookies/cache, reset settings |
| Start page | Scale 50–200%, width/background, greeting, logo, clock, wallpaper/palette, link columns/layout |

Settings save automatically. Interface scale updates tabs, panels, controls and extension popups immediately.
Small windows cap the applied scale to keep controls accessible; enlarging the window restores your chosen
scale. **Ctrl Shift 0** resets interface scale. Site zoom and start-page scale remain independent.
Some page-content settings apply to new tabs; changing preferred website languages requires a restart.

History supports 100–10,000 entries and retention until cleared, 7/30/90 days or one year.
Disabling history does not erase existing entries. Clearing cookies signs you out of websites.
Resetting settings retains history, bookmarks and downloads. Clear-on-exit requires normal shutdown.
DNT requests cooperation from websites; it does not itself block tracking.

**Data → Import browser data** previews Firefox/Chromium profiles, Chromium JSON
bookmarks and HTML exports. Select bookmarks/history before applying; a profile
backup is saved first. See [supported formats, limits and restoration](docs/importing-browser-data.md).
Included in the current installers; legacy Preview 3.1 installers predate it.

### Start page

Use **Customize page** on the new tab page. Scale controls **− / 100% / +**, Ctrl +/−,
Ctrl+wheel and Ctrl+0 adjust its independent zoom. Scroll overflow with the wheel or Page Up/Down/Home/End.
Set width, glow, greeting, subtitle, logo, link cards/rows and up to six columns.

**Quick links** lets you add, rename, edit addresses, reorder, delete and undo deletion.
HTTP/HTTPS links are supported, with a safety limit of 1,000 entries. Right-click a link to manage it.

Import PNG/JPEG wallpaper through **Settings → Start page → Background image → Browse**,
or enter an absolute path. Aura copies the image into the profile. Fill/Fit and 0–80% dimming are available.
**Match theme to image** adapts shell colors while retaining your light/dark choice.
Images may be up to 20 MB and 8192 pixels per side; there is no 16-megapixel limit.
Dimensions are checked before decoding, and the in-memory copy is reduced. The original is unchanged.
Windows uses its native file dialog; Linux uses `zenity` when installed, with manual path entry as a fallback.

## Ad blocking, Dark Reader and extensions

The **shield** shows blocked requests and per-site controls. Blocking is enabled by default using
**Brave adblock-rust 0.13.3 + EasyList/EasyPrivacy**. Matching requests are canceled before loading;
cosmetic filters also hide dynamically inserted elements. Filter exceptions are respected.
Bundled list sources/checksums are in `resources/protection/dependencies.json`.
**Filter lists** are available in the shield/settings menu: manual and optional daily
EasyList/EasyPrivacy updates, separate list switches, custom rule editing/import and bundled recovery.
Failed or canceled updates keep the working rules. Private-only launches do not start background checks.
Supported advanced rules include literal procedural matching, CSS styling, tracking-parameter removal
on subresources and local noop JS/CSS/transparent-image replacements. Scriptlets, CSP response
modification, regex/XPath procedural rules and destructive DOM actions remain unsupported.
See [filter list management](docs/filter-lists.md).
Blocking every ad, including embedded video ads, is not guaranteed.

Built-in **Dark Reader 4.9.133** offers Off, Always on and Follow Aura theme, plus brightness,
contrast, sepia and domain exceptions. Changes apply to open pages. Protection scripts run in a separate
CEF world; global CSP/web security is not disabled. Third-party notices are in `resources/vendor/`.

Open the **puzzle** menu to manage extensions. On a Chrome Web Store extension page, select
**Install in Aura**, or paste a store URL, extension ID or path to a signed `.crx` into the manager.
Aura verifies the CRX3 signature and ID, then asks you to review permissions and confirm installation.
Installing, updating, disabling or removing extensions takes effect after restarting Aura.
Use **Check** for a Web Store installation, or select a newer signed CRX for a local one.
Added permissions are highlighted. **Restart Aura** applies changes and restores normal tabs; private tabs close.
The manager distinguishes a requested load from observed activity. See [extension lifecycle](docs/extensions-lifecycle.md)
for limits and verification.

Extension popups appear as overlays below the toolbar. Escape, clicking outside, close or clicking the
icon again dismisses them. Options pages open in tabs. Use **Pin** or the popup pin icon for quick toolbar
access; pinning persists immediately. Right-click a toolbar icon to unpin it. Overflow remains in the puzzle menu.
Popups are unavailable in private tabs.

**Manifest V3** content scripts, service workers, messaging, storage, declarativeNetRequest and extension
pages have been exercised. CEF OSR/Alloy does not provide Chrome's full toolbar or `chrome.tabs` model;
`tabs.query`, for example, may return an empty list. Extensions depending on these APIs may work only
partially. Full Chrome Web Store compatibility is not claimed. MV2 is rejected with an explanation.
Extensions are available only to the regular profile.

**Session snapshots** (Ctrl Alt E) save named sets of normal windows: URLs, order, active/pinned tabs, workspaces, groups, containers and split pairs. Restore opens new windows and keeps current pages. Cookies and unsaved forms are excluded; missing containers block restoration. Private tabs are never captured.

**Page screenshots** (Ctrl Shift S) save the visible document, a mouse/keyboard selection or the full page as PNG through a native Save dialog. Page zoom and display density are respected. Very large documents are reduced to 32 MP / 32766 pixels per side. Only the active page is captured in split view. Explicit captures work in private windows; no capture list is stored in the profile. Full-page capture requires leaving reader view.


Normal windows share a profile; private windows share temporary storage until the last private tab/window closes.
The Aura logo menu includes window commands and restoration of the previous normal session. After an interrupted
exit, Aura offers recovery. Sessions preserve window/tab order, selection and pinning, and exclude private URLs.
See [windows and sessions](docs/windows-and-sessions.md).

## Keyboard shortcuts

Open **Keyboard shortcuts** from the Aura menu or command palette to remap 34 commands. Click a combination and press new keys; Escape cancels recording. Conflicts keep the old binding. Each command can be disabled or reset; restore defaults resets the whole list. Changes are saved per profile and apply to all its windows. System and text editing combinations remain reserved; F3/F5 and tab navigation keep their standard alternatives. The table below lists defaults.

| Shortcut | Action |
|---|---|
| Ctrl L / Alt Home | Address bar / homepage |
| Ctrl T / Ctrl W / Ctrl Shift T | New / close / restore tab |
| Ctrl Tab / Ctrl Shift Tab | Next / previous tab |
| Ctrl 1…8 / Ctrl 9 | Select tab / last tab |
| Ctrl N / Ctrl Shift N | New window / private window |
| Ctrl Alt N | New private tab |
| Ctrl Shift E / Ctrl Alt G | Workspaces / tab groups  |
| Ctrl Alt S | Put current tab to sleep  |
| Ctrl Alt V | Two websites side by side / exit split  |
| Ctrl Alt C | Site containers  |
| Alt ← / Alt → | Back / forward |
| Ctrl R or F5 / Ctrl Shift R | Reload/stop / reload without cache |
| Ctrl + / Ctrl − / Ctrl 0 | Site zoom, or independent start-page zoom |
| Ctrl Shift 0 | Reset interface scale |
| Ctrl D / Ctrl B | Bookmark / bookmarks panel |
| Ctrl H / Ctrl J | History / downloads |
| Ctrl F / F3 / Shift F3 | Find / next / previous match |
| Ctrl P / F12 | Print / DevTools |
| F6 / F7 | Cycle theme / toggle light/dark |
| F10 | Focus browser interface from page |
| Tab / Shift Tab | Navigate controls in open panel |
| Enter / Space | Activate focused native control |
| Page Up / Page Down / Home / End | Scroll open panel |
| Ctrl Q / Ctrl Shift W | Exit Aura / close window |

Right-click a tab for pin/mute/duplicate/close; middle-click closes it.
The caption and borders move/resize the window; native caption controls minimize, maximize and close.

## Profiles and command-line use

Source launchers `./run.sh` and `./run.ps1` use `.aura-profile/` beside the project.
Installer profiles and portable profiles have separate locations described above.

| File/directory | Contents |
|---|---|
| `profile.toml` | Quick links, history, bookmarks, session, language and settings |
| `wallpaper.png` / `wallpaper.jpg` | Imported wallpaper copy |
| `cache/` | Persistent CEF profile, cookies and site storage |
| `extensions/` | Verified packages and enabled-extension registry |
| `downloads/` | Default download folder (configurable) |
| `aura.log`, `cef.log` | Logs |

Clearing Aura history does not clear CEF cookies/site data. Private CEF contexts stay in memory until exit.

```bash
./run.sh --url=https://example.com/ --theme=ember-light
./run.sh --data-dir=/tmp/aura-test-profile --scale=1.5
```

`--report=FILE.json` writes local state and a final shutdown summary; reports may contain visited URLs.

## Build from source

### Linux

Requirements: C++20 Clang, CMake 3.31+, Python 3.12+, pkg-config, X11/EGL/GLES3,
Xext/Xpresent/Xrandr, wayland-client/libxkbcommon development headers and
wayland-protocols, Rust/Cargo 1.88+,
OpenSSL/libarchive/SQLite3 headers and your distribution's CEF runtime dependencies.
Bootstrap downloads GN/Ninja and locked dependencies, but does not install system packages.
Allow several GB of disk space. URLs/checksums are pinned in `third_party/dependencies.lock.json`;
Cargo dependencies are pinned in `core/adblock/Cargo.lock`.

```bash
python3 scripts/bootstrap.py --jobs 8
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release --parallel 8
ctest --preset linux-x64-release
./run.sh
```

Use `python3 scripts/bootstrap.py --offline --jobs 8` to reuse populated dependency caches.

### Windows

Requirements: Visual Studio 2022 C++ tools/Windows SDK, LLVM clang-cl, CMake 3.31+,
Python 3.12+, Git and Rust 1.90+. In **x64 Native Tools Command Prompt**:

```bat
git clone https://github.com/Sttanors/aura-browser.git
cd aura-browser
git clone https://github.com/microsoft/vcpkg.git third_party/vcpkg
git -C third_party/vcpkg checkout 469a0504e4a6ebf6173d1c0f51a7fa34c382fd43
call third_party\vcpkg\bootstrap-vcpkg.bat -disableMetrics
python scripts/bootstrap.py --jobs 4
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release --parallel 4
ctest --preset windows-x64-release
python scripts/package.py
```

OpenSSL/libarchive/SQLite3 use vcpkg's static CRT triplet. Chromium sandbox is enabled.
`aura_browser.exe` is the CEF sandbox bootstrap and loads `aura_browser.dll`; both are required.
Use `./run.ps1` to launch the source build from PowerShell.
The optional legacy GPU harness requires bootstrap `--diagnostic-angle` and CMake
`-DAURA_BUILD_COMPATIBILITY_HARNESS=ON`; normal browser builds do not need it.

## Validation

[Windows browser CI](https://github.com/Sttanors/aura-browser/actions/runs/39602429743)
passed compilation, unit tests, native browser integration (Unicode input, settings,
filter editing, multiple normal/private windows and relaunch), portable packaging and installer checks.
The current development branch passes 170 CTest entries. Feature-specific GUI results
and remaining limits are recorded in [the current progress journal](docs/improvements-progress.md);
[the release journal](docs/browser-roadmap-progress.md) records earlier release checks.
All Linux GUI checks use an isolated software display; physical GPU/monitor behavior is a separate check.

```bash
python3 scripts/verify_browser.py --suite=all
```

Linux GUI checks require `weston`, `Xwayland`, `xdotool`, ImageMagick `import` and `xclip`.
Wallpaper checks also use `zenity` and Pillow. The wrapper creates its own headless Weston/Pixman
and Xwayland SHM display with GLX disabled; it does not use your desktop display sockets.
Direct GUI smoke scripts refuse to run outside that isolated display.
Checks cover navigation/input, tabs, bookmarks/find, dialogs/downloads, private cookies,
settings/content permissions, protection/extensions and persistence.
Individual suites include `browser`, `settings`, `features`, `interface` and `wallpaper`.
Native Wayland checks use `python3 scripts/verify_wayland.py` (and `--scale=2`),
requiring Weston 15 with libweston development headers, a C compiler and Pillow.
They use a separate private Pixman compositor without DISPLAY/XWayland/GPU;
see [the Wayland validation limits](docs/wayland.md).
Reports/screenshots are written under `artifacts/`. Windows GUI checks run only on disposable CI runners.

## Packaging and source layout

```bash
python3 scripts/package.py
python3 scripts/build_linux_installer.py
python3 -m unittest discover -s tests/python -p test_installers.py
```

Runtime archives include CEF, the application, resources and third-party notices.
Linux system libraries are not bundled. Use GitHub’s built-in **Source code (zip)** / **Source code (tar.gz)** downloads for source snapshots.
Release assets contain installable/portable binaries and their checksums; logo files stay in the application and repository.
The Windows installer is built with [Inno Setup](https://jrsoftware.org/isinfo.php);
Windows CI packages the freshly built browser and tests install/reinstall/uninstall, executable identity and native window icons.

| Directory | Purpose |
|---|---|
| `app/` | Browser entry point and CEF lifecycle |
| `core/browser/` | Tabs, request contexts, profiles, history, downloads and CEF handlers |
| `ui/` | Native Skia interface and input routing |
| `platform/linux/x11/`, `platform/windows/` | Platform windows, rendering and input |
| `common/` | Paint buffers, UTF-8 editor, geometry and scheduling |
| `design/` | Theme, typography, spacing and motion tokens |
| `packaging/` | Linux shell installer and Windows Inno Setup definitions |
| `docs/milestones/` | Detailed development/validation notes (currently mostly Russian) |

The legacy M0 diagnostic executable remains separate from the browser:
`./build/linux-x64-release/aura_diagnostic` and `scripts/verify_m0.py --software-rendering`.
GPU validation is separate and is not covered by normal software-rendered browser checks.
See [interface design notes](docs/design/interface-refinement.md) for implementation/accessibility details.

### Passwords and encrypted transfer

Aura provides a master-password vault, explicit HTTPS login filling and
manual transfer through an encrypted file. Open **Settings → Data → Passwords**
or press **Ctrl+Shift+Y**. [Usage, backup and limitations](docs/password-vault.md).
Available since 0.4.0-preview.1; earlier preview.3.1 installations require one manual upgrade.
